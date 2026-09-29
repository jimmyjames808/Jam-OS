/* xhci-noop: the M6 done test on real hardware (M6-PLAN.md "Phase 2").
 *
 * Brings up an xHCI controller just far enough to run commands: takes it
 * from the firmware (USB Legacy Support handoff, SMIs off), stops and
 * resets it, gives it a DCBAA (+ scratchpad buffers), a command ring and
 * one event ring on interrupter 0, starts it, and has it complete three
 * No Op commands, each reported by a real MSI / MSI-X interrupt arriving
 * as a port packet. Then it stops and resets the controller again and
 * leaves it quiet. Runs on the PC's Intel 8086:7A60 and QEMU's qemu-xhci
 * (1b36:000d), as a kernel process or as a process.
 *
 * Handles: DR_PCIDEV (its function, for config reads), DR_BAR(0) (the
 * registers), DR_IRQ(0) (MSI, or MSI-X entry 0 = interrupter 0), DR_DMA
 * (bus master off until the controller is halted and reset: M7, a
 * controller a dead driver left running must not reach memory again).
 * Every wait is bounded; any failure is reported with the step and the
 * registers, and the controller is left halted.
 *
 * Exit codes: 0 all three No-Ops completed via interrupt; 1 a step failed;
 * 2 a handle is missing. */
#include <jam/driver.h>

#define US   1000ull
#define MS   1000000ull
#define PAGE 4096u

/* ---- register layout (xHCI 1.2, chapter 5) --------------------------------- */

/* capability registers */
#define CAP_CAPLENGTH   0x00   /* 7:0 length, 31:16 HCIVERSION */
#define CAP_HCSPARAMS1  0x04   /* 7:0 MaxSlots, 18:8 MaxIntrs, 31:24 MaxPorts */
#define CAP_HCSPARAMS2  0x08   /* 25:21 scratchpads hi, 31:27 scratchpads lo */
#define CAP_HCCPARAMS1  0x10   /* 0 AC64, 2 CSZ, 31:16 xECP (dwords) */
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* operational registers (from CAPLENGTH) */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38

#define CMD_RS    (1u << 0)
#define CMD_HCRST (1u << 1)
#define CMD_INTE  (1u << 2)
#define CMD_HSEE  (1u << 3)

#define STS_HCH  (1u << 0)
#define STS_HSE  (1u << 2)
#define STS_EINT (1u << 3)
#define STS_PCD  (1u << 4)
#define STS_CNR  (1u << 11)
#define STS_HCE  (1u << 12)

#define CRCR_RCS (1u << 0)

/* interrupter 0 (runtime registers + 0x20) */
#define IR0        0x20
#define IR_IMAN    0x00
#define IR_IMOD    0x04
#define IR_ERSTSZ  0x08
#define IR_ERSTBA  0x10
#define IR_ERDP    0x18
#define IMAN_IP    (1u << 0)
#define IMAN_IE    (1u << 1)
#define ERDP_EHB   (1u << 3)
#define IMOD_40US  160u        /* 250 ns units */

/* extended capabilities */
#define XCAP_LEGACY    1
#define XCAP_PROTOCOL  2
#define LEG_BIOS_OWNED (1u << 16)
#define LEG_OS_OWNED   (1u << 24)
/* USBLEGCTLSTS: the SMI enables (0, 4, 13, 14, 15) and the RW1C SMI
 * status bits (29, 30, 31). */
#define LEGCTL_SMI_ENABLES 0x0000e011u
#define LEGCTL_SMI_STATUS  0xe0000000u

/* TRBs */
#define TRB_LINK        6
#define TRB_NOOP_CMD    23
#define TRB_TRANSFER_EV 32
#define TRB_CMD_DONE_EV 33
#define TRB_PORT_EV     34
#define TRB_HC_EV       37
#define TRB_C           (1u << 0)
#define TRB_TC          (1u << 1)   /* Link TRB: toggle cycle */
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(c)  (((c) >> 10) & 0x3f)
#define CC_SUCCESS      1

#define RING_TRBS 256   /* one 4 KiB segment */
#define NOOPS     3
#define MAX_SLOTS 8

struct trb {
    uint32_t d0, d1, d2, d3;
};

/* The DMA area, one contiguous DMA32 VMO: */
#define DMA_DCBAA   0x0000   /* 256 entries max; we use MAX_SLOTS + 1 */
#define DMA_ERST    0x0800   /* one 16-byte entry */
#define DMA_CMDRING 0x1000
#define DMA_EVRING  0x2000
#define DMA_SPARRAY 0x3000   /* scratchpad buffer array (pages as needed) */

/* ---- state -------------------------------------------------------------------- */

#define MAX_MAPS 48

struct xhc {
    const char *name;
    handle_t dev, bar, irq, dma, port;
    /* register pages mapped so far (page offset in BAR0 -> pointer) */
    struct { uint32_t page; volatile uint8_t *va; } map[MAX_MAPS];
    unsigned nmap;
    bool map_failed;
    uint32_t map_fail_off;
    status_t map_fail_st;

    uint16_t vendor, device;
    uint8_t revision;
    bool msix;
    uint32_t irq_vectors;
    uint32_t caplen, hciver, hcs1, hcs2, hcc1, dboff, rtsoff;
    uint32_t ports, slots, scratchpads, pagesize;
    const char *handoff;         /* "none", "ok", "ok (not BIOS-owned)", "timeout" */
    uint64_t handoff_ms;

    handle_t ctx_vmo, sp_vmo;
    uint8_t *ctx;                /* the DMA area, mapped */
    uint64_t ctx_dev;            /* its device address */
    uint32_t ctx_pages;
    uint64_t ctx_pin, sp_pin;
    bool ctx_pinned, sp_pinned;

    uint32_t cmd_enq, cmd_cycle;
    uint32_t ev_deq, ev_cycle;
    uint32_t other_events;
    uint64_t lat_ns[NOOPS];
};

static uint32_t hi32(uint64_t v) { return (uint32_t)(v >> 32); }
static uint32_t lo32(uint64_t v) { return (uint32_t)v; }

/* ---- registers ------------------------------------------------------------------ */

/* The pointer for BAR0 offset `off`, mapping its page on first use. Pages
 * are mapped one at a time: the kernel refuses pages holding an MSI-X
 * table or PBA, which qemu-xhci keeps in BAR0 next to the registers. On a
 * failure the access goes to a dummy word and map_failed says so. */
static volatile uint8_t *reg(struct xhc *x, uint32_t off)
{
    static uint32_t sink[2];
    uint32_t page = off & ~(PAGE - 1);
    for (unsigned i = 0; i < x->nmap; i++)
        if (x->map[i].page == page)
            return x->map[i].va + (off - page);
    volatile void *va = NULL;
    status_t st = x->nmap < MAX_MAPS ? drv_mmio_map(x->bar, page, PAGE, VMO_CACHE_UC, &va)
                                     : ERR_NO_RESOURCES;
    if (st != OK) {
        if (!x->map_failed) {
            x->map_failed = true;
            x->map_fail_off = off;
            x->map_fail_st = st;
        }
        sink[0] = 0xffffffffu;
        return (volatile uint8_t *)sink;
    }
    x->map[x->nmap].page = page;
    x->map[x->nmap].va = va;
    x->nmap++;
    return (volatile uint8_t *)va + (off - page);
}

static uint32_t rd(struct xhc *x, uint32_t off) { return drv_read32(reg(x, off), 0); }
static void wr(struct xhc *x, uint32_t off, uint32_t v) { drv_write32(reg(x, off), 0, v); }
static uint32_t op_rd(struct xhc *x, uint32_t r) { return rd(x, x->caplen + r); }
static void op_wr(struct xhc *x, uint32_t r, uint32_t v) { wr(x, x->caplen + r, v); }
static uint32_t ir_rd(struct xhc *x, uint32_t r) { return rd(x, x->rtsoff + IR0 + r); }
static void ir_wr(struct xhc *x, uint32_t r, uint32_t v) { wr(x, x->rtsoff + IR0 + r, v); }

/* 64-bit registers as two dword writes, low first (xHCI 5.1: every
 * controller takes that; not all take a qword access). */
static void op_wr64(struct xhc *x, uint32_t r, uint64_t v)
{
    op_wr(x, r, lo32(v));
    op_wr(x, r + 4, hi32(v));
}

static void ir_wr64(struct xhc *x, uint32_t r, uint64_t v)
{
    ir_wr(x, r, lo32(v));
    ir_wr(x, r + 4, hi32(v));
}

/* ---- failure reporting ------------------------------------------------------------ */

static void snapshot(struct xhc *x)
{
    if (!x->caplen)
        return;
    bool mf = x->map_failed;
    drv_report("regs: USBCMD %08x USBSTS %08x CRCR %08x IMAN %08x ERDP %08x", op_rd(x, OP_USBCMD),
               op_rd(x, OP_USBSTS), op_rd(x, OP_CRCR), x->rtsoff ? ir_rd(x, IR_IMAN) : 0,
               x->rtsoff ? ir_rd(x, IR_ERDP) : 0);
    x->map_failed = mf;
}

/* A step failed: report which (RESULTS box) with the registers; 1. */
#define fail(x, step, fmt, ...) \
    (drv_report("FAILED at %s: " fmt, (step), ##__VA_ARGS__), snapshot(x), 1)

/* Poll until (reg & mask) == want or timeout_ms passes; *last (may be
 * NULL) gets the last value read. All ones (a device still in reset, or
 * gone) just means "not yet". */
static bool wait_op(struct xhc *x, uint32_t r, uint32_t mask, uint32_t want, uint64_t timeout_ms,
                    uint32_t *last)
{
    uint64_t end = drv_clock_ns() + timeout_ms * MS;
    for (;;) {
        uint32_t v = op_rd(x, r);
        if (last)
            *last = v;
        if (v != 0xffffffffu && (v & mask) == want)
            return true;
        if (drv_clock_ns() > end || x->map_failed)
            return false;
        drv_sleep_until(drv_clock_ns() + 50 * US);
    }
}

/* ---- PCI config ----------------------------------------------------------------- */

static uint32_t cfg(struct xhc *x, uint32_t off, uint32_t width)
{
    uint32_t v = 0;
    if (drv_pci_config_read(x->dev, off, width, &v) != OK)
        return 0xffffffffu;
    return v;
}

/* Standard capability `id`'s offset, or 0. */
static uint32_t pci_cap(struct xhc *x, uint32_t id)
{
    if (!(cfg(x, 0x06, 2) & (1u << 4)))
        return 0;   /* no capability list */
    uint32_t off = cfg(x, 0x34, 1) & 0xfc;
    for (int guard = 0; off >= 0x40 && off < 0x100 && guard < 48; guard++) {
        if ((cfg(x, off, 1) & 0xff) == id)
            return off;
        off = cfg(x, off + 1, 1) & 0xfc;
    }
    return 0;
}

static int check_pci(struct xhc *x)
{
    uint32_t id = cfg(x, 0x00, 4);
    if (id == 0xffffffffu)
        return fail(x, "PCI config", "can't read the function's config space (DR_PCIDEV)");
    x->vendor = (uint16_t)id;
    x->device = (uint16_t)(id >> 16);
    x->revision = (uint8_t)cfg(x, 0x08, 1);
    uint32_t class = cfg(x, 0x08, 4) >> 8;
    if (class != 0x0c0330)
        return fail(x, "PCI config", "class %06x is not an xHCI (0c0330)", class);
    uint32_t cmd = cfg(x, 0x04, 2);
    if (!(cmd & (1u << 1)))
        return fail(x, "PCI config", "memory decode is off (command %04x)", cmd);
    /* Bus mastering is turned on (drv_dma_bus_master) after the reset. */

    /* Power state: devmgr wakes a function found in D1-D3 before it binds
     * a driver (the power state is the kernel's and devmgr's to change). */
    uint32_t pm = pci_cap(x, 0x01);
    if (pm && (cfg(x, pm + 4, 2) & 3))
        return fail(x, "PCI power", "the function is in D%u, not D0", cfg(x, pm + 4, 2) & 3);

    /* Which interrupt we were given: the enabled one of MSI-X / MSI. */
    uint32_t msix = pci_cap(x, 0x11), msi = pci_cap(x, 0x05);
    uint32_t mc_x = msix ? cfg(x, msix + 2, 2) : 0, mc = msi ? cfg(x, msi + 2, 2) : 0;
    if (mc_x & (1u << 15)) {
        x->msix = true;
        x->irq_vectors = (mc_x & 0x7ff) + 1;
    } else if (mc & 1) {
        x->msix = false;
        x->irq_vectors = 1u << ((mc >> 1) & 7);   /* capable of */
    } else {
        return fail(x, "PCI config", "neither MSI-X (%04x) nor MSI (%04x) is enabled", mc_x, mc);
    }
    drv_log("%04x:%04x rev %02x, command %04x, %s (MSI-X cap %s, MSI cap %s)", x->vendor,
            x->device, x->revision, cmd, x->msix ? "MSI-X on" : "MSI on",
            msix ? "yes" : "no", msi ? "yes" : "no");
    return 0;
}

/* ---- bring-up steps --------------------------------------------------------------- */

static int read_caps(struct xhc *x)
{
    uint32_t v = rd(x, CAP_CAPLENGTH);
    if (x->map_failed)
        return fail(x, "map BAR0", "offset %x: %s", x->map_fail_off, status_str(x->map_fail_st));
    if (v == 0xffffffffu)
        return fail(x, "capability registers", "BAR0 reads all ones (device not answering)");
    x->caplen = v & 0xff;
    x->hciver = v >> 16;
    x->hcs1 = rd(x, CAP_HCSPARAMS1);
    x->hcs2 = rd(x, CAP_HCSPARAMS2);
    x->hcc1 = rd(x, CAP_HCCPARAMS1);
    x->dboff = rd(x, CAP_DBOFF) & ~3u;
    x->rtsoff = rd(x, CAP_RTSOFF) & ~0x1fu;
    x->ports = x->hcs1 >> 24;
    x->slots = x->hcs1 & 0xff;
    x->scratchpads = (((x->hcs2 >> 21) & 0x1f) << 5) | (x->hcs2 >> 27);
    if (x->caplen < 0x20 || x->caplen >= PAGE || !x->dboff || !x->rtsoff)
        return fail(x, "capability registers", "CAPLENGTH %x DBOFF %x RTSOFF %x make no sense",
                    x->caplen, x->dboff, x->rtsoff);
    drv_log("xHCI %x.%02x: CAPLENGTH %x, %u ports, %u slots, %u interrupters, %u scratchpads, "
            "AC64 %u, CSZ %u, xECP %x, DBOFF %x, RTSOFF %x",
            x->hciver >> 8, x->hciver & 0xff, x->caplen, x->ports, x->slots,
            (x->hcs1 >> 8) & 0x7ff, x->scratchpads, x->hcc1 & 1, (x->hcc1 >> 2) & 1,
            (x->hcc1 >> 16) * 4, x->dboff, x->rtsoff);
    return 0;
}

/* The USB Legacy Support capability: take the controller from the
 * firmware and switch its SMIs off. */
static int bios_handoff(struct xhc *x)
{
    x->handoff = "none";
    uint32_t off = (x->hcc1 >> 16) * 4, protocols = 0;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t v = rd(x, off);
        if (x->map_failed) {
            /* Not fatal: the legacy capability comes first in practice. */
            drv_log("extended capabilities: can't map offset %x (%s); stopped walking",
                    x->map_fail_off, status_str(x->map_fail_st));
            x->map_failed = false;
            break;
        }
        if (v == 0xffffffffu)
            break;
        uint32_t id = v & 0xff, next = (v >> 8) & 0xff;
        if (id == XCAP_PROTOCOL) {
            uint32_t name = rd(x, off + 8);
            drv_log("supported protocol: USB %x.%02x, ports %u-%u", v >> 24, (v >> 16) & 0xff,
                    name & 0xff, (name & 0xff) + ((name >> 8) & 0xff) - 1);
            protocols++;
        }
        if (id == XCAP_LEGACY) {
            uint32_t ctl = rd(x, off + 4);
            drv_log("legacy support at %x: USBLEGSUP %08x (BIOS %s, OS %s), USBLEGCTLSTS %08x",
                    off, v, v & LEG_BIOS_OWNED ? "owns it" : "-", v & LEG_OS_OWNED ? "owns it" : "-",
                    ctl);
            uint64_t t0 = drv_clock_ns();
            /* The OS Owned byte alone: a dword write could put back a BIOS
             * Owned bit the firmware clears meanwhile. */
            drv_write8(reg(x, off + 3), 0, 1);
            bool released = false;
            while (drv_clock_ns() - t0 < 1000 * MS) {
                if (!(rd(x, off) & LEG_BIOS_OWNED)) {
                    released = true;
                    break;
                }
                drv_sleep_until(drv_clock_ns() + 1 * MS);
            }
            x->handoff_ms = (drv_clock_ns() - t0) / MS;
            if (released) {
                x->handoff = v & LEG_BIOS_OWNED ? "ok" : "ok (not BIOS-owned)";
            } else {
                /* Firmware that never answers (Linux does the same): take it. */
                x->handoff = "timeout";
                drv_log("BIOS did not release the controller in 1 s; clearing BIOS Owned");
                drv_write8(reg(x, off + 2), 0, 0);
            }
            ctl = rd(x, off + 4);
            wr(x, off + 4, (ctl & ~(LEGCTL_SMI_ENABLES | LEGCTL_SMI_STATUS)) | LEGCTL_SMI_STATUS);
            drv_log("handoff %s after %lu ms: USBLEGSUP %08x, USBLEGCTLSTS %08x -> %08x",
                    x->handoff, (unsigned long)x->handoff_ms, rd(x, off), ctl, rd(x, off + 4));
        }
        if (!next)
            break;
        off += next * 4;
    }
    if (!protocols)
        drv_log("no supported-protocol capability found");
    return 0;
}

static int stop(struct xhc *x, const char *step)
{
    uint32_t cmd = op_rd(x, OP_USBCMD), sts;
    op_wr(x, OP_USBCMD, cmd & ~(CMD_RS | CMD_INTE | CMD_HSEE));
    if (!wait_op(x, OP_USBSTS, STS_HCH, STS_HCH, 100, &sts))
        return fail(x, step, "no HCH 100 ms after RS=0 (USBCMD was %08x, USBSTS %08x)", cmd, sts);
    return 0;
}

static int reset(struct xhc *x, const char *step)
{
    uint32_t v;
    if (!wait_op(x, OP_USBSTS, STS_CNR, 0, 1000, &v))
        return fail(x, step, "Controller Not Ready 1 s before reset (USBSTS %08x)", v);
    op_wr(x, OP_USBCMD, CMD_HCRST);
    /* Some Intel controllers must not be touched right after HCRST. */
    drv_sleep_until(drv_clock_ns() + 1 * MS);
    if (!wait_op(x, OP_USBCMD, CMD_HCRST, 0, 1000, &v))
        return fail(x, step, "HCRST still set 1 s after reset (USBCMD %08x)", v);
    if (!wait_op(x, OP_USBSTS, STS_CNR, 0, 1000, &v))
        return fail(x, step, "Controller Not Ready 1 s after reset (USBSTS %08x)", v);
    return 0;
}

/* Allocate, pin and map the DMA memory; program DCBAAP, CRCR and
 * interrupter 0. */
static int setup_memory(struct xhc *x)
{
    uint32_t ps = op_rd(x, OP_PAGESIZE) & 0xffff;
    x->pagesize = ps ? (1u << (__builtin_ctz(ps) + 12)) : 0;
    if (x->pagesize != PAGE)
        return fail(x, "PAGESIZE", "controller page size %u (register %x); only 4 KiB is supported",
                    x->pagesize, ps);

    uint32_t sp_array_pages = (x->scratchpads * 8 + PAGE - 1) / PAGE;
    x->ctx_pages = DMA_SPARRAY / PAGE + sp_array_pages;
    uint64_t len = (uint64_t)x->ctx_pages * PAGE;
    status_t st = drv_vmo_create(len, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &x->ctx_vmo);
    if (st != OK)
        return fail(x, "DMA memory", "contiguous DMA32 VMO of %u pages: %s", x->ctx_pages,
                    status_str(st));
    uint64_t addrs[8];
    if (x->ctx_pages > 8)
        return fail(x, "DMA memory", "%u scratchpads need too big an array", x->scratchpads);
    st = drv_vmo_pin(x->ctx_vmo, x->dma, 0, len, addrs, &x->ctx_pin);
    if (st != OK)
        return fail(x, "DMA memory", "pin: %s", status_str(st));
    x->ctx_pinned = true;
    x->ctx_dev = addrs[0];
    for (uint32_t i = 1; i < x->ctx_pages; i++)
        if (addrs[i] != x->ctx_dev + (uint64_t)i * PAGE)
            return fail(x, "DMA memory", "contiguous VMO pinned as scattered pages");
    if (x->ctx_dev + len > (1ull << 32))
        return fail(x, "DMA memory", "DMA32 memory at %lx is above 4 GiB", x->ctx_dev);
    void *p;
    st = drv_vmo_map(x->ctx_vmo, 0, len, VMAR_READ | VMAR_WRITE, &p);
    if (st != OK)
        return fail(x, "DMA memory", "map: %s", status_str(st));
    x->ctx = p;
    for (uint64_t i = 0; i < len; i++)
        x->ctx[i] = 0;

    volatile uint64_t *dcbaa = (volatile uint64_t *)(x->ctx + DMA_DCBAA);
    if (x->scratchpads) {
        uint64_t splen = (uint64_t)x->scratchpads * PAGE;
        st = drv_vmo_create(splen, DRV_VMO_DMA32, &x->sp_vmo);
        if (st != OK)
            return fail(x, "scratchpads", "VMO of %u pages: %s", x->scratchpads, status_str(st));
        uint64_t *sp = drv_malloc(x->scratchpads * sizeof(uint64_t));
        if (!sp)
            return fail(x, "scratchpads", "no memory for %u addresses", x->scratchpads);
        st = drv_vmo_pin(x->sp_vmo, x->dma, 0, splen, sp, &x->sp_pin);
        if (st != OK) {
            drv_free(sp);
            return fail(x, "scratchpads", "pin %u pages: %s", x->scratchpads, status_str(st));
        }
        x->sp_pinned = true;
        volatile uint64_t *arr = (volatile uint64_t *)(x->ctx + DMA_SPARRAY);
        for (uint32_t i = 0; i < x->scratchpads; i++)
            arr[i] = sp[i];
        drv_free(sp);
        dcbaa[0] = x->ctx_dev + DMA_SPARRAY;
    }

    /* Command ring: Link TRB at the end back to the start, toggle cycle. */
    struct trb *cr = (struct trb *)(x->ctx + DMA_CMDRING);
    uint64_t cr_dev = x->ctx_dev + DMA_CMDRING;
    cr[RING_TRBS - 1].d0 = lo32(cr_dev);
    cr[RING_TRBS - 1].d1 = hi32(cr_dev);
    cr[RING_TRBS - 1].d3 = TRB_TYPE(TRB_LINK) | TRB_TC;   /* C = 0: not the xHC's yet */
    x->cmd_enq = 0;
    x->cmd_cycle = 1;

    /* Event ring segment table: one segment of RING_TRBS. */
    volatile uint32_t *erst = (volatile uint32_t *)(x->ctx + DMA_ERST);
    uint64_t ev_dev = x->ctx_dev + DMA_EVRING;
    erst[0] = lo32(ev_dev);
    erst[1] = hi32(ev_dev);
    erst[2] = RING_TRBS;
    erst[3] = 0;
    x->ev_deq = 0;
    x->ev_cycle = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint32_t slots = x->slots < MAX_SLOTS ? x->slots : MAX_SLOTS;
    op_wr(x, OP_CONFIG, (op_rd(x, OP_CONFIG) & ~0xffu) | slots);
    op_wr64(x, OP_DCBAAP, x->ctx_dev + DMA_DCBAA);
    op_wr64(x, OP_CRCR, cr_dev | CRCR_RCS);

    /* Interrupter 0, in the spec's order: size, dequeue pointer, base. */
    ir_wr(x, IR_ERSTSZ, (ir_rd(x, IR_ERSTSZ) & ~0xffffu) | 1);
    ir_wr64(x, IR_ERDP, ev_dev);
    ir_wr64(x, IR_ERSTBA, x->ctx_dev + DMA_ERST);
    ir_wr(x, IR_IMOD, IMOD_40US);
    ir_wr(x, IR_IMAN, IMAN_IE | IMAN_IP);   /* enable; clear a stale pending */
    if (x->map_failed)
        return fail(x, "map registers", "offset %x: %s", x->map_fail_off, status_str(x->map_fail_st));
    drv_log("DMA area at %lx (%u pages), %u scratchpads, MaxSlotsEn %u", x->ctx_dev, x->ctx_pages,
            x->scratchpads, slots);
    return 0;
}

static int run(struct xhc *x)
{
    op_wr(x, OP_USBSTS, STS_HSE | STS_EINT | STS_PCD);   /* RW1C leftovers */
    op_wr(x, OP_USBCMD, CMD_RS | CMD_INTE | CMD_HSEE);
    uint32_t sts;
    if (!wait_op(x, OP_USBSTS, STS_HCH, 0, 100, &sts))
        return fail(x, "run", "HCH still set 100 ms after RS=1 (USBSTS %08x)", sts);
    return 0;
}

/* Consume every event the xHC has posted. Returns true if it held the
 * completion of the command at cmd_dev (with *cc its completion code). */
static bool drain_events(struct xhc *x, uint64_t cmd_dev, uint32_t *cc)
{
    volatile struct trb *ev = (volatile struct trb *)(x->ctx + DMA_EVRING);
    bool found = false;
    for (int guard = 0; guard < RING_TRBS; guard++) {
        volatile struct trb *e = &ev[x->ev_deq];
        uint32_t d3 = e->d3;
        if ((d3 & TRB_C) != x->ev_cycle)
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t type = TRB_TYPE_OF(d3);
        uint64_t ptr = e->d0 | (uint64_t)e->d1 << 32;
        uint32_t status = e->d2;
        if (type == TRB_CMD_DONE_EV && ptr == cmd_dev) {
            found = true;
            *cc = status >> 24;
        } else if (type == TRB_PORT_EV) {
            x->other_events++;
            drv_log("event: port %u status change", (uint32_t)(ptr >> 24) & 0xff);
        } else {
            x->other_events++;
            drv_log("event: type %u, pointer %lx, status %08x", type, (unsigned long)ptr, status);
        }
        if (++x->ev_deq == RING_TRBS) {
            x->ev_deq = 0;
            x->ev_cycle ^= 1;
        }
    }
    /* Hand the consumed TRBs back and clear Event Handler Busy (RW1C). */
    uint64_t deq = x->ctx_dev + DMA_EVRING + (uint64_t)x->ev_deq * sizeof(struct trb);
    ir_wr64(x, IR_ERDP, deq | ERDP_EHB);
    return found;
}

/* Right after RS=1 a real controller reports its connected ports (Port
 * Status Change events, with an interrupt). Take those first, so the No-Op
 * latencies measure only the No-Ops. */
static void settle(struct xhc *x)
{
    uint64_t end = drv_clock_ns() + 20 * MS;
    uint32_t packets = 0, cc;
    for (;;) {
        struct port_packet pkt;
        if (drv_port_wait(x->port, end, &pkt) != OK)
            break;
        packets++;
        drv_interrupt_ack(x->irq);
        ir_wr(x, IR_IMAN, IMAN_IE | IMAN_IP);
        op_wr(x, OP_USBSTS, op_rd(x, OP_USBSTS) & (STS_EINT | STS_PCD));
        drain_events(x, 0, &cc);
    }
    if (packets)
        drv_log("%u interrupt(s) in the first 20 ms (%u events)", packets, x->other_events);
}

/* One No Op command: ring doorbell 0, wait for its completion event via
 * the interrupt. */
static int noop(struct xhc *x, unsigned i)
{
    struct trb *cr = (struct trb *)(x->ctx + DMA_CMDRING);
    uint64_t cmd_dev = x->ctx_dev + DMA_CMDRING + (uint64_t)x->cmd_enq * sizeof(struct trb);
    volatile struct trb *t = &cr[x->cmd_enq];
    t->d0 = 0;
    t->d1 = 0;
    t->d2 = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->d3 = TRB_TYPE(TRB_NOOP_CMD) | x->cmd_cycle;   /* the cycle bit last: now it's the xHC's */
    if (++x->cmd_enq == RING_TRBS - 1) {
        /* At the Link TRB: give it to the xHC and wrap. */
        volatile struct trb *l = &cr[RING_TRBS - 1];
        l->d3 = (l->d3 & ~TRB_C) | x->cmd_cycle;
        x->cmd_enq = 0;
        x->cmd_cycle ^= 1;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint64_t t0 = drv_clock_ns(), deadline = t0 + 1000 * MS;
    wr(x, x->dboff, 0);   /* doorbell 0, target 0: the command ring */
    uint32_t packets = 0;
    for (;;) {
        struct port_packet pkt;
        status_t st = drv_port_wait(x->port, deadline, &pkt);
        uint64_t t1 = drv_clock_ns();
        if (st == ERR_TIMED_OUT) {
            /* Say whether the command completed at all (interrupt lost)
             * or never ran. */
            uint32_t cc = 0;
            bool done = drain_events(x, cmd_dev, &cc);
            return fail(x, "No-Op", "#%u: no interrupt within 1 s (%u packets); completion %s "
                        "in the event ring", i + 1, packets, done ? "IS" : "not");
        }
        if (st != OK)
            return fail(x, "No-Op", "#%u: port wait: %s", i + 1, status_str(st));
        packets++;
        /* Re-arm first: a fire from here on is a new packet (the MSI may be
         * a plain edge MSI that the kernel doesn't mask). */
        drv_interrupt_ack(x->irq);
        ir_wr(x, IR_IMAN, IMAN_IE | IMAN_IP);
        uint32_t sts = op_rd(x, OP_USBSTS);
        op_wr(x, OP_USBSTS, sts & (STS_EINT | STS_PCD));
        if (sts & (STS_HSE | STS_HCE))
            return fail(x, "No-Op", "#%u: host %s error (USBSTS %08x)", i + 1,
                        sts & STS_HCE ? "controller" : "system", sts);
        uint32_t cc = 0;
        if (drain_events(x, cmd_dev, &cc)) {
            if (cc != CC_SUCCESS)
                return fail(x, "No-Op", "#%u: completion code %u (want 1, Success)", i + 1, cc);
            x->lat_ns[i] = t1 - t0;
            return 0;
        }
        if (t1 > deadline)
            return fail(x, "No-Op", "#%u: %u interrupts in 1 s but no completion for it", i + 1,
                        packets);
    }
}

/* Leave the controller quiet: interrupter off, stopped, reset (which also
 * forgets every DMA pointer we gave it). */
static int shutdown(struct xhc *x)
{
    /* Never touched it: page 0 (capability + operational registers) isn't
     * mapped. */
    if (!x->caplen || !x->nmap || x->map[0].page != 0)
        return 0;
    if (x->rtsoff)
        ir_wr(x, IR_IMAN, IMAN_IP);   /* IE = 0 */
    /* A later page that failed to map (the doorbells, say, or that one)
     * must not skip the halt: the controller may be running on our DMA
     * memory, which release() frees once this returns 0. The operational
     * registers are on page 0. (Review of M6 phase 2.) */
    x->map_failed = false;
    int r = stop(x, "final halt");
    return r ? r : reset(x, "final reset");
}

/* quiet: the controller is known to be halted and reset, so its DMA
 * memory can go. If not, the pins stay until we exit: the kernel closes
 * the DMA capability (Bus Master Enable off) before it drops them. */
static void release(struct xhc *x, bool quiet)
{
    if (x->ctx_pinned && quiet)
        drv_vmo_unpin(x->ctx_vmo, x->dma, x->ctx_pin);
    if (x->sp_pinned && quiet)
        drv_vmo_unpin(x->sp_vmo, x->dma, x->sp_pin);
    if (x->ctx)
        drv_vmo_unmap(x->ctx, (uint64_t)x->ctx_pages * PAGE);
    if (x->ctx_vmo != HANDLE_INVALID)
        drv_handle_close(x->ctx_vmo);
    if (x->sp_vmo != HANDLE_INVALID)
        drv_handle_close(x->sp_vmo);
    for (unsigned i = 0; i < x->nmap; i++)
        drv_vmo_unmap((void *)x->map[i].va, PAGE);
    x->nmap = 0;
}

static int bring_up(struct xhc *x)
{
    int r;
    if ((r = check_pci(x)) || (r = read_caps(x)) || (r = bios_handoff(x)))
        return r;
    /* Controller Not Ready: firmware may still be finishing. */
    uint32_t sts;
    if (!wait_op(x, OP_USBSTS, STS_CNR, 0, 1000, &sts))
        return fail(x, "start", "Controller Not Ready still set after 1 s (USBSTS %08x)", sts);
    if ((r = stop(x, "halt")) || (r = reset(x, "reset")))
        return r;
    /* Quiet now (halted and reset: it holds no DMA pointer of anyone's):
     * bus mastering on, for our DMA and the MSI. */
    status_t bm = drv_dma_bus_master(x->dma, 1);
    if (bm != OK)
        return fail(x, "bus master", "can't turn it on (%s)", status_str(bm));
    if ((r = setup_memory(x)))
        return r;
    status_t st = drv_port_create(&x->port);
    if (st == OK)
        st = drv_port_bind(x->port, x->irq, 0x7a60, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st != OK)
        return fail(x, "interrupt", "port for DR_IRQ(0): %s", status_str(st));
    drv_interrupt_ack(x->irq);   /* start from a clean, unmasked state */
    if ((r = run(x)))
        return r;
    settle(x);
    for (unsigned i = 0; i < NOOPS; i++)
        if ((r = noop(x, i)))
            return r;
    return 0;
}

int driver_main(const struct driver_start *s)
{
    struct xhc *x = drv_malloc(sizeof(*x));
    if (!x)
        return 2;
    for (size_t i = 0; i < sizeof(*x); i++)
        ((uint8_t *)x)[i] = 0;
    x->name = s->name;
    x->dev = drv_handle(s, DR_PCIDEV);
    x->bar = drv_handle(s, DR_BAR(0));
    x->irq = drv_handle(s, DR_IRQ(0));
    x->dma = drv_handle(s, DR_DMA);
    x->port = x->ctx_vmo = x->sp_vmo = HANDLE_INVALID;
    if (x->dev == HANDLE_INVALID || x->bar == HANDLE_INVALID || x->irq == HANDLE_INVALID ||
        x->dma == HANDLE_INVALID) {
        drv_report("missing handles: DR_PCIDEV %s, DR_BAR(0) %s, DR_IRQ(0) %s, DR_DMA %s",
                   x->dev == HANDLE_INVALID ? "no" : "yes", x->bar == HANDLE_INVALID ? "no" : "yes",
                   x->irq == HANDLE_INVALID ? "no" : "yes", x->dma == HANDLE_INVALID ? "no" : "yes");
        drv_free(x);
        return 2;
    }

    int r = bring_up(x);
    int q = shutdown(x);
    if (r == 0)
        r = q;
    if (r == 0) {
        uint64_t *l = x->lat_ns;
        for (int a = 0; a < NOOPS; a++)   /* sort the three */
            for (int b = a + 1; b < NOOPS; b++)
                if (l[b] < l[a]) {
                    uint64_t t = l[a];
                    l[a] = l[b];
                    l[b] = t;
                }
        drv_report("%04x:%04x rev %02x, %u ports, %u scratchpads, %s vector 0 of %u, BIOS handoff %s",
                   x->vendor, x->device, x->revision, x->ports, x->scratchpads,
                   x->msix ? "MSI-X" : "MSI", x->irq_vectors, x->handoff);
        drv_report("%u No-Op commands completed via interrupt, latency min/median/max %lu/%lu/%lu us",
                   NOOPS, (unsigned long)(l[0] / US), (unsigned long)(l[NOOPS / 2] / US),
                   (unsigned long)(l[NOOPS - 1] / US));
        if (x->other_events)
            drv_log("%u other events (port status changes) seen and skipped", x->other_events);
    }
    if (x->port != HANDLE_INVALID)
        drv_handle_close(x->port);
    release(x, q == 0);
    drv_free(x);
    return r;
}
