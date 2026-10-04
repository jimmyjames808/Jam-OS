/* hda: the IOMMU (VT-d) checks, on a `vtdtest` boot (M11 stage 5).
 *
 * The point of M11 is that a device reaches only the memory its driver
 * pinned for it and raises only the interrupts it was given. These checks
 * make the controller try to break both, on purpose, so the kernel's VT-d
 * fault machinery (kernel/dev/vtd_fault.c) records them and the owner can
 * read them in the log:
 *
 *   A. the command ring (CORB) pointed at an address the driver did not
 *      pin. With per-device translation the read is blocked (a fault
 *      naming the controller, 00:1f.3 on the PC), so no codec answers;
 *      without it (pass-through, or iommu=off) the controller reads the
 *      address and a codec answers.
 *   B. only when A was blocked: the response ring (RIRB) pointed at the
 *      interrupt window 0xfee00000, and a command sent so the controller
 *      writes a response there. With interrupt remapping on, that write is
 *      blocked (VT-d fault 25h) and raises no interrupt (QEMU 10.0 passes
 *      old-format writes through, so this half is the PC's: the top-level
 *      system note and kernel/test/test_vtd_irq.c).
 *
 * The controller is already up and the rings run (main.c calls this after
 * hda_ctrl_start) because the checks need a working RIRB to tell a blocked
 * read (no answer) from a passed one (an answer), and a codec to answer.
 * Each phase repoints one ring, observes, and the rings are put back to
 * the real page at the end, so the driver serves sound normally after.
 *
 * Whether the device is translated with only its pinned pages mapped is
 * not asked of the kernel (a driver has no such call): it is read from the
 * controller's own behaviour in phase A. On a kernel with DMA translation
 * but drivers still on pass-through (M11 D1 without D2), the read is not
 * blocked, so the checks say so and stop: there is nothing to see in the
 * fault log, and provoking the interrupt-window write would be unsafe
 * where the write is not blocked. */
#include "hda.h"

#define PAGE          4096u
#define IRQ_WINDOW    0xfee00000ull    /* the interrupt window (VT-d 5.1.4) */
#define REG_WAIT_NS   (100 * NS_PER_MS)
#define ANSWER_WAIT   (50 * NS_PER_MS)  /* a codec answers in microseconds */

static uint8_t rd8(struct hda *h, uint32_t o) { return drv_read8(h->regs, o); }
static uint16_t rd16(struct hda *h, uint32_t o) { return drv_read16(h->regs, o); }
static void wr8(struct hda *h, uint32_t o, uint8_t v) { drv_write8(h->regs, o, v); }
static void wr16(struct hda *h, uint32_t o, uint16_t v) { drv_write16(h->regs, o, v); }
static void wr32(struct hda *h, uint32_t o, uint32_t v) { drv_write32(h->regs, o, v); }

/* A GET_PARAMETER vendor-id command to codec cad's root node: a harmless
 * read every codec answers (spec 7.3.3.1). */
static uint32_t vendor_cmd(unsigned cad)
{
    return (uint32_t)cad << 28 | (uint32_t)V_GET_PARAM << 8 | P_VENDOR;
}

/* Wait until (8-bit register o & mask) == want, bounded. */
static bool wait8(struct hda *h, uint32_t o, uint8_t mask, uint8_t want)
{
    uint64_t deadline = drv_clock_ns() + REG_WAIT_NS;
    while ((rd8(h, o) & mask) != want)
        if (drv_clock_ns() > deadline)
            return false;
    return true;
}

/* Stop the CORB, point it at `addr`, reset its read pointer and start it;
 * the write pointer is left at 0 (the next doorbell fetches entry 1). */
static void corb_point(struct hda *h, uint64_t addr)
{
    wr8(h, HDA_CORBCTL, (uint8_t)(rd8(h, HDA_CORBCTL) & ~CORBCTL_RUN));
    (void)wait8(h, HDA_CORBCTL, CORBCTL_RUN, 0);
    wr8(h, HDA_CORBSTS, CORBSTS_CMEI);   /* clear a past fetch error (RW1C) */
    wr32(h, HDA_CORBLBASE, (uint32_t)addr);
    wr32(h, HDA_CORBUBASE, (uint32_t)(addr >> 32));
    wr16(h, HDA_CORBRP, CORBRP_RST);
    uint64_t deadline = drv_clock_ns() + NS_PER_MS;
    while (!(rd16(h, HDA_CORBRP) & CORBRP_RST) && drv_clock_ns() < deadline)
        ;
    wr16(h, HDA_CORBRP, 0);
    (void)wait8(h, HDA_CORBCTL, 0, 0);   /* small settle; errors ignored */
    wr16(h, HDA_CORBWP, 0);
    wr8(h, HDA_CORBCTL, (uint8_t)(rd8(h, HDA_CORBCTL) | CORBCTL_RUN));
    (void)wait8(h, HDA_CORBCTL, CORBCTL_RUN, CORBCTL_RUN);
}

/* Phase A: the CORB at `scaddr` (unpinned), its entry 1 a valid command;
 * true if a codec answered (a response advanced the RIRB) within the
 * deadline, i.e. the controller could read the unpinned address. */
static bool corb_unpinned_answered(struct hda *h, uint64_t scaddr)
{
    wr8(h, HDA_RIRBSTS, RIRBSTS_RINTFL | RIRBSTS_OIS);   /* QEMU pauses the CORB until clear */
    uint8_t wp0 = (uint8_t)(rd16(h, HDA_RIRBWP) & 0xffu);
    corb_point(h, scaddr);
    wr16(h, HDA_CORBWP, 1);   /* fetch entry 1 -> the codec -> a RIRB write */
    uint64_t deadline = drv_clock_ns() + ANSWER_WAIT;
    while (drv_clock_ns() < deadline) {
        if ((uint8_t)(rd16(h, HDA_RIRBWP) & 0xffu) != wp0) {
            wr8(h, HDA_RIRBSTS, RIRBSTS_RINTFL);
            return true;
        }
        drv_sleep_until(drv_clock_ns() + 100 * NS_PER_US);
    }
    return false;
}

/* Phase B: the RIRB at the interrupt window, a command sent so the
 * controller writes a response into it. The CORB is the real pinned ring
 * (restored first), so the fetch and the codec answer are fine; only the
 * response write targets the window. Nothing is observed here: the PC's
 * fault log is the result. */
static void rirb_into_window(struct hda *h, unsigned cad)
{
    wr8(h, HDA_RIRBCTL, 0);
    (void)wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, 0);
    wr32(h, HDA_RIRBLBASE, (uint32_t)IRQ_WINDOW);
    wr32(h, HDA_RIRBUBASE, (uint32_t)(IRQ_WINDOW >> 32));
    wr16(h, HDA_RIRBWP, RIRBWP_RST);
    wr16(h, HDA_RINTCNT, 1);
    wr8(h, HDA_RIRBSTS, RIRBSTS_RINTFL | RIRBSTS_OIS);
    wr8(h, HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);
    (void)wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, RIRBCTL_DMAEN);
    /* One command through the real CORB; its answer is written to the
     * window (blocked on the PC). The interrupt is never enabled (INTCTL
     * stays off), so the only way one could fire is the blocked write
     * itself, which the PC does not allow. */
    h->corb[1] = vendor_cmd(cad);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr16(h, HDA_CORBWP, 1);
    drv_sleep_until(drv_clock_ns() + ANSWER_WAIT);
}

/* Put both rings back on the real pinned page and running, as
 * ctrl.c's start left them, so hda_command works again. */
static void rings_restore(struct hda *h)
{
    wr8(h, HDA_CORBCTL, (uint8_t)(rd8(h, HDA_CORBCTL) & ~CORBCTL_RUN));
    wr8(h, HDA_RIRBCTL, 0);
    (void)wait8(h, HDA_CORBCTL, CORBCTL_RUN, 0);
    (void)wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, 0);
    wr8(h, HDA_CORBSTS, CORBSTS_CMEI);
    wr32(h, HDA_CORBLBASE, (uint32_t)h->ring_addr);
    wr32(h, HDA_CORBUBASE, (uint32_t)(h->ring_addr >> 32));
    wr32(h, HDA_RIRBLBASE, (uint32_t)(h->ring_addr + HDA_RIRB_OFF));
    wr32(h, HDA_RIRBUBASE, (uint32_t)((h->ring_addr + HDA_RIRB_OFF) >> 32));
    wr16(h, HDA_CORBRP, CORBRP_RST);
    uint64_t deadline = drv_clock_ns() + NS_PER_MS;
    while (!(rd16(h, HDA_CORBRP) & CORBRP_RST) && drv_clock_ns() < deadline)
        ;
    wr16(h, HDA_CORBRP, 0);
    (void)wait8(h, HDA_CORBCTL, 0, 0);
    wr16(h, HDA_CORBWP, 0);
    h->corb_wp = 0;
    wr16(h, HDA_RIRBWP, RIRBWP_RST);
    h->rirb_rp = 0;
    wr16(h, HDA_RINTCNT, 1);
    wr8(h, HDA_RIRBSTS, RIRBSTS_RINTFL | RIRBSTS_OIS);
    wr8(h, HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);
    wr8(h, HDA_CORBCTL, (uint8_t)(rd8(h, HDA_CORBCTL) | CORBCTL_RUN));
    (void)wait8(h, HDA_CORBCTL, CORBCTL_RUN, CORBCTL_RUN);
    (void)wait8(h, HDA_RIRBCTL, RIRBCTL_DMAEN, RIRBCTL_DMAEN);
}

/* An unpinned DMA32 page whose device address we know: pin it (so we are
 * handed its address), fill its entry 1 with a valid command, then unpin
 * it (the page stays mapped in us, so its contents hold: under per-device
 * translation its address is no longer reachable by the controller).
 * *out_vmo must be closed by the caller. ERR_* with nothing held. */
static status_t scratch_page(struct hda *h, uint64_t *out_addr, handle_t *out_vmo, void **out_map)
{
    handle_t vmo;
    void *m = NULL;
    uint64_t addr, pin;
    status_t st = drv_vmo_create(PAGE, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &vmo);
    if (st != OK)
        return st;
    if ((st = drv_vmo_map(vmo, 0, PAGE, VMAR_READ | VMAR_WRITE, &m)) != OK) {
        drv_handle_close(vmo);
        return st;
    }
    if ((st = drv_vmo_pin(vmo, h->dma, 0, PAGE, &addr, &pin)) != OK) {
        drv_vmo_unmap(m, PAGE);
        drv_handle_close(vmo);
        return st;
    }
    ((volatile uint32_t *)m)[1] = vendor_cmd((unsigned)__builtin_ctz(h->codec_mask));
    ((volatile uint32_t *)m)[0] = 0;
    status_t up = drv_vmo_unpin(vmo, h->dma, pin);
    if (up != OK) {   /* could not take its mapping away: the test can't run */
        drv_vmo_unmap(m, PAGE);
        drv_handle_close(vmo);
        return up;
    }
    *out_addr = addr;
    *out_vmo = vmo;
    *out_map = m;
    return OK;
}

void hda_vtdtest(struct hda *h, unsigned cad)
{
    if (!h->rings || !h->codec_mask) {
        drv_report("vtdtest: no command rings or no codec: skipped");
        return;
    }
    uint64_t scaddr;
    handle_t vmo;
    void *m;
    status_t st = scratch_page(h, &scaddr, &vmo, &m);
    if (st != OK) {
        drv_report("vtdtest: no scratch page (%s): skipped", status_str(st));
        return;
    }
    bool answered = corb_unpinned_answered(h, scaddr);
    rings_restore(h);   /* the CORB is back on the real ring for phase B */
    /* The RESULTS box and the log show about 120 characters of a line. */
    if (answered) {
        drv_log("vtdtest: the controller read an unpinned page: its device is not translated "
                "with only its pins mapped (iommu=off, or pass-through until per-device "
                "domains, M11 D2), so there is no fault to provoke");
        drv_report("vtdtest: unpinned read at %#lx answered: DMA not restricted: skipped", scaddr);
    } else {
        drv_report("vtdtest: unpinned read at %#lx blocked: look for 'vtd: fault:' (a read)",
                   scaddr);
        rirb_into_window(h, cad);
        rings_restore(h);
        drv_report("vtdtest: RIRB write to %#llx sent: PC: blocked, fault 25h, no interrupt",
                   IRQ_WINDOW);
    }
    drv_vmo_unmap(m, PAGE);
    drv_handle_close(vmo);
}
