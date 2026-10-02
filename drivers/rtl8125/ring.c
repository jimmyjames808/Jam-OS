/* rtl8125: the driver's DMA memory (drv/rtl8125).
 *
 * One contiguous VMO holds the receive ring (256 descriptors of 32
 * bytes), the transmit ring (256 of 32 bytes: tx.c fills it) and the
 * tally dump. The 2 KiB receive buffers (two per page) are an ordinary
 * VMO, pinned page by page; in full mode the transmit buffers are another
 * one of the same shape: the driver's own copy of every frame it sends,
 * which nothing outside the driver can see or write. Nothing asks for
 * memory below 4 GiB: the chip does 64-bit DMA (the PC's stage-0 run).
 *
 * The receive side's descriptors are walked here for both modes:
 * ring_rx_peek says what the chip handed back, ring_rx_done gives it back.
 * The length comes from the descriptor; the bytes stay in the buffer for
 * the caller to read what it may (netframe.h). */
#include "rtl8125.h"

/* Make, map and pin `bytes` of DMA memory; one device address per page. */
static status_t dma_vmo(struct rtl *t, uint64_t bytes, uint32_t flags, handle_t *vmo,
                        uint8_t **map, uint64_t *addrs, uint64_t *pin)
{
    status_t st = drv_vmo_create(bytes, flags, vmo);
    if (st != OK)
        return st;
    st = drv_vmo_map(*vmo, 0, bytes, VMAR_READ | VMAR_WRITE, (void **)map);
    if (st == OK)
        st = drv_vmo_pin(*vmo, t->dma, 0, bytes, addrs, pin);
    return st;   /* ring_free undoes what was done, whatever failed */
}

/* Receive buffer i's device address (two buffers per page). */
static uint64_t rx_buf_addr(const struct rtl *t, unsigned i)
{
    return t->buf_addr[i / 2] + (i % 2) * RX_BUF;
}

/* Descriptor i to the chip, whole: its buffer's address too, every time
 * (rxdesc.h: the chip's write-back may have put a timestamp there). */
static void rx_arm(struct rtl *t, unsigned i)
{
    rtl_rxd_arm(t->ring + i * RX_DESC_SIZE, rx_buf_addr(t, i), i, RX_DESCS, RX_BUF);
}

static void log_where(const struct rtl *t)
{
    uint64_t lo = t->buf_addr[0], hi = t->buf_addr[0];
    for (unsigned i = 1; i < BUF_PAGES; i++) {
        lo = t->buf_addr[i] < lo ? t->buf_addr[i] : lo;
        hi = t->buf_addr[i] > hi ? t->buf_addr[i] : hi;
    }
    drv_log("ring: %u receive descriptors at %#lx, %u transmit at %#lx, tally at %#lx, "
            "receive buffers %#lx-%#lx: %s", RX_DESCS, (unsigned long)t->ring_addr,
            t->txbufs ? TX_DESCS : 0, (unsigned long)(t->ring_addr + TX_RING_OFF),
            (unsigned long)(t->ring_addr + TALLY_OFF), (unsigned long)lo,
            (unsigned long)(hi + 4095),
            t->ring_addr >= (1ull << 32) && lo >= (1ull << 32) ? "all above 4 GiB (64-bit DMA)"
            : t->ring_addr >= (1ull << 32) || hi >= (1ull << 32) ? "partly above 4 GiB"
            : "all below 4 GiB");
}

status_t ring_setup(struct rtl *t)
{
    uint64_t addrs[RING_VMO / 4096];
    status_t st = dma_vmo(t, RING_VMO, DRV_VMO_CONTIGUOUS, &t->ring_vmo, &t->ring, addrs,
                          &t->ring_pin);
    t->ring_pinned = st == OK;
    if (st == OK) {
        st = dma_vmo(t, BUF_BYTES, 0, &t->buf_vmo, &t->bufs, t->buf_addr, &t->buf_pin);
        t->buf_pinned = st == OK;
    }
    if (st == OK && t->mode == RTL_MODE_FULL) {
        st = dma_vmo(t, TXBUF_BYTES, 0, &t->txbuf_vmo, &t->txbufs, t->txbuf_addr,
                     &t->txbuf_pin);
        t->txbuf_pinned = st == OK;
    }
    if (st != OK) {
        drv_log("ring: can't set it up (%s)", status_str(st));
        return st;
    }
    t->ring_addr = addrs[0];
    for (unsigned i = 0; i < RX_DESCS; i++)
        rx_arm(t, i);
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the chip reads them once RXENB is set */
    log_where(t);
    return OK;
}

/* Unpin, unmap and close one of dma_vmo's results, as far as it got. */
static void dma_free(struct rtl *t, handle_t *vmo, uint8_t **map, uint64_t bytes, bool *pinned,
                     uint64_t pin)
{
    if (*pinned && drv_vmo_unpin(*vmo, t->dma, pin) == OK)
        *pinned = false;
    if (*map)
        (void)drv_vmo_unmap(*map, bytes);   /* nothing to do if it fails: we exit */
    *map = NULL;
    if (*vmo)
        drv_handle_close(*vmo);
    *vmo = HANDLE_INVALID;
}

void ring_free(struct rtl *t)
{
    dma_free(t, &t->txbuf_vmo, &t->txbufs, TXBUF_BYTES, &t->txbuf_pinned, t->txbuf_pin);
    dma_free(t, &t->buf_vmo, &t->bufs, BUF_BYTES, &t->buf_pinned, t->buf_pin);
    dma_free(t, &t->ring_vmo, &t->ring, RING_VMO, &t->ring_pinned, t->ring_pin);
}

bool ring_rx_peek(struct rtl *t, struct rx_slot *out)
{
    unsigned i = t->next;
    volatile uint8_t *d = t->ring + i * RX_DESC_SIZE;
    uint32_t st = *(volatile uint32_t *)(d + RX_DESC_CMDSTS);
    if (st & RX_OWN)
        return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the buffer after the status */
    uint64_t addr = rtl_rxd_addr(d);
    if (addr != rx_buf_addr(t, i)) {   /* the chip wrote there (rxdesc.h): counted, not used */
        if (!t->rx.addr_changed)
            t->rx.addr_first = addr;
        t->rx.addr_changed++;
    }
    uint32_t len = st & RX_LEN;
    *out = (struct rx_slot){
        .status = st,
        .len = len >= RX_CRC ? len - RX_CRC : 0,
        .buf = t->bufs + (size_t)i * RX_BUF,
        .whole = !(st & RX_ERRSUM) && (st & (RX_SOF | RX_EOF)) == (RX_SOF | RX_EOF) &&
                 len <= RX_BUF,
    };
    return true;
}

unsigned ring_rx_owned(const struct rtl *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < RX_DESCS; i++)
        n += !!(*(const volatile uint32_t *)(t->ring + i * RX_DESC_SIZE + RX_DESC_CMDSTS) &
                RX_OWN);
    return n;
}

void ring_rx_done(struct rtl *t)
{
    rx_arm(t, t->next);
    t->next = (t->next + 1) % RX_DESCS;
}
