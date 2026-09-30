/* usb-bus: bulk endpoints (mass storage's data path).
 *
 * A class driver opens its interface's bulk-IN / bulk-OUT pair once
 * (usb.open_bulk). usb-bus then makes the BULK_SIZE buffer, pins it with
 * its own dma_cap and hands the driver a VMO handle to map; the two
 * endpoints are added to the controller with a ring each. Data only ever
 * moves between the device and that buffer: a transfer names an offset
 * and a length, never bytes in a message, and usb-bus itself never maps
 * the buffer.
 *
 * One bulk transfer runs at a time, controller-wide (g_hc.bulk), inside
 * the request that asked for it, like a control transfer: the TD is one
 * Normal TRB per buffer page it touches (the pages are not contiguous for
 * the device), chained, with the interrupt on the last; then a bounded
 * wait that keeps servicing the controller. bulk_event matches the
 * transfer's events. A short packet ends an IN transfer early: its event
 * names the TRB it happened in (Interrupt on Short Packet), and the
 * controller skips the rest of the TD.
 *
 * What ends a transfer:
 *   - done, or short: OK and the bytes moved;
 *   - a STALL or another error halts the endpoint: it is reset here (the
 *     controller's side only), ERR_IO. The device's own halt is cleared
 *     by usb.clear_halt;
 *   - no completion in time: Stop Endpoint, the TD is removed,
 *     ERR_TIMED_OUT;
 *   - the device left (its port, or a hub above it, says so; QEMU drops
 *     the transfers of an unplugged device without an event), or usb-bus
 *     is being stopped: ERR_PEER_CLOSED, without waiting for the timeout.
 *
 * clear_halt is CLEAR_FEATURE(ENDPOINT_HALT) at the device, which also
 * puts its data toggle (SuperSpeed: sequence number) back to 0, and the
 * same for the controller's side: a halted endpoint is reset; then the
 * endpoint is dropped and added again with an empty ring, which is how a
 * running endpoint's toggle is reset (a Reset Endpoint command only works
 * on a halted one; Linux's xhci_endpoint_reset does the same).
 *
 * The pair belongs to the interface channel that opened it. It is
 * released (endpoints dropped, buffer unpinned) when that channel closes,
 * on set_interface, and when the device goes. A buffer is unpinned only
 * once the controller can't run a transfer into it any more; until then
 * it is parked, still pinned. The same channel opening it
 * again gets a fresh pair: devmgr restarts a class driver that died with
 * a duplicate of the old channel, which therefore never closes, and the
 * new driver can't have the dead one's buffer. Another channel of the
 * interface is refused (ERR_BAD_STATE) while the pair is open. */
#include "usbbus.h"

#define EP_STATE_HALTED 2   /* xHCI 6.2.3: the endpoint context's EP State */
#define MAX_PARKED      16  /* buffers kept pinned until the controller is reset */

/* Buffers whose endpoints the controller may still run (the device left
 * and its slot couldn't be disabled, or usb-bus is stopping): they stay
 * pinned until hc_shutdown has reset the controller (bulk_unpin_parked). */
static struct {
    handle_t vmo;   /* our handle */
    uint64_t pin;   /* its pin id */
} parked[MAX_PARKED];
static unsigned nparked;

/* f's endpoint `addr` if it is a bulk endpoint of the given type. */
static struct ep *bulk_ep(struct usbdev *d, const struct iface *f, uint8_t addr, uint8_t type)
{
    struct ep *e = &d->eps[ep_dci(addr)];
    bool mine = false;
    for (int i = 0; i < f->nep; i++)
        mine |= f->ep_addr[i] == addr;
    return mine && e->dci == ep_dci(addr) && e->type == type && e->mps ? e : NULL;
}

/* ---- the endpoints in the controller ------------------------------------------- */

/* Start e afresh on the controller's side, toggle and ring: whatever
 * state it is in, it ends stopped, and a drop and add in one Configure
 * Endpoint gives it back running with an empty ring. */
static uint32_t ep_restart(struct usbdev *d, struct ep *e)
{
    if (!e->configured)
        return CC_SUCCESS;
    if ((out_ctx(d, e->dci)[0] & 7) == EP_STATE_HALTED)
        ep_reset_tsp(d, e->dci, &e->ring, false);
    else
        ep_stop(d, e->dci, &e->ring);
    ring_reset(&e->ring);
    uint32_t cc = configure_eps(d, 1u << e->dci, 1u << e->dci);
    if (cc != CC_SUCCESS) {
        if (!d->gone)
            drv_log("usb %s: ep %02x: re-adding it: %s", d->path, e->addr, cc_str(cc));
        /* Still the old endpoint: point it at the emptied ring's start. */
        ep_stop(d, e->dci, &e->ring);
    }
    return cc;
}

/* Both endpoints running with empty rings: added, or (left configured by
 * a release that couldn't drop them) restarted. */
static uint32_t eps_start(struct usbdev *d, struct ep *in, struct ep *out)
{
    uint32_t add = 0, cc = CC_SUCCESS;
    struct ep *es[2] = { in, out };
    for (int i = 0; i < 2 && cc == CC_SUCCESS; i++) {
        if (es[i]->configured)
            cc = ep_restart(d, es[i]);
        else
            add |= 1u << es[i]->dci;
    }
    if (cc == CC_SUCCESS && add)
        cc = configure_eps(d, add, 0);
    return cc;
}

/* ---- open and release ------------------------------------------------------------ */

/* The buffer: a DMA32 VMO pinned with our dma_cap, never mapped here. */
static status_t buffer_make(struct bulk *b)
{
    status_t st = drv_vmo_create(BULK_SIZE, DRV_VMO_DMA32, &b->vmo);
    if (st != OK)
        return st;
    st = drv_vmo_pin(b->vmo, g_hc.dma, 0, BULK_SIZE, b->addr, &b->pin);
    if (st != OK) {
        drv_handle_close(b->vmo);
        return st;
    }
    b->pinned = true;
    return OK;
}

void bulk_release(struct usbdev *d, struct iface *f, bool slot_off)
{
    struct bulk *b = f->bulk;
    if (!b)
        return;
    /* May the buffer be unpinned? Only once the controller can't run a
     * transfer into it: the slot is off, or its endpoints are dropped. */
    bool quiet = slot_off;
    if (!slot_off && !d->gone) {
        uint32_t drop = 0;
        if (d->eps[ep_dci(b->in)].configured)
            drop |= 1u << ep_dci(b->in);
        if (d->eps[ep_dci(b->out)].configured)
            drop |= 1u << ep_dci(b->out);
        uint32_t cc = drop ? configure_eps(d, 0, drop) : CC_SUCCESS;
        quiet = cc == CC_SUCCESS;
        if (!quiet)
            drv_log("usb %s: if%u: dropping its bulk endpoints: %s; the buffer stays pinned",
                    d->path, f->number, cc_str(cc));
    }
    if (b->pinned && !quiet && nparked < MAX_PARKED) {
        parked[nparked].vmo = b->vmo;
        parked[nparked++].pin = b->pin;
    } else {
        /* Not checked: a pin left is quarantined when our dma_cap closes
         * (so is one that found the parking full). */
        if (b->pinned && quiet)
            (void)drv_vmo_unpin(b->vmo, g_hc.dma, b->pin);
        drv_handle_close(b->vmo);
    }
    drv_free(b);
    f->bulk = NULL;
}

void bulk_unpin_parked(void)
{
    for (unsigned i = 0; i < nparked; i++) {
        (void)drv_vmo_unpin(parked[i].vmo, g_hc.dma, parked[i].pin);   /* as above */
        drv_handle_close(parked[i].vmo);
    }
    nparked = 0;
}

void bulk_chan_closed(struct usbdev *d, struct iface *f, int chan)
{
    if (f->bulk && f->bulk->chan == chan)
        bulk_release(d, f, false);
}

status_t bulk_open(struct usbdev *d, struct iface *f, int chan, uint8_t ep_in, uint8_t ep_out,
                   handle_t *buffer, uint32_t *size)
{
    struct ep *in = (ep_in & 0x80) ? bulk_ep(d, f, ep_in, EPT_BULK_IN) : NULL;
    struct ep *out = (ep_out & 0x80) ? NULL : bulk_ep(d, f, ep_out, EPT_BULK_OUT);
    if (!in || !out)
        return ERR_INVALID_ARGS;
    if (f->bulk) {
        if (f->bulk->chan != chan)
            return ERR_BAD_STATE;
        bulk_release(d, f, false);   /* a restarted class driver, on the old channel */
    }
    struct bulk *b = drv_malloc(sizeof(*b));
    if (!b)
        return ERR_NO_MEMORY;
    zero(b, sizeof(*b));
    status_t st = buffer_make(b);
    if (st != OK) {
        drv_free(b);
        return st;
    }
    b->in = ep_in;
    b->out = ep_out;
    b->chan = chan;
    f->bulk = b;
    uint32_t cc = eps_start(d, in, out);
    handle_t h = HANDLE_INVALID;
    st = cc == CC_SUCCESS ? OK : cc_status(cc);
    /* The driver's handle: to map, read and write, and no more. */
    if (st == OK)
        st = drv_handle_duplicate(b->vmo, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER,
                                  &h);
    if (st != OK) {
        bulk_release(d, f, false);
        return st;
    }
    *buffer = h;
    *size = BULK_SIZE;
    return OK;
}

/* ---- a transfer ------------------------------------------------------------------ */

/* Queue the TD: one Normal TRB per page of [offset, offset + length). TD
 * Size (xHCI 4.11.2.4) is the packets still to come after each TRB, at
 * most 31. */
static void td_queue(struct hc *h, struct ep *e, const struct bulk *b, bool in, uint32_t offset,
                     uint32_t length)
{
    h->bulk.first = e->ring.enq;
    h->bulk.ntrb = 0;
    uint32_t off = offset, left = length;
    while (left && h->bulk.ntrb < BULK_TRBS) {
        uint32_t n = PAGE - off % PAGE;
        if (n > left)
            n = left;
        left -= n;
        uint64_t a = b->addr[off / PAGE] + off % PAGE;
        uint32_t packets = (left + e->mps - 1) / e->mps;
        if (packets > 31)
            packets = 31;
        ring_push(&e->ring, lo32(a), hi32(a), n | packets << 17,
                  TRB_TYPE(TRB_NORMAL) | (left ? TRB_CH : TRB_IOC) | (in ? TRB_ISP : 0));
        h->bulk.len[h->bulk.ntrb++] = n;
        off += n;
    }
}

void bulk_event(struct hc *h, uint64_t trb, uint32_t cc, uint32_t residual)
{
    struct usbdev *d = dev_by_slot(h->bulk.slot);
    if (h->bulk.done || !trb || !d)
        return;
    /* Which TRB of the TD: ring indices wrap past the Link TRB. */
    uint32_t idx = ring_index(&d->eps[h->bulk.dci].ring, trb);
    if (idx >= RING_TRBS - 1)
        return;
    uint32_t k = (idx + (RING_TRBS - 1) - h->bulk.first) % (RING_TRBS - 1);
    if (k >= h->bulk.ntrb)
        return;   /* a leftover of an earlier transfer */
    if (cc == CC_SUCCESS && k != h->bulk.ntrb - 1)
        return;   /* not the end yet */
    uint32_t moved = 0;
    for (uint32_t i = 0; i < k; i++)
        moved += h->bulk.len[i];
    moved += h->bulk.len[k] - (residual < h->bulk.len[k] ? residual : h->bulk.len[k]);
    h->bulk.actual = moved;
    h->bulk.cc = cc;
    h->bulk.done = true;
}

/* Has d left the bus? Its root port, or a hub on the way up, reports a
 * change that the main loop hasn't looked at yet: ask it. Looked at no
 * more than every 100 ms (*next: when again). */
static bool branch_lost(struct usbdev *d, uint64_t *next)
{
    struct hc *h = &g_hc;
    uint64_t now = drv_clock_ns();
    if (now < *next)
        return false;
    *next = now + 100 * NS_PER_MS;
    for (int guard = 0; guard <= MAX_LEVEL && d->parent >= 0; guard++) {
        struct usbdev *hub = &g_devs[d->parent];
        if (hub->gone)
            return true;
        if ((hub->hub_change[d->port / 32] & (1u << (d->port % 32))) &&
            hub_port_lost(hub, d->port))
            return true;
        d = hub;
    }
    uint32_t p = d->port;
    if (d->parent >= 0 || !(h->port_changed[p / 32] & (1u << (p % 32))))
        return false;
    uint32_t v = hc_portsc(h, p);
    return v == 0xffffffff || !(v & PS_CCS) || (v & PS_CSC);
}

/* Wait for the queued TD; its completion code (CC_TIMEOUT, CC_GONE: ours). */
static uint32_t td_wait(struct hc *h, struct usbdev *d, uint32_t timeout_ms)
{
    uint64_t deadline = drv_clock_ns() + (uint64_t)timeout_ms * NS_PER_MS, look = 0;
    bool lost = false;
    while (!h->bulk.done && !lost && !d->gone && !h->dead && !h->stopping &&
           drv_clock_ns() < deadline) {
        hc_wait(h, deadline);
        lost = !h->bulk.done && branch_lost(d, &look);
    }
    /* Not busy from here: the events of a TD that Stop Endpoint cuts
     * short must not count as a result. */
    h->bulk.busy = false;
    if (h->bulk.done)
        return h->bulk.cc;
    return lost || d->gone || h->dead || h->stopping ? CC_GONE : CC_TIMEOUT;
}

static status_t td_status(const struct usbdev *d, const struct ep *e, uint32_t cc)
{
    switch (cc) {
    case CC_SUCCESS: case CC_SHORT_PACKET: return OK;
    case CC_TIMEOUT: return ERR_TIMED_OUT;
    case CC_GONE: return ERR_PEER_CLOSED;
    case CC_STALL: return ERR_IO;   /* the device's answer: not logged */
    default:
        drv_log("usb %s: bulk ep %02x: %s", d->path, e->addr, cc_str(cc));
        return ERR_IO;
    }
}

status_t bulk_transfer(struct usbdev *d, struct iface *f, int chan, bool in, uint32_t offset,
                       uint32_t length, uint32_t timeout_ms, uint32_t *actual)
{
    struct hc *h = &g_hc;
    const struct bulk *b = f->bulk;
    if (!b || b->chan != chan)
        return ERR_BAD_STATE;
    if (!length || !timeout_ms || timeout_ms > 60000)
        return ERR_INVALID_ARGS;
    if (offset > BULK_SIZE || length > BULK_SIZE - offset)
        return ERR_OUT_OF_RANGE;
    struct ep *e = &d->eps[ep_dci(in ? b->in : b->out)];
    if (!e->configured)
        return ERR_BAD_STATE;
    if (h->dead)
        return ERR_PEER_CLOSED;
    h->bulk.busy = true;
    h->bulk.done = false;
    h->bulk.slot = d->slot;
    h->bulk.dci = e->dci;
    h->bulk.cc = 0;
    h->bulk.actual = 0;
    td_queue(h, e, b, in, offset, length);
    hc_doorbell(h, d->slot, e->dci);
    uint32_t cc = td_wait(h, d, timeout_ms);
    if (cc == CC_TIMEOUT || cc == CC_GONE) {
        if (!h->dead)
            ep_stop(d, e->dci, &e->ring);   /* the TD comes off the ring */
        e->errors++;
    } else if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        ep_reset_tsp(d, e->dci, &e->ring, false);   /* halted: STALL, transaction error */
        e->errors++;
    } else {
        e->reports++;
    }
    status_t st = td_status(d, e, cc);
    if (st == OK)
        *actual = h->bulk.actual;
    return st;
}

status_t bulk_clear_halt(struct usbdev *d, struct iface *f, int chan, uint8_t endpoint)
{
    const struct bulk *b = f->bulk;
    if (!b || b->chan != chan)
        return ERR_BAD_STATE;
    if (endpoint != b->in && endpoint != b->out)
        return ERR_INVALID_ARGS;
    uint32_t n = 0;
    /* CLEAR_FEATURE(ENDPOINT_HALT), recipient endpoint (USB 2.0 9.4.1). */
    uint32_t cc = usb_control(d, 0x02, 1, 0, endpoint, 0, NULL, &n, 1000);
    uint32_t host = ep_restart(d, &d->eps[ep_dci(endpoint)]);
    return cc_status(cc != CC_SUCCESS ? cc : host);
}
