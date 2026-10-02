/* e1000e: the descriptor rings and buffers, in the driver's own DMA
 * memory (drv/e1000e).
 *
 * Both rings (256 legacy descriptors of 16 bytes each, receive first) are
 * one contiguous VMO; the 2 KiB buffers (receive 0..255, then transmit
 * 0..255, two per page) are an ordinary VMO pinned page by page. Neither
 * asks for memory below 4 GiB (the 82574 does 64-bit DMA). Transmit
 * buffer i belongs to transmit descriptor i, so a buffer is free again
 * when its descriptor is reaped. None of it is ever shared with netstack:
 * the netdev rings are separate, ordinary memory (serve.c). */
#include "e1000e.h"

volatile uint8_t *ring_rx_desc(const struct e1k *t, uint32_t i)
{
    return t->ring + RX_RING_OFF + (size_t)(i % RX_DESCS) * DESC_SIZE;
}

volatile uint8_t *ring_tx_desc(const struct e1k *t, uint32_t i)
{
    return t->ring + TX_RING_OFF + (size_t)(i % TX_DESCS) * DESC_SIZE;
}

static uint64_t buf_addr(const struct e1k *t, uint32_t off)
{
    return t->buf_addr[off / 4096] + off % 4096;
}

uint64_t ring_rx_buf_addr(const struct e1k *t, uint32_t i)
{
    return buf_addr(t, RX_BUF_OFF + (i % RX_DESCS) * BUF_SIZE);
}

uint64_t ring_tx_buf_addr(const struct e1k *t, uint32_t i)
{
    return buf_addr(t, TX_BUF_OFF + (i % TX_DESCS) * BUF_SIZE);
}

static status_t ring_alloc(struct e1k *t)
{
    uint64_t addrs[RINGS_BYTES / 4096];
    status_t st = drv_vmo_create(RINGS_BYTES, DRV_VMO_CONTIGUOUS, &t->ring_vmo);
    if (st == OK)
        st = drv_vmo_map(t->ring_vmo, 0, RINGS_BYTES, VMAR_READ | VMAR_WRITE, (void **)&t->ring);
    if (st == OK && (st = drv_vmo_pin(t->ring_vmo, t->dma, 0, RINGS_BYTES, addrs,
                                      &t->ring_pin)) == OK)
        t->ring_pinned = true;
    if (st == OK)
        st = drv_vmo_create(BUFS_BYTES, 0, &t->buf_vmo);
    if (st == OK)
        st = drv_vmo_map(t->buf_vmo, 0, BUFS_BYTES, VMAR_READ | VMAR_WRITE, (void **)&t->bufs);
    if (st == OK && (st = drv_vmo_pin(t->buf_vmo, t->dma, 0, BUFS_BYTES, t->buf_addr,
                                      &t->buf_pin)) == OK)
        t->buf_pinned = true;
    if (st == OK)
        t->ring_addr = addrs[0];
    return st;
}

status_t ring_setup(struct e1k *t)
{
    status_t st = ring_alloc(t);
    if (st != OK) {
        drv_log("rings: can't set them up (%s)", status_str(st));
        return st;
    }
    for (uint32_t i = 0; i < RX_DESCS; i++) {
        volatile uint8_t *d = ring_rx_desc(t, i);
        *(volatile uint64_t *)(d + RXD_ADDR) = ring_rx_buf_addr(t, i);
        *(volatile uint64_t *)(d + RXD_LEN) = 0;   /* length, checksum, status, errors, special */
    }
    for (uint32_t i = 0; i < TX_DESCS; i++) {
        volatile uint8_t *d = ring_tx_desc(t, i);
        *(volatile uint64_t *)(d + TXD_ADDR) = ring_tx_buf_addr(t, i);
        *(volatile uint64_t *)(d + TXD_LEN) = 0;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the descriptors before the chip may read them */
    uint64_t rx = t->ring_addr + RX_RING_OFF;
    wr32(t, E1K_RDBAL, (uint32_t)rx);
    wr32(t, E1K_RDBAH, (uint32_t)(rx >> 32));
    wr32(t, E1K_RDLEN, RX_DESCS * DESC_SIZE);
    wr32(t, E1K_RDH, 0);
    wr32(t, E1K_RDT, RX_DESCS - 1);   /* every descriptor but one is the chip's */
    t->rx_next = 0;
    uint64_t lo = t->buf_addr[0], hi = t->buf_addr[0];
    for (unsigned i = 1; i < BUF_PAGES; i++) {
        lo = t->buf_addr[i] < lo ? t->buf_addr[i] : lo;
        hi = t->buf_addr[i] > hi ? t->buf_addr[i] : hi;
    }
    drv_log("rings: %u receive and %u transmit descriptors at %#lx, buffers %#lx-%#lx", RX_DESCS,
            TX_DESCS, (unsigned long)t->ring_addr, (unsigned long)lo, (unsigned long)(hi + 4095));
    return OK;
}

void ring_free(struct e1k *t)
{
    if (t->buf_pinned && drv_vmo_unpin(t->buf_vmo, t->dma, t->buf_pin) == OK)
        t->buf_pinned = false;
    if (t->ring_pinned && drv_vmo_unpin(t->ring_vmo, t->dma, t->ring_pin) == OK)
        t->ring_pinned = false;
    if (t->bufs)
        (void)drv_vmo_unmap(t->bufs, BUFS_BYTES);   /* nothing to do if it fails: we exit */
    if (t->ring)
        (void)drv_vmo_unmap(t->ring, RINGS_BYTES);
    t->bufs = t->ring = NULL;
    if (t->buf_vmo != HANDLE_INVALID)
        drv_handle_close(t->buf_vmo);
    if (t->ring_vmo != HANDLE_INVALID)
        drv_handle_close(t->ring_vmo);
    t->buf_vmo = t->ring_vmo = HANDLE_INVALID;
}
