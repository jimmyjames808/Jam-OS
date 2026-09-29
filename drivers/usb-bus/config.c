/* usb-bus: the configuration descriptor, endpoints in the controller, and
 * SET_INTERFACE.
 *
 * parse_config reads the active configuration: its interfaces (alternate
 * setting 0 is the active one; the others are only counted) and each
 * one's endpoints, keyed by DCI. usb-bus configures only interrupt-IN
 * endpoints in the controller (Configure Endpoint, configure_eps); class
 * drivers ask for other alternate settings with dev_set_interface, which
 * keeps the controller and usb-bus in agreement whatever fails. */
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

/* Parse the configuration descriptor: interfaces (alternate setting 0 is
 * the active one; the others are counted) and the endpoints of each. */
void parse_config(struct usbdev *d)
{
    const uint8_t *p = d->cfg, *end = d->cfg + d->cfg_len;
    struct iface *cur = NULL;
    struct ep *last_ep = NULL;
    d->nifs = 0;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        uint8_t len = p[0], type = p[1];
        if (type == 4 && len >= 9) {   /* interface */
            last_ep = NULL;
            cur = NULL;
            uint8_t num = p[2], alt = p[3];
            struct iface *f = NULL;
            for (int i = 0; i < d->nifs; i++)
                if (d->ifs[i].number == num)
                    f = &d->ifs[i];
            if (f) {
                f->num_alts++;
            } else if (alt == 0 && d->nifs < MAX_IFS) {
                f = &d->ifs[d->nifs++];
                f->number = num;
                f->alt = 0;
                f->cls = p[5];
                f->sub = p[6];
                f->proto = p[7];
                f->num_alts = 1;
                f->nep = 0;
                f->devmgr_chan = -1;
                cur = f;
            }
        } else if (type == 5 && len >= 7 && cur) {   /* endpoint of an active interface */
            uint8_t addr = p[2], attr = p[3];
            uint8_t dci = (uint8_t)((addr & 0xf) * 2 + ((addr & 0x80) ? 1 : 0));
            /* Endpoint 0, or one another interface already lists: not this
             * interface's (its class driver may only reach its own). */
            if (dci < 2 || (d->eps[dci].dci && d->eps[dci].ifnum != cur->number)) {
                drv_log("usb %s: if%u lists endpoint %02x, not its own: ignored", d->path,
                        cur->number, addr);
                p += len;
                continue;
            }
            if (cur->nep < MAX_EPS_IF)
                cur->ep_addr[cur->nep++] = addr;
            if (dci >= 2 && dci < 32) {
                struct ep *e = &d->eps[dci];
                uint16_t w = le16(p + 4);
                e->dci = dci;
                e->addr = addr;
                e->attr = attr;
                e->ifnum = cur->number;
                e->binterval = p[6];
                e->mps = w & 0x7ff;
                e->burst = (d->speed == SPEED_HIGH && (attr & 3) >= 1 && (attr & 3) != 2)
                               ? (uint8_t)((w >> 11) & 3) : 0;
                e->esit = (uint16_t)(e->mps * (e->burst + 1));
                e->interval = ep_interval(d->speed, attr & 3, p[6]);
                if ((attr & 3) == 3 && (addr & 0x80))
                    e->type = EPT_INTR_IN;
                else
                    e->type = 0;   /* usb-bus configures interrupt IN endpoints only */
                last_ep = e;
            }
        } else if (type == 0x30 && len >= 6 && last_ep) {   /* SuperSpeed companion */
            last_ep->burst = p[2];
            uint16_t bpi = le16(p + 4);
            if (bpi)
                last_ep->esit = bpi;
        }
        p += len;
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

/* SET_INTERFACE. The controller and usb-bus must agree on every endpoint
 * whatever fails: the old endpoints are dropped by a Configure Endpoint of
 * their own first (refused: nothing changed on either side, every old
 * endpoint still configured with its ring), and only then do the new ones
 * get rings and an add (refused: configure_eps frees their rings, none is
 * configured). A ring is never freed while the controller's context still
 * points at it. */
uint32_t dev_set_interface(struct usbdev *d, struct iface *f, uint8_t alt)
{
    if (alt >= f->num_alts)
        return CC_PARAMETER;
    /* Find the alternate setting's endpoints. */
    const uint8_t *p = d->cfg, *end = d->cfg + d->cfg_len;
    bool in_alt = false, found = false;
    uint8_t nep = 0, nacc = 0, addrs[MAX_EPS_IF];
    struct ep neweps[MAX_EPS_IF];
    uint8_t cls = f->cls, sub = f->sub, proto = f->proto;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == 4 && p[0] >= 9) {
            in_alt = p[2] == f->number && p[3] == alt;
            if (in_alt) {
                found = true;
                cls = p[5];
                sub = p[6];
                proto = p[7];
            }
        } else if (p[1] == 5 && p[0] >= 7 && in_alt && nep < MAX_EPS_IF) {
            struct ep *e = &neweps[nep];
            zero(e, sizeof(*e));
            uint8_t addr = p[2], attr = p[3];
            uint16_t w = le16(p + 4);
            e->dci = (uint8_t)((addr & 0xf) * 2 + ((addr & 0x80) ? 1 : 0));
            e->addr = addr;
            e->attr = attr;
            e->ifnum = f->number;
            e->binterval = p[6];
            e->mps = w & 0x7ff;
            e->burst = d->speed == SPEED_HIGH && (attr & 3) == 3 ? (uint8_t)((w >> 11) & 3) : 0;
            e->esit = (uint16_t)(e->mps * (e->burst + 1));
            e->interval = ep_interval(d->speed, attr & 3, p[6]);
            e->type = ((attr & 3) == 3 && (addr & 0x80)) ? EPT_INTR_IN : 0;
            nep++;
        }
        p += p[0];
    }
    if (!found)
        return CC_PARAMETER;
    /* Close this interface's endpoints, then drop them. */
    uint32_t drop = 0, add = 0, cc;
    for (int k = 2; k < 32; k++) {
        struct ep *e = &d->eps[k];
        if (e->dci && e->ifnum == f->number) {
            ep_close(d, e);
            if (e->configured)
                drop |= 1u << k;
        }
    }
    if (drop && (cc = configure_eps(d, 0, drop)) != CC_SUCCESS) {
        drv_log("usb %s: if%u: dropping its endpoints for alt %u: %s (unchanged)", d->path,
                f->number, alt, cc_str(cc));
        return cc;
    }
    for (int i = 0; i < nep; i++) {
        struct ep *n = &neweps[i];
        if (n->dci < 2 || n->dci >= 32)
            continue;
        struct ep *e = &d->eps[n->dci];
        if ((e->dci && e->ifnum != f->number) || e->configured)
            continue;   /* another interface's; leave it (and don't list it) */
        addrs[nacc++] = n->addr;
        int kb = e->buf_page;
        struct ring keep = e->ring;   /* none after the drop: a new one comes with the add */
        *e = *n;
        e->ring = keep;
        e->buf_page = kb;
        e->buf = kb >= 0 ? pool_va(&g_hc, kb) : NULL;
        e->buf_dev = kb >= 0 ? pool_dev(&g_hc, kb) : 0;
        e->chan = -1;
        if (e->type == EPT_INTR_IN)
            add |= 1u << n->dci;
    }
    if (add && (cc = configure_eps(d, add, 0)) != CC_SUCCESS) {
        drv_log("usb %s: if%u: adding the endpoints of alt %u: %s (none configured)", d->path,
                f->number, alt, cc_str(cc));
        return cc;
    }
    uint32_t n = 0;
    cc = usb_control(d, 0x01, 11, alt, f->number, 0, NULL, &n, 1000);
    if (cc != CC_SUCCESS && !(cc == CC_STALL && alt == 0 && f->num_alts == 1))
        return cc;
    f->alt = alt;
    f->cls = cls;
    f->sub = sub;
    f->proto = proto;
    f->nep = nacc;
    for (int i = 0; i < nacc; i++)
        f->ep_addr[i] = addrs[i];
    return CC_SUCCESS;
}
