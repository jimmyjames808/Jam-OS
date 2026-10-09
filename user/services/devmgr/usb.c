/* devmgr: USB interfaces and their class drivers.
 *
 * usb-bus sends `usbbus_events.interface_attached` (abi/idl/usbbus.idl)
 * for each interface of a device it configured, on DR_EVENTS, a channel
 * devmgr makes for it when it starts it (usb_bus_handles) and serves from
 * the other end (usb_bus_events): the event carries ONE handle, that
 * interface's `usb` channel. devmgr keeps the channel in usb_ifs until the
 * device goes (usb-bus closes its end: PEER_CLOSED) or the usb-bus that
 * reported it dies. The DR_SERVE channel, whose client end anyone may be
 * handed (GET_SERVICE), carries `usbbus` requests and replies alone.
 *
 * Class drivers: an interface of class 3 (HID, every subclass: drv/hid
 * itself skips the ones that aren't boot keyboards or mice, exit 0) gets
 * drv/hid in a job of its own, supervised like any driver (a BIND_USB
 * binding in devs[]). Its handles:
 *   DR_USB    a DUPLICATE of the kept channel (devmgr's own copy stays), so
 *             a restarted hid gets a working channel to the same interface
 *             (usb-bus lets a new reader open an endpoint whose old report
 *             channel is dead). Replies to a dead hid's calls that were
 *             still queued are drained before the next start;
 *   DR_INPUT  a new source's channel, whose other end goes to the
 *             compositor (compctl.add_source, an event) on its INPUT
 *             channel, which init gives us (SR_CONSOLE, and
 *             DEVMGR_SET_CONSOLE again when the compositor restarts).
 *             Without one hid logs each key DOWN (keytest, init + utest).
 * Both arrive without RIGHT_DUPLICATE / RIGHT_TRANSFER.
 *
 * A mass-storage interface's driver (STORAGE_DRIVER, for the interfaces
 * usb_match gives it) is a class driver like hid, with two differences: it
 * serves the `storage` protocol on a DR_SERVE channel, and it gets no
 * DR_INPUT. Its binding is a disk's (b->disk): devmgr asks it for a
 * `storage` channel of volumes' own and hands it over (disks.c), and
 * tells volumes when the device has gone.
 *
 * How a hid ends decides what happens:
 *   - its interface is gone (unplugged, or its usb-bus died): the binding
 *     is freed, whatever the exit code; the next attach binds a new one;
 *   - exit 0 after the console it was connected to went away: restarted
 *     (not counted as a restart) as soon as there is a new console;
 *   - exit 0 otherwise: finished (a non-boot interface it skipped);
 *   - a crash, a kill, an error exit: restarted with backoff
 *     (supervise.c), given up on after 5 in a minute. */
#include <idl/compctl.h>
#include <idl/usbbus.h>
#include "internal.h"


/* What usb-bus said of an interface (usbbus_events.interface_attached). */
struct if_info {
    uint32_t id;                   /* usb-bus's device id */
    uint16_t vendor, product;
    uint8_t  interface_number, class_code, subclass, protocol, speed;
    char     path[24];             /* its port as usbbus.device tells it, NUL-terminated */
};

struct usb_if {
    handle_t ch;        /* our end of the interface's `usb` channel; 0: a free slot */
    uint32_t bus;       /* devs index of the usb-bus that reported it */
    int32_t  bind;      /* devs index of its class driver's binding; -1: none */
    uint16_t gen;       /* bumped at every use of the slot: in its port key */
    struct if_info info;   /* what usb-bus said about it */
};

/* A usb-bus's events channel: devmgr's end, and the usb-bus's binding. */
#define MAX_BUS_EVENTS 8
static struct bus_events {
    handle_t ch;        /* 0: a free slot */
    uint32_t bus;       /* devs index of the usb-bus */
    uint16_t gen;       /* bumped at every use of the slot: in its port key */
} bev[MAX_BUS_EVENTS];

static struct usb_if usb_ifs[MAX_USB_IFS];
static handle_t console;       /* our client end of the console (0: none) */
static uint32_t console_gen;   /* bumped with every console we are given */

static const char *if_path(const struct usb_if *u)
{
    return u->info.path;
}

static void drop(struct usb_if *u)
{
    jam_port_unbind(port, u->ch, KEY_IF_OF(u - usb_ifs, u->gen));
    jam_handle_close(u->ch);
    u->ch = HANDLE_INVALID;
    u->bind = -1;
}

void discard(handle_t h, uint32_t n, uint32_t nh)
{
    uint8_t *big = malloc(n ? n : 1);
    handle_t *bh = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = h, .rd_cap = n, .rd_bytes = (uint64_t)(uintptr_t)big,
        .rd_actual = (uint64_t)(uintptr_t)&n2, .rd_handles = (uint64_t)(uintptr_t)bh,
        .rd_hcap = nh, .rd_hactual = (uint64_t)(uintptr_t)&nh2,
    };
    if (big && bh && jam_channel_read(&a) == OK)
        for (uint32_t i = 0; i < nh2; i++)
            jam_handle_close(bh[i]);
    free(big);
    free(bh);
}

/* Everything queued on h (replies nobody waits for any more). */
static void drain(handle_t h)
{
    for (int guard = 0; guard < 256; guard++) {
        _Alignas(8) uint8_t buf[64];
        handle_t hs[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = h, .rd_cap = sizeof(buf), .rd_bytes = (uint64_t)(uintptr_t)buf,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = 4, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(h, n, nh);
            continue;
        }
        if (st != OK)
            return;
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
    }
}

static bool closed(handle_t h)
{
    signals_t seen = 0;
    return !h || jam_object_wait_one(h, SIG_PEER_CLOSED, 0, &seen) == OK;
}

/* ---- class drivers ------------------------------------------------------------------- */

static const char *usb_match(const struct if_info *m)
{
    if (m->class_code == 3)
        return "drv/hid";
    if (m->class_code == 8 && m->subclass == 6 && m->protocol == 0x50)
        return "drv/usb-storage";
    return NULL;
}

const char *usb_driver_arg(const struct binding *b)
{
    return hidboot && b->path && !strcmp(b->path, "drv/hid") ? "hidboot" : NULL;
}

static const char *what(const struct if_info *m)
{
    if (m->class_code == 3 && m->subclass == 1 && m->protocol == 1)
        return "boot keyboard";
    if (m->class_code == 3 && m->subclass == 1 && m->protocol == 2)
        return "boot mouse";
    if (m->class_code == 8)
        return "mass storage";
    return "not a boot device: hid skips it";
}

/* A free BIND_USB binding (or a new one). */
static struct binding *new_binding(void)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->kind == BIND_USB && !b->path && !b->proc && b->state == DEVMGR_SUP_NONE)
            return b;
    }
    if (ndevs == MAX_DEVS)
        return NULL;
    struct binding *b = &devs[ndevs++];
    *b = (struct binding){ .kind = BIND_USB, .usb_if = -1 };
    return b;
}

bool usb_gone(const struct binding *b)
{
    return b->usb_if < 0 || closed(usb_ifs[b->usb_if].ch);
}

bool usb_console_gone(const struct binding *b)
{
    return b->input_gen && (b->input_gen != console_gen || closed(console));
}

void usb_retire(struct binding *b, const char *why)
{
    say(false, "devmgr: %s %s: %s; binding freed", bdf(b), b->path ? b->path : "-", why);
    sup_reset(b);
    if (b->usb_if >= 0 && usb_ifs[b->usb_if].bind == b - devs)
        usb_ifs[b->usb_if].bind = -1;
    b->usb_if = -1;
    b->state = DEVMGR_SUP_NONE;
    b->console_wait = false;
    b->input_gen = 0;
    disks_gone(b);   /* a disk's driver: volumes forgets the disk */
    b->disk = false;
    b->path = NULL;   /* a free slot now */
}

status_t usb_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (usb_gone(b))
        return ERR_PEER_CLOSED;
    struct usb_if *u = &usb_ifs[b->usb_if];
    b->input_gen = 0;
    handle_t in = HANDLE_INVALID;
    if (console && !b->disk) {   /* a disk's driver has nothing to type */
        if (closed(console))
            return ERR_SHOULD_WAIT;   /* the compositor is restarting: wait for the new one */
        /* The source's channel: our end goes to the compositor
         * (compctl.add_source, an event: nothing waits, nothing comes
         * late), the other to the driver. */
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            return st;
        st = compctl_add_source(console, mine);   /* mine is gone either way */
        if (st != OK) {
            jam_handle_close(theirs);
            return st == ERR_PEER_CLOSED || st == ERR_SHOULD_WAIT ? ERR_SHOULD_WAIT : st;
        }
        st = jam_handle_replace(theirs, DEVMGR_DRV_CHAN_RIGHTS | RIGHT_TRANSFER, &in);
        if (st != OK)
            return st;   /* theirs is gone either way */
    }
    /* The dead driver's calls may have left replies behind. */
    drain(u->ch);
    handle_t h;
    status_t st = jam_handle_duplicate(u->ch, DEVMGR_DRV_CHAN_RIGHTS | RIGHT_TRANSFER, &h);
    if (st != OK) {
        if (in)
            jam_handle_close(in);
        return st;
    }
    x[*n] = (struct spawn_handle){ SR_DRIVER(DR_USB), h };
    xr[(*n)++] = DEVMGR_DRV_CHAN_RIGHTS;
    if (in) {
        x[*n] = (struct spawn_handle){ SR_DRIVER(DR_INPUT), in };
        xr[(*n)++] = DEVMGR_DRV_CHAN_RIGHTS;
        b->input_gen = console_gen;
    }
    return OK;
}

status_t usb_channel(const struct binding *b, handle_t *out)
{
    if (usb_gone(b))
        return ERR_PEER_CLOSED;
    drain(usb_ifs[b->usb_if].ch);   /* replies the stopped driver left behind */
    return jam_handle_duplicate(usb_ifs[b->usb_if].ch, RIGHT_SAME, out);
}

/* Start a class driver for interface slot `slot`. */
static void bind_interface(unsigned slot, const char *path)
{
    struct usb_if *u = &usb_ifs[slot];
    const struct if_info *m = &u->info;
    struct binding *b = new_binding();
    if (!b) {
        say(true, "devmgr: usb %04x:%04x if%u -> %s: no room for another driver", m->vendor,
            m->product, m->interface_number, path);
        return;
    }
    uint32_t gen = b->gen;   /* kept across uses: stale port packets stay stale */
    *b = (struct binding){ .kind = BIND_USB, .path = path, .gen = gen, .usb_if = (int32_t)slot,
                           .usb_id = m->id, .usb_ifnum = m->interface_number };
    b->info.vendor = m->vendor;
    b->info.device = m->product;
    /* "drv/hid" on port 6.1, interface 0: "hid-6.1:0" */
    snprintf(b->name, sizeof(b->name), "%s-%s:%u", path + 4, if_path(u), m->interface_number);
    u->bind = (int32_t)(b - devs);
    b->disk = strcmp(path, STORAGE_DRIVER) == 0;   /* its disk is volumes' (disks.c) */
    b->last = start_driver(b);
    if (b->last == ERR_SHOULD_WAIT) {
        b->state = DEVMGR_SUP_RESTARTING;
        b->restart_at = DEADLINE_NEVER;
        b->console_wait = true;
        say(false, "devmgr: %s %s: waits for the %s", bdf(b), path, usb_hub_name());
        return;
    }
    if (b->last != OK) {
        say(true, "devmgr: usb %04x:%04x if%u -> %s: bind FAILED (%s)", m->vendor, m->product,
            m->interface_number, path, status_str(b->last));
        problems++;
        usb_retire(b, "not started");
        return;
    }
    say(true, "devmgr: usb %04x:%04x if%u -> %s (%s, port %s)%s", m->vendor, m->product,
        m->interface_number, path, what(m), if_path(u),
        b->input_gen || b->disk ? "" : ", no console: to the log");
}

static void attached(struct binding *bus, const struct if_info *m, handle_t ch)
{
    unsigned slot = MAX_USB_IFS;
    for (unsigned i = 0; i < MAX_USB_IFS && slot == MAX_USB_IFS; i++)
        if (!usb_ifs[i].ch)
            slot = i;
    if (slot == MAX_USB_IFS) {
        say(true, "devmgr: usb %04x:%04x if%u: more than %u interfaces: ignored", m->vendor,
            m->product, m->interface_number, MAX_USB_IFS);
        jam_handle_close(ch);
        return;
    }
    struct usb_if *u = &usb_ifs[slot];
    u->ch = ch;
    u->bus = (uint32_t)(bus - devs);
    u->bind = -1;
    u->gen++;
    u->info = *m;
    if (jam_port_bind(port, ch, KEY_IF_OF(slot, u->gen), SIG_PEER_CLOSED, PORT_BIND_ONCE) != OK)
        say(false, "devmgr: usb %s if%u: can't watch its channel", if_path(u),
            m->interface_number);
    const char *path = usb_match(m);
    bool have = path && in_bootfs(path);
    say(false, "devmgr: usb %s %04x:%04x if%u (%02x/%02x/%02x) attached%s%s", if_path(u),
        m->vendor, m->product, m->interface_number, m->class_code, m->subclass, m->protocol,
        !path ? "; no class driver" : have ? "" : "; not in bootfs: ", path && !have ? path : "");
    if (have)
        bind_interface(slot, path);
}

/* Only a usb-bus reports interfaces: it alone gets an events channel, so
 * no other driver can have a class driver started on a channel it serves
 * itself -- with a console input source (a compromised driver could type
 * into the shell). */
static bool is_usb_bus(const struct binding *b)
{
    return b->kind == BIND_PCI && b->path && strcmp(b->path, "drv/usb-bus") == 0;
}

/* Slot e's channel is done (its usb-bus ended): what it still holds,
 * unread events and their handles, goes with it. */
static void bus_events_drop(struct bus_events *e)
{
    jam_port_unbind(port, e->ch, KEY_BUSEV_OF(e - bev, e->gen));
    jam_handle_close(e->ch);
    e->ch = HANDLE_INVALID;
}

status_t usb_bus_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (!is_usb_bus(b))
        return OK;
    struct bus_events *e = NULL;
    for (unsigned i = 0; i < MAX_BUS_EVENTS; i++) {
        if (bev[i].ch && bev[i].bus == (uint32_t)(b - devs))
            bus_events_drop(&bev[i]);   /* the last start's: it is over */
        if (!bev[i].ch && !e)
            e = &bev[i];
    }
    if (!e) {
        say(true, "devmgr: %s: more than %u usb-bus events channels", bdf(b), MAX_BUS_EVENTS);
        return ERR_NO_RESOURCES;
    }
    handle_t ours, theirs;
    status_t st = jam_channel_create(&ours, &theirs);
    if (st != OK)
        return st;
    e->gen++;
    st = jam_port_bind(port, ours, KEY_BUSEV_OF(e - bev, e->gen), SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(ours);
        jam_handle_close(theirs);
        return st;
    }
    e->ch = ours;
    e->bus = (uint32_t)(b - devs);
    x[*n] = (struct spawn_handle){ SR_DRIVER(DR_EVENTS), theirs };
    xr[(*n)++] = DEVMGR_DRV_CHAN_RIGHTS;
    return OK;
}

static void ev_interface_attached(void *ctx, uint32_t id, uint16_t vendor, uint16_t product,
                                  uint8_t interface_number, uint8_t class_code,
                                  uint8_t subclass, uint8_t protocol, uint8_t speed,
                                  const char *path, uint32_t path_len, handle_t usb)
{
    const struct bus_events *e = ctx;
    struct if_info m = { .id = id, .vendor = vendor, .product = product,
                         .interface_number = interface_number, .class_code = class_code,
                         .subclass = subclass, .protocol = protocol, .speed = speed };
    memcpy(m.path, path, path_len < sizeof(m.path) ? path_len : sizeof(m.path) - 1);
    attached(&devs[e->bus], &m, usb);
}

static const struct usbbus_events_ops ev_ops = { .interface_attached = ev_interface_attached };

void usb_bus_events(uint64_t key)
{
    uint32_t slot = KEY_INDEX(key);
    if (slot >= MAX_BUS_EVENTS || !bev[slot].ch || KEY_GEN(key) != bev[slot].gen)
        return;   /* stale */
    struct bus_events *e = &bev[slot];
    /* Everything queued: the binding fires on an edge, and a message left
     * unread would never ring again. (A usb-bus has a few events per
     * device; the bound is only against one that never stops.) */
    for (int guard = 0; guard < 4096 && e->ch; guard++) {
        status_t st = usbbus_events_serve_one(e->ch, &ev_ops, e);
        if (st == ERR_PEER_CLOSED)
            bus_events_drop(e);   /* the usb-bus ended */
        if (st != OK)
            return;
    }
}

/* Interface u is gone: its binding (if any) is freed now, or -- while its
 * driver still runs -- when the driver ends (it sees its channel close). */
static void if_gone(struct usb_if *u)
{
    if (u->bind >= 0) {
        struct binding *b = &devs[u->bind];
        b->usb_if = -1;
        if (!b->proc)
            usb_retire(b, "device gone");
    }
    drop(u);
}

void usb_if_closed(uint64_t key)
{
    uint32_t slot = KEY_INDEX(key);
    if (slot >= MAX_USB_IFS || !usb_ifs[slot].ch || KEY_GEN(key) != usb_ifs[slot].gen)
        return;   /* stale */
    struct usb_if *u = &usb_ifs[slot];
    say(false, "devmgr: usb %s %04x:%04x if%u gone", if_path(u), u->info.vendor,
        u->info.product, u->info.interface_number);
    if_gone(u);
}

void usb_bus_gone(struct binding *b)
{
    if (b->kind != BIND_PCI)
        return;
    for (unsigned i = 0; i < MAX_BUS_EVENTS; i++)
        if (bev[i].ch && bev[i].bus == (uint32_t)(b - devs))
            bus_events_drop(&bev[i]);
    for (unsigned i = 0; i < MAX_USB_IFS; i++)
        if (usb_ifs[i].ch && usb_ifs[i].bus == (uint32_t)(b - devs))
            if_gone(&usb_ifs[i]);
}

const char *usb_hub_name(void)
{
    return "compositor";
}

void usb_new_console(handle_t ch)
{
    if (console)
        jam_handle_close(console);
    console = ch;
    console_gen++;
    unsigned waiting = 0;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->kind == BIND_USB && b->path && b->state == DEVMGR_SUP_RESTARTING &&
            b->console_wait) {
            b->restart_at = now();
            waiting++;
        }
    }
    say(false, "devmgr: a %s%s; %u class driver(s) reconnect to it", usb_hub_name(),
        console_gen > 1 ? " again" : "", waiting);
}
