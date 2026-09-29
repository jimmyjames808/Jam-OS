/* usb-bus: the DMA page pool and the transfer rings.
 *
 * The pool is POOL_PAGES pages of one DMA32 VMO that hc.c pins and maps
 * at bring-up (pool_setup); device contexts, rings and transfer buffers
 * take a zeroed page each, and pool_used says which are out.
 *
 * A ring is one pool page of RING_TRBS TRBs whose last is a Link TRB back
 * to the first (Toggle Cycle set). ring_push fills a TRB and then flips
 * its cycle bit to the producer's cycle state: that last write hands it to
 * the controller, so the fences keep it after the other three words. */
#include "usbbus.h"

/* ---- the DMA page pool ---------------------------------------------------------- */

int pool_alloc(struct hc *h)
{
    for (int i = 0; i < POOL_PAGES; i++)
        if (!h->pool_used[i]) {
            h->pool_used[i] = 1;
            zero(h->pool + (uint64_t)i * PAGE, PAGE);
            if (++h->pool_inuse > h->pool_peak)
                h->pool_peak = h->pool_inuse;
            return i;
        }
    drv_log("DMA pool: all %u pages in use", POOL_PAGES);
    return -1;
}

void pool_free(struct hc *h, int page)
{
    if (page >= 0 && page < POOL_PAGES && h->pool_used[page]) {
        h->pool_used[page] = 0;
        h->pool_inuse--;
    }
}

void *pool_va(struct hc *h, int page)
{
    return h->pool + (uint64_t)page * PAGE;
}

uint64_t pool_dev(struct hc *h, int page)
{
    return h->pool_addr[page];
}

/* ---- rings ------------------------------------------------------------------------ */

bool ring_init(struct hc *h, struct ring *r)
{
    r->page = pool_alloc(h);
    if (r->page < 0)
        return false;
    r->t = pool_va(h, r->page);
    r->dev = pool_dev(h, r->page);
    r->t[RING_TRBS - 1].d0 = lo32(r->dev);
    r->t[RING_TRBS - 1].d1 = hi32(r->dev);
    r->t[RING_TRBS - 1].d3 = TRB_TYPE(TRB_LINK) | TRB_TC;
    r->enq = 0;
    r->cycle = 1;
    return true;
}

void ring_free(struct hc *h, struct ring *r)
{
    if (r->page >= 0)
        pool_free(h, r->page);
    r->page = -1;
    r->t = NULL;
}

uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    uint32_t i = r->enq;
    volatile struct trb *t = &r->t[i];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->d3 = (d3 & ~TRB_C) | r->cycle;   /* the cycle bit last: now the xHC's */
    uint64_t addr = r->dev + (uint64_t)i * sizeof(struct trb);
    if (++r->enq == RING_TRBS - 1) {
        volatile struct trb *l = &r->t[RING_TRBS - 1];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        l->d3 = (l->d3 & ~TRB_C) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return addr;
}

uint32_t ring_index(const struct ring *r, uint64_t trb_dev)
{
    if (!r->t || trb_dev < r->dev || trb_dev >= r->dev + PAGE || (trb_dev & 15))
        return RING_TRBS;
    return (uint32_t)((trb_dev - r->dev) / sizeof(struct trb));
}
