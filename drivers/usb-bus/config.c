/* usb-bus: the configuration descriptor, endpoints in the controller, and
 * SET_INTERFACE.
 *
 * parse_config reads the active configuration: its interfaces (alternate
 * setting 0 is the active one; the others are only counted) and each
 * one's endpoints, keyed by DCI. usb-bus configures only interrupt-IN
 * endpoints in the controller (Configure Endpoint, configure_eps); class
 * drivers ask for other alternate settings with dev_set_interface, which
 * keeps the controller and usb-bus in agreement whatever fails. Both read
 * an alternate setting through read_alt, so an endpoint gets the same
 * Interval, Max Burst and Max ESIT Payload (SuperSpeed companions
 * included) whichever path installs it. */
#include "usbbus.h"

/* Interval field (xHCI 6.2.3.6): 2^n x 125 us. */
static uint8_t ep_interval(uint8_t speed, uint8_t xfer, uint8_t b)
{
    if (xfer == 3) {   /* interrupt */
        if (speed >= SPEED_HIGH) {
            if (b < 1)
                b = 1;
            if (b > 16)
                b = 16;
            return (uint8_t)(b - 1);
        }
        uint32_t v = (b ? b : 1) * 8u;
        int n = 31 - __builtin_clz(v);
        if (n < 3)
            n = 3;
        if (n > 10)
            n = 10;
        return (uint8_t)n;
    }
    if (xfer == 1) {   /* isochronous */
        if (b < 1)
            b = 1;
        if (b > 16)
            b = 16;
        return (uint8_t)(b - 1 + (speed == SPEED_FULL ? 3 : 0));
    }
    return 0;
}

/* ---- reading the configuration descriptor ------------------------------------ */

/* An endpoint as its descriptors give it: the fields parse_config and
 * dev_set_interface install into d->eps, so both derive them one way. */
struct ep_desc {
    uint8_t  dci, addr, attr, type, binterval;
    uint8_t  interval;   /* xHCI Interval field */
    uint8_t  burst;      /* xHCI Max Burst Size */
    uint16_t mps;        /* wMaxPacketSize bits 10:0 */
    uint16_t esit;       /* xHCI Max ESIT Payload, bytes */
};

/* One alternate setting of an interface, as the configuration lists it. */
struct alt_desc {
    uint8_t        cls, sub, proto;
    uint8_t        nep;
    struct ep_desc eps[MAX_EPS_IF];
};

/* The endpoint descriptor at p (7 bytes or more) of a device at `speed`.
 * A high-speed periodic endpoint's Max Burst is its additional
 * transactions per microframe, wMaxPacketSize bits 12:11 (xHCI 6.2.3.4);
 * a SuperSpeed one's comes with its companion (ep_companion). */
static void ep_from_desc(struct ep_desc *e, uint8_t speed, const uint8_t *p)
{
    uint8_t addr = p[2], attr = p[3], xfer = attr & 3;
    uint16_t w = le16(p + 4);
    bool periodic = xfer == 1 || xfer == 3;   /* isochronous or interrupt */
    zero(e, sizeof(*e));
    e->dci = ep_dci(addr);
    e->addr = addr;
    e->attr = attr;
    e->binterval = p[6];
    e->mps = w & 0x7ff;
    e->burst = speed == SPEED_HIGH && periodic ? (uint8_t)((w >> 11) & 3) : 0;
    e->esit = (uint16_t)(e->mps * (e->burst + 1));
    e->interval = ep_interval(speed, xfer, p[6]);
    /* usb-bus configures interrupt IN endpoints only */
    e->type = xfer == 3 && (addr & 0x80) ? EPT_INTR_IN : 0;
}

/* The SuperSpeed Endpoint Companion at p (6 bytes or more) of endpoint e
 * (USB 3.2 9.6.7): bMaxBurst, and wBytesPerInterval as the Max ESIT
 * Payload (xHCI 4.14.2). */
static void ep_companion(struct ep_desc *e, const uint8_t *p)
{
    e->burst = p[2] > 15 ? 15 : p[2];   /* 0..15; the field is 8 bits */
    uint16_t bpi = le16(p + 4);
    if (bpi)
        e->esit = bpi;
}

/* Interface `number`'s alternate setting `alt` in d's configuration: its
 * class and its first MAX_EPS_IF endpoints, each with its companion. Only
 * the first descriptor of that setting counts. false if there is none. */
static bool read_alt(const struct usbdev *d, uint8_t number, uint8_t alt, struct alt_desc *out)
{
    const uint8_t *p = d->cfg, *end = d->cfg + d->cfg_len;
    bool in_alt = false;
    struct ep_desc *last = NULL;   /* the endpoint a companion belongs to */
    out->nep = 0;
    for (; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
        uint8_t len = p[0], type = p[1];
        if (type == DESC_INTERFACE && len >= 9) {
            if (in_alt)
                return true;   /* the setting ends at the next interface */
            in_alt = p[2] == number && p[3] == alt;
            if (in_alt) {
                out->cls = p[5];
                out->sub = p[6];
                out->proto = p[7];
            }
        } else if (in_alt && type == DESC_ENDPOINT && len >= 7) {
            last = out->nep < MAX_EPS_IF ? &out->eps[out->nep++] : NULL;
            if (last)
                ep_from_desc(last, d->speed, p);
        } else if (in_alt && type == DESC_SS_COMPANION && len >= 6 && last) {
            ep_companion(last, p);
        }
    }
    return in_alt;
}

/* e takes the descriptor's fields and a clean state; its ring and buffer
 * page stay (after a drop there is no ring: one comes with the add). */
static void ep_install(struct ep *e, uint8_t ifnum, const struct ep_desc *s)
{
    int kb = e->buf_page;
    struct ring keep = e->ring;
    zero(e, sizeof(*e));
    e->ring = keep;
    e->buf_page = kb;
    e->buf = kb >= 0 ? pool_va(&g_hc, kb) : NULL;
    e->buf_dev = kb >= 0 ? pool_dev(&g_hc, kb) : 0;
    e->chan = -1;
    e->dci = s->dci;
    e->addr = s->addr;
    e->attr = s->attr;
    e->type = s->type;
    e->ifnum = ifnum;
    e->binterval = s->binterval;
    e->mps = s->mps;
    e->burst = s->burst;
    e->esit = s->esit;
    e->interval = s->interval;
}

/* Install setting a's endpoints as interface `ifnum`'s and list their
 * addresses in addrs; how many. Endpoint 0, or one another interface has
 * (or that is still configured), is not this interface's: its class
 * driver may only reach its own. */
static uint8_t install_eps(struct usbdev *d, uint8_t ifnum, const struct alt_desc *a,
                           uint8_t *addrs)
{
    uint8_t n = 0;
    for (int i = 0; i < a->nep; i++) {
        const struct ep_desc *s = &a->eps[i];
        struct ep *e = &d->eps[s->dci];
        if (s->dci < 2 || (e->dci && e->ifnum != ifnum) || e->configured) {
            drv_log("usb %s: if%u lists endpoint %02x, not its own: ignored", d->path, ifnum,
                    s->addr);
            continue;
        }
        ep_install(e, ifnum, s);
        addrs[n++] = s->addr;
    }
    return n;
}

/* One entry per interface number, from its first alternate setting 0 (the
 * active one after SET_CONFIGURATION); the other settings are counted. */
static void find_interfaces(struct usbdev *d)
{
    const uint8_t *p = d->cfg, *end = d->cfg + d->cfg_len;
    d->nifs = 0;
    for (; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
        if (p[1] != DESC_INTERFACE || p[0] < 9)
            continue;
        struct iface *f = usb_iface(d, p[2]);
        if (f) {
            f->num_alts++;
        } else if (p[3] == 0 && d->nifs < MAX_IFS) {
            f = &d->ifs[d->nifs++];
            f->number = p[2];
            f->alt = 0;
            f->cls = p[5];
            f->sub = p[6];
            f->proto = p[7];
            f->num_alts = 1;
            f->nep = 0;
            f->devmgr_chan = -1;
        }
    }
}

/* Parse the configuration descriptor: interfaces (alternate setting 0 is
 * the active one; the others are counted) and the endpoints of each. */
void parse_config(struct usbdev *d)
{
    find_interfaces(d);
    for (int i = 0; i < d->nifs; i++) {
        struct iface *f = &d->ifs[i];
        struct alt_desc a;
        f->nep = read_alt(d, f->number, 0, &a) ? install_eps(d, f->number, &a, f->ep_addr) : 0;
    }
}

struct iface *usb_iface(struct usbdev *d, uint8_t number)
{
    for (int i = 0; i < d->nifs; i++)
        if (d->ifs[i].number == number)
            return &d->ifs[i];
    return NULL;
}

/* ---- Configure Endpoint --------------------------------------------------------- */

static void slot_set_entries(struct usbdev *d, uint8_t max_dci)
{
    volatile uint32_t *s = in_ctx(d, 1);
    s[0] = (s[0] & ~(0x1fu << 27)) | ((uint32_t)(max_dci ? max_dci : 1) << 27);
}

/* The hub fields of the slot context (xHCI 6.2.2): Hub, MTT, Number of
 * Ports, TT Think Time. */
static void slot_set_hub(struct usbdev *d)
{
    volatile uint32_t *s = in_ctx(d, 1);
    if (!d->is_hub)
        return;
    s[0] |= 1u << 26;
    if (d->hub_mtt)
        s[0] |= 1u << 25;
    else
        s[0] &= ~(1u << 25);
    s[1] = (s[1] & ~(0xffu << 24)) | ((uint32_t)d->hub_ports << 24);
    if (d->speed == SPEED_HIGH)
        s[2] = (s[2] & ~(3u << 16)) | ((uint32_t)d->ttt << 16);
}

static void ep_ctx_fill(volatile uint32_t *c, struct ep *e)
{
    uint32_t esit = e->esit ? e->esit : e->mps;
    c[0] = ((uint32_t)e->interval << 16) | ((esit >> 16) << 24);
    c[1] = (3u << 1) | ((uint32_t)e->type << 3) | ((uint32_t)e->burst << 8) |
           ((uint32_t)e->mps << 16);
    c[2] = (uint32_t)e->ring.dev | 1;   /* DCS = 1 */
    c[3] = (uint32_t)(e->ring.dev >> 32);
    c[4] = (esit & 0xffff) | ((esit & 0xffff) << 16);   /* average TRB length, max ESIT lo */
}

/* Configure Endpoint: add every interrupt-IN endpoint in `add`, drop
 * those in `drop` (DCI bitmaps), with the slot's Context Entries and hub
 * fields. */
uint32_t configure_eps(struct usbdev *d, uint32_t add, uint32_t drop)
{
    in_reset(d);
    uint8_t max_dci = 1;
    for (int k = 2; k < 32; k++) {
        struct ep *e = &d->eps[k];
        bool on = (e->configured && !(drop & (1u << k))) || (add & (1u << k));
        if (on)
            max_dci = (uint8_t)k;
    }
    for (int k = 2; k < 32; k++) {
        if (!(add & (1u << k)))
            continue;
        struct ep *e = &d->eps[k];
        if (e->ring.page < 0 && !ring_init(&g_hc, &e->ring))
            return CC_RESOURCE;
        ep_ctx_fill(in_ctx(d, (unsigned)k + 1), e);
    }
    volatile uint32_t *ctl = in_ctx(d, 0);
    ctl[0] = drop & ~3u;
    ctl[1] = add | 1u;   /* A0: the slot context (Context Entries, hub fields) */
    slot_set_entries(d, max_dci);
    slot_set_hub(d);
    uint32_t cc = hc_command(&g_hc, (uint32_t)in_dev(d), (uint32_t)(in_dev(d) >> 32), 0,
                             TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24), NULL, 2000);
    if (cc == CC_SUCCESS) {
        for (int k = 2; k < 32; k++) {
            if ((drop & (1u << k)) && !(add & (1u << k))) {
                d->eps[k].configured = false;
                ring_free(&g_hc, &d->eps[k].ring);
            }
            if (add & (1u << k))
                d->eps[k].configured = true;
        }
        d->max_dci = max_dci;
    } else {
        for (int k = 2; k < 32; k++)
            if ((add & (1u << k)) && !d->eps[k].configured)
                ring_free(&g_hc, &d->eps[k].ring);
    }
    return cc;
}

/* ---- SET_INTERFACE -------------------------------------------------------------- */

/* Close interface f's endpoints and drop the configured ones in a
 * Configure Endpoint of their own. Refused: nothing changed on either
 * side, every old endpoint still configured with its ring. */
static uint32_t drop_iface_eps(struct usbdev *d, const struct iface *f, uint8_t alt)
{
    uint32_t drop = 0;
    for (int k = 2; k < 32; k++) {
        struct ep *e = &d->eps[k];
        if (e->dci && e->ifnum == f->number) {
            ep_close(d, e);
            if (e->configured)
                drop |= 1u << k;
        }
    }
    uint32_t cc = drop ? configure_eps(d, 0, drop) : CC_SUCCESS;
    if (cc != CC_SUCCESS)
        drv_log("usb %s: if%u: dropping its endpoints for alt %u: %s (unchanged)", d->path,
                f->number, alt, cc_str(cc));
    return cc;
}

/* Add the interrupt-IN endpoints among addrs (n of them, installed), each
 * with a ring of its own. Refused: configure_eps frees those rings and
 * none is configured. */
static uint32_t add_iface_eps(struct usbdev *d, const struct iface *f, uint8_t alt,
                              const uint8_t *addrs, uint8_t n)
{
    uint32_t add = 0;
    for (int i = 0; i < n; i++)
        if (d->eps[ep_dci(addrs[i])].type == EPT_INTR_IN)
            add |= 1u << ep_dci(addrs[i]);
    uint32_t cc = add ? configure_eps(d, add, 0) : CC_SUCCESS;
    if (cc != CC_SUCCESS)
        drv_log("usb %s: if%u: adding the endpoints of alt %u: %s (none configured)", d->path,
                f->number, alt, cc_str(cc));
    return cc;
}

/* SET_INTERFACE. The controller and usb-bus must agree on every endpoint
 * whatever fails: the old endpoints are dropped first, and only then do
 * the new ones get rings and an add. A ring is never freed while the
 * controller's context still points at it. The new endpoints come from
 * the same reading of the descriptors as parse_config's. */
uint32_t dev_set_interface(struct usbdev *d, struct iface *f, uint8_t alt)
{
    struct alt_desc a;
    if (alt >= f->num_alts || !read_alt(d, f->number, alt, &a))
        return CC_PARAMETER;
    uint32_t cc = drop_iface_eps(d, f, alt);
    if (cc != CC_SUCCESS)
        return cc;
    uint8_t addrs[MAX_EPS_IF];
    uint8_t n = install_eps(d, f->number, &a, addrs);
    cc = add_iface_eps(d, f, alt, addrs, n);
    if (cc != CC_SUCCESS)
        return cc;
    uint32_t got = 0;
    cc = usb_control(d, 0x01, 11, alt, f->number, 0, NULL, &got, 1000);
    if (cc != CC_SUCCESS && !(cc == CC_STALL && alt == 0 && f->num_alts == 1))
        return cc;
    f->alt = alt;
    f->cls = a.cls;
    f->sub = a.sub;
    f->proto = a.proto;
    f->nep = n;
    copy(f->ep_addr, addrs, n);
    return CC_SUCCESS;
}
