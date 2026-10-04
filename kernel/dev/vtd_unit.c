/* The VT-d remapping units (see vtd_internal.h for the model): the units
 * the probe found and mapped, the boot words, starting them, their
 * registers and Global Commands, the write-buffer flush, cache-line
 * flushes for tables the unit reads without snooping, and the callbacks
 * vtd_pt and vtd_ir take.
 *
 * Off by default: without the boot word `iommu=on` nothing here writes a
 * VT-d register; the probe's lines are the only trace of VT-d in a boot.
 * With it, each unit the probe could read gets its invalidation queue
 * (vtd_qi.c) and its fault interrupt (vtd_fault.c), in that order, so a
 * queue error found while the fault interrupt is set up is already
 * handled. A unit that fails to start is reported and left alone; the
 * others start anyway. Then interrupt remapping is turned on (vtd_irq.c),
 * only if every unit started.
 *
 * The units' register pages stay the kernel's (vtd_regs_overlap): every
 * unit the DMAR table lists, whether it answered or not. */
#include <jam/cmdline.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/vtd.h>
#include <jam/x86.h>

#include "vtd_internal.h"
#include "vtd_irq.h"

static struct vtd_unit units[VTD_MAX_UNITS];   /* by the DMAR's unit number */
static bool mapped[VTD_MAX_UNITS];             /* the probe read this unit's registers */

/* Every unit's register set, from the DMAR table: written at boot by the
 * probe, before user space, read-only afterwards. */
static struct {
    uint64_t base, len;
} regs_range[VTD_MAX_UNITS];
static uint32_t nranges;

/* ---- what the probe found -------------------------------------------------------- */

void vtd_unit_found(uint32_t index, uint64_t base, uint64_t len)
{
    if (index >= VTD_MAX_UNITS || nranges >= VTD_MAX_UNITS)
        return;
    units[index].base = base;
    regs_range[nranges].base = base;
    regs_range[nranges].len = len;
    nranges++;
}

void vtd_unit_mapped(uint32_t index, volatile uint8_t *regs, uint64_t span, unsigned haw)
{
    if (index >= VTD_MAX_UNITS)
        return;
    struct vtd_unit *u = &units[index];
    u->index = index;
    u->regs = regs;
    u->span = span;
    u->haw = haw;
    u->ver = vtd_rd32(u, VTD_VER);
    u->cap = vtd_rd64(u, VTD_CAP);
    u->ecap = vtd_rd64(u, VTD_ECAP);
    mapped[index] = true;
}

bool vtd_regs_overlap(uint64_t phys, uint64_t len)
{
    for (uint32_t i = 0; i < nranges; i++) {
        uint64_t b = regs_range[i].base, l = regs_range[i].len;
        if (phys < b + l && b < phys + len)
            return true;
    }
    return false;
}

struct vtd_unit *vtd_unit_get(uint32_t i)
{
    if (i >= VTD_MAX_UNITS || !__atomic_load_n(&units[i].started, __ATOMIC_ACQUIRE))
        return NULL;
    return &units[i];
}

/* ---- registers --------------------------------------------------------------------- */

uint32_t vtd_rd32(const struct vtd_unit *u, uint32_t off)
{
    return *(volatile uint32_t *)(u->regs + off);
}

uint64_t vtd_rd64(const struct vtd_unit *u, uint32_t off)
{
    return *(volatile uint64_t *)(u->regs + off);
}

void vtd_wr32(const struct vtd_unit *u, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(u->regs + off) = v;
}

void vtd_wr64(const struct vtd_unit *u, uint32_t off, uint64_t v)
{
    *(volatile uint64_t *)(u->regs + off) = v;
}

status_t vtd_gcmd(struct vtd_unit *u, uint32_t bit, bool on, uint32_t status)
{
    /* 11.4.4.1: read GSTS, drop the one-shot bits, change one bit, write
     * GCMD, wait for GSTS to show it. One command at a time (6.9). */
    uint64_t f = spin_lock_irqsave(&u->gcmd_lock);
    uint32_t cmd = vtd_rd32(u, VTD_GSTS) & VTD_GSTS_KEEP;
    vtd_wr32(u, VTD_GCMD, on ? cmd | bit : cmd & ~bit);
    uint64_t deadline = uptime_ns() + VTD_REG_WAIT_NS;
    uint32_t g;
    while (((g = vtd_rd32(u, VTD_GSTS)) & status) != (on ? status : 0)) {
        if (uptime_ns() > deadline) {
            spin_unlock_irqrestore(&u->gcmd_lock, f);
            kprintf("vtd: unit %u: command %08x (%s) not done after %u ms: gsts %08x\n",
                    u->index, bit, on ? "on" : "off", VTD_WAIT_MS, g);
            return ERR_TIMED_OUT;
        }
        cpu_relax();
    }
    spin_unlock_irqrestore(&u->gcmd_lock, f);
    return OK;
}

status_t vtd_unit_flush_write_buffer(struct vtd_unit *u)
{
    if (!VTD_CAP_RWBF(u->cap))
        return OK;
    /* 6.8, 11.4.4.1: WBF is a one-shot command; WBFS stays set while the
     * flush runs and clears when it is done. */
    uint64_t f = spin_lock_irqsave(&u->gcmd_lock);
    vtd_wr32(u, VTD_GCMD, (vtd_rd32(u, VTD_GSTS) & VTD_GSTS_KEEP) | VTD_GCMD_WBF);
    uint64_t deadline = uptime_ns() + VTD_REG_WAIT_NS;
    status_t st = OK;
    while (vtd_rd32(u, VTD_GSTS) & VTD_GSTS_WBFS) {
        if (uptime_ns() > deadline) {
            st = ERR_TIMED_OUT;
            break;
        }
        cpu_relax();
    }
    spin_unlock_irqrestore(&u->gcmd_lock, f);
    if (st != OK)
        kprintf("vtd: unit %u: the write-buffer flush did not finish\n", u->index);
    return st;
}

/* ---- cache lines ------------------------------------------------------------------- */

/* CPUID 7.EBX[23] (CLFLUSHOPT) and the flush line size, CPUID 1.EBX[15:8]
 * times 8; read on first use. */
static int flush_opt = -1;
static unsigned flush_line = 64;

void vtd_flush_lines(const void *va, size_t len)
{
    if (__atomic_load_n(&flush_opt, __ATOMIC_RELAXED) < 0) {
        uint32_t a, b, c, d;
        cpuid(1, 0, &a, &b, &c, &d);
        unsigned line = ((b >> 8) & 0xff) * 8;
        if (line >= 16 && !(line & (line - 1)))
            flush_line = line;
        cpuid(7, 0, &a, &b, &c, &d);
        __atomic_store_n(&flush_opt, (b >> 23) & 1, __ATOMIC_RELAXED);
    }
    uintptr_t p = (uintptr_t)va & ~(uintptr_t)(flush_line - 1);
    uintptr_t end = (uintptr_t)va + len;
    /* Both are ordered after this CPU's earlier stores to the line;
     * SFENCE orders CLFLUSHOPT before whatever tells the unit to look. */
    for (; p < end; p += flush_line) {
        if (flush_opt)
            __asm__ volatile("clflushopt (%0)" ::"r"(p) : "memory");
        else
            __asm__ volatile("clflush (%0)" ::"r"(p) : "memory");
    }
    __asm__ volatile("sfence" ::: "memory");
}

/* ---- the callbacks for vtd_pt and vtd_ir ----------------------------------------- */

static uint64_t pt_alloc_page(void *ctx)
{
    (void)ctx;
    return pmm_alloc_page_phys(0);
}

static void pt_free_page(void *ctx, uint64_t pa)
{
    (void)ctx;
    pmm_free_page_phys(pa);
}

static void pt_flush(void *ctx, const void *va, size_t len)
{
    (void)ctx;
    vtd_flush_lines(va, len);
}

const struct vtd_pt_ops vtd_unit_pt_ops = {
    .alloc_page = pt_alloc_page,
    .free_page = pt_free_page,
    .flush = pt_flush,
    .invalidate = vtd_unit_pt_invalidate,
};

static void ir_flush(void *ctx, const void *va, size_t len)
{
    const struct vtd_unit *u = ctx;
    if (!VTD_ECAP_C(u->ecap))   /* covers the interrupt remapping table too (11.4.3) */
        vtd_flush_lines(va, len);
}

static status_t ir_invalidate(void *ctx, uint32_t index, uint32_t count)
{
    return vtd_inv_iec_index(ctx, index, count);
}

const struct vtd_ir_ops vtd_unit_ir_ops = {
    .flush = ir_flush,
    .invalidate = ir_invalidate,
};

status_t vtd_unit_pt_geom(const struct vtd_unit *u, struct vtd_pt_geom *out)
{
    struct vtd_pt_geom g;
    status_t st = vtd_pt_geom_from_caps(u->cap, u->ecap, u->haw, &g);
    if (st != OK)
        return st;
    if (VTD_CAP_RWBF(u->cap))
        g.caching_mode = true;
    *out = g;
    return OK;
}

/* ---- the boot words and the start ------------------------------------------------- */

/* Is `word` a whole space-separated word of line? */
static bool has_word(const char *line, const char *word)
{
    size_t wl = strlen(word);
    for (const char *p = line; *p; p++) {
        if (p != line && p[-1] != ' ')
            continue;
        size_t i = 0;
        while (i < wl && p[i] == word[i])
            i++;
        if (i == wl && (p[wl] == ' ' || p[wl] == '\0'))
            return true;
    }
    return false;
}

bool vtd_iommu_wanted(const char *cmdline)
{
    return has_word(cmdline, "iommu=on") && !has_word(cmdline, "iommu=off");
}

/* The queue's first batch: every cache dropped, globally (context cache,
 * then the IOTLB, as 6.5.2.1 asks, then the interrupt entry cache), and
 * timed. It proves the queue works before anything relies on it, and it
 * leaves an invalidation wait as the last descriptor the unit completed,
 * which is what turning the queue off later requires (6.5.2): a kexec'd
 * kernel that takes the queue over can then turn it off, even when this
 * kernel never used it again. */
static status_t first_batch(struct vtd_unit *u)
{
    struct vtd_desc d[3] = {
        vtd_desc_cc(VTD_CC_GLOBAL, 0, 0, 0),
        vtd_desc_iotlb(VTD_IOTLB_GLOBAL, 0, 0, 0, 0, false),
        vtd_desc_iec(true, 0, 0),
    };
    uint64_t t0 = uptime_ns();
    status_t st = vtd_qi_submit(u, d, VTD_ECAP_IR(u->ecap) ? 3 : 2);
    uint64_t ns = uptime_ns() - t0;
    if (st == OK)
        kprintf("vtd:         unit %u: first invalidations (context, IOTLB%s; global) done in "
                "%lu.%03lu us\n", u->index, VTD_ECAP_IR(u->ecap) ? ", interrupt entries" : "",
                (uint64_t)(ns / NS_PER_US), (uint64_t)(ns % NS_PER_US));
    return st;
}

static status_t start_unit(struct vtd_unit *u)
{
    spin_init(&u->gcmd_lock, "vtd gcmd");
    status_t st = vtd_qi_init(u);
    if (st == OK)
        st = first_batch(u);
    if (st != OK) {
        report("vtd: unit %u: %s: the unit is left alone", u->index,
               st == ERR_NOT_SUPPORTED ? "no queued invalidation"
               : st == ERR_BAD_STATE   ? "a queue left on that does not drain"
               : st == ERR_NO_MEMORY   ? "no memory for its queue"
               : st == ERR_IO          ? "its first invalidations were refused"
                                       : "its queue did not start");
        return st;
    }
    st = vtd_fault_init(u);
    if (st != OK) {
        /* The queue runs; faults just aren't reported. */
        report("vtd: unit %u: no fault interrupt (%d): faults are not reported", u->index, st);
    }
    __atomic_store_n(&u->started, true, __ATOMIC_RELEASE);
    kprintf("vtd:         unit %u: started: invalidation queue at %lx (%u entries), fault "
            "interrupt %s cpu %u vector %x\n", u->index, u->queue_phys, VTD_QI_ENTRIES,
            st == OK ? "on" : "OFF", u->fault_cpu, u->fault_vec);
    return OK;
}

void vtd_units_start(void)
{
    const char *line = cmdline_get();
    if (!vtd_iommu_wanted(line)) {
        if (has_word(line, "iommu=off"))
            kprintf("vtd:         iommu=off: the units are left alone\n");
        return;
    }
    uint32_t n = 0, ok = 0;
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++) {
        if (!mapped[i])
            continue;
        n++;
        ok += start_unit(&units[i]) == OK;
    }
    kprintf("vtd:         iommu=on: %u of %u unit%s started (queue and faults)\n", ok, n,
            n == 1 ? "" : "s");
    if (n == 0)
        report("vtd: iommu=on, but there is no VT-d unit to start");
    else
        vtd_irq_start(ok, n);   /* interrupt remapping (vtd_irq.c) */
}
