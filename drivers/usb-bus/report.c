/* usb-bus: what it says about the controller (its RESULTS line) and about
 * devices: the one-line summary of each (dev_line, in the log and in
 * RESULTS), the detailed log lines, the device paths ("9.1": root port 9,
 * hub port 1) and the RESULTS summary.
 *
 * RESULTS gets one line per device once enumeration first settles, in
 * tree order, and a summary; usb_report_all is called again when the
 * driver stops, and then lists only the devices that came later. The
 * lines are built piece by piece with drv_snprintf (append). */
#include "usbbus.h"

/* ---- building a line --------------------------------------------------------- */

/* A device line holds at most 105 characters, what a RESULTS line shows. */
#define LINE_CAP 106

/* Append to buf (n characters so far, cap bytes) and return the new
 * length. Past the cap the text is cut, as snprintf cuts it. */
static unsigned append(char *buf, unsigned n, unsigned cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static unsigned append(char *buf, unsigned n, unsigned cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = drv_vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (r < 0)
        return n;
    return n + (unsigned)r < cap ? n + (unsigned)r : cap - 1;
}

/* ---- the controller ---------------------------------------------------------- */

void report_controller(const struct hc *h)
{
    char r2[48] = "", r3[48] = "";   /* "1-4,9": the USB 2 and USB 3 root ports */
    unsigned n2 = 0, n3 = 0;
    for (unsigned i = 0; i < h->nproto; i++) {
        bool usb3 = h->proto[i].major >= 3;
        char *o = usb3 ? r3 : r2;
        unsigned *k = usb3 ? &n3 : &n2;
        unsigned first = h->proto[i].first, last = first + h->proto[i].count - 1;
        if (first == last)
            *k = append(o, *k, sizeof(r2), "%s%u", *k ? "," : "", first);
        else
            *k = append(o, *k, sizeof(r2), "%s%u-%u", *k ? "," : "", first, last);
    }
    drv_report("xHCI %04x:%04x rev %02x: %u ports (USB 2: %s, USB 3: %s), %u slots, %u-byte "
               "contexts, %s, BIOS handoff %s", h->vendor, h->device, h->revision, h->ports,
               n2 ? r2 : "-", n3 ? r3 : "-", h->max_slots_en, h->csz,
               h->msix ? "MSI-X" : "MSI", h->handoff);
}

/* ---- names ------------------------------------------------------------------ */

const char *cc_str(uint32_t cc)
{
    switch (cc) {
    case 0: return "Invalid";
    case CC_SUCCESS: return "Success";
    case CC_DATA_BUFFER: return "Data Buffer Error";
    case CC_BABBLE: return "Babble Detected";
    case CC_TRANSACTION: return "USB Transaction Error";
    case CC_TRB: return "TRB Error";
    case CC_STALL: return "Stall";
    case CC_RESOURCE: return "Resource Error";
    case CC_BANDWIDTH: return "Bandwidth Error";
    case CC_NO_SLOTS: return "No Slots Available";
    case 11: return "Slot Not Enabled";
    case 12: return "Endpoint Not Enabled";
    case CC_SHORT_PACKET: return "Short Packet";
    case CC_PARAMETER: return "Parameter Error";
    case CC_CONTEXT_STATE: return "Context State Error";
    case 22: return "Incompatible Device";
    case CC_RING_STOPPED: return "Command Ring Stopped";
    case CC_ABORTED: return "Command Aborted";
    case CC_STOPPED: return "Stopped";
    case CC_STOPPED_LEN: return "Stopped - Length Invalid";
    case 35: return "Secondary Bandwidth Error";
    case 36: return "Split Transaction Error";
    case CC_TIMEOUT: return "timed out";
    case CC_GONE: return "device gone";
    case CC_BAD_SLOT: return "slot id out of range";
    default: return "error";
    }
}

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
    if (parent)
        drv_snprintf(d->path, sizeof(d->path), "%s.%u", parent->path, port);
    else
        drv_snprintf(d->path, sizeof(d->path), "%u", port);
}

/* ---- the device line -------------------------------------------------------- */

/* Everything before the interfaces, into s (LINE_CAP bytes); its length.
 * " TT(slot 2 port 2)" (" mtt" inside when the TT is a multi-TT hub's),
 * " hub 4p TTT1" (TTT: a high-speed hub's TT think time), " SS-hub 4p". */
static unsigned line_head(char *s, const struct usbdev *d, const char *prefix)
{
    unsigned n = append(s, 0, LINE_CAP, "%sport %s %04x:%04x %s", prefix ? prefix : "", d->path,
                        d->vid, d->pid, speed_str(d->speed));
    if (d->tt_slot)
        n = append(s, n, LINE_CAP, " TT(slot %u port %u%s)", d->tt_slot, d->tt_port,
                   d->tt_mtt ? " mtt" : "");
    n = append(s, n, LINE_CAP, " a%u mps%u cfg%u/%u", d->address, d->mps0, d->cfg_value,
               d->nconfigs);
    if (d->is_hub)
        n = append(s, n, LINE_CAP, " %s %up", d->ss_hub ? "SS-hub" : "hub", d->hub_ports);
    if (d->is_hub && d->speed == SPEED_HIGH)
        n = append(s, n, LINE_CAP, " TTT%u", d->ttt);
    if (d->problem)
        n = append(s, n, LINE_CAP, " PROBLEM: %s", d->problem);
    return n;
}

/* " if0 03/01/01 kbd" into t (cap bytes); its length. */
static unsigned line_iface(char *t, unsigned cap, const struct iface *f)
{
    const char *k = iface_kind(f);
    return append(t, 0, cap, " if%u %02x/%02x/%02x%s%s", f->number, f->cls, f->sub, f->proto,
                  k ? " " : "", k ? k : "");
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
    char a[LINE_CAP], b[LINE_CAP], t[48];
    unsigned na = line_head(a, d, prefix), nb = 0;
    char *o = a;
    unsigned *on = &na;
    b[0] = 0;
    for (int i = 0; i < d->nifs; i++) {
        unsigned nt = line_iface(t, sizeof(t), &d->ifs[i]);
        if (o == a && na + nt + 1 >= LINE_CAP) {
            o = b;
            on = &nb;
            nb = append(b, 0, LINE_CAP, "  port %s (cont.):", d->path);
        }
        *on = append(o, *on, LINE_CAP, "%s", t);
    }
    if (d->product[0]) {
        unsigned nt = append(t, 0, sizeof(t), " \"%s\"", d->product);
        if (*on + nt + 1 < LINE_CAP)
            *on = append(o, *on, LINE_CAP, "%s", t);
    }
    line_emit(a, report_it);
    if (nb)
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
            const struct ep *e = &d->eps[ep_dci(a)];
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
        if (!d->used || d->gone || !d->vid)
            continue;   /* a gone one may wait a moment for a task to let go of it */
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

static void usb_report_summary(const char *when)
{
    uint32_t n, nh, ni, nhid, np;
    usb_counts(&n, &nh, &ni, &nhid, &np);
    uint32_t kbd = 0, mouse = 0;
    for (int i = 0; i < MAX_DEVS; i++)
        for (int k = 0; g_devs[i].used && !g_devs[i].gone && k < g_devs[i].nifs; k++) {
            struct iface *f = &g_devs[i].ifs[k];
            kbd += f->cls == 3 && f->sub == 1 && f->proto == 1;
            mouse += f->cls == 3 && f->sub == 1 && f->proto == 2;
        }
    drv_report("%s%u device%s (%u hub%s), %u HID interface%s (%u boot kbd, %u boot mouse), "
               "%u failed, hot-plug +%u -%u", when, n, n == 1 ? "" : "s", nh, nh == 1 ? "" : "s",
               nhid, nhid == 1 ? "" : "s", kbd, mouse, g_failed, g_attached, g_detached);
}

/* Hub i's children onto the stack (sp entries so far), port 15 first so
 * that port 1 comes off first. */
static void push_children(int i, int *stack, int *sp)
{
    for (int c = 15; c >= 1; c--)
        for (int q = 0; q < MAX_DEVS; q++)
            if (g_devs[q].used && g_devs[q].parent == i && g_devs[q].port == c &&
                *sp < MAX_DEVS)
                stack[(*sp)++] = q;
}

/* Tree order: each root port's device, then what hangs below it (depth
 * first). The g_devs indexes into order (MAX_DEVS); how many. */
static int tree_order(int *order)
{
    int n = 0;
    for (uint32_t p = 1; p <= g_hc.ports; p++) {
        int stack[MAX_DEVS], sp = 0;
        for (int i = 0; i < MAX_DEVS; i++)
            if (g_devs[i].used && g_devs[i].parent < 0 && g_devs[i].port == p)
                stack[sp++] = i;
        while (sp && n < MAX_DEVS) {
            int i = stack[--sp];
            order[n++] = i;
            push_children(i, stack, &sp);
        }
    }
    return n;
}

void usb_report_all(bool at_stop)
{
    g_first_report_done = true;
    g_report_generation = g_generation;
    int order[MAX_DEVS];
    int n = tree_order(order);
    for (int k = 0; k < n; k++) {
        struct usbdev *d = &g_devs[order[k]];
        if ((d->vid || d->pid) && !d->reported) {
            dev_line(d, true, at_stop ? "new: " : NULL);
            d->reported = true;
        }
    }
    usb_report_summary(at_stop ? "at stop: " : "");
}
