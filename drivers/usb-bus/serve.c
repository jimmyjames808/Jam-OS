/* usb-bus: the xHCI + USB core driver process.
 *
 * One thread, one port. Everything it waits for arrives on that port:
 * the controller's interrupt (MSI / MSI-X entry 0 -> interrupter 0), its
 * DR_SERVE channel (the `usbbus` protocol, abi/idl/usbbus.idl; devmgr
 * holds the other end), and every channel it serves: one `usb` channel
 * per interface handed out (abi/idl/usb.idl) and one report channel per
 * open interrupt-IN endpoint. Packets for channels only mark them
 * pending, so a wait deep inside an enumeration (hc_wait) never runs a
 * request; the main loop serves them between steps.
 *
 * Files: hc.c the controller (registers, rings, commands, events);
 * devices.c the device table and contexts; control.c control transfers
 * and descriptors; intr.c interrupt-IN endpoints; config.c configurations,
 * endpoints and SET_INTERFACE; report.c log and RESULTS lines; attach.c
 * enumeration and detach; hub.c hubs; rootport.c root ports; work.c the
 * port work the loop drives; this file the servers and the loop.
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
#include "usbbus.h"
#include <idl/usb.h>
#include <idl/usbbus.h>

#define SETTLE_NS     (500 * NS_PER_MS)
#define MAX_WAITERS   8

struct chan {
    handle_t h;
    uint8_t kind;
    uint8_t a;           /* CHAN_IFACE: interface number; CHAN_REPORTS: DCI */
    bool pending;
    uint16_t gen;
    uint32_t dev_id;
};

static struct chan chans[MAX_CHANS];
static struct {
    bool used;
    uint32_t txid;
    uint64_t deadline;
} waiters[MAX_WAITERS];
static bool serve_closed;

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
    drv_handle_close(chans[i].h);
    chans[i].h = HANDLE_INVALID;
    chans[i].pending = false;
    chans[i].gen++;
}

handle_t chan_handle(int i)
{
    return i >= 0 && i < MAX_CHANS ? chans[i].h : HANDLE_INVALID;
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

static struct usbdev *dev_find(uint32_t id)
{
    for (int i = 0; id && i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].id == id && !g_devs[i].gone)
            return &g_devs[i];
    return NULL;
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

/* ---- the usb protocol (one interface per channel) ---------------------------------- */

static struct usbdev *ctx_dev(void *ctx, struct iface **f)
{
    struct chan *c = ctx;
    struct usbdev *d = dev_find(c->dev_id);
    *f = d ? usb_iface(d, c->a) : NULL;
    return *f ? d : NULL;
}

static status_t cc_status(uint32_t cc)
{
    switch (cc) {
    case CC_SUCCESS: return OK;
    case CC_STALL: return ERR_NOT_SUPPORTED;   /* the device refused the request */
    case CC_TIMEOUT: return ERR_TIMED_OUT;
    case CC_GONE: return ERR_PEER_CLOSED;
    case CC_PARAMETER: return ERR_INVALID_ARGS;
    case CC_BANDWIDTH: case CC_RESOURCE: return ERR_NO_RESOURCES;
    default: return ERR_INTERNAL;              /* a transfer error (logged) */
    }
}

static status_t u_info(void *ctx, uint16_t *vendor, uint16_t *product, uint8_t *speed,
                       uint8_t *ifnum, uint8_t *cls, uint8_t *sub, uint8_t *proto, uint8_t *nep,
                       uint8_t *alt, uint8_t *address)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    *vendor = d->vid;
    *product = d->pid;
    *speed = d->speed;
    *ifnum = f->number;
    *cls = f->cls;
    *sub = f->sub;
    *proto = f->proto;
    *nep = f->nep;
    *alt = f->alt;
    *address = d->address;
    return OK;
}

static status_t u_get_descriptor(void *ctx, uint8_t type, uint8_t index, uint16_t lang,
                                 uint16_t length, uint8_t ir, uint16_t *actual, uint8_t data[1024])
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    if (!length || length > 1024 || ir > 1)
        return ERR_INVALID_ARGS;
    uint32_t n = 0;
    uint32_t cc = usb_control(d, ir ? 0x81 : 0x80, 6, (uint16_t)(type << 8 | index),
                              ir ? f->number : lang, length, data, &n, 1000);
    *actual = (uint16_t)n;
    return cc_status(cc);
}

/* May this interface's client send this request? Its own interface or
 * one of its endpoints only; of the standard requests only the harmless
 * ones (the rest would change state usb-bus keeps: SET_INTERFACE goes
 * through set_interface, CLEAR_FEATURE(HALT) would desync the xHC). */
static status_t check_request(struct iface *f, uint8_t rt, uint8_t req, uint16_t index)
{
    uint8_t recip = rt & 0x1f, type = (rt >> 5) & 3;
    if (recip == 1) {
        if ((index & 0xff) != f->number)
            return ERR_ACCESS_DENIED;
    } else if (recip == 2) {
        bool mine = false;
        for (int i = 0; i < f->nep; i++)
            mine |= f->ep_addr[i] == (index & 0xff);
        if (!mine)
            return ERR_ACCESS_DENIED;
    } else {
        return ERR_ACCESS_DENIED;
    }
    if (type == 0 && !(req == 0 || req == 6 || req == 10))   /* GET_STATUS/DESCRIPTOR/INTERFACE */
        return ERR_ACCESS_DENIED;
    if (type == 3)
        return ERR_ACCESS_DENIED;
    return OK;
}

static status_t u_control_in(void *ctx, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                             uint16_t length, uint16_t *actual, uint8_t data[1024])
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    if (!(rt & 0x80) || length > 1024)
        return ERR_INVALID_ARGS;
    status_t st = check_request(f, rt, req, index);
    if (st != OK)
        return st;
    uint32_t n = 0;
    uint32_t cc = usb_control(d, rt, req, value, index, length, data, &n, 1000);
    *actual = (uint16_t)n;
    return cc_status(cc);
}

static status_t u_control_out(void *ctx, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                              uint16_t length, const uint8_t data[64])
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    if ((rt & 0x80) || length > 64)
        return ERR_INVALID_ARGS;
    status_t st = check_request(f, rt, req, index);
    if (st != OK)
        return st;
    uint8_t buf[64];
    __builtin_memcpy(buf, data, length);
    uint32_t n = 0;
    return cc_status(usb_control(d, rt, req, value, index, length, buf, &n, 1000));
}

static struct ep *iface_ep(struct usbdev *d, struct iface *f, uint8_t addr)
{
    for (int i = 0; i < f->nep; i++)
        if (f->ep_addr[i] == addr) {
            uint8_t dci = ep_dci(addr);
            if (dci >= 2 && d->eps[dci].dci == dci)
                return &d->eps[dci];
        }
    return NULL;
}

static status_t u_open_interrupt_in(void *ctx, uint8_t endpoint, handle_t *reports,
                                    uint16_t *max_packet, uint8_t *interval_ms)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    struct ep *e = (endpoint & 0x80) ? iface_ep(d, f, endpoint) : NULL;
    if (!e || e->type != EPT_INTR_IN)
        return ERR_INVALID_ARGS;
    if (!e->configured)
        return ERR_BAD_STATE;
    if (e->open) {
        /* A class driver that died and was restarted may ask before the
         * main loop reaped its old report channel: if that one's reader is
         * gone, the endpoint is free. */
        signals_t seen = 0;
        handle_t old = chan_handle(e->chan);
        if (e->owner != EP_OWNER_CLIENT || !old ||
            drv_object_wait_one(old, SIG_PEER_CLOSED, 0, &seen) != OK)
            return ERR_ALREADY_BOUND;
        ep_close(d, e);
    }
    handle_t a, b;
    status_t st = drv_channel_create(&a, &b);
    if (st != OK)
        return st;
    int c = chan_add(a, CHAN_REPORTS, d->id, e->dci);
    if (c < 0) {
        drv_handle_close(a);
        drv_handle_close(b);
        return ERR_NO_RESOURCES;
    }
    if (ep_open_intr(d, e, EP_OWNER_CLIENT, c) != 0) {
        chan_close(c);
        drv_handle_close(b);
        return ERR_NO_RESOURCES;
    }
    uint32_t us = (1u << e->interval) * 125u;
    *reports = b;
    *max_packet = e->mps;
    *interval_ms = (uint8_t)(us < 1000 ? 1 : us / 1000 > 255 ? 255 : us / 1000);
    return OK;
}

static status_t u_endpoint_stats(void *ctx, uint8_t endpoint, uint64_t *reports,
                                 uint64_t *dropped, uint64_t *errors, uint8_t *open)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    struct ep *e = iface_ep(d, f, endpoint);
    if (!e)
        return ERR_INVALID_ARGS;
    *reports = e->reports;
    *dropped = e->dropped;
    *errors = e->errors;
    /* Open for a reader that is still there (a dead class driver's report
     * channel may not be reaped yet). */
    *open = 0;
    if (e->open) {
        signals_t seen = 0;
        handle_t c = chan_handle(e->chan);
        *open = c && drv_object_wait_one(c, SIG_PEER_CLOSED, 0, &seen) != OK;
    }
    return OK;
}

static status_t u_set_interface(void *ctx, uint8_t alt)
{
    struct iface *f;
    struct usbdev *d = ctx_dev(ctx, &f);
    if (!d)
        return ERR_PEER_CLOSED;
    return cc_status(dev_set_interface(d, f, alt));
}

static const struct usb_ops usb_ops = {
    .info = u_info,
    .get_descriptor = u_get_descriptor,
    .control_in = u_control_in,
    .control_out = u_control_out,
    .open_interrupt_in = u_open_interrupt_in,
    .endpoint_stats = u_endpoint_stats,
    .set_interface = u_set_interface,
};

/* ---- the usbbus protocol (DR_SERVE) ----------------------------------------------------- */

static bool settled(void)
{
    return !usb_busy() && drv_clock_ns() - g_last_change_ns >= SETTLE_NS;
}

static status_t b_status(void *ctx, uint32_t *devices, uint32_t *hubs, uint32_t *ifaces,
                         uint32_t *hid, uint32_t *problems, uint32_t *generation, uint8_t *st)
{
    (void)ctx;
    usb_counts(devices, hubs, ifaces, hid, problems);
    *generation = g_generation;
    *st = settled();
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
    drv_channel_write(g_hc.serve, &r, sizeof(r), NULL, 0);
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
            idl_drain(h->serve, n, nh);
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

static void serve_chan(int i)
{
    struct chan *c = &chans[i];
    c->pending = false;
    /* A time budget as well as a count: one request can take a second (a
     * device that NAKs a control transfer until the timeout), and 64 of
     * them from one client would hold the loop -- hot-plug, the other
     * class drivers, error recovery -- for a minute. */
    uint64_t t0 = drv_clock_ns();
    for (int guard = 0; guard < 64 && c->h && drv_clock_ns() - t0 < 20 * NS_PER_MS; guard++) {
        status_t st;
        if (c->kind == CHAN_IFACE) {
            st = usb_serve_one(c->h, &usb_ops, c);
        } else {
            /* Report channels carry nothing our way: drop what comes. */
            uint8_t b[64];
            handle_t hs[4];
            uint32_t n, nh;
            st = drv_channel_read(c->h, b, sizeof(b), &n, hs, 4, &nh);
            if (st == ERR_BUFFER_TOO_SMALL) {
                st = idl_drain(c->h, n, nh);
            } else if (st == OK) {
                for (uint32_t k = 0; k < nh; k++)
                    drv_handle_close(hs[k]);
            }
        }
        if (st == OK)
            continue;
        if (st == ERR_PEER_CLOSED) {
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
                chan_close(i);
            }
        }
        return;
    }
    c->pending = true;   /* more than 64 queued: come back */
}

/* ---- main --------------------------------------------------------------------------- */

static void report_controller(struct hc *h)
{
    char r2[48] = "", r3[48] = "";
    unsigned n2 = 0, n3 = 0;
    for (unsigned i = 0; i < h->nproto; i++) {
        char *o = h->proto[i].major >= 3 ? r3 : r2;
        unsigned *k = h->proto[i].major >= 3 ? &n3 : &n2;
        unsigned first = h->proto[i].first, last = first + h->proto[i].count - 1;
        /* "a-b" appended by hand: no snprintf in drivers */
        char t[16];
        unsigned tn = 0;
        if (*k)
            t[tn++] = ',';
        unsigned v[2] = { first, last };
        for (int j = 0; j < (first == last ? 1 : 2); j++) {
            if (j)
                t[tn++] = '-';
            char dg[4];
            int nd = 0;
            unsigned x = v[j];
            do {
                dg[nd++] = (char)('0' + x % 10);
                x /= 10;
            } while (x && nd < 3);
            while (nd)
                t[tn++] = dg[--nd];
        }
        for (unsigned j = 0; j < tn && *k + 1 < sizeof(r2); j++)
            o[(*k)++] = t[j];
        o[*k] = 0;
    }
    drv_report("xHCI %04x:%04x rev %02x: %u ports (USB 2: %s, USB 3: %s), %u slots, %u-byte "
               "contexts, %s, BIOS handoff %s", h->vendor, h->device, h->revision, h->ports,
               n2 ? r2 : "-", n3 ? r3 : "-", h->max_slots_en, h->csz,
               h->msix ? "MSI-X" : "MSI", h->handoff);
}

int driver_main(const struct driver_start *s)
{
    struct hc *h = &g_hc;
    /* Fresh state. A new process's statics are zero already (a restart is
     * always a new process); clearing them here keeps that from mattering. */
    __builtin_memset(h, 0, sizeof(*h));
    __builtin_memset(chans, 0, sizeof(chans));
    __builtin_memset(waiters, 0, sizeof(waiters));
    serve_closed = false;
    usb_reset_state();
    h->name = s->name;
    h->dev = drv_handle(s, DR_PCIDEV);
    h->bar = drv_handle(s, DR_BAR(0));
    h->irq = drv_handle(s, DR_IRQ(0));
    h->dma = drv_handle(s, DR_DMA);
    h->serve = drv_handle(s, DR_SERVE);
    h->port = h->ctx_vmo = h->sp_vmo = h->pool_vmo = HANDLE_INVALID;
    h->ctl_page = -1;
    if (h->dev == HANDLE_INVALID || h->bar == HANDLE_INVALID || h->irq == HANDLE_INVALID ||
        h->dma == HANDLE_INVALID) {
        drv_report("missing handles: DR_PCIDEV %s, DR_BAR(0) %s, DR_IRQ(0) %s, DR_DMA %s",
                   h->dev == HANDLE_INVALID ? "no" : "yes", h->bar == HANDLE_INVALID ? "no" : "yes",
                   h->irq == HANDLE_INVALID ? "no" : "yes", h->dma == HANDLE_INVALID ? "no" : "yes");
        return 2;
    }
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

    int r = hc_bring_up(h);
    if (r == 0) {
        report_controller(h);
        usb_start(h);
        uint64_t start = drv_clock_ns(), no_serve_end = start + 10000 * NS_PER_MS;
        for (;;) {
            if (h->serve_pending) {
                h->serve_pending = false;
                serve_bus(h);
            }
            if (serve_closed)
                break;
            for (int i = 0; i < MAX_CHANS; i++)
                if (chans[i].pending && chans[i].h)
                    serve_chan(i);
            bool did = usb_work(h);
            bool st_now = settled();
            /* The list: once settled, and not before 2 s (USB 3 links may
             * still be training after the reset). */
            if (st_now && !g_first_report_done && drv_clock_ns() - start >= 2000 * NS_PER_MS)
                usb_report_all(false);
            uint64_t now = drv_clock_ns(), next = now + 1000 * NS_PER_MS;
            for (int i = 0; i < MAX_WAITERS; i++) {
                if (!waiters[i].used)
                    continue;
                if (st_now || now >= waiters[i].deadline)
                    reply_waiter(i);
                else if (waiters[i].deadline < next)
                    next = waiters[i].deadline;
            }
            if (!g_first_report_done && next > now + 100 * NS_PER_MS)
                next = now + 100 * NS_PER_MS;
            if (h->serve == HANDLE_INVALID && (g_first_report_done || now > no_serve_end))
                break;
            if (did || (usb_busy() && !h->dead))   /* dead: usb_work does nothing; don't spin */
                next = now;
            bool any = h->serve_pending;
            for (int i = 0; i < MAX_CHANS && !any; i++)
                any = chans[i].pending && chans[i].h;
            if (!any)
                hc_wait_idle(h, next);
        }
        if (!g_first_report_done)
            usb_report_all(false);
        else if (g_generation != g_report_generation)
            usb_report_all(true);   /* what came since the list, and the counts now */
        h->stopping = true;
        usb_stop_all(h);
    }
    for (int i = 0; i < MAX_CHANS; i++)
        chan_close(i);
    int q = hc_shutdown(h);
    if (r == 0)
        r = q;
    drv_log("stopped: %lu interrupts, %lu events, DMA pool peak %u of %u pages", h->irqs,
            h->events, h->pool_peak, POOL_PAGES);
    if (h->port != HANDLE_INVALID)
        drv_handle_close(h->port);
    hc_release(h, q == 0);
    drv_free(g_devs);
    g_devs = NULL;
    return r;
}
