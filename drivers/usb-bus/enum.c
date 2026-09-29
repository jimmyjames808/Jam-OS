/* usb-bus: devices, enumeration and hubs (xHCI 1.2 chapter 4, USB 2.0
 * chapters 9 and 11, USB 3.2 chapter 10).
 *
 * Enumeration, one device at a time (M7-PLAN.md "Enumeration order"):
 * port connect -> debounce -> reset (USB 2 root ports: PORTSC.PR; USB 3
 * root ports train by themselves, warm reset if stuck; hub ports:
 * SET_FEATURE(PORT_RESET)) -> speed -> Enable Slot -> input context (slot:
 * route string, speed, root port, and for a full/low-speed device behind a
 * high-speed hub its Transaction Translator: that hub's slot and port, MTT;
 * EP0 max packet by speed) -> Address Device (BSR=0) -> GET_DESCRIPTOR
 * (device, 8) -> Evaluate Context if EP0's max packet differs -> the full
 * device descriptor, the configuration descriptor, the product string ->
 * Configure Endpoint for the interrupt-IN endpoints (Linux's order: the
 * controller's bandwidth check before the device is configured) ->
 * SET_CONFIGURATION. Hubs then: hub descriptor (USB 2 0x29 / SuperSpeed
 * 0x2A), SET_HUB_DEPTH (SuperSpeed), a second Configure Endpoint setting
 * the slot's Hub bit, Number of Ports and TT Think Time, port power, the
 * status-change endpoint, and a scan of every port.
 *
 * Hubs run single-TT (alternate setting 0, which every multi-TT hub
 * supports), so MTT is 0 for them and for the devices behind them.
 *
 * Every wait is bounded; a device that fails is logged with the step and
 * the completion code, its slot is disabled, and the port is tried at
 * most three times until it disconnects. Nothing here can hang the
 * driver: commands abort after their timeout, control transfers stop
 * their endpoint after theirs. */
#include "usbbus.h"

struct usbdev *g_devs;
uint32_t g_generation, g_attached, g_detached, g_failed, g_report_generation;
uint64_t g_last_change_ns;
bool g_first_report_done;
static uint32_t next_id;
static uint8_t root_fail[256];
static bool started;

void usb_reset_state(void)
{
    g_generation = g_attached = g_detached = g_failed = g_report_generation = 0;
    g_last_change_ns = 0;
    g_first_report_done = false;
    next_id = 0;
    started = false;
    __builtin_memset(root_fail, 0, sizeof(root_fail));
}

/* ---- small helpers ---------------------------------------------------------- */

static void zero(void *p, uint64_t n) { __builtin_memset(p, 0, n); }
static void copy(void *d, const void *s, uint64_t n) { __builtin_memcpy(d, s, n); }

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

static const char *speed_long(uint8_t s)
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

int dev_index(const struct usbdev *d)
{
    return (int)(d - g_devs);
}

struct usbdev *dev_by_slot(uint8_t slot)
{
    if (!slot || !g_devs)
        return NULL;
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].slot == slot)
            return &g_devs[i];
    return NULL;
}

static struct usbdev *child_at(int parent, uint8_t port)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent == parent && g_devs[i].port == port)
            return &g_devs[i];
    return NULL;
}

static struct usbdev *dev_alloc(void)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (d->used)
            continue;
        zero(d, sizeof(*d));
        d->used = true;
        d->id = ++next_id;
        d->parent = -1;
        d->out_page = d->in_page = -1;
        d->ep0.page = -1;
        for (int k = 0; k < 32; k++) {
            d->eps[k].ring.page = -1;
            d->eps[k].buf_page = -1;
            d->eps[k].chan = -1;
        }
        for (int k = 0; k < MAX_IFS; k++)
            d->ifs[k].devmgr_chan = -1;
        return d;
    }
    return NULL;
}

/* ---- contexts --------------------------------------------------------------- */

static volatile uint32_t *in_ctx(struct usbdev *d, unsigned index)   /* 0 control, 1 slot, dci+1 */
{
    return (volatile uint32_t *)((uint8_t *)pool_va(&g_hc, d->in_page) + index * g_hc.csz);
}

static volatile uint32_t *out_ctx(struct usbdev *d, unsigned index)  /* 0 slot, dci */
{
    return (volatile uint32_t *)((uint8_t *)pool_va(&g_hc, d->out_page) + index * g_hc.csz);
}

/* A fresh input context: control flags cleared, the slot context copied
 * from the output (device) context. */
static void in_reset(struct usbdev *d)
{
    zero(pool_va(&g_hc, d->in_page), PAGE);
    volatile uint32_t *s = in_ctx(d, 1), *o = out_ctx(d, 0);
    for (int i = 0; i < 4; i++)
        s[i] = o[i];
    s[3] = 0;   /* address and slot state: the xHC's */
}

static uint64_t in_dev(struct usbdev *d)
{
    return pool_dev(&g_hc, d->in_page);
}

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

/* ---- control transfers ------------------------------------------------------ */

static void ep_set_deq(struct usbdev *d, uint8_t dci, struct ring *r)
{
    uint64_t p = r->dev + (uint64_t)r->enq * sizeof(struct trb);
    uint32_t cc = hc_command(&g_hc, (uint32_t)p | r->cycle, (uint32_t)(p >> 32), 0,
                             TRB_TYPE(TRB_SET_TR_DEQ) | ((uint32_t)dci << 16) |
                             ((uint32_t)d->slot << 24), NULL, 1000);
    if (cc != CC_SUCCESS && !d->gone)
        drv_log("usb %s: Set TR Dequeue (ep %u): %s", d->path, dci, cc_str(cc));
}

/* After a halt (STALL, a transaction error): Reset Endpoint, then move the
 * dequeue pointer past whatever was queued (xHCI 4.6.8, 4.8.3). tsp:
 * Transfer State Preserve, a soft retry that keeps the data toggle (after
 * a transaction error the device's toggle didn't move either). */
static void ep_reset_tsp(struct usbdev *d, uint8_t dci, struct ring *r, bool tsp)
{
    uint32_t cc = hc_command(&g_hc, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)dci << 16) |
                             ((uint32_t)d->slot << 24) | (tsp ? 1u << 9 : 0), NULL, 1000);
    if (cc != CC_SUCCESS && cc != CC_CONTEXT_STATE && !d->gone)
        drv_log("usb %s: Reset Endpoint (ep %u): %s", d->path, dci, cc_str(cc));
    ep_set_deq(d, dci, r);
}

static void ep_reset(struct usbdev *d, uint8_t dci, struct ring *r)
{
    ep_reset_tsp(d, dci, r, false);
}

/* A running endpoint whose transfer never finished: Stop Endpoint, then
 * move the dequeue pointer past it. */
static void ep_stop(struct usbdev *d, uint8_t dci, struct ring *r)
{
    uint32_t cc = hc_command(&g_hc, 0, 0, 0, TRB_TYPE(TRB_STOP_EP) | ((uint32_t)dci << 16) |
                             ((uint32_t)d->slot << 24), NULL, 1000);
    if (cc == CC_CONTEXT_STATE && d->out_page >= 0 && (out_ctx(d, dci)[0] & 7) == 2) {
        /* Halted (an error the main loop hasn't recovered yet, or one it
         * gave up on): Stop can't touch it and Set TR Dequeue would fail
         * the same way, leaving it halted for the next open. Reset it. */
        ep_reset(d, dci, r);
        return;
    }
    if (cc != CC_SUCCESS && cc != CC_CONTEXT_STATE && !d->gone)
        drv_log("usb %s: Stop Endpoint (ep %u): %s", d->path, dci, cc_str(cc));
    ep_set_deq(d, dci, r);
}

uint32_t usb_control(struct usbdev *d, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                     uint16_t length, void *data, uint32_t *actual, uint64_t timeout_ms)
{
    struct hc *h = &g_hc;
    if (actual)
        *actual = 0;
    if (d->gone || !d->slot || d->ep0.page < 0)
        return CC_GONE;
    if (h->dead)
        return CC_GONE;
    if (length > PAGE)
        return CC_TRB;
    bool in = rt & 0x80;
    uint8_t *buf = pool_va(h, h->ctl_page);
    uint64_t bdev = pool_dev(h, h->ctl_page);
    if (!in && length)
        copy(buf, data, length);
    h->ctl.busy = true;
    h->ctl.done = false;
    h->ctl.slot = d->slot;
    h->ctl.cc = 0;
    h->ctl.residual = 0;
    h->ctl.short_seen = false;
    h->ctl.data_trb = 0;
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    h->ctl.setup_trb = ring_push(&d->ep0, rt | (uint32_t)req << 8 | (uint32_t)value << 16,
                                 index | (uint32_t)length << 16, 8,
                                 TRB_TYPE(TRB_SETUP) | TRB_IDT | trt << 16);
    if (length)
        h->ctl.data_trb = ring_push(&d->ep0, (uint32_t)bdev, (uint32_t)(bdev >> 32), length,
                                    TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN | TRB_ISP : 0));
    h->ctl.status_trb = ring_push(&d->ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                                  ((in && length) ? 0 : TRB_DIR_IN));
    hc_doorbell(h, d->slot, 1);
    uint64_t deadline = drv_clock_ns() + timeout_ms * MS;
    while (!h->ctl.done && !d->gone && !h->dead && drv_clock_ns() < deadline)
        hc_wait(h, deadline);
    uint32_t cc;
    if (h->ctl.done) {
        cc = h->ctl.cc;
    } else {
        cc = d->gone || h->dead ? CC_GONE : CC_TIMEOUT;
        h->ctl.busy = false;
        if (!h->dead)
            ep_stop(d, 1, &d->ep0);
    }
    h->ctl.busy = false;
    if (cc == CC_SUCCESS) {
        uint32_t n = h->ctl.short_seen ? length - (h->ctl.residual < length ? h->ctl.residual : length)
                                       : length;
        if (in && n)
            copy(data, buf, n);
        if (actual)
            *actual = n;
    } else if (cc != CC_TIMEOUT && cc != CC_GONE) {
        /* A halted default endpoint: STALL, transaction error, babble. */
        ep_reset(d, 1, &d->ep0);
    }
    return cc;
}

static void ctl_event(struct hc *h, uint64_t trb, uint32_t cc, uint32_t residual)
{
    if (!h->ctl.busy || h->ctl.done)
        return;
    if (trb && trb == h->ctl.data_trb) {
        if (cc == CC_SHORT_PACKET) {
            h->ctl.short_seen = true;
            h->ctl.residual = residual;
            return;   /* the status stage follows */
        }
        if (cc == CC_SUCCESS)
            return;
    } else if (trb && trb == h->ctl.status_trb) {
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            h->ctl.cc = CC_SUCCESS;
            h->ctl.done = true;
            return;
        }
    } else if (trb && trb != h->ctl.setup_trb && cc == CC_SUCCESS) {
        return;   /* a leftover of an earlier transfer */
    }
    h->ctl.cc = cc;
    h->ctl.done = true;
}

/* ---- interrupt IN endpoints ----------------------------------------------------- */

#define INTR_STRIDE (PAGE / INTR_TRBS)

static uint32_t intr_len(struct ep *e)
{
    uint32_t n = e->esit ? e->esit : e->mps;
    if (!n)
        n = 8;
    return n > INTR_STRIDE ? INTR_STRIDE : n;
}

static void intr_queue(struct usbdev *d, struct ep *e, uint8_t slot)
{
    (void)d;
    uint64_t b = e->buf_dev + (uint64_t)slot * INTR_STRIDE;
    uint64_t trb = ring_push(&e->ring, (uint32_t)b, (uint32_t)(b >> 32), intr_len(e),
                             TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    if (e->ninflight < INTR_TRBS) {
        e->inflight[e->ninflight].idx = (uint16_t)ring_index(&e->ring, trb);
        e->inflight[e->ninflight].slot = slot;
        e->ninflight++;
    }
}

static void intr_fill(struct usbdev *d, struct ep *e)
{
    e->ninflight = 0;
    for (uint8_t s = 0; s < INTR_TRBS; s++)
        intr_queue(d, e, s);
    hc_doorbell(&g_hc, d->slot, e->dci);
}

int ep_open_intr(struct usbdev *d, struct ep *e, uint8_t owner, int chan)
{
    if (!e->configured || e->type != EPT_INTR_IN)
        return -1;
    if (e->open)
        return -2;
    if (e->buf_page < 0) {
        e->buf_page = pool_alloc(&g_hc);
        if (e->buf_page < 0)
            return -3;
        e->buf = pool_va(&g_hc, e->buf_page);
        e->buf_dev = pool_dev(&g_hc, e->buf_page);
    }
    e->open = true;
    e->halted = false;
    e->owner = owner;
    e->chan = chan;
    e->errors_in_row = 0;
    intr_fill(d, e);
    return 0;
}

/* Stop polling e (the client went away, or set_interface). */
void ep_close(struct usbdev *d, struct ep *e)
{
    if (!e->open)
        return;
    e->open = false;
    if (!d->gone && e->configured)
        ep_stop(d, e->dci, &e->ring);
    e->ninflight = 0;
    if (e->chan >= 0)
        chan_close(e->chan);
    e->chan = -1;
    d->ep_recover &= ~(1u << e->dci);
    d->ep_drop &= ~(1u << e->dci);
}

static void intr_event(struct usbdev *d, struct ep *e, uint64_t trb, uint32_t cc,
                       uint32_t residual)
{
    uint32_t idx = ring_index(&e->ring, trb);
    int k = -1;
    for (int i = 0; i < e->ninflight; i++)
        if (e->inflight[i].idx == idx) {
            k = i;
            break;
        }
    if (k < 0)
        return;   /* abandoned by a reset or a stop */
    uint8_t slot = e->inflight[k].slot;
    for (int i = k; i + 1 < e->ninflight; i++)
        e->inflight[i] = e->inflight[i + 1];
    e->ninflight--;
    if (!e->open)
        return;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        uint32_t want = intr_len(e);
        uint32_t n = want - (residual < want ? residual : want);
        const uint8_t *p = e->buf + (uint64_t)slot * INTR_STRIDE;
        e->reports++;
        e->errors_in_row = 0;
        if (e->owner == EP_OWNER_HUB) {
            for (uint32_t i = 0; i < n && i < sizeof(d->hub_change); i++)
                ((uint8_t *)d->hub_change)[i] |= p[i];
        } else if (e->owner == EP_OWNER_CLIENT) {
            bool dropped = false;
            if (!serve_report(e->chan, p, n, &dropped)) {
                d->ep_drop |= 1u << e->dci;   /* closed in the main loop */
                return;
            }
            if (dropped)
                e->dropped++;
        }
        if (!e->halted) {
            intr_queue(d, e, slot);
            hc_doorbell(&g_hc, d->slot, e->dci);
        }
        return;
    }
    if (cc == CC_STOPPED || cc == CC_STOPPED_LEN || cc == CC_STOPPED_SHORT)
        return;
    /* An error halts the endpoint: reset it from the main loop. */
    e->last_cc = (uint16_t)cc;
    e->errors++;
    e->errors_in_row++;
    e->halted = true;
    d->ep_recover |= 1u << e->dci;
    if (e->errors_in_row <= 3)
        drv_log("usb %s: interrupt ep %02x: %s", d->path, e->addr, cc_str(cc));
}

void usb_transfer_event(struct hc *h, uint8_t slot, uint8_t dci, uint64_t trb, uint32_t cc,
                        uint32_t residual)
{
    struct usbdev *d = dev_by_slot(slot);
    if (dci == 1) {
        if (h->ctl.busy && h->ctl.slot == slot)
            ctl_event(h, trb, cc, residual);
        return;
    }
    if (!d || dci >= 32 || !d->eps[dci].dci)
        return;
    intr_event(d, &d->eps[dci], trb, cc, residual);
}

/* ---- descriptors --------------------------------------------------------------- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint32_t get_desc(struct usbdev *d, uint8_t type, uint8_t index, uint16_t lang, void *buf,
                         uint16_t len, uint32_t *actual)
{
    uint32_t cc = CC_TIMEOUT;
    for (int attempt = 0; attempt < 3; attempt++) {
        cc = usb_control(d, 0x80, 6, (uint16_t)(type << 8 | index), lang, len, buf, actual, 1000);
        if (cc == CC_SUCCESS || cc == CC_STALL || cc == CC_GONE || g_hc.stopping)
            break;
        hc_sleep(&g_hc, 10);
    }
    return cc;
}

/* A string descriptor as ASCII (others -> '?'), trimmed. */
static void get_string(struct usbdev *d, uint8_t index, uint16_t lang, char *out, unsigned cap)
{
    out[0] = 0;
    if (!index || !lang)
        return;
    uint8_t b[128];
    uint32_t n = 0;
    /* One short try: strings are only for the log. */
    if (usb_control(d, 0x80, 6, (uint16_t)(3 << 8 | index), lang, sizeof(b), b, &n, 500) !=
            CC_SUCCESS || n < 2 || b[1] != 3)
        return;
    if (b[0] < n)
        n = b[0];
    unsigned o = 0;
    for (uint32_t i = 2; i + 1 < n && o + 1 < cap; i += 2) {
        uint16_t c = le16(b + i);
        out[o++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

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
static void parse_config(struct usbdev *d)
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
                    e->type = 0;   /* not configured by usb-bus in M7 */
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

/* Configure Endpoint: add every interrupt-IN endpoint in `add`, drop
 * those in `drop` (DCI bitmaps), with the slot's Context Entries and hub
 * fields. */
static uint32_t configure_eps(struct usbdev *d, uint32_t add, uint32_t drop)
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

uint32_t dev_set_interface(struct usbdev *d, struct iface *f, uint8_t alt)
{
    if (alt >= f->num_alts)
        return CC_PARAMETER;
    /* Close and drop this interface's endpoints. */
    uint32_t drop = 0, add = 0;
    for (int k = 2; k < 32; k++) {
        struct ep *e = &d->eps[k];
        if (e->dci && e->ifnum == f->number) {
            ep_close(d, e);
            if (e->configured)
                drop |= 1u << k;
        }
    }
    /* Find the alternate setting's endpoints. */
    const uint8_t *p = d->cfg, *end = d->cfg + d->cfg_len;
    bool in_alt = false, found = false;
    uint8_t nep = 0, addrs[MAX_EPS_IF];
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
            addrs[nep++] = addr;
        }
        p += p[0];
    }
    if (!found)
        return CC_PARAMETER;
    for (int i = 0; i < nep; i++) {
        struct ep *n = &neweps[i];
        if (n->dci < 2 || n->dci >= 32)
            continue;
        struct ep *e = &d->eps[n->dci];
        bool was = e->configured && !(drop & (1u << n->dci));
        if (was)
            continue;   /* another interface's; leave it */
        int kb = e->buf_page;
        if (drop & (1u << n->dci))
            ring_free(&g_hc, &e->ring);   /* a new ring: the context starts at its first TRB */
        struct ring keep = e->ring;
        *e = *n;
        e->ring = keep;
        e->buf_page = kb;
        e->buf = kb >= 0 ? pool_va(&g_hc, kb) : NULL;
        e->buf_dev = kb >= 0 ? pool_dev(&g_hc, kb) : 0;
        e->chan = -1;
        if (e->type == EPT_INTR_IN)
            add |= 1u << n->dci;
    }
    uint32_t cc = CC_SUCCESS;
    if (add || drop)
        cc = configure_eps(d, add, drop);
    if (cc != CC_SUCCESS)
        return cc;
    uint32_t n = 0;
    cc = usb_control(d, 0x01, 11, alt, f->number, 0, NULL, &n, 1000);
    if (cc != CC_SUCCESS && !(cc == CC_STALL && alt == 0 && f->num_alts == 1))
        return cc;
    f->alt = alt;
    f->cls = cls;
    f->sub = sub;
    f->proto = proto;
    f->nep = nep;
    for (int i = 0; i < nep; i++)
        f->ep_addr[i] = addrs[i];
    return CC_SUCCESS;
}

/* ---- reporting ----------------------------------------------------------------- */

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

/* One line: port 3.2 258a:0033 FS TT(slot 2 port 2) a5 mps8 cfg1/1 if0 03/01/01 kbd
 * if1 03/00/00 hid "Name" (a5: USB address 5; mps8: EP0 max packet; cfg1/1: the
 * configuration set / how many the device has; ifN class/subclass/protocol).
 * What doesn't fit goes on a second line. */
static void dev_line(struct usbdev *d, bool report_it, const char *prefix)
{
    char a[160], b[160];
    struct sb s = { a, 0, 106 }, s2 = { b, 0, 106 };
    a[0] = b[0] = 0;
    if (prefix)
        sb_s(&s, prefix);
    sb_s(&s, "port ");
    sb_s(&s, d->path);
    sb_c(&s, ' ');
    sb_x(&s, d->vid, 4);
    sb_c(&s, ':');
    sb_x(&s, d->pid, 4);
    sb_c(&s, ' ');
    sb_s(&s, speed_str(d->speed));
    if (d->tt_slot) {
        sb_s(&s, " TT(slot ");
        sb_u(&s, d->tt_slot);
        sb_s(&s, " port ");
        sb_u(&s, d->tt_port);
        if (d->tt_mtt)
            sb_s(&s, " mtt");
        sb_c(&s, ')');
    }
    sb_s(&s, " a");
    sb_u(&s, d->address);
    sb_s(&s, " mps");
    sb_u(&s, d->mps0);
    sb_s(&s, " cfg");
    sb_u(&s, d->cfg_value);
    sb_c(&s, '/');
    sb_u(&s, d->nconfigs);
    if (d->is_hub) {
        sb_s(&s, d->ss_hub ? " SS-hub " : " hub ");
        sb_u(&s, d->hub_ports);
        sb_c(&s, 'p');
        if (d->speed == SPEED_HIGH) {
            sb_s(&s, " TTT");
            sb_u(&s, d->ttt);
        }
    }
    if (d->problem) {
        sb_s(&s, " PROBLEM: ");
        sb_s(&s, d->problem);
    }
    struct sb *o = &s;
    for (int i = 0; i < d->nifs; i++) {
        struct iface *f = &d->ifs[i];
        char t[48];
        struct sb ts = { t, 0, sizeof(t) };
        t[0] = 0;
        sb_s(&ts, " if");
        sb_u(&ts, f->number);
        sb_c(&ts, ' ');
        sb_x(&ts, f->cls, 2);
        sb_c(&ts, '/');
        sb_x(&ts, f->sub, 2);
        sb_c(&ts, '/');
        sb_x(&ts, f->proto, 2);
        const char *k = iface_kind(f);
        if (k) {
            sb_c(&ts, ' ');
            sb_s(&ts, k);
        }
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
    if (report_it) {
        drv_report("%s", a);
        if (s2.n)
            drv_report("%s", b);
    } else {
        drv_log("%s", a);
        if (s2.n)
            drv_log("%s", b);
    }
}

static void dev_log_detail(struct usbdev *d)
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

/* ---- attach ----------------------------------------------------------------------- */

static void dev_free(struct usbdev *d, bool slot_disabled)
{
    if (!slot_disabled && !g_hc.dead) {
        /* Disable Slot failed: the controller may still own the slot and
         * run its endpoints (queued TRBs into our buffers), so none of its
         * DMA pages can go back to the pool. Leaked, like a quarantine. */
        drv_log("usb %s: slot %u not disabled: keeping its DMA pages", d->path, d->slot);
        if (d->cfg)
            drv_free(d->cfg);
        d->cfg = NULL;
        d->used = false;
        return;
    }
    for (int k = 2; k < 32; k++) {
        struct ep *e = &d->eps[k];
        e->chan = -1;   /* closed by serve_iface_gone() */
        e->open = false;
        ring_free(&g_hc, &e->ring);
        if (e->buf_page >= 0)
            pool_free(&g_hc, e->buf_page);
        e->buf_page = -1;
    }
    ring_free(&g_hc, &d->ep0);
    if (d->slot)
        hc_set_dcbaa(&g_hc, d->slot, 0);
    if (d->out_page >= 0)
        pool_free(&g_hc, d->out_page);
    if (d->in_page >= 0)
        pool_free(&g_hc, d->in_page);
    d->out_page = d->in_page = -1;
    if (d->cfg)
        drv_free(d->cfg);
    d->cfg = NULL;
    d->used = false;
}

/* True once the controller has let go of d's slot (or never had one). */
static bool disable_slot(struct usbdev *d)
{
    if (!d->slot)
        return true;
    uint32_t cc = hc_command(&g_hc, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)d->slot << 24),
                             NULL, 1000);
    if (cc != CC_SUCCESS)
        drv_log("usb %s: Disable Slot %u: %s", d->path, d->slot, cc_str(cc));
    return cc == CC_SUCCESS || cc == 11;   /* 11: Slot Not Enabled */
}

static void detach(struct usbdev *d, const char *why, bool quiet)
{
    int me = dev_index(d);
    d->gone = true;
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent == me)
            detach(&g_devs[i], "its hub went away", quiet);
    serve_iface_gone(d->id);   /* its interface and report channels: the peers see PEER_CLOSED */
    if (!quiet)
        drv_log("usb %s: %04x:%04x detached (%s)", d->path, d->vid, d->pid, why);
    if (d->vid && g_first_report_done && !quiet)
        g_detached++;
    bool off = disable_slot(d);
    dev_free(d, off);
    g_generation++;
    g_last_change_ns = drv_clock_ns();
}

static void attach_failed(struct usbdev *d, const char *step, uint32_t cc)
{
    g_failed++;
    const char *tt = d->tt_slot ? " via TT" : "";
    if (g_failed <= 8)
        drv_report("usb %s: %s device%s: FAILED at %s: %s (cc %u)", d->path, speed_long(d->speed),
                   tt, step, cc_str(cc), cc);
    else
        drv_log("usb %s: %s device%s: FAILED at %s: %s (cc %u)", d->path, speed_long(d->speed), tt,
                step, cc_str(cc), cc);
    if (d->tt_slot)
        drv_log("usb %s: slot context had TT hub slot %u, TT port %u, MTT %u, route %05x",
                d->path, d->tt_slot, d->tt_port, d->tt_mtt, d->route);
    d->gone = true;
    serve_iface_gone(d->id);
    bool off = disable_slot(d);
    dev_free(d, off);
    g_last_change_ns = drv_clock_ns();
}

static bool hub_setup(struct usbdev *d);

/* Enumerate the device just reset on `port` of hub `parent` (-1: a root
 * port). True if it ended configured. */
static bool enumerate(int parent, uint8_t port, uint8_t speed)
{
    struct hc *h = &g_hc;
    struct usbdev *d = dev_alloc();
    if (!d) {
        g_failed++;
        drv_report("usb: port %u: more than %u devices; not enumerated", port, MAX_DEVS);
        return false;
    }
    struct usbdev *p = parent >= 0 ? &g_devs[parent] : NULL;
    d->parent = parent;
    d->port = port;
    d->speed = speed;
    struct sb s = { d->path, 0, sizeof(d->path) };
    if (p) {
        sb_s(&s, p->path);
        sb_c(&s, '.');
        d->root_port = p->root_port;
        d->level = (uint8_t)(p->level + 1);
        d->route = p->route | ((uint32_t)(port > 15 ? 15 : port) << (4 * (p->level - 1)));
        if ((speed == SPEED_FULL || speed == SPEED_LOW) && p->speed == SPEED_HIGH) {
            d->tt_slot = p->slot;
            d->tt_port = port;
            d->tt_mtt = p->hub_mtt;
        } else if (p->tt_slot && speed < SPEED_HIGH) {
            d->tt_slot = p->tt_slot;
            d->tt_port = p->tt_port;
            d->tt_mtt = p->tt_mtt;
        }
    } else {
        d->root_port = port;
        d->level = 1;
        d->route = 0;
    }
    sb_u(&s, port);

    /* Enable Slot (the slot type of the root port's protocol). */
    uint32_t slot_type = 0;
    for (unsigned i = 0; i < h->nproto; i++)
        if (d->root_port >= h->proto[i].first &&
            d->root_port < h->proto[i].first + h->proto[i].count)
            slot_type = h->proto[i].slot_type;
    uint32_t slot = 0;
    uint32_t cc = hc_command(h, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT) | slot_type << 16, &slot, 1000);
    if (cc != CC_SUCCESS || !slot || slot > h->max_slots_en) {
        attach_failed(d, "Enable Slot", cc);
        return false;
    }
    d->slot = (uint8_t)slot;
    d->out_page = pool_alloc(h);
    d->in_page = pool_alloc(h);
    if (d->out_page < 0 || d->in_page < 0 || !ring_init(h, &d->ep0)) {
        attach_failed(d, "memory for the contexts", CC_RESOURCE);
        return false;
    }
    hc_set_dcbaa(h, slot, pool_dev(h, d->out_page));

    /* Address Device: slot context + EP0. */
    d->mps0 = speed >= SPEED_SUPER ? 512 : speed == SPEED_HIGH ? 64 : 8;
    zero(pool_va(h, d->in_page), PAGE);
    volatile uint32_t *ctl = in_ctx(d, 0), *sc = in_ctx(d, 1), *e0 = in_ctx(d, 2);
    ctl[1] = 3;   /* A0 | A1 */
    sc[0] = (d->route & 0xfffff) | ((uint32_t)speed << 20) | ((uint32_t)d->tt_mtt << 25) | (1u << 27);
    sc[1] = (uint32_t)d->root_port << 16;
    sc[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
    sc[3] = 0;
    e0[0] = 0;
    e0[1] = (3u << 1) | (EPT_CONTROL << 3) | ((uint32_t)d->mps0 << 16);
    e0[2] = (uint32_t)d->ep0.dev | 1;
    e0[3] = (uint32_t)(d->ep0.dev >> 32);
    e0[4] = 8;
    cc = CC_TIMEOUT;
    for (int attempt = 0; attempt < 2 && !h->stopping; attempt++) {
        cc = hc_command(h, (uint32_t)in_dev(d), (uint32_t)(in_dev(d) >> 32), 0,
                        TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24), NULL, 3000);
        if (cc == CC_SUCCESS)
            break;
        drv_log("usb %s: Address Device: %s; %s", d->path, cc_str(cc),
                attempt ? "giving up" : "trying again");
        hc_sleep(h, 20);
    }
    if (cc != CC_SUCCESS) {
        attach_failed(d, "Address Device (SET_ADDRESS)", cc);
        return false;
    }
    d->address = (uint8_t)(out_ctx(d, 0)[3] & 0xff);
    hc_sleep(h, 10);

    /* The first 8 bytes of the device descriptor: EP0's max packet. */
    uint8_t dd[18];
    uint32_t n = 0;
    cc = get_desc(d, 1, 0, 0, dd, 8, &n);
    if (cc != CC_SUCCESS || n < 8) {
        attach_failed(d, "GET_DESCRIPTOR(device, 8)", cc == CC_SUCCESS ? CC_SHORT_PACKET : cc);
        return false;
    }
    uint16_t mps = dd[7];
    if (speed >= SPEED_SUPER)
        mps = (uint16_t)(1u << (dd[7] & 15));   /* an exponent from USB 3.0 on */
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64 && mps != 512)
        mps = d->mps0;
    if (mps != d->mps0) {
        d->mps0 = mps;
        zero(pool_va(h, d->in_page), PAGE);
        volatile uint32_t *o0 = out_ctx(d, 1);
        ctl = in_ctx(d, 0);
        e0 = in_ctx(d, 2);
        ctl[1] = 2;   /* A1 */
        for (int i = 0; i < 5; i++)
            e0[i] = o0[i];
        e0[1] = (e0[1] & 0xffffu) | ((uint32_t)mps << 16);
        cc = hc_command(h, (uint32_t)in_dev(d), (uint32_t)(in_dev(d) >> 32), 0,
                        TRB_TYPE(TRB_EVAL_CTX) | (slot << 24), NULL, 1000);
        if (cc != CC_SUCCESS) {
            attach_failed(d, "Evaluate Context (EP0 max packet)", cc);
            return false;
        }
    }

    cc = get_desc(d, 1, 0, 0, dd, 18, &n);
    if (cc != CC_SUCCESS || n < 18) {
        attach_failed(d, "GET_DESCRIPTOR(device)", cc == CC_SUCCESS ? CC_SHORT_PACKET : cc);
        return false;
    }
    d->bcd = le16(dd + 2);
    d->cls = dd[4];
    d->sub = dd[5];
    d->proto = dd[6];
    d->vid = le16(dd + 8);
    d->pid = le16(dd + 10);
    d->nconfigs = dd[17];
    d->iserial = dd[16];
    uint8_t iproduct = dd[15];

    /* Configuration 0: its header, then all of it. */
    uint8_t ch[9];
    cc = get_desc(d, 2, 0, 0, ch, 9, &n);
    if (cc != CC_SUCCESS || n < 9 || ch[1] != 2) {
        attach_failed(d, "GET_DESCRIPTOR(configuration, 9)", cc == CC_SUCCESS ? CC_SHORT_PACKET : cc);
        return false;
    }
    uint16_t total = le16(ch + 2);
    if (total < 9)
        total = 9;
    if (total > CFG_MAX)
        total = CFG_MAX;
    d->cfg = drv_malloc(total);
    if (!d->cfg) {
        attach_failed(d, "memory for the configuration", CC_RESOURCE);
        return false;
    }
    cc = get_desc(d, 2, 0, 0, d->cfg, total, &n);
    if (cc != CC_SUCCESS || n < 9) {
        attach_failed(d, "GET_DESCRIPTOR(configuration)", cc == CC_SUCCESS ? CC_SHORT_PACKET : cc);
        return false;
    }
    d->cfg_len = (uint16_t)n;
    parse_config(d);

    /* Strings: the first language, the product and the serial number. */
    uint8_t langs[8];
    if (usb_control(d, 0x80, 6, 3 << 8, 0, sizeof(langs), langs, &n, 500) == CC_SUCCESS &&
        n >= 4 && langs[1] == 3) {
        uint16_t lang = le16(langs + 2);
        get_string(d, iproduct, lang, d->product, sizeof(d->product));
        get_string(d, d->iserial, lang, d->serial, sizeof(d->serial));
    }

    d->is_hub = d->cls == 9 || (d->nifs && d->ifs[0].cls == 9);
    d->ss_hub = d->is_hub && speed >= SPEED_SUPER;
    if (d->is_hub && d->level >= MAX_LEVEL) {
        d->problem = "hub too deep (5 tiers)";
        d->is_hub = false;
    }

    /* Configure Endpoint (the interrupt-IN endpoints), then
     * SET_CONFIGURATION. */
    uint32_t add = 0;
    for (int k = 2; k < 32; k++)
        if (d->eps[k].dci && d->eps[k].type == EPT_INTR_IN)
            add |= 1u << k;
    bool hub = d->is_hub;
    d->is_hub = false;   /* the hub fields go in once the hub descriptor is read */
    cc = add ? configure_eps(d, add, 0) : CC_SUCCESS;
    d->is_hub = hub;
    if (cc != CC_SUCCESS) {
        d->problem = cc == CC_BANDWIDTH ? "Configure Endpoint: no bandwidth"
                                        : "Configure Endpoint failed";
        drv_log("usb %s: Configure Endpoint: %s", d->path, cc_str(cc));
    } else {
        d->cfg_value = d->cfg[5];
        cc = usb_control(d, 0x00, 9, d->cfg_value, 0, 0, NULL, &n, 1000);
        if (cc != CC_SUCCESS) {
            attach_failed(d, "SET_CONFIGURATION", cc);
            return false;
        }
        d->configured = true;
    }
    if (d->is_hub && d->configured && !hub_setup(d))
        d->is_hub = false;

    g_generation++;
    g_last_change_ns = drv_clock_ns();
    if (g_first_report_done) {
        g_attached++;
        dev_line(d, false, "attached: ");
    }
    dev_log_detail(d);
    if (d->configured && !d->is_hub)
        serve_device_ready(d);
    return d->configured;
}

/* ---- hubs ------------------------------------------------------------------------- */

#define HUB_PORT_CONNECTION 0
#define HUB_PORT_ENABLE     1
#define HUB_PORT_RESET      4
#define HUB_PORT_POWER      8
#define HUB_C_CONNECTION    16
#define HUB_C_ENABLE        17
#define HUB_C_SUSPEND       18
#define HUB_C_OVER_CURRENT  19
#define HUB_C_RESET         20
#define HUB_C_LINK_STATE    25
#define HUB_C_CONFIG_ERROR  26
#define HUB_C_BH_RESET      29
#define HUB_BH_PORT_RESET   28

static uint32_t hub_feature(struct usbdev *d, bool set, uint16_t feature, uint16_t port)
{
    uint32_t n;
    return usb_control(d, 0x23, set ? 3 : 1, feature, port, 0, NULL, &n, 1000);
}

static uint32_t hub_port_status(struct usbdev *d, uint8_t port, uint16_t *status,
                                uint16_t *change)
{
    uint8_t b[4];
    uint32_t n = 0;
    uint32_t cc = usb_control(d, 0xa3, 0, 0, port, 4, b, &n, 1000);
    if (cc == CC_SUCCESS && n < 4)
        cc = CC_SHORT_PACKET;
    if (cc == CC_SUCCESS) {
        *status = le16(b);
        *change = le16(b + 2);
    }
    return cc;
}

static bool hub_setup(struct usbdev *d)
{
    struct hc *h = &g_hc;
    uint8_t b[72];
    uint32_t n = 0, cc;
    if (d->ss_hub) {
        cc = usb_control(d, 0x20, 12, (uint16_t)(d->level - 1), 0, 0, NULL, &n, 1000);
        if (cc != CC_SUCCESS)
            drv_log("usb %s: SET_HUB_DEPTH %u: %s", d->path, d->level - 1, cc_str(cc));
        cc = usb_control(d, 0xa0, 6, 0x2a00, 0, 12, b, &n, 1000);
    } else {
        cc = usb_control(d, 0xa0, 6, 0x2900, 0, 71, b, &n, 1000);
    }
    if (cc != CC_SUCCESS || n < 7) {
        d->problem = "no hub descriptor";
        drv_log("usb %s: hub descriptor: %s (%u bytes)", d->path, cc_str(cc), n);
        return false;
    }
    d->hub_ports = b[2];
    d->hub_chars = le16(b + 3);
    d->pgood_ms = b[5] * 2u;
    d->ttt = (uint8_t)((d->hub_chars >> 5) & 3);
    d->hub_mtt = false;   /* single TT (alternate setting 0) */
    if (d->hub_ports > 15) {
        drv_log("usb %s: hub has %u ports; using the first 15", d->path, d->hub_ports);
        d->hub_ports = 15;
    }
    /* Hub bit, Number of Ports, TT Think Time: a Configure Endpoint of
     * just the slot context (what Linux does). */
    cc = configure_eps(d, 0, 0);
    if (cc != CC_SUCCESS) {
        d->problem = "Configure Endpoint (hub fields) failed";
        drv_log("usb %s: Configure Endpoint (hub fields): %s", d->path, cc_str(cc));
        return false;
    }
    for (uint8_t p = 1; p <= d->hub_ports; p++)
        hub_feature(d, true, HUB_PORT_POWER, p);
    hc_sleep(h, d->pgood_ms > 100 ? d->pgood_ms : 100);
    /* The status-change endpoint. */
    for (int k = 2; k < 32; k++)
        if (d->eps[k].configured && d->eps[k].type == EPT_INTR_IN) {
            if (ep_open_intr(d, &d->eps[k], EP_OWNER_HUB, -1) == 0)
                d->hub_intr_dci = (uint8_t)k;
            break;
        }
    if (!d->hub_intr_dci)
        drv_log("usb %s: hub without a status-change endpoint; ports are scanned on attach only",
                d->path);
    d->hub_scan_all = true;
    return true;
}

static void hub_port(struct usbdev *hub, uint8_t port)
{
    struct hc *h = &g_hc;
    uint16_t st = 0, chg = 0;
    uint32_t cc = hub_port_status(hub, port, &st, &chg);
    if (cc != CC_SUCCESS) {
        if (!hub->gone)
            drv_log("usb %s: port %u status: %s", hub->path, port, cc_str(cc));
        return;
    }
    /* Clear every change bit we saw (wPortChange bit -> C_* feature). */
    static const uint8_t usb2[16] = { HUB_C_CONNECTION, HUB_C_ENABLE, HUB_C_SUSPEND,
                                      HUB_C_OVER_CURRENT, HUB_C_RESET };
    static const uint8_t usb3[16] = { HUB_C_CONNECTION, 0, 0, HUB_C_OVER_CURRENT, HUB_C_RESET,
                                      HUB_C_BH_RESET, HUB_C_LINK_STATE, HUB_C_CONFIG_ERROR };
    const uint8_t *tab = hub->ss_hub ? usb3 : usb2;
    for (unsigned i = 0; i < 16; i++)
        if ((chg & (1u << i)) && tab[i])
            hub_feature(hub, false, tab[i], port);
    if (chg & (1u << 3))
        drv_log("usb %s: port %u over-current", hub->path, port);

    struct usbdev *c = child_at(dev_index(hub), port);
    bool connected = st & 1, enabled = st & 2;
    if (!connected) {
        hub->port_fail[port] = 0;
        if (c)
            detach(c, "unplugged", false);
        return;
    }
    if (c && ((chg & 1) || !enabled)) {
        detach(c, chg & 1 ? "replugged" : "port disabled", false);
        c = NULL;
    }
    if (c || hub->port_fail[port] >= 3 || h->stopping)
        return;

    /* A new device: debounce, reset, speed. */
    hc_sleep(h, 100);
    if (hub_port_status(hub, port, &st, &chg) != CC_SUCCESS || !(st & 1))
        return;
    /* A SuperSpeed port whose link is stuck (SS.Inactive, Compliance)
     * needs a warm reset (BH_PORT_RESET); the rest a (hot) PORT_RESET. */
    uint32_t link = (st >> 5) & 0xf;
    bool warm = hub->ss_hub && (link == PLS_INACTIVE || link == PLS_COMPLIANCE);
    cc = hub_feature(hub, true, warm ? HUB_BH_PORT_RESET : HUB_PORT_RESET, port);
    if (cc != CC_SUCCESS) {
        hub->port_fail[port]++;
        drv_log("usb %s: port %u: SET_FEATURE(%s): %s", hub->path, port,
                warm ? "BH_PORT_RESET" : "PORT_RESET", cc_str(cc));
        return;
    }
    uint64_t end = drv_clock_ns() + 800 * MS;
    bool done = false;
    uint16_t reset_chg = warm ? (1u << 5) | (1u << 4) : (1u << 4);
    while (drv_clock_ns() < end && !hub->gone && !h->stopping) {
        hc_sleep(h, 10);
        if (hub_port_status(hub, port, &st, &chg) != CC_SUCCESS)
            continue;
        if ((chg & reset_chg) && !(st & (1u << 4))) {
            done = true;
            break;
        }
    }
    if (!done) {
        hub->port_fail[port]++;
        drv_report("usb %s.%u: FAILED at hub port reset: no reset change in 800 ms (status %04x "
                   "change %04x)", hub->path, port, st, chg);
        g_failed++;
        return;
    }
    hub_feature(hub, false, HUB_C_RESET, port);
    if (hub->ss_hub && (chg & (1u << 5)))
        hub_feature(hub, false, HUB_C_BH_RESET, port);
    if (chg & 1)
        hub_feature(hub, false, HUB_C_CONNECTION, port);
    if (!(st & 2)) {
        hub->port_fail[port]++;
        drv_report("usb %s.%u: FAILED: port not enabled after reset (status %04x)", hub->path,
                   port, st);
        g_failed++;
        return;
    }
    uint8_t speed;
    if (hub->ss_hub)
        speed = SPEED_SUPER;
    else if (st & (1u << 9))
        speed = SPEED_LOW;
    else if (st & (1u << 10))
        speed = SPEED_HIGH;
    else
        speed = SPEED_FULL;
    hc_sleep(h, 10);
    if (!enumerate(dev_index(hub), port, speed))
        hub->port_fail[port]++;
}

/* One unit of a hub's work: its own status change, or one port. The
 * main loop serves channels between units, so a class driver's request
 * never waits behind a whole hub's worth of enumerations. */
static void hub_work(struct usbdev *hub)
{
    if (hub->hub_scan_all) {
        hub->hub_scan_all = false;
        for (uint8_t p = 1; p <= hub->hub_ports; p++)
            hub->hub_change[p / 32] |= 1u << (p % 32);
    }
    if (hub->hub_change[0] & 1) {
        hub->hub_change[0] &= ~1u;
        uint8_t b[4];
        uint32_t n = 0;
        if (usb_control(hub, 0xa0, 0, 0, 0, 4, b, &n, 1000) == CC_SUCCESS && n == 4) {
            uint16_t chg = le16(b + 2);
            if (chg & 1)
                usb_control(hub, 0x20, 1, 0, 0, 0, NULL, &n, 1000);   /* C_HUB_LOCAL_POWER */
            if (chg & 2) {
                drv_log("usb %s: hub over-current", hub->path);
                usb_control(hub, 0x20, 1, 1, 0, 0, NULL, &n, 1000);   /* C_HUB_OVER_CURRENT */
            }
        }
        return;
    }
    for (unsigned p = 1; p < 32 * 8; p++)
        if (hub->hub_change[p / 32] & (1u << (p % 32))) {
            hub->hub_change[p / 32] &= ~(1u << (p % 32));
            if (p <= hub->hub_ports)
                hub_port(hub, (uint8_t)p);
            return;
        }
}

/* ---- root ports ------------------------------------------------------------------- */

static bool root_reset(struct hc *h, uint32_t p, uint32_t *v)
{
    if (!hc_port_is_usb3(h, p)) {
        hc_portsc_write(h, p, PS_PR);
        uint64_t end = drv_clock_ns() + 500 * MS;
        while (drv_clock_ns() < end && !h->stopping) {
            hc_sleep(h, 2);
            *v = hc_portsc(h, p);
            if ((*v & PS_PRC) && !(*v & PS_PR))
                break;
        }
        *v = hc_portsc(h, p);
        hc_portsc_write(h, p, *v & (PS_PRC | PS_PEC | PS_CSC));
        if (!(*v & PS_PRC)) {
            drv_report("usb %u: FAILED at port reset: no PRC 500 ms after PR (PORTSC %08x)", p, *v);
            return false;
        }
        return true;
    }
    /* USB 3: the link trains by itself; a port stuck in Inactive or
     * Compliance (or not enabled after 1 s) gets a warm reset. */
    uint64_t end = drv_clock_ns() + 1000 * MS;
    for (;;) {
        *v = hc_portsc(h, p);
        if ((*v & PS_PED) && PS_PLS(*v) == PLS_U0)
            return true;
        if (!(*v & PS_CCS))
            return false;
        uint32_t pls = PS_PLS(*v);
        if (pls == PLS_INACTIVE || pls == PLS_COMPLIANCE || drv_clock_ns() > end || h->stopping)
            break;
        hc_sleep(h, 10);
    }
    drv_log("usb %u: USB 3 port not in U0 (PORTSC %08x, link state %u): warm reset", p, *v,
            PS_PLS(*v));
    hc_portsc_write(h, p, PS_WPR);
    end = drv_clock_ns() + 1000 * MS;
    while (drv_clock_ns() < end && !h->stopping) {
        hc_sleep(h, 10);
        *v = hc_portsc(h, p);
        if ((*v & (PS_WRC | PS_PRC)) && (*v & PS_PED))
            break;
    }
    *v = hc_portsc(h, p);
    hc_portsc_write(h, p, *v & (PS_WRC | PS_PRC | PS_PEC | PS_CSC | PS_PLC));
    if (!(*v & PS_PED)) {
        drv_report("usb %u: FAILED: USB 3 port not enabled after a warm reset (PORTSC %08x)", p, *v);
        return false;
    }
    return true;
}

static void root_port(struct hc *h, uint32_t p)
{
    uint32_t v = hc_portsc(h, p);
    if (v == 0xffffffffu)
        return;
    if (v & PS_CHANGES)
        hc_portsc_write(h, p, v & PS_CHANGES);
    struct usbdev *d = child_at(-1, (uint8_t)p);
    if (!(v & PS_CCS)) {
        root_fail[p] = 0;
        if (d)
            detach(d, "unplugged", false);
        return;
    }
    if (d && ((v & PS_CSC) || !(v & PS_PED))) {
        detach(d, v & PS_CSC ? "replugged" : "port disabled", false);
        d = NULL;
    }
    if (d || root_fail[p] >= 3 || h->stopping)
        return;
    bool usb3 = hc_port_is_usb3(h, p);
    if (!usb3) {
        hc_sleep(h, 100);   /* debounce */
        v = hc_portsc(h, p);
        if (!(v & PS_CCS))
            return;
    }
    if (!root_reset(h, p, &v)) {
        root_fail[p]++;
        g_failed++;
        return;
    }
    if (!usb3)
        hc_sleep(h, 10);   /* reset recovery */
    v = hc_portsc(h, p);
    if (!(v & PS_PED) || !(v & PS_CCS)) {
        root_fail[p]++;
        return;
    }
    uint8_t speed = (uint8_t)PS_SPEED(v);
    if (!speed || speed > SPEED_SUPERPLUS) {
        drv_report("usb %u: unknown port speed ID %u (PORTSC %08x): not enumerated", p, speed, v);
        root_fail[p]++;
        g_failed++;
        return;
    }
    if (!enumerate(-1, (uint8_t)p, speed))
        root_fail[p]++;
}

void usb_start(struct hc *h)
{
    for (uint32_t p = 1; p <= h->ports && p < 256; p++)
        h->port_changed[p / 32] |= 1u << (p % 32);
    g_last_change_ns = drv_clock_ns();
    started = true;
}

bool usb_busy(void)
{
    for (int i = 0; i < 8; i++)
        if (g_hc.port_changed[i])
            return true;
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || !d->is_hub)
            continue;
        if (d->hub_scan_all)
            return true;
        for (int k = 0; k < 8; k++)
            if (d->hub_change[k])
                return true;
    }
    return false;
}

bool usb_work(struct hc *h)
{
    bool did = false;
    if (!started || h->dead)
        return false;
    /* Endpoint upkeep first: cheap, and keeps input flowing. */
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || d->gone)
            continue;
        for (int k = 2; k < 32 && (d->ep_recover | d->ep_drop); k++) {
            struct ep *e = &d->eps[k];
            if (d->ep_drop & (1u << k)) {
                d->ep_drop &= ~(1u << k);
                ep_close(d, e);
                did = true;
            } else if (d->ep_recover & (1u << k)) {
                d->ep_recover &= ~(1u << k);
                if (!e->open)
                    continue;
                if (e->errors_in_row > 20) {
                    drv_log("usb %s: interrupt ep %02x: %u errors in a row; stopped polling it",
                            d->path, e->addr, e->errors_in_row);
                    ep_close(d, e);
                } else {
                    /* A STALL is the device's halt: clear it there too
                     * (both toggles back to DATA0). Other errors: a soft
                     * retry that keeps the toggle. */
                    bool stall = e->last_cc == CC_STALL;
                    ep_reset_tsp(d, e->dci, &e->ring, !stall);
                    if (stall) {
                        uint32_t n = 0;
                        usb_control(d, 0x02, 1, 0, e->addr, 0, NULL, &n, 1000);
                    }
                    e->halted = false;
                    if (e->errors_in_row > 3)
                        hc_sleep(h, 10);
                    intr_fill(d, e);
                }
                did = true;
            }
        }
    }
    /* Then ONE port (a root port, or a hub's): enumerating takes a while,
     * and the main loop serves requests between ports. */
    for (uint32_t p = 1; p <= h->ports && p < 256 && !h->stopping; p++)
        if (h->port_changed[p / 32] & (1u << (p % 32))) {
            h->port_changed[p / 32] &= ~(1u << (p % 32));
            root_port(h, p);
            return true;
        }
    for (int i = 0; i < MAX_DEVS && !h->stopping; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || !d->is_hub || d->gone)
            continue;
        bool any = d->hub_scan_all;
        for (int k = 0; k < 8; k++)
            any |= d->hub_change[k] != 0;
        if (any) {
            hub_work(d);
            return true;
        }
    }
    return did;
}

/* Shutdown: every endpoint stopped, every slot disabled (the controller
 * is reset right after, but this keeps the DMA story simple: nothing is
 * queued when the halt comes). */
void usb_stop_all(struct hc *h)
{
    (void)h;
    for (int i = 0; g_devs && i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].parent < 0)
            detach(&g_devs[i], "driver stopping", true);
}
