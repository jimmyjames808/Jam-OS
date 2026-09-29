/* usb-bus: what it says about devices: the one-line summary of each
 * (dev_line, in the log and in RESULTS), the detailed log lines, the
 * device paths ("9.1": root port 9, hub port 1) and the RESULTS summary.
 *
 * RESULTS gets one line per device once enumeration first settles, in
 * tree order, and a summary; usb_report_all is called again when the
 * driver stops, and then lists only the devices that came later. The
 * lines are built with a small string builder (sb_*): drivers have no
 * snprintf. */
#include "usbbus.h"

/* ---- the string builder ----------------------------------------------------- */

struct sb {
    char *b;
    unsigned n, cap;
};

static void sb_c(struct sb *s, char c)
{
    if (s->n + 1 < s->cap)
        s->b[s->n++] = c;
    s->b[s->n] = 0;
}

static void sb_s(struct sb *s, const char *str)
{
    while (*str)
        sb_c(s, *str++);
}

static void sb_u(struct sb *s, uint32_t v)
{
    char t[12];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        sb_c(s, t[--i]);
}

static void sb_x(struct sb *s, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        sb_c(s, "0123456789abcdef"[(v >> (4 * i)) & 0xf]);
}

/* ---- names ------------------------------------------------------------------ */

static const char *speed_str(uint8_t s)
{
    switch (s) {
    case SPEED_LOW: return "LS";
    case SPEED_FULL: return "FS";
    case SPEED_HIGH: return "HS";
    case SPEED_SUPER: return "SS";
    case SPEED_SUPERPLUS: return "SS+";
    default: return "speed?";
    }
}

const char *speed_long(uint8_t s)
{
    switch (s) {
    case SPEED_LOW: return "low-speed";
    case SPEED_FULL: return "full-speed";
    case SPEED_HIGH: return "high-speed";
    case SPEED_SUPER: return "SuperSpeed";
    case SPEED_SUPERPLUS: return "SuperSpeed+";
    default: return "unknown-speed";
    }
}

static const char *iface_kind(const struct iface *f)
{
    if (f->cls == 3 && f->sub == 1 && f->proto == 1)
        return "kbd";
    if (f->cls == 3 && f->sub == 1 && f->proto == 2)
        return "mouse";
    if (f->cls == 3)
        return "hid";
    if (f->cls == 9)
        return "hub";
    if (f->cls == 8)
        return "storage";
    if (f->cls == 1)
        return "audio";
    if (f->cls == 0xff)
        return "vendor";
    return NULL;
}

void dev_set_path(struct usbdev *d, const struct usbdev *parent, uint8_t port)
{
    struct sb s = { d->path, 0, sizeof(d->path) };
    if (parent) {
        sb_s(&s, parent->path);
        sb_c(&s, '.');
    }
    sb_u(&s, port);
}

/* ---- the device line -------------------------------------------------------- */

/* " TT(slot 2 port 2)", " mtt" inside when the TT is a multi-TT hub's. */
static void line_tt(struct sb *s, const struct usbdev *d)
{
    if (!d->tt_slot)
        return;
    sb_s(s, " TT(slot ");
    sb_u(s, d->tt_slot);
    sb_s(s, " port ");
    sb_u(s, d->tt_port);
    if (d->tt_mtt)
        sb_s(s, " mtt");
    sb_c(s, ')');
}

/* " hub 4p TTT1" (TTT: a high-speed hub's TT think time), " SS-hub 4p". */
static void line_hub(struct sb *s, const struct usbdev *d)
{
    if (!d->is_hub)
        return;
    sb_s(s, d->ss_hub ? " SS-hub " : " hub ");
    sb_u(s, d->hub_ports);
    sb_c(s, 'p');
    if (d->speed == SPEED_HIGH) {
        sb_s(s, " TTT");
        sb_u(s, d->ttt);
    }
}

/* Everything before the interfaces. */
static void line_head(struct sb *s, const struct usbdev *d, const char *prefix)
{
    if (prefix)
        sb_s(s, prefix);
    sb_s(s, "port ");
    sb_s(s, d->path);
    sb_c(s, ' ');
    sb_x(s, d->vid, 4);
    sb_c(s, ':');
    sb_x(s, d->pid, 4);
    sb_c(s, ' ');
    sb_s(s, speed_str(d->speed));
    line_tt(s, d);
    sb_s(s, " a");
    sb_u(s, d->address);
    sb_s(s, " mps");
    sb_u(s, d->mps0);
    sb_s(s, " cfg");
    sb_u(s, d->cfg_value);
    sb_c(s, '/');
    sb_u(s, d->nconfigs);
    line_hub(s, d);
    if (d->problem) {
        sb_s(s, " PROBLEM: ");
        sb_s(s, d->problem);
    }
}

/* " if0 03/01/01 kbd" */
static void line_iface(struct sb *s, const struct iface *f)
{
    sb_s(s, " if");
    sb_u(s, f->number);
    sb_c(s, ' ');
    sb_x(s, f->cls, 2);
    sb_c(s, '/');
    sb_x(s, f->sub, 2);
    sb_c(s, '/');
    sb_x(s, f->proto, 2);
    const char *k = iface_kind(f);
    if (k) {
        sb_c(s, ' ');
        sb_s(s, k);
    }
}

static void line_emit(const char *line, bool report_it)
{
    if (report_it)
        drv_report("%s", line);
    else
        drv_log("%s", line);
}

/* One line: port 3.2 258a:0033 FS TT(slot 2 port 2) a5 mps8 cfg1/1 if0 03/01/01 kbd
 * if1 03/00/00 hid "Name" (a5: USB address 5; mps8: EP0 max packet; cfg1/1: the
 * configuration set / how many the device has; ifN class/subclass/protocol).
 * The interfaces that don't fit go on a second line; the product name
 * goes last, and only if it fits. */
void dev_line(struct usbdev *d, bool report_it, const char *prefix)
{
    char a[160], b[160];
    struct sb s = { a, 0, 106 }, s2 = { b, 0, 106 };
    a[0] = b[0] = 0;
    line_head(&s, d, prefix);
    struct sb *o = &s;
    for (int i = 0; i < d->nifs; i++) {
        char t[48];
        struct sb ts = { t, 0, sizeof(t) };
        t[0] = 0;
        line_iface(&ts, &d->ifs[i]);
        if (o == &s && s.n + ts.n + 1 >= s.cap) {
            o = &s2;
            sb_s(&s2, "  port ");
            sb_s(&s2, d->path);
            sb_s(&s2, " (cont.):");
        }
        sb_s(o, t);
    }
    if (d->product[0]) {
        char t[48];
        struct sb ts = { t, 0, sizeof(t) };
        t[0] = 0;
        sb_s(&ts, " \"");
        sb_s(&ts, d->product);
        sb_c(&ts, '"');
        if (o->n + ts.n + 1 < o->cap)
            sb_s(o, t);
    }
    line_emit(a, report_it);
    if (s2.n)
        line_emit(b, report_it);
}

void dev_log_detail(struct usbdev *d)
{
    drv_log("usb %s: %04x:%04x %s, USB %x.%02x, class %02x/%02x/%02x, slot %u, address %u, "
            "route %05x, root port %u, level %u, EP0 max packet %u, %u configuration(s), "
            "active %u, %u interface(s)%s%s", d->path, d->vid, d->pid, speed_long(d->speed),
            d->bcd >> 8, d->bcd & 0xff, d->cls, d->sub, d->proto, d->slot, d->address,
            d->route, d->root_port, d->level, d->mps0, d->nconfigs, d->cfg_value, d->nifs,
            d->product[0] ? ", " : "", d->product);
    for (int i = 0; i < d->nifs; i++) {
        struct iface *f = &d->ifs[i];
        for (int k = 0; k < f->nep; k++) {
            uint8_t a = f->ep_addr[k];
            uint8_t dci = (uint8_t)((a & 0xf) * 2 + ((a & 0x80) ? 1 : 0));
            struct ep *e = dci < 32 ? &d->eps[dci] : NULL;
            if (!e)
                continue;
            drv_log("usb %s:   if%u alt %u ep %02x %s max packet %u interval %u (%u us)%s",
                    d->path, f->number, f->alt, a,
                    (e->attr & 3) == 3 ? "interrupt" : (e->attr & 3) == 2 ? "bulk" : "isoch",
                    e->mps, e->binterval, (1u << e->interval) * 125,
                    e->configured ? ", configured" : "");
        }
    }
}

/* ---- RESULTS ---------------------------------------------------------------- */

void usb_counts(uint32_t *devices, uint32_t *hubs, uint32_t *ifaces, uint32_t *hid,
                uint32_t *problems)
{
    uint32_t n = 0, nh = 0, ni = 0, nhid = 0, np = 0;
    for (int i = 0; g_devs && i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || !d->vid)
            continue;
        n++;
        nh += d->is_hub;
        np += d->problem != NULL;
        ni += d->nifs;
        for (int k = 0; k < d->nifs; k++)
            nhid += d->ifs[k].cls == 3;
    }
    if (devices)
        *devices = n;
    if (hubs)
        *hubs = nh;
    if (ifaces)
        *ifaces = ni;
    if (hid)
        *hid = nhid;
    if (problems)
        *problems = np + g_failed;
}

void usb_report_summary(const char *when)
{
    uint32_t n, nh, ni, nhid, np;
    usb_counts(&n, &nh, &ni, &nhid, &np);
    uint32_t kbd = 0, mouse = 0;
    for (int i = 0; i < MAX_DEVS; i++)
        for (int k = 0; g_devs[i].used && k < g_devs[i].nifs; k++) {
            struct iface *f = &g_devs[i].ifs[k];
            kbd += f->cls == 3 && f->sub == 1 && f->proto == 1;
            mouse += f->cls == 3 && f->sub == 1 && f->proto == 2;
        }
    drv_report("%s%u device%s (%u hub%s), %u HID interface%s (%u boot kbd, %u boot mouse), "
               "%u failed, hot-plug +%u -%u", when, n, n == 1 ? "" : "s", nh, nh == 1 ? "" : "s",
               nhid, nhid == 1 ? "" : "s", kbd, mouse, g_failed, g_attached, g_detached);
}

void usb_report_all(bool at_stop)
{
    g_first_report_done = true;
    g_report_generation = g_generation;
    /* Tree order: each root port's device, then what hangs below it. */
    int order[MAX_DEVS], n = 0;
    for (uint32_t p = 1; p <= g_hc.ports; p++) {
        int stack[MAX_DEVS], sp = 0;
        for (int i = 0; i < MAX_DEVS; i++)
            if (g_devs[i].used && g_devs[i].parent < 0 && g_devs[i].port == p)
                stack[sp++] = i;
        while (sp && n < MAX_DEVS) {
            int i = stack[--sp];
            order[n++] = i;
            for (int c = 15; c >= 1; c--)
                for (int q = 0; q < MAX_DEVS; q++)
                    if (g_devs[q].used && g_devs[q].parent == i && g_devs[q].port == c &&
                        sp < MAX_DEVS)
                        stack[sp++] = q;
        }
    }
    for (int k = 0; k < n; k++) {
        struct usbdev *d = &g_devs[order[k]];
        if ((d->vid || d->pid) && !d->reported) {
            dev_line(d, true, at_stop ? "new: " : NULL);
            d->reported = true;
        }
    }
    usb_report_summary(at_stop ? "at stop: " : "");
}
