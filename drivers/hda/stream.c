/* hda: the output stream. One stream descriptor (spec chapter 3, stream
 * descriptor registers), fed from a ring shared with the client, set up
 * and torn down as spec chapter 4 (stream management) describes.
 *
 * Memory, all pinned DMA32 while the stream is open, made fresh at each
 * open and released at its close:
 *   the ring    64 KiB contiguous, 4 periods of 16 KiB (4096 frames,
 *               85 ms each at 48 kHz 16-bit stereo); the client maps it
 *               and writes samples ahead of the play position
 *   one page    the Buffer Descriptor List at 0 (4 entries, one per
 *               period, each with IOC: an interrupt per period) and the
 *               DMA position buffer at POS_OFF (the controller writes
 *               each stream's position there, 8 bytes per descriptor)
 *
 * The position comes from the position buffer, with LPIB read beside it
 * as a check (the largest gap is in the close line). It is read at every
 * interrupt and at least once a period (irq.c's deadline), so it can
 * never wrap unseen. At every read the driver zeroes the ring behind the
 * position (clear-behind): a client that stops writing gives silence
 * within one ring, never a loop of the last 341 ms, and a client may
 * write anywhere in [position, position + ring) since everything behind
 * the position it was told has already been zeroed.
 *
 * The converter: the DAC at the start of the path main.c chose and set
 * up muted (path.c, verbs.c) gets the stream's format and tag, through
 * verbs.c's hda_set like every other SET verb. The path itself is opened
 * (hda_output_open: unmuted at the gain, the pin's output and EAPD on)
 * just before RUN is set, and closed (muted, pin output off) just after
 * RUN clears, at stop, at the stream's close and at the driver's exit:
 * the jack is silent whenever the stream does not run. A driver killed
 * mid-stream can't mute; its successor's start sets the path up muted
 * again before anything else.
 *
 * TCSEL (Intel's PCI config 0x44, bits 2:0) is the PCI Express traffic
 * class the controller tags its DMA with. It is set to TC0 before the
 * first stream runs: TC0 is the class every platform maps to the default
 * virtual channel, which firmware always sets up, while a higher class a
 * firmware left behind is only as good as its virtual-channel mapping,
 * and an unmapped one can stall audio DMA. */
#include "hda.h"

#define PAGE     4096u
#define POS_OFF  2048u   /* the position buffer in the page (128-byte aligned, spec 3.3.32) */
#define LOG_MAX  3u      /* FIFO/descriptor errors logged per open */

/* A Buffer Descriptor List entry (spec 3.6.3). */
struct bdl_entry {
    uint64_t addr;     /* 128-byte aligned */
    uint32_t len;      /* bytes */
    uint32_t flags;    /* bit 0: interrupt on completion */
};

static uint8_t sd_rd8(struct hda *h, struct stream *s, uint32_t r)
{
    return drv_read8(h->regs, s->sd_regs + r);
}
static void sd_wr8(struct hda *h, struct stream *s, uint32_t r, uint8_t v)
{
    drv_write8(h->regs, s->sd_regs + r, v);
}

void stream_init(struct hda *h, struct stream *s, handle_t dev, struct output *out)
{
    unsigned iss = (h->gcap >> 8) & 0xfu, oss = (h->gcap >> 12) & 0xfu;
    *s = (struct stream){ .sd = HDA_MAX_STREAMS, .dev = dev, .out = out };
    if (out && out->c) {
        s->cad = out->c->cad;
        s->dac = out->p->nid[0];
        s->has_dac = true;
    }
    if (!oss)
        return;
    s->sd = iss;
    s->sd_regs = HDA_SD_BASE + iss * HDA_SD_STRIDE;
}

/* ---- the converter ---------------------------------------------------------- */

/* TCSEL to TC0 (why: the file's header), once per driver start. */
static void tcsel(struct stream *s)
{
    static bool done;
    uint32_t v;
    if (done || s->dev == HANDLE_INVALID || drv_pci_config_read(s->dev, PCI_TCSEL, 1, &v) != OK)
        return;
    done = true;
    if (!(v & 7u))
        return;
    status_t st = drv_pci_config_write(s->dev, PCI_TCSEL, 1, v & ~7u);
    drv_log("stream: TCSEL was TC%u: %s", v & 7u, st == OK ? "set to TC0" : status_str(st));
}

/* ---- buffers ----------------------------------------------------------------- */

/* b: a contiguous DMA32 VMO of `size` bytes, mapped and pinned. Its
 * pages must be contiguous to the device too (one BDL entry spans a
 * period of 4 pages). */
static status_t dma_buf_make(struct hda *h, struct dma_buf *b, uint64_t size)
{
    uint64_t addrs[RING_BYTES / PAGE];
    void *m = NULL;
    status_t st = drv_vmo_create(size, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &b->vmo);
    if (st == OK)
        st = drv_vmo_map(b->vmo, 0, size, VMAR_READ | VMAR_WRITE, &m);
    b->map = m;
    if (st == OK)
        st = drv_vmo_pin(b->vmo, h->dma, 0, size, addrs, &b->pin);
    if (st != OK)
        return st;
    b->pinned = true;
    b->addr = addrs[0];
    for (uint64_t i = 1; i < size / PAGE; i++)
        if (addrs[i] != addrs[0] + i * PAGE)
            return ERR_INTERNAL;
    return OK;
}

/* Unpin, unmap and close b. A pin whose unpin fails stays (and so does
 * its VMO): the dma_cap's close quarantines it. */
static void dma_buf_free(struct hda *h, struct dma_buf *b, uint64_t size)
{
    if (b->pinned && drv_vmo_unpin(b->vmo, h->dma, b->pin) == OK)
        b->pinned = false;
    if (b->map)
        drv_vmo_unmap(b->map, size);
    b->map = NULL;
    if (b->vmo && !b->pinned) {
        drv_handle_close(b->vmo);
        b->vmo = HANDLE_INVALID;
    }
}

/* The ring, and the page with the BDL (one entry per period, each with
 * IOC) and the position buffer; the descriptor and DPLBASE pointed at
 * them. */
static status_t buffers_make(struct hda *h, struct stream *s)
{
    status_t st = dma_buf_make(h, &s->ring, RING_BYTES);
    if (st == OK)
        st = dma_buf_make(h, &s->page, PAGE);
    if (st != OK) {
        drv_log("stream: no DMA buffers (%s)", status_str(st));
        return st;
    }
    volatile struct bdl_entry *bdl = (volatile struct bdl_entry *)s->page.map;
    for (unsigned i = 0; i < PERIODS; i++) {
        bdl[i].addr = s->ring.addr + (uint64_t)i * PERIOD_BYTES;
        bdl[i].len = PERIOD_BYTES;
        bdl[i].flags = 1;   /* IOC */
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the list before the registers point at it */
    uint64_t pos = s->page.addr + POS_OFF;
    drv_write32(h->regs, s->sd_regs + SD_BDPL, (uint32_t)s->page.addr);
    drv_write32(h->regs, s->sd_regs + SD_BDPU, (uint32_t)(s->page.addr >> 32));
    drv_write32(h->regs, HDA_DPUBASE, (uint32_t)(pos >> 32));
    drv_write32(h->regs, HDA_DPLBASE, (uint32_t)pos | DPLBASE_ENABLE);
    return OK;
}

/* ---- the descriptor ------------------------------------------------------------ */

/* RUN clear and waited for (spec 3.3.35: the engine stops at the next
 * frame boundary). */
static status_t sd_halt(struct hda *h, struct stream *s)
{
    uint8_t ctl = sd_rd8(h, s, SD_CTL0);
    s->running = false;
    if (!(ctl & SDCTL_RUN))
        return OK;
    sd_wr8(h, s, SD_CTL0, (uint8_t)(ctl & ~SDCTL_RUN));
    return hda_wait8(h, s->sd_regs + SD_CTL0, SDCTL_RUN, 0, "stream stop");
}

/* SRST 1 then 0, each read back (spec 3.3.35): every register of the
 * descriptor back to its default, the FIFO emptied. */
static status_t sd_reset(struct hda *h, struct stream *s)
{
    uint8_t ctl = sd_rd8(h, s, SD_CTL0);
    sd_wr8(h, s, SD_CTL0, (uint8_t)(ctl | SDCTL_SRST));
    if (hda_wait8(h, s->sd_regs + SD_CTL0, SDCTL_SRST, SDCTL_SRST, "stream reset") != OK)
        return ERR_TIMED_OUT;
    sd_wr8(h, s, SD_CTL0, (uint8_t)(ctl & ~SDCTL_SRST));
    return hda_wait8(h, s->sd_regs + SD_CTL0, SDCTL_SRST, 0, "stream out of reset");
}

/* The descriptor programmed (buffers_make has set the BDL address), the
 * converter told, the stream's interrupt enabled. */
static status_t sd_program(struct hda *h, struct stream *s)
{
    uint32_t r = s->sd_regs;
    sd_wr8(h, s, SD_STS, SDSTS_BCIS | SDSTS_FIFOE | SDSTS_DESE);   /* RW1C: nothing stale */
    drv_write32(h->regs, r + SD_CBL, RING_BYTES);
    drv_write16(h->regs, r + SD_LVI, PERIODS - 1);
    drv_write16(h->regs, r + SD_FMT, STREAM_FORMAT);
    sd_wr8(h, s, SD_CTL2, (uint8_t)(STREAM_TAG << 4));
    sd_wr8(h, s, SD_CTL0, SDCTL_IOCE | SDCTL_FEIE | SDCTL_DEIE);
    status_t st = hda_set(h, s->cad, s->dac, V4_SET_FORMAT, STREAM_FORMAT);
    if (st == OK)
        st = hda_set(h, s->cad, s->dac, V_SET_STREAM, STREAM_TAG << 4);
    if (st != OK) {
        drv_log("stream: converter %u/%02x does not take the format or tag (%s)", s->cad, s->dac,
                status_str(st));
        return st;
    }
    uint32_t ic = drv_read32(h->regs, HDA_INTCTL);
    drv_write32(h->regs, HDA_INTCTL, ic | INTCTL_GIE | (1u << s->sd));
    return OK;
}

/* ---- open, start, stop, close ------------------------------------------------- */

status_t stream_open(struct hda *h, struct stream *s, handle_t *ring)
{
    if (s->sd >= HDA_MAX_STREAMS || !h->codec_mask || (!h->rings && !h->immediate_ok))
        return ERR_NOT_SUPPORTED;
    if (s->open || s->ring.pinned || s->page.pinned)
        return ERR_BAD_STATE;   /* open, or a stream whose DMA never stopped still holds them */
    if (!s->has_dac) {
        drv_log("stream: no path to a jack was set up: nothing to play to");
        return ERR_NOT_FOUND;
    }
    tcsel(s);
    status_t st;
    s->open = true;   /* from here on stream_close undoes what was done */
    if (sd_rd8(h, s, SD_CTL0) & SDCTL_RUN)
        drv_log("stream: descriptor %u was running at open: stopping it", s->sd);
    if ((st = sd_halt(h, s)) != OK || (st = sd_reset(h, s)) != OK ||
        (st = buffers_make(h, s)) != OK || (st = sd_program(h, s)) != OK) {
        stream_close(h, s, "open failed");
        return st;
    }
    /* The client's handle: to map, read and write, and no more. */
    st = drv_handle_duplicate(s->ring.vmo, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER,
                              ring);
    if (st != OK) {
        stream_close(h, s, "no handle for the client");
        return st;
    }
    s->last_off = 0;
    s->played = s->cleared = 0;
    s->iocs = s->fifo_errors = s->lpib_diff_max = 0;
    drv_log("stream: open on descriptor %u, tag %u, format %#06x, converter %u/%02x; ring %u "
            "bytes in %u periods; FIFO %u bytes", s->sd, STREAM_TAG, STREAM_FORMAT, s->cad, s->dac,
            RING_BYTES, PERIODS, drv_read16(h->regs, s->sd_regs + SD_FIFOS));
    return OK;
}

status_t stream_start(struct hda *h, struct stream *s)
{
    if (!s->open)
        return ERR_BAD_STATE;
    if (s->running)
        return OK;
    status_t st = hda_output_open(h, s->out);   /* the ring holds what the client wrote */
    if (st != OK)
        return st;
    uint8_t ctl = sd_rd8(h, s, SD_CTL0);
    sd_wr8(h, s, SD_CTL0, (uint8_t)(ctl | SDCTL_RUN));
    if (hda_wait8(h, s->sd_regs + SD_CTL0, SDCTL_RUN, SDCTL_RUN, "stream start") != OK) {
        (void)sd_halt(h, s);
        (void)hda_output_close(h, s->out);
        return ERR_TIMED_OUT;
    }
    s->running = true;
    s->progress_ns = drv_clock_ns();
    return OK;
}

status_t stream_stop(struct hda *h, struct stream *s)
{
    if (!s->open)
        return ERR_BAD_STATE;
    status_t st = sd_halt(h, s);
    status_t mute = hda_output_close(h, s->out);   /* muted even if RUN did not clear */
    stream_update(h, s);
    return st != OK ? st : mute;
}

void stream_close(struct hda *h, struct stream *s, const char *why)
{
    if (!s->open)
        return;
    bool quiet = sd_halt(h, s) == OK;
    (void)hda_output_close(h, s->out);   /* logged if a verb fails */
    if (quiet)
        quiet = sd_reset(h, s) == OK;
    uint32_t ic = drv_read32(h->regs, HDA_INTCTL) & ~(1u << s->sd);
    drv_write32(h->regs, HDA_INTCTL, (ic & ~INTCTL_GIE) ? ic : 0);
    drv_write32(h->regs, HDA_DPLBASE, 0);
    drv_write32(h->regs, HDA_DPUBASE, 0);
    (void)hda_set(h, s->cad, s->dac, V_SET_STREAM, 0);   /* stream 0: none */
    if (quiet) {
        dma_buf_free(h, &s->page, PAGE);
        dma_buf_free(h, &s->ring, RING_BYTES);
    } else {
        drv_log("stream: the DMA engine did not stop: its buffers stay pinned");
    }
    s->open = false;
    if (why)
        drv_log("stream: closed (%s): %lu frames played, %u period interrupt(s), %u FIFO "
                "error(s), position buffer vs LPIB up to %u bytes", why,
                (unsigned long)(s->played / FRAME_BYTES), s->iocs, s->fifo_errors,
                s->lpib_diff_max);
}

/* ---- position and status ------------------------------------------------------- */

/* Zero ring bytes [from, to) (absolute counts, at most a ring apart). */
static void clear_ring(struct stream *s, uint64_t from, uint64_t to)
{
    if (to - from > RING_BYTES)
        from = to - RING_BYTES;
    while (from < to) {
        uint32_t off = (uint32_t)(from % RING_BYTES);
        uint32_t n = RING_BYTES - off;
        if (n > to - from)
            n = (uint32_t)(to - from);
        __builtin_memset(s->ring.map + off, 0, n);
        from += n;
    }
}

void stream_update(struct hda *h, struct stream *s)
{
    if (!s->open || !s->page.map || !s->ring.map)
        return;
    uint32_t pos = *(volatile uint32_t *)(s->page.map + POS_OFF + 8 * s->sd) % RING_BYTES;
    uint32_t lpib = drv_read32(h->regs, s->sd_regs + SD_LPIB) % RING_BYTES;
    uint32_t gap = (pos - lpib + RING_BYTES) % RING_BYTES;
    if (gap > RING_BYTES / 2)
        gap = RING_BYTES - gap;
    if (s->running && gap > s->lpib_diff_max)   /* stopped, LPIB may not be reset yet (QEMU) */
        s->lpib_diff_max = gap;
    uint32_t delta = (pos - s->last_off + RING_BYTES) % RING_BYTES;
    s->last_off = pos;
    if (!delta)
        return;
    s->played += delta;
    s->progress_ns = drv_clock_ns();
    clear_ring(s, s->cleared, s->played);
    s->cleared = s->played;
}

void stream_status(struct hda *h, struct stream *s)
{
    if (!s->open)
        return;
    uint8_t sts = sd_rd8(h, s, SD_STS) & (SDSTS_BCIS | SDSTS_FIFOE | SDSTS_DESE);
    if (!sts)
        return;
    sd_wr8(h, s, SD_STS, sts);   /* RW1C */
    if (sts & SDSTS_BCIS)
        s->iocs++;
    if (sts & (SDSTS_FIFOE | SDSTS_DESE) && s->fifo_errors++ < LOG_MAX)
        drv_log("stream: %s", sts & SDSTS_DESE ? "descriptor error" : "FIFO error");
}
