/* devmgr: USB interfaces and their class drivers (M7).
 *
 * usb-bus writes `usbbus.interface_attached` (abi/idl/usbbus.idl) on its
 * DR_SERVE channel, txid 0, by itself, for each interface of a device it
 * configured, with ONE handle: that interface's `usb` channel. devmgr
 * keeps the channel in usb_ifs until the device goes (usb-bus closes its
 * end: PEER_CLOSED) or the usb-bus that reported it dies.
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
 *   DR_INPUT  with a console (SR_CONSOLE, or DEVMGR_SET_CONSOLE after a
 *             console restart): a new source from console.connect_input.
 *             Without one hid logs each key DOWN (keytest, init + utest).
 * Both arrive without RIGHT_DUPLICATE / RIGHT_TRANSFER.
 *
 * How a hid ends decides what happens:
 *   - its interface is gone (unplugged, or its usb-bus died): the binding
 *     is freed, whatever the exit code; the next attach binds a new one;
 *   - exit 0 after the console it was connected to went away: restarted
 *     (not counted as a restart) as soon as there is a new console;
 *   - exit 0 otherwise: finished (a non-boot interface it skipped);
 *   - a crash, a kill, an error exit: restarted with backoff
 *     (supervise.c), given up on after 5 in a minute. */
#include "internal.h"
#include <idl/console.h>
#include <idl/usbbus.h>

#define CONNECT_WAIT (2 * S)   /* console.connect_input */

struct usb_if {
    handle_t ch;        /* our end of the interface's `usb` channel; 0: a free slot */
    uint32_t bus;       /* devs index of the usb-bus that reported it */
    int32_t  bind;      /* devs index of its class driver's binding; -1: none */
    uint16_t gen;       /* bumped at every use of the slot: in its port key */
    struct usbbus_interface_attached_req info;
};

static struct usb_if usb_ifs[MAX_USB_IFS];
handle_t console;
static uint32_t console_gen;   /* bumped with every console we are given */

static const char *if_path(const struct usb_if *u)
{
    static char p[25];
    memcpy(p, u->info.path, 24);
    p[24] = 0;
    return p;
}

static void drop(struct usb_if *u)
{
    jam_port_unbind(port, u->ch, KEY_IF_OF(u - usb_ifs, u->gen));
    jam_handle_close(u->ch);
    u->ch = HANDLE_INVALID;
    u->bind = -1;
}

/* One message is queued on h that doesn't fit: take it off anyway. */
static void discard(handle_t h, uint32_t n, uint32_t nh)
{
    uint8_t *big = malloc(n ? n : 1);
    handle_t *bh = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = h, .bytes_cap = n, .bytes = (uint64_t)(uintptr_t)big,
        .actual_bytes = (uint64_t)(uintptr_t)&n2, .handles = (uint64_t)(uintptr_t)bh,
        .handles_cap = nh, .actual_handles = (uint64_t)(uintptr_t)&nh2,
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
            .h = h, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
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

static const char *usb_match(const struct usbbus_interface_attached_req *m)
{
    if (m->class_code == 3)
        return "drv/hid";
    return NULL;
}

static const char *what(const struct usbbus_interface_attached_req *m)
{
    if (m->class_code == 3 && m->subclass == 1 && m->protocol == 1)
        return "boot keyboard";
    if (m->class_code == 3 && m->subclass == 1 && m->protocol == 2)
        return "boot mouse";
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
    b->path = NULL;   /* a free slot now */
}

status_t usb_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (usb_gone(b))
        return ERR_PEER_CLOSED;
    struct usb_if *u = &usb_ifs[b->usb_if];
    b->input_gen = 0;
    handle_t in = HANDLE_INVALID;
    if (console) {
        if (closed(console))
            return ERR_SHOULD_WAIT;   /* the console is restarting: wait for the new one */
        handle_t src;
        status_t st = console_connect_input_until(console, (uint64_t)jam_clock_get() + CONNECT_WAIT,
                                                  &src);
        if (st == ERR_PEER_CLOSED)
            return ERR_SHOULD_WAIT;
        if (st != OK)
            return st;
        st = jam_handle_replace(src, DEVMGR_DRV_CHAN_RIGHTS | RIGHT_TRANSFER, &in);
        if (st != OK) {
            jam_handle_close(src);
            return st;
        }
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

/* Start a class driver for interface slot `slot`. */
static void bind_interface(unsigned slot, const char *path)
{
    struct usb_if *u = &usb_ifs[slot];
    const struct usbbus_interface_attached_req *m = &u->info;
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
    snprintf(b->name, sizeof(b->name), "hid-%s:%u", if_path(u), m->interface_number);
    u->bind = (int32_t)(b - devs);
    b->last = start_driver(b);
    if (b->last == ERR_SHOULD_WAIT) {
        b->state = DEVMGR_SUP_RESTARTING;
        b->restart_at = DEADLINE_NEVER;
        b->console_wait = true;
        say(false, "devmgr: %s %s: waits for the console", bdf(b), path);
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
        m->interface_number, path, what(m), if_path(u), b->input_gen ? "" : ", no console: to the log");
}

static void attached(struct binding *bus, const struct usbbus_interface_attached_req *m,
                     handle_t ch)
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

void usb_driver_events(struct binding *b)
{
    for (int guard = 0; guard < 1024 && b->client; guard++) {
        _Alignas(8) uint8_t buf[sizeof(struct usbbus_interface_attached_req)];
        handle_t hs[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = b->client, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(b->client, n, nh);   /* not ours (a late reply nobody waits for) */
            continue;
        }
        if (st != OK)
            return;
        const struct usbbus_interface_attached_req *m = (const void *)buf;
        if (n != sizeof(*m) || m->ordinal != USBBUS_INTERFACE_ATTACHED || nh != 1) {
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(hs[i]);
            continue;
        }
        attached(b, m, hs[0]);
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
    for (unsigned i = 0; i < MAX_USB_IFS; i++)
        if (usb_ifs[i].ch && usb_ifs[i].bus == (uint32_t)(b - devs))
            if_gone(&usb_ifs[i]);
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
            b->restart_at = (uint64_t)jam_clock_get();
            waiting++;
        }
    }
    say(false, "devmgr: a console%s; %u class driver(s) reconnect to it",
        console_gen > 1 ? " again" : "", waiting);
}
