/* usb-bus: the xHCI + USB core driver process.
 *
 * One thread, one port. Everything it waits for arrives on that port:
 * the controller's interrupt (MSI / MSI-X entry 0 -> interrupter 0), its
 * DR_SERVE channel (the `usbbus` protocol, abi/idl/usbbus.idl; devmgr
 * holds the other end), and every channel it serves: one `usb` channel
 * per interface handed out (abi/idl/usb.idl) and one report channel per
 * open interrupt-IN endpoint. (Bulk data has no channel: it moves through
 * a buffer the class driver shares, bulk.c.) Packets for channels only
 * mark them pending; the main loop then reads DR_SERVE and the report
 * channels itself (nothing there waits), and hands a device's interface
 * channels to that device's task (task.c, work.c), which serves their
 * requests while other devices' tasks wait in theirs. The main loop also
 * starts the ports' tasks and runs every task that may go on.
 *
 * Files: hc.c the controller (registers, bring-up, events); command.c
 * the command ring; task.c the tasks;
 * ring.c the DMA page pool and transfer rings; devices.c the device table
 * and contexts; control.c control transfers and descriptors; intr.c
 * interrupt-IN endpoints; bulk.c bulk endpoints and transfers; config.c
 * configurations, endpoints and SET_INTERFACE; report.c log and RESULTS lines; attach.c enumeration and
 * detach; hub.c hubs; rootport.c root ports; work.c the tasks' work and
 * starting them; iface.c the `usb` protocol's requests; this file the channels,
 * the `usbbus` protocol and the loop.
 *
 * devmgr: for each interface of a configured device (hubs aside) usb-bus
 * writes a `usbbus.interface_attached` message on DR_SERVE (txid 0) with
 * the interface's `usb` channel; devmgr starts a class driver with it. A
 * device that goes away has all its channels closed.
 *
 * RESULTS: the controller line, then once enumeration first settles one
 * line per device (hubs included) and a summary; failures as they happen;
 * a summary again when devmgr stops the driver.
 *
 * Exit: when DR_SERVE's peer closes (devmgr stopping), usb-bus detaches
 * everything, halts and resets the controller and returns 0. Exit codes:
 * 0 clean; 1 the controller failed to come up or to stop; 2 missing
 * handles. */
#include <idl/usbbus.h>
#include "usbbus.h"

#define SETTLE_NS     (500 * NS_PER_MS)
#define MAX_WAITERS   8

static struct chan chans[MAX_CHANS];
/* wait_settled requests waiting for their answer. */
static struct {
    bool used;           /* taken */
    uint32_t txid;       /* the request's, for the reply */
    uint64_t deadline;   /* answer by then, settled or not */
} waiters[MAX_WAITERS];
static bool serve_closed;   /* DR_SERVE's peer is gone (or there was none) */

/* ---- channels ------------------------------------------------------------------ */

int chan_add(handle_t h, uint8_t kind, uint32_t dev_id, uint8_t a)
{
    for (int i = 0; i < MAX_CHANS; i++) {
        struct chan *c = &chans[i];
        if (c->h)
            continue;
        c->gen++;
        uint64_t key = KEY_CHAN | ((uint64_t)c->gen << 8) | (uint64_t)i;
        status_t st = drv_port_bind(g_hc.port, h, key, SIG_READABLE | SIG_PEER_CLOSED,
                                    PORT_BIND_PERSISTENT);
        if (st != OK) {
            drv_log("can't watch a channel: %s", status_str(st));
            return -1;
        }
        c->h = h;
        c->kind = kind;
        c->a = a;
        c->dev_id = dev_id;
        c->serving = c->close_after = false;
        c->pending = true;   /* look once: something may be there already */
        return i;
    }
    drv_log("more than %u channels", MAX_CHANS);
    return -1;
}

void chan_close(int i)
{
    if (i < 0 || i >= MAX_CHANS || !chans[i].h)
        return;
    if (chans[i].serving) {
        /* A device task is in a request on it and will write the reply
         * to this handle: closed when that request ends (serve_chan). */
        chans[i].close_after = true;
        return;
    }
    drv_handle_close(chans[i].h);
    chans[i].h = HANDLE_INVALID;
    chans[i].pending = false;
    chans[i].gen++;
}

handle_t chan_handle(int i)
{
    return i >= 0 && i < MAX_CHANS ? chans[i].h : HANDLE_INVALID;
}

int chan_slot(const struct chan *c)
{
    return (int)(c - chans);
}

/* The interface channels first, then the report channels: a class driver
 * that sees its reports channel close finds DR_USB already closed when the
 * device is gone (hid tells "unplugged" from "endpoint given up" so). */
void serve_iface_gone(uint32_t dev_id)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MAX_CHANS; i++)
            if (chans[i].h && chans[i].dev_id == dev_id &&
                (chans[i].kind == CHAN_IFACE) == (pass == 0))
                chan_close(i);
}

void serve_packet(struct hc *h, const struct port_packet *p)
{
    if (p->key == KEY_SERVE) {
        h->serve_pending = true;
        if (p->signal.observed & SIG_PEER_CLOSED)
            h->stopping = true;   /* devmgr is stopping us: cut the waits short */
        return;
    }
    if ((p->key & ~0xffffffull) == KEY_CHAN) {
        unsigned i = p->key & 0xff, gen = (p->key >> 8) & 0xffff;
        if (i < MAX_CHANS && chans[i].h && chans[i].gen == gen)
            chans[i].pending = true;
    }
}

bool serve_report(int c, const void *data, uint32_t len, bool *dropped)
{
    if (c < 0 || c >= MAX_CHANS || !chans[c].h)
        return false;
    status_t st = drv_channel_write(chans[c].h, data, len, NULL, 0);
    if (st == ERR_SHOULD_WAIT || st == ERR_NO_RESOURCES) {
        *dropped = true;   /* the class driver is behind: its queue is full */
        return true;
    }
    return st == OK;
}

/* A new `usb` channel for interface `num` of d: our end served, the
 * other end returned. */
static status_t iface_channel(struct usbdev *d, uint8_t num, handle_t *out, int *slot)
{
    if (!usb_iface(d, num))
        return ERR_NOT_FOUND;
    handle_t a, b;
    status_t st = drv_channel_create(&a, &b);
    if (st != OK)
        return st;
    int c = chan_add(a, CHAN_IFACE, d->id, num);
    if (c < 0) {
        drv_handle_close(a);
        drv_handle_close(b);
        return ERR_NO_RESOURCES;
    }
    *out = b;
    if (slot)
        *slot = c;
    return OK;
}

static void copy_str(uint8_t *out, unsigned cap, const char *s)
{
    unsigned i = 0;
    for (; i + 1 < cap && s[i]; i++)
        out[i] = (uint8_t)s[i];
    for (; i < cap; i++)
        out[i] = 0;
}

void serve_device_ready(struct usbdev *d)
{
    if (serve_closed)
        return;
    for (int i = 0; i < d->nifs; i++) {
        struct iface *f = &d->ifs[i];
        handle_t b;
        int c;
        if (iface_channel(d, f->number, &b, &c) != OK)
            continue;
        struct usbbus_interface_attached_req m;
        m.txid = 0;
        m.ordinal = USBBUS_INTERFACE_ATTACHED;
        m.id = d->id;
        m.vendor = d->vid;
        m.product = d->pid;
        m.interface_number = f->number;
        m.class_code = f->cls;
        m.subclass = f->sub;
        m.protocol = f->proto;
        m.speed = d->speed;
        copy_str(m.path, sizeof(m.path), d->path);
        status_t st = drv_channel_write(g_hc.serve, &m, sizeof(m), &b, 1);
        if (st != OK) {
            drv_log("usb %s: interface %u: can't tell devmgr (%s)", d->path, f->number,
                    status_str(st));
            drv_handle_close(b);
            chan_close(c);
            continue;
        }
        f->devmgr_chan = c;
    }
}

/* ---- the usbbus protocol (DR_SERVE) ----------------------------------------------------- */

static bool settled(void)
{
    return !usb_busy() && drv_clock_ns() - g_last_change_ns >= SETTLE_NS;
}

static status_t b_status(void *ctx, uint32_t *devices, uint32_t *hubs, uint32_t *ifaces,
                         uint32_t *hid, uint32_t *problems, uint32_t *generation,
                         uint8_t *is_settled)
{
    (void)ctx;
    usb_counts(devices, hubs, ifaces, hid, problems);
    *generation = g_generation;
    *is_settled = settled();
    return OK;
}

static struct usbdev *nth_dev(uint32_t index)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || d->gone || !d->vid)
            continue;
        if (!index--)
            return d;
    }
    return NULL;
}

static status_t b_device(void *ctx, uint32_t index, uint32_t *id, uint32_t *parent_id,
                         uint16_t *vendor, uint16_t *product, uint16_t *bcd, uint8_t *speed,
                         uint8_t *address, uint8_t *slot, uint8_t *root_port, uint8_t *port,
                         uint8_t *level, uint32_t *route, uint8_t *tt_slot, uint8_t *tt_port,
                         uint8_t *cls, uint8_t *sub, uint8_t *proto, uint8_t *nconfigs,
                         uint8_t *config, uint8_t *nifs, uint16_t *mps0, uint8_t *hub_ports,
                         uint8_t path[24], uint8_t name[40], uint8_t serial[24])
{
    (void)ctx;
    struct usbdev *d = nth_dev(index);
    if (!d)
        return ERR_OUT_OF_RANGE;
    *id = d->id;
    *parent_id = d->parent >= 0 ? g_devs[d->parent].id : 0;
    *vendor = d->vid;
    *product = d->pid;
    *bcd = d->bcd;
    *speed = d->speed;
    *address = d->address;
    *slot = d->slot;
    *root_port = d->root_port;
    *port = d->port;
    *level = d->level;
    *route = d->route;
    *tt_slot = d->tt_slot;
    *tt_port = d->tt_port;
    *cls = d->cls;
    *sub = d->sub;
    *proto = d->proto;
    *nconfigs = d->nconfigs;
    *config = d->configured ? d->cfg_value : 0;
    *nifs = d->nifs;
    *mps0 = d->mps0;
    *hub_ports = d->is_hub ? d->hub_ports : 0;
    copy_str(path, 24, d->path);
    copy_str(name, 40, d->product);
    copy_str(serial, 24, d->serial);
    return OK;
}

static status_t b_interface(void *ctx, uint32_t id, uint8_t index, uint8_t *number, uint8_t *alt,
                            uint8_t *nalts, uint8_t *cls, uint8_t *sub, uint8_t *proto,
                            uint8_t *nep, uint8_t eps[8])
{
    (void)ctx;
    struct usbdev *d = dev_find(id);
    if (!d)
        return ERR_NOT_FOUND;
    if (index >= d->nifs)
        return ERR_OUT_OF_RANGE;
    struct iface *f = &d->ifs[index];
    *number = f->number;
    *alt = f->alt;
    *nalts = f->num_alts;
    *cls = f->cls;
    *sub = f->sub;
    *proto = f->proto;
    *nep = f->nep;
    for (int i = 0; i < 8; i++)
        eps[i] = i < f->nep ? f->ep_addr[i] : 0;
    return OK;
}

static status_t b_open_interface(void *ctx, uint32_t id, uint8_t num, handle_t *out)
{
    (void)ctx;
    struct usbdev *d = dev_find(id);
    if (!d || !d->configured)
        return ERR_NOT_FOUND;
    /* A hub is usb-bus's own: a client's set_interface or endpoint calls
     * would take its status-change endpoint away, and usb-bus would stop
     * seeing its ports change. */
    struct iface *f = usb_iface(d, num);
    if (d->is_hub || (f && f->cls == 9))
        return ERR_ACCESS_DENIED;
    return iface_channel(d, num, out, NULL);
}

static const struct usbbus_ops bus_ops = {
    .status = b_status,
    .device = b_device,
    .interface = b_interface,
    .open_interface = b_open_interface,
};

static void reply_waiter(int i)
{
    struct usbbus_wait_settled_rep r;
    r.txid = waiters[i].txid;
    r.status = OK;
    uint32_t n, nh, ni, nhid, np;
    usb_counts(&n, &nh, &ni, &nhid, &np);
    r.devices = n;
    r.hubs = nh;
    r.interfaces = ni;
    r.hid_interfaces = nhid;
    r.problems = np;
    r.generation = g_generation;
    r.settled = settled();
    (void)drv_channel_write(g_hc.serve, &r, sizeof(r), NULL, 0);   /* devmgr gone: no one waits */
    waiters[i].used = false;
}

/* Everything queued on DR_SERVE. wait_settled is kept for later; the rest
 * go through the generated dispatch. */
static void serve_bus(struct hc *h)
{
    for (int guard = 0; guard < 64; guard++) {
        _Alignas(8) uint8_t q[USBBUS_REQ_MAX + 8];
        _Alignas(8) uint8_t r[USBBUS_REP_MAX];
        handle_t hs[IDL_READ_HANDLES];
        uint32_t n = 0, nh = 0;
        status_t st = drv_channel_read(h->serve, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
        if (st == ERR_BUFFER_TOO_SMALL) {
            (void)idl_drain(h->serve, n, nh);   /* the next read says what is left */
            continue;
        }
        if (st == ERR_PEER_CLOSED) {
            serve_closed = true;
            return;
        }
        if (st != OK)
            return;
        if (nh) {
            idl_close_all(hs, nh);
            idl_reply_status(h->serve, q, n, ERR_INVALID_ARGS);
            continue;
        }
        const struct idl_req_hdr *hdr = (const void *)q;
        if (n == sizeof(struct usbbus_wait_settled_req) && hdr->ordinal == USBBUS_WAIT_SETTLED) {
            const struct usbbus_wait_settled_req *w = (const void *)q;
            int i = 0;
            while (i < MAX_WAITERS && waiters[i].used)
                i++;
            if (i == MAX_WAITERS) {
                idl_reply_status(h->serve, q, n, ERR_NO_RESOURCES);
                continue;
            }
            waiters[i].used = true;
            waiters[i].txid = w->txid;
            uint32_t ms = w->timeout_ms > 60000 ? 60000 : w->timeout_ms;
            waiters[i].deadline = drv_clock_ns() + (uint64_t)ms * NS_PER_MS;
            continue;
        }
        handle_t rhs[IDL_REP_HANDLES];
        uint32_t rhn = 0;
        uint32_t rn = usbbus_dispatch(&bus_ops, NULL, q, n, r, rhs, &rhn);
        if (!rn || drv_channel_write(h->serve, r, rn, rhs, rhn) != OK)
            idl_close_all(rhs, rhn);
    }
    /* 64 taken and maybe more queued: the binding is edge-triggered and the
     * channel stays readable, so no packet will say so. Come back. */
    h->serve_pending = true;
}

/* Report channels carry nothing our way: drop what comes. */
static status_t drop_input(const struct chan *c)
{
    uint8_t b[64];
    handle_t hs[4];
    uint32_t n, nh;
    status_t st = drv_channel_read(c->h, b, sizeof(b), &n, hs, 4, &nh);
    if (st == ERR_BUFFER_TOO_SMALL) {
        st = idl_drain(c->h, n, nh);
    } else if (st == OK) {
        for (uint32_t k = 0; k < nh; k++)
            drv_handle_close(hs[k]);
    }
    return st;
}

/* Channel i's peer is gone. A report channel's endpoint is closed by the
 * main loop (ep_close closes the channel); an interface channel closes. */
static void chan_peer_closed(int i)
{
    struct chan *c = &chans[i];
    if (c->kind == CHAN_REPORTS) {
        struct usbdev *d = dev_find(c->dev_id);
        if (d && c->a < 32 && d->eps[c->a].chan == i)
            d->ep_drop |= 1u << c->a;   /* ep_close closes the channel */
        else
            chan_close(i);
    } else {
        struct usbdev *d = dev_find(c->dev_id);
        struct iface *f = d ? usb_iface(d, c->a) : NULL;
        if (f && f->devmgr_chan == i)
            f->devmgr_chan = -1;
        if (f)
            bulk_chan_closed(d, f, i);   /* the bulk pair it opened goes with it */
        chan_close(i);
    }
}

/* Channel i's queued requests (a bounded number). On an interface
 * channel a request can wait: that runs in the device's task. */
static bool serve_chan(int i)
{
    struct chan *c = &chans[i];
    c->pending = false;
    /* A time budget as well as a count: one request can take a second (a
     * device that NAKs a control transfer until the timeout), and 64 of
     * them from one client would hold its device's other clients for a
     * minute. */
    uint64_t t0 = drv_clock_ns();
    for (int guard = 0; guard < 64 && c->h && drv_clock_ns() - t0 < 20 * NS_PER_MS; guard++) {
        c->serving = true;
        status_t st = c->kind == CHAN_IFACE ? iface_serve_one(c) : drop_input(c);
        c->serving = false;
        if (c->close_after) {
            c->close_after = false;
            chan_close(i);
            return true;
        }
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED)
            chan_peer_closed(i);
        return true;
    }
    c->pending = c->h != HANDLE_INVALID;   /* more than 64 queued: come back */
    return true;
}

bool serve_device_chans(uint32_t dev_id)
{
    bool did = false;
    for (int i = 0; i < MAX_CHANS; i++)
        if (chans[i].h && chans[i].pending && chans[i].kind == CHAN_IFACE &&
            chans[i].dev_id == dev_id)
            did |= serve_chan(i);
    return did;
}

void serve_chans_dispatch(void)
{
    for (int i = 0; i < MAX_CHANS; i++) {
        struct chan *c = &chans[i];
        if (!c->h || !c->pending || c->serving)
            continue;
        if (c->kind == CHAN_REPORTS || !dev_find(c->dev_id)) {
            serve_chan(i);   /* no waits: drops, or answers PEER_CLOSED at once */
            continue;
        }
        if (!task_find(TASK_DEVICE, c->dev_id, 0))
            (void)task_start(TASK_DEVICE, c->dev_id, 0, device_task);   /* no slot: next round */
    }
}

bool serve_main_pending(void)
{
    for (int i = 0; i < MAX_CHANS; i++) {
        const struct chan *c = &chans[i];
        if (c->h && c->pending && !c->serving &&
            (c->kind == CHAN_REPORTS || !dev_find(c->dev_id)))
            return true;
    }
    return false;
}

/* ---- main --------------------------------------------------------------------------- */

/* Fresh state, and the handles from s: 2 (the exit code) if one we need
 * is missing, else 0. */
static int take_handles(struct hc *h, const struct driver_start *s)
{
    /* A new process's statics are zero already (a restart is always a new
     * process); clearing them here keeps that from mattering. */
    __builtin_memset(h, 0, sizeof(*h));
    __builtin_memset(chans, 0, sizeof(chans));
    __builtin_memset(waiters, 0, sizeof(waiters));
    serve_closed = false;
    usb_reset_state();
    tasks_reset();
    h->name = s->name;
    h->dev = drv_handle(s, DR_PCIDEV);
    h->bar = drv_handle(s, DR_BAR(0));
    h->irq = drv_handle(s, DR_IRQ(0));
    h->dma = drv_handle(s, DR_DMA);
    h->serve = drv_handle(s, DR_SERVE);
    h->port = h->ctx_vmo = h->sp_vmo = h->pool_vmo = HANDLE_INVALID;
    if (h->dev == HANDLE_INVALID || h->bar == HANDLE_INVALID || h->irq == HANDLE_INVALID ||
        h->dma == HANDLE_INVALID) {
        drv_report("missing handles: DR_PCIDEV %s, DR_BAR(0) %s, DR_IRQ(0) %s, DR_DMA %s",
                   h->dev == HANDLE_INVALID ? "no" : "yes",
                   h->bar == HANDLE_INVALID ? "no" : "yes",
                   h->irq == HANDLE_INVALID ? "no" : "yes",
                   h->dma == HANDLE_INVALID ? "no" : "yes");
        return 2;
    }
    return 0;
}

/* The device table, and the port everything arrives on, with DR_SERVE
 * bound to it. 0, or the exit code. */
static int setup_port(struct hc *h)
{
    g_devs = drv_malloc(MAX_DEVS * sizeof(struct usbdev));
    if (!g_devs)
        return 2;
    __builtin_memset(g_devs, 0, MAX_DEVS * sizeof(struct usbdev));
    status_t st = drv_port_create(&h->port);
    if (st != OK) {
        drv_report("no port: %s", status_str(st));
        return 1;
    }
    if (h->serve != HANDLE_INVALID) {
        st = drv_port_bind(h->port, h->serve, KEY_SERVE, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
        if (st != OK)
            drv_log("can't watch DR_SERVE (%s): serving nothing", status_str(st));
        h->serve_pending = true;
    } else {
        serve_closed = true;   /* nobody to serve: enumerate, report, stop */
    }
    return 0;
}

/* Answer the wait_settled requests that are due (is_settled: now, or at
 * their deadline); the earliest deadline still waiting, or next. */
static uint64_t answer_waiters(bool is_settled, uint64_t now, uint64_t next)
{
    for (int i = 0; i < MAX_WAITERS; i++) {
        if (!waiters[i].used)
            continue;
        if (is_settled || now >= waiters[i].deadline)
            reply_waiter(i);
        else if (waiters[i].deadline < next)
            next = waiters[i].deadline;
    }
    return next;
}

/* The main loop, from the first scan until devmgr stops us (or, with no
 * DR_SERVE, until the list is out); then every device goes. Each round:
 * DR_SERVE, the channels, new tasks for the ports' and devices' work,
 * every task that may go on; then a wait for the next packet, at most
 * until the earliest deadline anyone has. */
static void run(struct hc *h)
{
    report_controller(h);
    usb_start(h);
    uint64_t start = drv_clock_ns(), no_serve_end = start + 10000 * NS_PER_MS;
    for (;;) {
        if (h->serve_pending) {
            h->serve_pending = false;
            serve_bus(h);
        }
        if (serve_closed || g_task_overflow)
            break;
        serve_chans_dispatch();
        work_dispatch(h);
        tasks_run();
        bool is_settled = settled();
        /* The list: once settled, and not before 2 s (USB 3 links may
         * still be training after the reset). */
        if (is_settled && !g_first_report_done && drv_clock_ns() - start >= 2000 * NS_PER_MS)
            usb_report_all(false);
        uint64_t now = drv_clock_ns();
        uint64_t next = answer_waiters(is_settled, now, now + 1000 * NS_PER_MS);
        if (!g_first_report_done && next > now + 100 * NS_PER_MS)
            next = now + 100 * NS_PER_MS;
        next = hub_retries(next);
        uint64_t retry = root_retries(h);   /* a failed port's retry, if sooner */
        if (retry < next)
            next = retry;
        next = tasks_next_wake(next);
        if (h->serve == HANDLE_INVALID && (g_first_report_done || now > no_serve_end))
            break;
        if (h->serve_pending || serve_main_pending())
            next = now;   /* more to do at once: just look at the port */
        hc_wait_idle(h, next);
    }
    if (!g_first_report_done)
        usb_report_all(false);
    else if (g_generation != g_report_generation)
        usb_report_all(true);   /* what came since the list, and the counts now */
    h->stopping = true;
    usb_stop_all(h);
}

int driver_main(const struct driver_start *s)
{
    struct hc *h = &g_hc;
    int r = take_handles(h, s);
    if (r == 0)
        r = setup_port(h);
    if (r != 0)
        return r;
    r = hc_bring_up(h);
    if (r == 0)
        run(h);
    for (int i = 0; i < MAX_CHANS; i++)
        chan_close(i);
    int q = hc_shutdown(h);
    if (r == 0)
        r = q;
    if (q == 0)
        bulk_unpin_parked();   /* halted and reset: nothing runs into them now */
    drv_log("stopped: %lu interrupts, %lu events, DMA pool peak %u of %u pages, at most %u bulk "
            "transfer(s) at once", h->irqs, h->events, h->pool_peak, POOL_PAGES, h->bulk_peak);
    if (g_task_overflow)
        r = 7;
    tasks_free();
    if (h->port != HANDLE_INVALID)
        drv_handle_close(h->port);
    hc_release(h, q == 0);
    drv_free(g_devs);
    g_devs = NULL;
    return r;
}
