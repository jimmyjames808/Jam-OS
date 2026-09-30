/* hda: the controller. Reset and codec discovery, the command rings
 * CORB and RIRB (spec chapter 4, the programming model), the immediate
 * command interface (the ICOI/ICII/ICIS registers, spec chapter 3) as a
 * fallback, and the stop at exit.
 *
 * Commands normally go through the rings: the CORB is a ring of 32-bit
 * commands in memory the controller reads by DMA, the RIRB a ring of
 * 64-bit responses it writes back (the low word the response, the high
 * word bits 3:0 the codec, bit 4 "unsolicited"). The rings are the
 * only way unsolicited responses (jack events) arrive, so the driver
 * that plays sound needs them; the probe proves them on the real
 * controller. Both rings share one pinned DMA32 page. The probe sends one
 * command at a time and polls the RIRB's write pointer (no interrupt):
 * a verb is answered within microseconds, and polling keeps the MSI path
 * out of the probe.
 *
 * The immediate interface (one command register, one response register)
 * needs no DMA but is optional in the spec and can't carry unsolicited
 * responses; it is used only if the rings don't answer, so a PC run
 * still yields a dump, and the log says which was used.
 *
 * Bus mastering goes on only after the reset: a reset stops every DMA
 * engine (streams and rings), so nothing a previous driver of this
 * function left running reaches memory. Unsolicited responses and
 * interrupts stay off (GCTL.UNSOL = 0, INTCTL = 0). At exit the rings are
 * stopped and the controller is put back into reset, which is also how
 * firmware leaves it. */
#include "hda.h"

#define RIRB_OFF      2048u                   /* 256 CORB entries x 4 bytes before it */
#define PAGE          4096u
#define REG_TIMEOUT   (100 * NS_PER_MS)       /* a register bit to settle */
#define CODEC_WAIT    (100 * NS_PER_MS)       /* codecs to report after the reset */
#define SPIN_NS       (200 * NS_PER_US)       /* poll without sleeping this long first */

static uint8_t rd8(struct hda *h, uint32_t o) { return drv_read8(h->regs, o); }
static uint16_t rd16(struct hda *h, uint32_t o) { return drv_read16(h->regs, o); }
static uint32_t rd32(struct hda *h, uint32_t o) { return drv_read32(h->regs, o); }
static void wr8(struct hda *h, uint32_t o, uint8_t v) { drv_write8(h->regs, o, v); }
static void wr16(struct hda *h, uint32_t o, uint16_t v) { drv_write16(h->regs, o, v); }
static void wr32(struct hda *h, uint32_t o, uint32_t v) { drv_write32(h->regs, o, v); }

/* Sleep between polls once the first SPIN_NS have passed. */
static void pause_poll(uint64_t start)
{
    uint64_t t = drv_clock_ns();
    if (t - start > SPIN_NS)
        drv_sleep_until(t + 50 * NS_PER_US);
}

/* Wait until (8-bit register o & mask) == want. */
static status_t wait8(struct hda *h, uint32_t o, uint8_t mask, uint8_t want, const char *what)
{
    uint64_t start = drv_clock_ns(), deadline = start + REG_TIMEOUT;
    uint8_t v;
    while (((v = rd8(h, o)) & mask) != want) {
        if (drv_clock_ns() > deadline) {
            drv_log("%s: timed out (register %#x reads %#x)", what, o, v);
            return ERR_TIMED_OUT;
        }
        pause_poll(start);
    }
    return OK;
}

static status_t wait32(struct hda *h, uint32_t o, uint32_t mask, uint32_t want, const char *what)
{
    uint64_t start = drv_clock_ns(), deadline = start + REG_TIMEOUT;
    uint32_t v;
    while (((v = rd32(h, o)) & mask) != want) {
        if (drv_clock_ns() > deadline) {
            drv_log("%s: timed out (register %#x reads %#x)", what, o, v);
            return ERR_TIMED_OUT;
        }
        pause_poll(start);
    }
    return OK;
}

static unsigned nstreams(uint16_t gcap)
{
    return ((gcap >> 8) & 0xfu) + ((gcap >> 12) & 0xfu) + ((gcap >> 3) & 0x1fu);
}

/* Every DMA engine off: the streams' RUN bits, the CORB and the RIRB.
 * Only out of reset (in reset no register but GCTL may be written, and
 * nothing runs anyway). */
static status_t stop_engines(struct hda *h)
{
    status_t st = OK;
    unsigned n = nstreams(h->gcap);
    for (unsigned i = 0; i < n && i < HDA_MAX_STREAMS; i++) {
        uint32_t sd = HDA_SD_BASE + i * HDA_SD_STRIDE;
        uint8_t ctl = rd8(h, sd + HDA_SD_CTL);
        if (!(ctl & SDCTL_RUN))
            continue;
        drv_log("stream %u was running: stopping it", i);
        wr8(h, sd + HDA_SD_CTL, (uint8_t)(ctl & ~SDCTL_RUN));
        if (wait8(h, sd + HDA_SD_CTL, SDCTL_RUN, 0, "stream stop") != OK)
            st = ERR_TIMED_OUT;
    }
    wr8(h, HDA_CORBCTL, rd8(h, HDA_CORBCTL) & (uint8_t)~CORBCTL_RUN);
    wr8(h, HDA_RIRBCTL, rd8(h, HDA_RIRBCTL) & (uint8_t)~RIRBCTL_DMAEN);
    if (wait8(h, HDA_CORBCTL, CORBCTL_RUN, 0, "CORB stop") != OK ||
        wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, 0, "RIRB stop") != OK)
        st = ERR_TIMED_OUT;
    wr32(h, HDA_INTCTL, 0);
    return st;
}

/* CRST 0 then 1 (spec chapter 4, controller reset), then the codecs' state-change bits. */
static status_t reset(struct hda *h)
{
    if (rd32(h, HDA_GCTL) & GCTL_CRST) {
        status_t st = stop_engines(h);
        if (st != OK)
            return st;
        wr32(h, HDA_GCTL, rd32(h, HDA_GCTL) & ~GCTL_CRST);
        if (wait32(h, HDA_GCTL, GCTL_CRST, 0, "enter reset") != OK)
            return ERR_TIMED_OUT;
    }
    /* The link's reset is held for at least 100 us (spec chapter 4, codec discovery). */
    drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    wr32(h, HDA_GCTL, rd32(h, HDA_GCTL) | GCTL_CRST);
    if (wait32(h, HDA_GCTL, GCTL_CRST, GCTL_CRST, "leave reset") != OK)
        return ERR_TIMED_OUT;
    /* Codecs ask for attention within 25 frames (521 us) of the reset's
     * end (spec chapter 4, codec discovery); some take longer, so wait for the first one up to
     * CODEC_WAIT, then a little more for the rest. */
    uint64_t start = drv_clock_ns();
    drv_sleep_until(start + NS_PER_MS);
    while (!(rd16(h, HDA_STATESTS) & 0x7fffu) && drv_clock_ns() < start + CODEC_WAIT)
        drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    h->codec_mask = rd16(h, HDA_STATESTS) & 0x7fffu;
    wr16(h, HDA_STATESTS, h->codec_mask);   /* RW1C */
    return OK;
}

/* The largest ring size a CORBSIZE/RIRBSIZE register offers: its code
 * (0: 2, 1: 16, 2: 256 entries) and entries. */
static uint32_t ring_size(uint8_t reg, uint8_t *code)
{
    if (reg & 0x40u) {
        *code = 2;
        return 256;
    }
    if (reg & 0x20u) {
        *code = 1;
        return 16;
    }
    *code = 0;
    return 2;
}

/* The ring page: made, mapped, pinned below 4 GiB. */
static status_t ring_page(struct hda *h, uint64_t *addr)
{
    void *m = NULL;
    status_t st = drv_vmo_create(PAGE, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &h->ring_vmo);
    if (st == OK)
        st = drv_vmo_map(h->ring_vmo, 0, PAGE, VMAR_READ | VMAR_WRITE, &m);
    if (st == OK)
        st = drv_vmo_pin(h->ring_vmo, h->dma, 0, PAGE, addr, &h->ring_pin);
    if (st != OK) {
        drv_log("no ring page (%s)", status_str(st));
        return st;
    }
    h->ring_pinned = true;
    h->corb = m;
    h->rirb = (volatile uint64_t *)((uint8_t *)m + RIRB_OFF);
    return OK;
}

/* CORBRP's reset (spec chapter 3, CORBRP): write 1 and see it read back 1, then
 * write 0 and see 0. Some controllers (and QEMU) never show the 1, so
 * only the 0 is required. */
static status_t corb_rp_reset(struct hda *h)
{
    wr16(h, HDA_CORBRP, CORBRP_RST);
    uint64_t deadline = drv_clock_ns() + NS_PER_MS;
    while (!(rd16(h, HDA_CORBRP) & CORBRP_RST) && drv_clock_ns() < deadline)
        ;
    wr16(h, HDA_CORBRP, 0);
    uint64_t start = drv_clock_ns();
    while (rd16(h, HDA_CORBRP) & CORBRP_RST) {
        if (drv_clock_ns() > start + REG_TIMEOUT) {
            drv_log("CORB read pointer stays in reset (%#x)", rd16(h, HDA_CORBRP));
            return ERR_TIMED_OUT;
        }
        pause_poll(start);
    }
    return OK;
}

/* Both rings set up and running (spec chapter 4, CORB and RIRB). */
static status_t rings_start(struct hda *h)
{
    uint64_t addr;
    status_t st = ring_page(h, &addr);
    if (st != OK)
        return st;
    uint8_t ccode, rcode;
    h->corb_entries = ring_size(rd8(h, HDA_CORBSIZE), &ccode);
    h->rirb_entries = ring_size(rd8(h, HDA_RIRBSIZE), &rcode);
    wr8(h, HDA_CORBSIZE, (uint8_t)((rd8(h, HDA_CORBSIZE) & ~3u) | ccode));
    wr8(h, HDA_RIRBSIZE, (uint8_t)((rd8(h, HDA_RIRBSIZE) & ~3u) | rcode));
    wr32(h, HDA_CORBLBASE, (uint32_t)addr);
    wr32(h, HDA_CORBUBASE, (uint32_t)(addr >> 32));
    wr32(h, HDA_RIRBLBASE, (uint32_t)(addr + RIRB_OFF));
    wr32(h, HDA_RIRBUBASE, (uint32_t)((addr + RIRB_OFF) >> 32));
    if ((st = corb_rp_reset(h)) != OK)
        return st;
    wr16(h, HDA_CORBWP, 0);
    h->corb_wp = 0;
    wr16(h, HDA_RIRBWP, RIRBWP_RST);
    h->rirb_rp = 0;
    /* A response "interrupt" after every response: RIRBSTS.RINTFL is set
     * and hda_get clears it after each. INTCTL's global enable stays off,
     * so nothing reaches the CPU. QEMU needs it: once RINTCNT responses
     * have come in it pauses the CORB until RINTFL goes from 1 to 0, and
     * with RINTCTL off RINTFL never becomes 1. */
    wr16(h, HDA_RINTCNT, 1);
    wr8(h, HDA_RIRBSTS, RIRBSTS_RINTFL | RIRBSTS_OIS);
    wr8(h, HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);
    wr8(h, HDA_CORBCTL, CORBCTL_RUN);
    if (wait8(h, HDA_CORBCTL, CORBCTL_RUN, CORBCTL_RUN, "CORB start") != OK ||
        wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, RIRBCTL_DMAEN, "RIRB start") != OK)
        return ERR_TIMED_OUT;
    h->rings = true;
    return OK;
}

static void rings_stop(struct hda *h)
{
    if (!h->regs || !(rd32(h, HDA_GCTL) & GCTL_CRST))
        return;
    wr8(h, HDA_CORBCTL, 0);
    wr8(h, HDA_RIRBCTL, 0);
    (void)wait8(h, HDA_CORBCTL, CORBCTL_RUN, 0, "CORB stop");   /* logged; reset follows */
    (void)wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, 0, "RIRB stop");
    h->rings = false;
}

/* ---- commands --------------------------------------------------------------- */

static status_t ring_cmd(struct hda *h, uint32_t cmd, unsigned cad, uint32_t *out)
{
    uint32_t wp = (h->corb_wp + 1) % h->corb_entries;
    h->corb[wp] = cmd;
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the entry before the doorbell */
    h->corb_wp = wp;
    wr16(h, HDA_CORBWP, (uint16_t)wp);
    uint64_t start = drv_clock_ns(), deadline = start + HDA_CMD_TIMEOUT;
    for (;;) {
        uint32_t hw = rd16(h, HDA_RIRBWP) & 0xffu;
        while (h->rirb_rp != hw % h->rirb_entries) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the entry after the pointer */
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_entries;
            uint64_t e = h->rirb[h->rirb_rp];
            wr8(h, HDA_RIRBSTS, RIRBSTS_RINTFL | RIRBSTS_OIS);
            uint32_t ex = (uint32_t)(e >> 32);
            if (ex & 0x10u) {
                h->unsol++;
                continue;
            }
            if ((ex & 0xfu) != cad)
                continue;   /* not ours: a late answer to a command that timed out */
            *out = (uint32_t)e;
            return OK;
        }
        if (drv_clock_ns() > deadline)
            return ERR_TIMED_OUT;
        pause_poll(start);
    }
}

static status_t imm_cmd(struct hda *h, uint32_t cmd, uint32_t *out)
{
    if (wait32(h, HDA_ICIS, ICIS_ICB, 0, "immediate busy") != OK)
        return ERR_TIMED_OUT;
    wr16(h, HDA_ICIS, ICIS_IRV);   /* RW1C: no stale response */
    wr32(h, HDA_ICOI, cmd);
    wr16(h, HDA_ICIS, ICIS_ICB);
    uint64_t start = drv_clock_ns(), deadline = start + HDA_CMD_TIMEOUT;
    while ((rd16(h, HDA_ICIS) & (ICIS_ICB | ICIS_IRV)) != ICIS_IRV) {
        if (drv_clock_ns() > deadline)
            return ERR_TIMED_OUT;
        pause_poll(start);
    }
    *out = rd32(h, HDA_ICII);
    wr16(h, HDA_ICIS, ICIS_IRV);
    return OK;
}

/* A GET verb: 12-bit verbs 0xf00-0xfff, or the 4-bit GET verbs 0xa
 * (converter format) and 0xb (amplifier gain/mute). Everything else sets
 * something in the codec (spec 7.3), and this driver sets nothing. */
static bool is_get(uint32_t verb, uint32_t payload)
{
    if (verb >= 0xf00 && verb <= 0xfff)
        return payload <= 0xff;
    return (verb == V4_GET_FORMAT || verb == V4_GET_AMP) && payload <= 0xffff;
}

status_t hda_get(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                 uint32_t *out)
{
    if (cad >= HDA_MAX_CODECS || nid > 0x7f || !is_get(verb, payload))
        return ERR_INVALID_ARGS;
    uint32_t cmd = (uint32_t)cad << 28 | (uint32_t)nid << 20;
    cmd |= verb >= 0x100 ? verb << 8 | payload : verb << 16 | payload;
    status_t st = h->rings ? ring_cmd(h, cmd, cad, out) : imm_cmd(h, cmd, out);
    if (st == ERR_TIMED_OUT)
        h->timeouts++;
    return st;
}

status_t hda_param(struct hda *h, unsigned cad, unsigned nid, uint32_t param, uint32_t *out)
{
    return hda_get(h, cad, nid, V_GET_PARAM, param, out);
}

/* ---- start and stop ----------------------------------------------------------- */

/* The first codec's vendor id through the rings; if they don't answer,
 * the rings stop and the immediate interface is tried. */
static status_t choose_path(struct hda *h)
{
    unsigned cad = (unsigned)__builtin_ctz(h->codec_mask);
    uint32_t v = 0;
    status_t st = rings_start(h);
    if (st == OK && (st = hda_param(h, cad, 0, P_VENDOR, &v)) == OK)
        return OK;
    drv_log("the command rings don't answer (%s): trying the immediate interface",
            status_str(st));
    rings_stop(h);
    st = hda_param(h, cad, 0, P_VENDOR, &v);
    if (st != OK) {
        drv_log("the immediate interface doesn't answer either (%s)", status_str(st));
        return st;
    }
    h->immediate_ok = true;
    return OK;
}

status_t hda_ctrl_start(struct hda *h, handle_t bar)
{
    status_t st = drv_mmio_map(bar, 0, PAGE, VMO_CACHE_UC, &h->regs);
    if (st != OK) {
        drv_log("can't map BAR 0 (%s)", status_str(st));
        return st;
    }
    h->gcap = rd16(h, HDA_GCAP);
    if (rd32(h, HDA_GCAP) == 0xffffffffu) {
        drv_log("BAR 0 reads all ones: the device is gone");
        return ERR_NOT_FOUND;
    }
    if ((st = reset(h)) != OK)
        return st;
    h->gcap = rd16(h, HDA_GCAP);
    if ((st = drv_dma_bus_master(h->dma, 1)) != OK) {
        drv_log("can't turn bus mastering on (%s)", status_str(st));
        return st;
    }
    if (!h->codec_mask) {
        drv_log("no codec reported after the reset (STATESTS 0)");
        return OK;
    }
    return choose_path(h);
}

void hda_ctrl_stop(struct hda *h)
{
    if (h->regs) {
        rings_stop(h);
        if (rd32(h, HDA_GCTL) & GCTL_CRST) {
            wr32(h, HDA_GCTL, rd32(h, HDA_GCTL) & ~GCTL_CRST);
            (void)wait32(h, HDA_GCTL, GCTL_CRST, 0, "enter reset");   /* logged */
        }
    }
    if (h->ring_pinned && drv_vmo_unpin(h->ring_vmo, h->dma, h->ring_pin) == OK)
        h->ring_pinned = false;
    if (h->ring_vmo && !h->ring_pinned) {
        drv_handle_close(h->ring_vmo);
        h->ring_vmo = HANDLE_INVALID;
    }
}
