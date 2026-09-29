/* usbtest: checks usb-bus from user space (M7 Track A). init runs it after
 * utest (boot/init.cfg), on QEMU and on the PC.
 *
 * It finds usb-bus among devmgr's drivers (GET_SERVICE 0xffff/0xffff: the
 * n-th bound driver; the one answering `usbbus.status`), waits for the bus
 * to settle and then checks, everywhere:
 *   settled        enumeration settles within 15 s
 *   device_list    every device is consistent (path, level, route, parent
 *                  hub, TT fields, configuration, interfaces)
 *   interfaces     each interface opens as a `usb` channel whose info and
 *                  GET_DESCRIPTOR(device) agree with the bus; a hub's
 *                  interface is refused (usb-bus's own)
 *   access         a class driver's channel refuses other interfaces,
 *                  device-recipient and state-changing standard requests
 * on QEMU's devices (vendor 0627) only:
 *   stall_recovered an unknown class request STALLs (ERR_NOT_SUPPORTED)
 *                  and the next request on endpoint 0 works
 * on a QEMU usb-ccid (08e6:4433, no driver binds it; tools/usb-test.sh):
 *   set_interface  its interrupt IN endpoint opened, then usb.set_interface
 *                  (0): usb-bus drops the endpoint (the reports channel
 *                  closes) and adds it back; it opens and polls again
 * and, when a keyboard with serial "jamos-keys" is attached (the
 * tools/usb-test.sh scenario, which types through the QEMU monitor), through
 * the real chain usb-bus -> devmgr -> drv/hid (M7 integration; hid owns the
 * endpoint, usbtest watches its counters and hid's supervision):
 *   keys           devmgr started hid for it and hid polls the endpoint;
 *                  `sendkey a` reaches hid (press + release)
 *   kill_hid       DEVMGR_KILL of that hid while `c` is down: restarted,
 *                  polling again, and `sendkey d` reaches the new one
 *   unplug         device_del: the interface channels see PEER_CLOSED, the
 *                  device leaves the list, hid ends and devmgr frees it
 *   replug         device_add: it comes back (new id), a new hid binds and
 *                  `sendkey b` reaches it
 *   unplug_hub     device_del of its hub: the hub and everything behind it
 *                  go (recursive detach), the keyboard's channel closes
 *
 * The marker lines ("usbtest: ready for keys", ...) are what the QEMU
 * monitor script waits for. Exit 0 if nothing failed; the summary goes to
 * the RESULTS box. */
#include <os.h>
#include <devmgr.h>
#include <idl/usb.h>
#include <idl/usbbus.h>

#define MS 1000000ull
#define S  1000000000ull
#define MAX_DEV 48

static const char *cur;
static unsigned passed, failed, skipped;
static handle_t bus;

#define FAIL(...)                                                   \
    do {                                                            \
        printf("usbtest: %s: FAILED at line %d: ", cur, __LINE__);  \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
        return false;                                               \
    } while (0)
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c))                                                   \
            FAIL("%s", #c);                                         \
    } while (0)
#define CHECK_ST(expr, want)                                        \
    do {                                                            \
        status_t _s = (expr), _w = (want);                          \
        if (_s != _w)                                               \
            FAIL("%s is %s, want %s", #expr, status_str(_s), status_str(_w)); \
    } while (0)

static uint64_t now(void) { return (uint64_t)jam_clock_get(); }
static uint64_t in(uint64_t ns) { return now() + ns; }

struct dev {
    uint32_t id, parent;
    uint16_t vid, pid, bcd, mps0;
    uint8_t speed, address, slot, root_port, port, level, tt_slot, tt_port, cls, sub, proto;
    uint8_t nconfigs, config, nifs, hub_ports;
    uint32_t route;
    char path[25], name[41], serial[25];
};

static struct dev devs[MAX_DEV];
static unsigned ndevs;

struct bus_status {
    uint32_t devices, hubs, ifaces, hid, problems, generation;
    uint8_t settled;
};

static status_t get_status(struct bus_status *b)
{
    return usbbus_status_until(bus, in(5 * S), &b->devices, &b->hubs, &b->ifaces, &b->hid,
                               &b->problems, &b->generation, &b->settled);
}

static status_t wait_settled(uint32_t ms, struct bus_status *b)
{
    return usbbus_wait_settled_until(bus, in((ms + 5000) * MS), ms, &b->devices, &b->hubs,
                                     &b->ifaces, &b->hid, &b->problems, &b->generation,
                                     &b->settled);
}

static status_t load(void)
{
    ndevs = 0;
    for (uint32_t i = 0; i < MAX_DEV; i++) {
        struct dev *d = &devs[ndevs];
        uint8_t path[24], name[40], serial[24];
        status_t st = usbbus_device_until(bus, in(5 * S), i, &d->id, &d->parent, &d->vid, &d->pid,
                                          &d->bcd, &d->speed, &d->address, &d->slot,
                                          &d->root_port, &d->port, &d->level, &d->route,
                                          &d->tt_slot, &d->tt_port, &d->cls, &d->sub, &d->proto,
                                          &d->nconfigs, &d->config, &d->nifs, &d->mps0,
                                          &d->hub_ports, path, name, serial);
        if (st == ERR_OUT_OF_RANGE)
            return OK;
        if (st != OK)
            return st;
        memcpy(d->path, path, 24);
        d->path[24] = 0;
        memcpy(d->name, name, 40);
        d->name[40] = 0;
        memcpy(d->serial, serial, 24);
        d->serial[24] = 0;
        ndevs++;
    }
    return OK;
}

static struct dev *by_id(uint32_t id)
{
    for (unsigned i = 0; i < ndevs; i++)
        if (devs[i].id == id)
            return &devs[i];
    return NULL;
}

static struct dev *by_serial(const char *s)
{
    for (unsigned i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].serial, s))
            return &devs[i];
    return NULL;
}

/* ---- tests ---------------------------------------------------------------------- */

static bool t_settled(void)
{
    struct bus_status b;
    CHECK_ST(wait_settled(15000, &b), OK);
    CHECK(b.settled);
    printf("usbtest: %u devices (%u hubs), %u interfaces (%u HID), %u problems\n", b.devices,
           b.hubs, b.ifaces, b.hid, b.problems);
    return true;
}

static bool t_device_list(void)
{
    struct bus_status b;
    CHECK_ST(get_status(&b), OK);
    CHECK_ST(load(), OK);
    CHECK(ndevs == b.devices);
    unsigned hubs = 0;
    for (unsigned i = 0; i < ndevs; i++) {
        struct dev *d = &devs[i];
        printf("usbtest: %s id %u %04x:%04x speed %u slot %u addr %u level %u route %05x "
               "tt %u/%u cfg %u/%u ifs %u mps0 %u%s%s\n", d->path, d->id, d->vid, d->pid,
               d->speed, d->slot, d->address, d->level, d->route, d->tt_slot, d->tt_port,
               d->config, d->nconfigs, d->nifs, d->mps0, d->name[0] ? " " : "", d->name);
        CHECK(d->path[0]);
        CHECK(d->speed >= 1 && d->speed <= 5);
        CHECK(d->slot && d->address);
        CHECK(d->level >= 1 && d->level <= 6);
        CHECK(d->nconfigs >= 1);
        CHECK(d->mps0 == 8 || d->mps0 == 16 || d->mps0 == 32 || d->mps0 == 64 || d->mps0 == 512);
        if (d->hub_ports)
            hubs++;
        if (!d->parent) {
            CHECK(d->level == 1 && d->route == 0 && d->port == d->root_port);
            CHECK(!d->tt_slot);
        } else {
            struct dev *p = by_id(d->parent);
            CHECK(p != NULL);
            CHECK(p->hub_ports && d->port >= 1 && d->port <= p->hub_ports);
            CHECK(d->level == p->level + 1 && d->root_port == p->root_port);
            uint32_t nib = (d->route >> (4 * (p->level - 1))) & 0xf;
            CHECK(nib == (d->port > 15 ? 15u : d->port));
            /* TT: a full/low-speed device right behind a high-speed hub uses
             * that hub's TT. */
            if (d->speed <= 2 && p->speed == 3)
                CHECK(d->tt_slot == p->slot && d->tt_port == d->port);
            if (d->speed >= 3)
                CHECK(!d->tt_slot);
        }
    }
    CHECK(hubs == b.hubs);
    return true;
}

static bool t_interfaces(void)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct dev *d = &devs[i];
        if (!d->config)
            continue;
        for (uint8_t k = 0; k < d->nifs; k++) {
            uint8_t num = 0, alt = 0, nalts = 0, cls = 0, sub = 0, proto = 0, nep = 0, eps[8];
            CHECK_ST(usbbus_interface_until(bus, in(5 * S), d->id, k, &num, &alt, &nalts, &cls,
                                            &sub, &proto, &nep, eps), OK);
            CHECK(nep <= 8 && nalts >= 1);
            handle_t ch;
            if (cls == 9 || d->hub_ports) {   /* hubs are usb-bus's own: refused */
                CHECK_ST(usbbus_open_interface_until(bus, in(5 * S), d->id, num, &ch),
                         ERR_ACCESS_DENIED);
                continue;
            }
            CHECK_ST(usbbus_open_interface_until(bus, in(5 * S), d->id, num, &ch), OK);
            uint16_t v = 0, p = 0;
            uint8_t sp = 0, n2 = 0, c2 = 0, s2 = 0, p2 = 0, e2 = 0, a2 = 0, ad = 0;
            status_t st = usb_info_until(ch, in(5 * S), &v, &p, &sp, &n2, &c2, &s2, &p2, &e2,
                                         &a2, &ad);
            uint16_t actual = 0;
            static uint8_t buf[1024];
            status_t st2 = usb_get_descriptor_until(ch, in(5 * S), 1, 0, 0, 18, 0, &actual, buf);
            jam_handle_close(ch);
            CHECK_ST(st, OK);
            CHECK(v == d->vid && p == d->pid && sp == d->speed && n2 == num && c2 == cls &&
                  s2 == sub && p2 == proto && ad == d->address);
            CHECK_ST(st2, OK);
            CHECK(actual == 18 && buf[1] == 1);
            CHECK((buf[8] | buf[9] << 8) == d->vid && (buf[10] | buf[11] << 8) == d->pid);
        }
    }
    return true;
}

/* The first non-hub interface of a configured device: its channel. */
static bool first_iface(const struct dev **dd, uint8_t *num, handle_t *ch, bool qemu_only)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct dev *d = &devs[i];
        if (!d->config || d->hub_ports || (qemu_only && d->vid != 0x0627))
            continue;
        uint8_t n = 0, alt, nalts, cls, sub, proto, nep, eps[8];
        if (usbbus_interface_until(bus, in(5 * S), d->id, 0, &n, &alt, &nalts, &cls, &sub,
                                   &proto, &nep, eps) != OK)
            continue;
        if (usbbus_open_interface_until(bus, in(5 * S), d->id, n, ch) != OK)
            continue;
        *dd = d;
        *num = n;
        return true;
    }
    return false;
}

static bool t_access(void)
{
    const struct dev *d;
    uint8_t num;
    handle_t ch;
    if (!first_iface(&d, &num, &ch, false)) {
        printf("usbtest: access: no device with an interface: skipped\n");
        skipped++;
        return true;
    }
    static uint8_t buf[1024];
    uint16_t actual;
    uint8_t out[64] = { 0 };
    /* device recipient: GET_STATUS(device) */
    status_t a = usb_control_in_until(ch, in(5 * S), 0x80, 0, 0, 0, 2, &actual, buf);
    /* another interface's wIndex */
    status_t b = usb_control_in_until(ch, in(5 * S), 0xa1, 1, 0x0100, (uint16_t)(num + 1), 8,
                                      &actual, buf);
    /* SET_CONFIGURATION (standard, device) and SET_INTERFACE by hand */
    status_t c = usb_control_out_until(ch, in(5 * S), 0x00, 9, 1, 0, 0, out);
    status_t e = usb_control_out_until(ch, in(5 * S), 0x01, 11, 0, num, 0, out);
    /* an OUT endpoint address, and one that isn't the interface's */
    handle_t rep;
    uint16_t mp;
    uint8_t iv;
    status_t f = usb_open_interrupt_in_until(ch, in(5 * S), 0x0f, &rep, &mp, &iv);
    status_t g = usb_control_in_until(ch, in(5 * S), 0x80, 6, 0x0100, 0, 2000, &actual, buf);
    jam_handle_close(ch);
    CHECK_ST(a, ERR_ACCESS_DENIED);
    CHECK_ST(b, ERR_ACCESS_DENIED);
    CHECK_ST(c, ERR_ACCESS_DENIED);
    CHECK_ST(e, ERR_ACCESS_DENIED);
    CHECK_ST(f, ERR_INVALID_ARGS);
    CHECK(g == ERR_INVALID_ARGS || g == ERR_ACCESS_DENIED);
    return true;
}

static bool t_stall_recovered(void)
{
    const struct dev *d;
    uint8_t num;
    handle_t ch;
    if (!first_iface(&d, &num, &ch, true)) {
        printf("usbtest: stall_recovered: no QEMU USB device (vendor 0627): skipped\n");
        skipped++;
        return true;
    }
    static uint8_t buf[1024];
    for (int round = 0; round < 3; round++) {
        uint16_t actual = 0;
        /* class request 0x55 to the interface: nobody implements it */
        status_t st = usb_control_in_until(ch, in(5 * S), 0xa1, 0x55, 0, num, 8, &actual, buf);
        status_t st2 = usb_get_descriptor_until(ch, in(5 * S), 1, 0, 0, 18, 0, &actual, buf);
        if (st != ERR_NOT_SUPPORTED || st2 != OK || actual != 18) {
            jam_handle_close(ch);
            FAIL("round %d: unknown request %s (want ERR_NOT_SUPPORTED), then GET_DESCRIPTOR %s "
                 "(%u bytes)", round, status_str(st), status_str(st2), actual);
        }
    }
    jam_handle_close(ch);
    return true;
}

/* More requests queued on DR_SERVE than usb-bus takes in one pass (64):
 * the channel stays readable, so no new port edge comes; usb-bus must come
 * back for the rest by itself, or DR_SERVE is dead for good (this call,
 * and devmgr's stop, never answered). txid 0: the replies go to devmgr's
 * end, which drops them. */
static bool t_set_interface(void)
{
    const struct dev *d = NULL;
    for (unsigned i = 0; i < ndevs && !d; i++)
        if (devs[i].vid == 0x08e6 && devs[i].pid == 0x4433 && devs[i].config)
            d = &devs[i];
    if (!d) {
        printf("usbtest: set_interface: no QEMU usb-ccid: skipped\n");
        skipped++;
        return true;
    }
    uint8_t num = 0, alt, nalts, cls, sub, proto, nep = 0, eps[8];
    CHECK_ST(usbbus_interface_until(bus, in(5 * S), d->id, 0, &num, &alt, &nalts, &cls, &sub,
                                    &proto, &nep, eps), OK);
    handle_t ch, rep = HANDLE_INVALID;
    CHECK_ST(usbbus_open_interface_until(bus, in(5 * S), d->id, num, &ch), OK);
    uint16_t mp;
    uint8_t iv, ep = 0;
    for (uint8_t i = 0; i < nep && !ep; i++)
        if ((eps[i] & 0x80) && usb_open_interrupt_in_until(ch, in(5 * S), eps[i], &rep, &mp,
                                                            &iv) == OK)
            ep = eps[i];
    status_t st = ep ? usb_set_interface_until(ch, in(5 * S), 0) : ERR_NOT_FOUND;
    bool closed = ep && jam_object_wait_one(rep, SIG_PEER_CLOSED, in(5 * S), NULL) == OK;
    if (rep != HANDLE_INVALID)
        jam_handle_close(rep);
    rep = HANDLE_INVALID;
    status_t again = ep ? usb_open_interrupt_in_until(ch, in(5 * S), ep, &rep, &mp, &iv)
                        : ERR_NOT_FOUND;
    uint64_t reports = 0, dropped = 0, errors = 0;
    uint8_t open = 0;
    status_t sst = ep ? usb_endpoint_stats_until(ch, in(5 * S), ep, &reports, &dropped, &errors,
                                                 &open) : ERR_NOT_FOUND;
    if (rep != HANDLE_INVALID)
        jam_handle_close(rep);
    jam_handle_close(ch);
    CHECK(ep != 0);   /* it has an interrupt IN endpoint */
    CHECK_ST(st, OK);
    CHECK(closed);    /* the old reports channel went with the old endpoint */
    CHECK_ST(again, OK);
    CHECK_ST(sst, OK);
    CHECK(open);
    printf("usbtest: set_interface(0) on %s if%u: endpoint %02x dropped, added back and open\n",
           d->path, num, ep);
    return true;
}

static bool t_serve_backlog(void)
{
    struct usbbus_status_req q = { .txid = 0, .ordinal = USBBUS_STATUS };
    unsigned sent = 0;
    for (int i = 0; i < 300; i++)
        if (jam_channel_write(bus, &q, sizeof(q), NULL, 0) == OK)
            sent++;
    struct bus_status b;
    status_t st = get_status(&b);
    if (st != OK)
        FAIL("%u requests queued, then usbbus.status: %s", sent, status_str(st));
    return true;
}

/* ---- the interactive part (QEMU monitor) ----------------------------------------------- */

/* M7 integration: devmgr binds drv/hid to the test keyboard (it owns the
 * interrupt endpoint), so usbtest watches through a channel of its own:
 * the endpoint's counters (usb.endpoint_stats: reports taken by hid, and
 * whether a reader is attached) and hid's supervision state in devmgr.
 * The keys themselves show up in the log as hid's "key 0x.. down" lines,
 * which tools/usb-test.sh checks. */

static handle_t dm;   /* devmgr */

struct kbd {
    handle_t ch;      /* ours, to the test keyboard's interface 0 */
    uint32_t id;
    char     name[32];   /* hid's process name, "hid-6.1:0" */
};

static status_t hid_sup(uint32_t id, struct devmgr_rep *r)
{
    return devmgr_call(dm, DEVMGR_SUPERVISION, DEVMGR_USB_IFACE, 0, id, r, NULL, 0, NULL,
                       in(5 * S));
}

static status_t ep_stats(struct kbd *k, uint64_t *reports, uint64_t *dropped, uint8_t *open)
{
    uint64_t errors = 0;
    return usb_endpoint_stats_until(k->ch, in(5 * S), 0x81, reports, dropped, &errors, open);
}

static bool open_keys(struct kbd *k)
{
    struct dev *d = by_serial("jamos-keys");
    CHECK(d != NULL);
    k->id = d->id;
    snprintf(k->name, sizeof(k->name), "hid-%s:0", d->path);
    CHECK_ST(usbbus_open_interface_until(bus, in(5 * S), d->id, 0, &k->ch), OK);
    return true;
}

/* Until hid runs for the test keyboard (at least `restarts` restarts
 * since its binding) and polls the endpoint; *reports: the count then. */
static bool hid_ready(struct kbd *k, uint32_t restarts, uint64_t *reports)
{
    uint64_t end = in(20 * S);
    struct devmgr_rep r = { 0 };
    status_t st = ERR_TIMED_OUT;
    uint8_t open = 0;
    while (now() < end) {
        uint64_t dropped = 0;
        st = hid_sup(k->id, &r);
        if (st == OK && r.a == DEVMGR_SUP_RUNNING && r.b >= restarts &&
            ep_stats(k, reports, &dropped, &open) == OK && open)
            return true;
        jam_nanosleep(in(20 * MS));
    }
    FAIL("%s not polling the keyboard after 20 s (supervision %s, state %u, restarts %u, open %u)",
         k->name, status_str(st), r.a, r.b, open);
}

/* Until hid has taken `n` more reports than `base`. */
static bool hid_got(struct kbd *k, uint64_t base, uint64_t n)
{
    uint64_t end = in(15 * S), reports = 0, dropped = 0;
    uint8_t open = 0;
    while (now() < end) {
        CHECK_ST(ep_stats(k, &reports, &dropped, &open), OK);
        if (reports >= base + n) {
            CHECK(dropped == 0);
            return true;
        }
        jam_nanosleep(in(10 * MS));
    }
    FAIL("%s took %lu report(s) in 15 s, want %lu", k->name, (unsigned long)(reports - base),
         (unsigned long)n);
}

static bool gone(handle_t h, uint64_t deadline)
{
    signals_t seen = 0;
    return jam_object_wait_one(h, SIG_PEER_CLOSED, deadline, &seen) == OK;
}

/* Until devmgr has let go of hid's binding for device `id`. */
static bool hid_unbound(uint32_t id)
{
    uint64_t end = in(10 * S);
    struct devmgr_rep r;
    status_t st = OK;
    while (now() < end && (st = hid_sup(id, &r)) != ERR_NOT_FOUND)
        jam_nanosleep(in(20 * MS));
    CHECK_ST(st, ERR_NOT_FOUND);
    return true;
}

static struct kbd kb;

static bool t_keys(void)
{
    if (!open_keys(&kb))
        return false;
    uint64_t base = 0;
    if (!hid_ready(&kb, 0, &base))
        return false;
    printf("usbtest: %s polls the test keyboard; ready for keys\n", kb.name);
    if (!hid_got(&kb, base, 2))   /* `sendkey a`: press, release */
        return false;
    return true;
}

/* DEVMGR_KILL hid while a key is down: devmgr restarts it with a duplicate
 * of the interface channel, and the next key works. */
static bool t_kill_hid(void)
{
    struct devmgr_rep r;
    CHECK_ST(hid_sup(kb.id, &r), OK);
    uint32_t restarts = r.b;
    uint64_t base = 0, dropped = 0;
    uint8_t open = 0;
    CHECK_ST(ep_stats(&kb, &base, &dropped, &open), OK);
    printf("usbtest: type c now (%s is killed while it is down)\n", kb.name);
    if (!hid_got(&kb, base, 1))   /* the press */
        return false;
    uint64_t t0 = now();
    CHECK_ST(devmgr_call(dm, DEVMGR_KILL, DEVMGR_USB_IFACE, 0, kb.id, &r, NULL, 0, NULL,
                         in(20 * S)), OK);
    if (!hid_ready(&kb, restarts + 1, &base))
        return false;
    printf("usbtest: %s restarted and polling again %lu ms after the kill; ready for keys after "
           "the restart\n", kb.name, (unsigned long)((now() - t0) / MS));
    if (!hid_got(&kb, base, 2))   /* `sendkey d` */
        return false;
    CHECK_ST(hid_sup(kb.id, &r), OK);
    CHECK(r.a == DEVMGR_SUP_RUNNING && r.b == restarts + 1);
    return true;
}

static bool t_unplug(void)
{
    struct bus_status before, after;
    CHECK_ST(get_status(&before), OK);
    printf("usbtest: unplug the test keyboard now\n");
    bool g = gone(kb.ch, in(15 * S));
    jam_handle_close(kb.ch);
    CHECK(g);
    /* hid saw its channel close too and ended; devmgr freed the binding */
    if (!hid_unbound(kb.id))
        return false;
    CHECK_ST(wait_settled(10000, &after), OK);
    CHECK(after.devices == before.devices - 1);
    CHECK_ST(load(), OK);
    CHECK(by_serial("jamos-keys") == NULL);
    return true;
}

static bool t_replug(void)
{
    printf("usbtest: plug the test keyboard back now\n");
    uint64_t end = in(20 * S);
    struct dev *d = NULL;
    while (now() < end) {
        struct bus_status b;
        if (wait_settled(2000, &b) == OK && load() == OK && (d = by_serial("jamos-keys")))
            break;
    }
    CHECK(d != NULL);
    CHECK(d->id != kb.id);
    if (!open_keys(&kb))
        return false;
    uint64_t base = 0;
    if (!hid_ready(&kb, 0, &base))
        return false;
    printf("usbtest: a new %s polls it; ready for keys again\n", kb.name);
    return hid_got(&kb, base, 2);   /* `sendkey b` */
}

/* The hub the test keyboard hangs on goes: it and everything behind it
 * leave the list, the keyboard's channel closes and its hid goes. */
static bool t_unplug_hub(void)
{
    CHECK_ST(load(), OK);
    struct dev *k = by_serial("jamos-keys");
    CHECK(k != NULL && k->parent);
    struct dev *hub = by_id(k->parent);
    CHECK(hub != NULL && hub->hub_ports);
    uint32_t hub_id = hub->id, kid = k->id;
    unsigned below = 0;
    for (unsigned i = 0; i < ndevs; i++)
        for (struct dev *p = by_id(devs[i].parent); p; p = by_id(p->parent))
            if (p->id == hub_id) {
                below++;
                break;
            }
    CHECK(below >= 1);
    struct bus_status before, after;
    CHECK_ST(get_status(&before), OK);
    printf("usbtest: unplug the hub now\n");
    bool g = gone(kb.ch, in(15 * S));
    jam_handle_close(kb.ch);
    kb.ch = HANDLE_INVALID;
    CHECK(g);
    if (!hid_unbound(kid))
        return false;
    CHECK_ST(wait_settled(10000, &after), OK);
    CHECK(after.devices == before.devices - 1 - below);
    CHECK(after.hubs == before.hubs - 1);
    CHECK_ST(load(), OK);
    CHECK(by_id(hub_id) == NULL && by_serial("jamos-keys") == NULL);
    return true;
}

/* ---- main ----------------------------------------------------------------------------- */

static bool find_bus(void)
{
    dm = startup_handle(SR_DEVMGR_CTL);   /* M7: it kills hid (control) */
    if (!dm)
        return false;
    for (uint32_t n = 0; n < 16; n++) {
        struct devmgr_rep r;
        handle_t hs[1];
        uint32_t nh = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, hs, 1, &nh,
                                  in(5 * S));
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        bus = hs[0];
        struct bus_status b;
        if (get_status(&b) == OK)
            return true;
        jam_handle_close(bus);
        bus = HANDLE_INVALID;
    }
    return false;
}

static void run(const char *name, bool (*fn)(void))
{
    cur = name;
    uint64_t t0 = now();
    unsigned sk = skipped;
    if (fn()) {
        if (skipped != sk)
            return;   /* it said why */
        passed++;
        printf("usbtest: %s ok (%lu ms)\n", name, (unsigned long)((now() - t0) / MS));
    } else {
        failed++;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[120];
    if (!find_bus()) {
        int n = snprintf(line, sizeof(line), "usbtest: no usb-bus driver (no xHCI, or no devmgr): "
                         "skipped");
        jam_debug_report(line, (uint64_t)n);
        return 0;
    }
    run("settled", t_settled);
    run("device_list", t_device_list);
    run("interfaces", t_interfaces);
    run("access", t_access);
    run("stall_recovered", t_stall_recovered);
    run("set_interface", t_set_interface);
    bool interactive = load() == OK && by_serial("jamos-keys");
    if (interactive) {
        run("keys", t_keys);
        run("kill_hid", t_kill_hid);
        run("unplug", t_unplug);
        run("replug", t_replug);
        run("unplug_hub", t_unplug_hub);
    } else {
        printf("usbtest: keys, kill_hid, unplug, replug, unplug_hub: no keyboard with serial "
               "jamos-keys (the QEMU USB scenario): skipped\n");
        skipped += 5;
    }
    run("serve_backlog", t_serve_backlog);
    jam_handle_close(bus);
    int n = failed ? snprintf(line, sizeof(line), "usbtest: %u passed, %u FAILED, %u skipped",
                              passed, failed, skipped)
                   : snprintf(line, sizeof(line), "usbtest: %u passed, %u skipped%s", passed,
                              skipped, interactive ? " (keys + unplug/replug ran)" : "");
    jam_debug_report(line, (uint64_t)n);
    return failed ? 1 : 0;
}
