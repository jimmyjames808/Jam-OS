/* usb-bus: interrupt-IN endpoints (a hub's status-change endpoint and the
 * ones clients open for reports).
 *
 * An open endpoint keeps INTR_TRBS Normal TRBs queued, one per slice of
 * its buffer page, and remembers which ring index uses which slice. A
 * completed transfer goes to its owner (the hub's change bitmap, or the
 * client's report channel) and is queued again at once. An error halts
 * the endpoint: the event only marks it in d->ep_recover, and the main
 * loop resets it (intr_upkeep), because the event arrives inside some
 * other wait and a command can't be run from there. A client whose
 * report channel went away is marked in d->ep_drop and closed the same
 * way. */
#include "usbbus.h"

#define INTR_STRIDE (PAGE / INTR_TRBS)

static uint32_t intr_len(const struct ep *e)
{
    uint32_t n = e->esit ? e->esit : e->mps;
    if (!n)
        n = 8;
    return n > INTR_STRIDE ? INTR_STRIDE : n;
}

static void intr_queue(const struct usbdev *d, struct ep *e, uint8_t slot)
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
        if (d && d->ctl.busy)
            ctl_event(d, trb, cc, residual);
        return;
    }
    if (!d || dci >= 32 || !d->eps[dci].dci)
        return;
    if (h->bulk.busy && h->bulk.slot == slot && h->bulk.dci == dci) {
        bulk_event(h, trb, cc, residual);
        return;
    }
    intr_event(d, &d->eps[dci], trb, cc, residual);
}

/* ---- upkeep (from the main loop) -------------------------------------------- */

/* A halted endpoint that is still open: reset and refill it, or give up
 * on it after too many errors in a row. */
static void ep_recover(struct hc *h, struct usbdev *d, struct ep *e)
{
    if (e->errors_in_row > 20) {
        drv_log("usb %s: interrupt ep %02x: %u errors in a row; stopped polling it",
                d->path, e->addr, e->errors_in_row);
        ep_close(d, e);
        return;
    }
    /* A STALL is the device's halt: clear it there too (both toggles back
     * to DATA0). Other errors: a soft retry that keeps the toggle. */
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

/* d's endpoints marked for a drop or a recovery; true if any was acted on. */
static bool dev_upkeep(struct hc *h, struct usbdev *d)
{
    bool did = false;
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
            ep_recover(h, d, e);
            did = true;
        }
    }
    return did;
}

bool intr_upkeep(struct hc *h)
{
    bool did = false;
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usbdev *d = &g_devs[i];
        if (!d->used || d->gone)
            continue;
        if (dev_upkeep(h, d))
            did = true;
    }
    return did;
}
