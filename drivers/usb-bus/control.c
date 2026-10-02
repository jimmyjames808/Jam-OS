/* usb-bus: control transfers on endpoint 0, endpoint recovery commands,
 * and descriptors.
 *
 * One control transfer runs at a time per device (d->ctl, with the
 * device's own bounce page; a task wanting EP0 while another task's
 * transfer or its recovery runs there waits its turn): Setup, optional
 * Data, Status, then a bounded wait that keeps servicing the controller. ctl_event matches the transfer
 * events to the stages. A transfer that never finishes stops its endpoint;
 * one that fails halts it and is reset here, so the next transfer starts
 * clean. A failed transfer to a full/low-speed device behind a high-speed
 * hub also clears the hub's TT buffer.
 *
 * The recovery commands (Reset Endpoint, Stop Endpoint, Set TR Dequeue
 * Pointer) are here because endpoint 0 needs them first; intr.c uses them
 * for the interrupt endpoints too. */
#include "usbbus.h"

/* ---- endpoint recovery ------------------------------------------------------ */

static void ep_set_deq(struct usbdev *d, uint8_t dci, const struct ring *r)
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
void ep_reset_tsp(struct usbdev *d, uint8_t dci, struct ring *r, bool tsp)
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
void ep_stop(struct usbdev *d, uint8_t dci, struct ring *r)
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

/* ---- control transfers ------------------------------------------------------ */

/* A control transfer to a full/low-speed device behind a high-speed hub
 * failed (not a STALL: that is the device's answer): the split transaction
 * may have left the hub's TT buffer busy, and the next transfer to that
 * endpoint could hang behind it. CLEAR_TT_BUFFER on the TT's hub (USB 2.0
 * 11.24.2.3; Linux does the same for control and bulk, never interrupt),
 * for both directions of the default endpoint. The hub is high speed, so
 * its own transfers never come back here. */
static void clear_tt_buffer(struct usbdev *d, uint32_t cc)
{
    struct usbdev *hub = dev_by_slot(d->tt_slot);
    if (!hub || hub->gone)
        return;
    dev_hold(hub);
    uint16_t tt = d->tt_mtt ? d->tt_port : 1;
    uint32_t r[2] = { CC_SUCCESS, CC_SUCCESS };
    for (int in = 0; in < 2; in++) {
        uint16_t info = (uint16_t)(0 | (uint16_t)(d->address & 0x7f) << 4 | 0u << 11 |
                                   (uint16_t)in << 15);   /* ep 0, control */
        uint32_t n = 0;
        r[in] = usb_control(hub, 0x23, 8, info, tt, 0, NULL, &n, 1000);
    }
    if (++d->tt_clears <= 4)
        drv_log("usb %s: control transfer failed (%s): CLEAR_TT_BUFFER on hub %s port %u: %s, %s",
                d->path, cc_str(cc), hub->path, tt, cc_str(r[0]), cc_str(r[1]));
    dev_put(hub);
}

/* Wait until no other task uses d's EP0, then take it. False if d went
 * or the driver is stopping meanwhile. */
static bool ep0_take(struct usbdev *d)
{
    struct hc *h = &g_hc;
    while (d->ctl.locked && !d->gone && !h->dead && !h->stopping && in_task())
        task_wait(g_tasks, drv_clock_ns() + 50 * NS_PER_MS);
    if (d->ctl.locked || d->gone || h->dead || h->stopping)
        return false;
    d->ctl.locked = true;
    return true;
}

static void ep0_give(struct usbdev *d)
{
    d->ctl.locked = false;
    task_kick(g_tasks);
}

/* One control transfer, with d's EP0 taken. */
static uint32_t control_locked(struct usbdev *d, uint8_t rt, uint8_t req, uint16_t value,
                               uint16_t index, uint16_t length, void *data, uint32_t *actual,
                               uint64_t timeout_ms)
{
    struct hc *h = &g_hc;
    bool in = rt & 0x80;
    uint8_t *buf = pool_va(h, d->ctl.page);
    uint64_t bdev = pool_dev(h, d->ctl.page);
    if (!in && length)
        copy(buf, data, length);
    d->ctl.busy = true;
    d->ctl.done = false;
    d->ctl.cc = 0;
    d->ctl.residual = 0;
    d->ctl.short_seen = false;
    d->ctl.data_trb = 0;
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    d->ctl.setup_trb = ring_push(&d->ep0, rt | (uint32_t)req << 8 | (uint32_t)value << 16,
                                 index | (uint32_t)length << 16, 8,
                                 TRB_TYPE(TRB_SETUP) | TRB_IDT | trt << 16);
    if (length)
        d->ctl.data_trb = ring_push(&d->ep0, (uint32_t)bdev, (uint32_t)(bdev >> 32), length,
                                    TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN | TRB_ISP : 0));
    d->ctl.status_trb = ring_push(&d->ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                                  ((in && length) ? 0 : TRB_DIR_IN));
    hc_doorbell(h, d->slot, 1);
    uint64_t deadline = drv_clock_ns() + timeout_ms * NS_PER_MS;
    while (!d->ctl.done && !d->gone && !h->dead && !h->stopping && drv_clock_ns() < deadline)
        hc_wait(h, deadline);
    /* Not busy from here: on a timeout, the events of the transfer that
     * Stop Endpoint cuts short must not count as a result. */
    d->ctl.busy = false;
    uint32_t cc;
    if (d->ctl.done) {
        cc = d->ctl.cc;
    } else {
        cc = d->gone || h->dead || h->stopping ? CC_GONE : CC_TIMEOUT;
        if (cc == CC_TIMEOUT)
            ep_stop(d, 1, &d->ep0);
    }
    if (cc == CC_SUCCESS) {
        uint32_t n = d->ctl.short_seen
                         ? length - (d->ctl.residual < length ? d->ctl.residual : length)
                         : length;
        if (in && n)
            copy(data, buf, n);
        if (actual)
            *actual = n;
    } else if (cc != CC_TIMEOUT && cc != CC_GONE) {
        /* A halted default endpoint: STALL, transaction error, babble. */
        ep_reset(d, 1, &d->ep0);
    }
    if (cc != CC_SUCCESS && cc != CC_STALL && cc != CC_GONE && d->tt_slot && !d->gone &&
        !h->dead && !h->stopping)
        clear_tt_buffer(d, cc);
    return cc;
}

uint32_t usb_control(struct usbdev *d, uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                     uint16_t length, void *data, uint32_t *actual, uint64_t timeout_ms)
{
    if (actual)
        *actual = 0;
    if (d->gone || !d->slot || d->ep0.page < 0 || d->ctl.page < 0 || g_hc.dead)
        return CC_GONE;
    if (length > PAGE)
        return CC_TRB;
    if (!ep0_take(d))
        return CC_GONE;
    uint32_t cc = control_locked(d, rt, req, value, index, length, data, actual, timeout_ms);
    ep0_give(d);
    return cc;
}

void ctl_event(struct usbdev *d, uint64_t trb, uint32_t cc, uint32_t residual)
{
    if (!d->ctl.busy || d->ctl.done)
        return;
    if (trb && trb == d->ctl.data_trb) {
        if (cc == CC_SHORT_PACKET) {
            d->ctl.short_seen = true;
            d->ctl.residual = residual;
            return;   /* the status stage follows */
        }
        if (cc == CC_SUCCESS)
            return;
    } else if (trb && trb == d->ctl.status_trb) {
        if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
            d->ctl.cc = CC_SUCCESS;
            d->ctl.done = true;
            return;
        }
    } else if (trb && trb != d->ctl.setup_trb && cc == CC_SUCCESS) {
        return;   /* a leftover of an earlier transfer */
    }
    d->ctl.cc = cc;
    d->ctl.done = true;
}

status_t cc_status(uint32_t cc)
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

/* ---- descriptors ------------------------------------------------------------ */

uint32_t get_desc(struct usbdev *d, uint8_t type, uint8_t index, uint16_t lang, void *buf,
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
void get_string(struct usbdev *d, uint8_t index, uint16_t lang, char *out, unsigned cap)
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
