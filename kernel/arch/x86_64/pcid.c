/* PCIDs. Without them every CR3 load drops the whole non-global
 * TLB, so a process->process call refills the TLB twice per round trip and
 * a CPU that goes idle between calls refills it on the way back.
 *
 * Design: per-CPU PCID slots with generations, lazily flushed, the way
 * Linux does it. PCID 0 is the kernel's own tables (kernel threads, idle)
 * and everything while the switch is off. Slots 0..PCID_SLOTS-1 are PCIDs
 * 1..PCID_SLOTS. A slot records the id of the address space it holds and
 * the TLB generation that address space had when the slot was last
 * loaded with a flush (or found current). On a load:
 *   - a slot with this id and the current generation: CR3 with NOFLUSH,
 *     the entries tagged with that PCID are still right;
 *   - a slot with this id and an older generation: CR3 without NOFLUSH,
 *     which flushes exactly that PCID's entries (and paging-structure
 *     caches), then record the generation;
 *   - no slot: take the next victim round robin, flush-load it.
 * Address-space ids are 64-bit and never reused, so a slot can never be
 * mistaken for a new address space at a recycled address; the entries of
 * a dead address space just wait in their slot until it is reused, and a
 * CPU never walks or uses entries of a PCID it hasn't loaded.
 *
 * Invalidation (without PCIDs the rule is "a CPU not in the active mask
 * has loaded another CR3 since, which dropped every non-global entry": with
 * PCIDs it didn't). An unmap/decommit/protect clears entries, then (aspace.c,
 * gather_note) increments the address space's generation with a locked,
 * sequentially consistent RMW, then reads the active mask and shoots down
 * exactly those CPUs. A switch-in sets its active bit (a seq_cst RMW),
 * then reads the generation (here), then loads CR3. Dekker again: either
 * the unmapping CPU sees the active bit (and the IPI flushes the range on
 * that CPU while it runs the address space; if it has already switched
 * away, its slot generation is older than the new one and its next load
 * flushes), or the switching CPU reads the new generation and flushes on
 * this very load. A CPU that ran the address space earlier and has since
 * switched away is not interrupted at all: its next load of it flushes.
 * So after the shootdown, no CPU can USE a stale entry, which is what
 * freeing the pages (tlb_gather_finish) needs. Kernel entries are global,
 * so kernel shootdowns (invlpg: global entries are dropped whatever the
 * current PCID; over 64 pages CR4.PGE is toggled, which drops every PCID)
 * are unaffected; the kernel's paging-structure entries above the leaves
 * never change once present (vmm.c creates all kernel PDPTs up front and
 * never frees kernel tables). PCIDs need PGE for that reason.
 *
 * INVPCID is not used: invalidating a PCID that isn't loaded is deferred
 * to its next load, which only needs CR3's NOFLUSH bit.
 *
 * The kernel shootdown above leans on invlpg dropping global entries, and
 * on some Intel CPUs it may not while PCIDs are on (Intel's specification
 * updates, errata ADL063 and RPL042: "INVLPG may not flush global
 * translations when PCIDs are enabled"). A stale global entry is a kernel
 * address still reaching a freed page. Microcode fixes it; pcid_decide
 * leaves PCIDs off on an affected model that runs an older revision, as
 * Linux does (arch/x86/mm/init.c, invlpg_miss_ids: the models and first
 * fixed revisions in erratum[] are that table's).
 *
 * The run-time switch (the benchmark) bumps an epoch; each CPU compares it
 * on every load and, when it changed, forgets every slot and flush-loads,
 * so entries cached under the other setting are never used.
 *
 * Finding the slot: each CPU remembers, per address-space id modulo
 * PCID_HINTS, the slot that id last had here. A load checks that slot
 * first and searches all of them only when the hint is wrong (another id
 * with the same remainder came since, or the slot was reused): a server
 * and its clients switching back and forth on one CPU never search. */
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/pathstat.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/string.h>
#include <jam/x86.h>

#define PCID_SLOTS 8
#define PCID_HINTS 16   /* hint entries per CPU, indexed by id % PCID_HINTS */

struct pcid_cpu {
    uint64_t id[PCID_SLOTS];   /* address-space id each slot holds, 0 = empty */
    uint64_t gen[PCID_SLOTS];  /* its TLB generation when last loaded */
    uint32_t victim;           /* round-robin cursor for the next slot to reuse */
    uint32_t epoch;            /* last epoch seen: a change forgets every slot */
    /* PCID 0 may hold user entries: a user address space was loaded as
     * PCID 0 (the switch off). The next PCID-0 load with the switch on
     * must flush, or kernel threads would keep translations to user pages
     * that later unmaps no longer shoot down here. */
    bool     zero_dirty;
    uint8_t  hint[PCID_HINTS]; /* id % PCID_HINTS -> the slot it last had (checked) */
    uint64_t flushed;          /* statistics: loads that flushed */
    uint64_t searched;         /* statistics: loads whose hint was wrong (searched) */
} __attribute__((aligned(64)));

static struct pcid_cpu pcpu[MAX_CPUS];
static int usable = -1;                   /* -1: not decided yet */
static bool on;                           /* the run-time switch (pcid_set) */
static uint32_t epoch = 1;                /* pcpu[].epoch starts at 0: a reset */
static uint64_t next_id = 1;

/* Family 6 models whose INVLPG may miss global entries with PCIDs on, and
 * the first microcode revision that fixes it. */
static const struct {
    uint32_t model;      /* CPUID model */
    uint32_t fixed;      /* PCIDs are safe from this revision on */
} erratum[] = {
    { 0x97, 0x2e },      /* Alder Lake */
    { 0x9a, 0x42c },     /* Alder Lake L */
    { 0xbe, 0x11 },      /* Gracemont (Alder Lake N) */
    { 0xb7, 0x118 },     /* Raptor Lake */
    { 0xba, 0x4117 },    /* Raptor Lake P */
    { 0xbf, 0x2e },      /* Raptor Lake S */
};

bool pcid_decide(const struct pcid_cpu_info *c, const char **why, uint32_t *fixed)
{
    *fixed = 0;
    for (unsigned i = 0; i < sizeof(erratum) / sizeof(erratum[0]); i++)
        if (c->intel && c->family == 6 && c->model == erratum[i].model)
            *fixed = erratum[i].fixed;
    if (!c->has_pcid || !c->has_pge) {
        *why = "the CPU has none";
        return false;
    }
    if (c->word_off) {
        *why = "boot word nopcid";
        return false;
    }
    if (c->word_on) {
        *why = "boot word forcepcid";
        return true;
    }
    if (*fixed && c->microcode < *fixed) {
        *why = "INVLPG erratum, microcode older than the fix (forcepcid overrides)";
        return false;
    }
    *why = *fixed ? "microcode has the INVLPG fix" : "not an affected CPU";
    return true;
}

static bool decide_here(const char **why, uint32_t *fixed)
{
    const struct pcid_cpu_info c = {
        .has_pcid = cpu_features.pcid, .has_pge = cpu_features.pge,
        .intel = !strcmp(cpu_features.vendor, "GenuineIntel"),
        .family = cpu_features.family, .model = cpu_features.model,
        .microcode = cpu_features.microcode,
        .word_off = cmdline_has("nopcid"), .word_on = cmdline_has("forcepcid"),
    };
    return pcid_decide(&c, why, fixed);
}

void pcid_report(void)
{
    const char *why;
    uint32_t fixed;
    bool use = decide_here(&why, &fixed);
    if (fixed)
        kprintf("pcid:        %s: %s (microcode %x, fixed in %x)\n", use ? "on" : "off", why,
                cpu_features.microcode, fixed);
    else
        kprintf("pcid:        %s: %s\n", use ? "on" : "off", why);
}

bool pcid_usable(void)
{
    if (__atomic_load_n(&usable, __ATOMIC_RELAXED) < 0) {
        const char *why;
        uint32_t fixed;
        int u = decide_here(&why, &fixed);
        __atomic_store_n(&usable, u, __ATOMIC_RELAXED);
        __atomic_store_n(&on, u, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&usable, __ATOMIC_RELAXED);
}

uint64_t pcid_new_id(void)
{
    return __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
}

void pcid_set(bool want)
{
    if (!pcid_usable())
        return;
    /* The epoch first: a CPU that sees the new setting (read before the
     * epoch in pcid_load) also sees the new epoch and forgets its slots. */
    __atomic_add_fetch(&epoch, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&on, want, __ATOMIC_SEQ_CST);
}

bool pcid_is_on(void)
{
    return pcid_usable() && __atomic_load_n(&on, __ATOMIC_SEQ_CST);
}

/* The slot that holds `id` on this CPU, or PCID_SLOTS: the hint first. */
static unsigned find_slot(struct pcid_cpu *pc, uint64_t id)
{
    unsigned h = pc->hint[id % PCID_HINTS];
    if (pc->id[h] == id)
        return h;
    pc->searched++;
    for (unsigned i = 0; i < PCID_SLOTS; i++)
        if (pc->id[i] == id)
            return i;
    return PCID_SLOTS;
}

/* The decision for one load on the CPU whose slots are *pc: the PCID to
 * use and whether its entries may be kept. `gen` is only read (after the
 * caller's active-bit RMW) when a user slot is involved. */
static uint32_t decide(struct pcid_cpu *pc, bool sw, uint64_t id, const uint64_t *gen,
                       bool *keep)
{
    uint32_t e = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
    bool reset = pc->epoch != e;
    if (reset) {
        for (unsigned i = 0; i < PCID_SLOTS; i++)
            pc->id[i] = 0;
        pc->epoch = e;
    }
    if (!sw || !id) {
        /* PCID 0. The kernel's tables have no user half, so with the
         * switch on nothing tagged 0 can be stale unless a user address
         * space was loaded as PCID 0 while the switch was off; off, every
         * load flushes, as without PCIDs. */
        if (!sw && id)
            pc->zero_dirty = true;
        *keep = sw && !reset && !pc->zero_dirty;
        if (sw)
            pc->zero_dirty = false;
        return 0;
    }
    uint64_t g = __atomic_load_n(gen, __ATOMIC_SEQ_CST);
    unsigned slot = find_slot(pc, id);
    *keep = slot < PCID_SLOTS && pc->gen[slot] == g;
    if (slot == PCID_SLOTS) {
        slot = pc->victim++ % PCID_SLOTS;
        pc->id[slot] = id;
    }
    pc->hint[id % PCID_HINTS] = (uint8_t)slot;
    pc->gen[slot] = g;
    if (!*keep)
        pc->flushed++;
    return slot + 1;
}

void pcid_load(uint64_t pml4, uint64_t id, const uint64_t *gen)
{
    if (__atomic_load_n(&usable, __ATOMIC_RELAXED) <= 0) {
        PATH_COUNT(PATH_CR3_FLUSH);   /* without PCIDs every load drops the user entries */
        write_cr3(pml4);
        return;
    }
    bool keep;
    bool sw = __atomic_load_n(&on, __ATOMIC_SEQ_CST);
    uint32_t pcid = decide(&pcpu[this_cpu()->index], sw, id, gen, &keep);
    if (!keep)
        PATH_COUNT(PATH_CR3_FLUSH);
    write_cr3(pml4 | pcid | (keep ? CR3_NOFLUSH : 0));
}

#ifndef JAM_NO_KTESTS
/* QEMU's TCG has no PCIDs, so the bookkeeping is tested on made-up CPUs:
 * what pcid_load would load (the PCID, plus PCID_TEST_KEEP when it keeps
 * the entries) on fake CPU `cpu` (< 4), as if PCIDs were in use and the
 * switch were `sw`. */
static struct pcid_cpu fake[4];

void pcid_test_reset(void)
{
    memset(fake, 0, sizeof(fake));
}

uint32_t pcid_test_decide(uint32_t cpu, uint64_t id, uint64_t gen, bool sw)
{
    bool keep;
    uint32_t pcid = decide(&fake[cpu % 4], sw, id, &gen, &keep);
    return pcid | (keep ? PCID_TEST_KEEP : 0);
}

uint64_t pcid_test_searches(uint32_t cpu)
{
    return fake[cpu % 4].searched;
}
#endif

uint64_t pcid_flushed_loads(uint32_t cpu)
{
    return cpu < MAX_CPUS ? __atomic_load_n(&pcpu[cpu].flushed, __ATOMIC_RELAXED) : 0;
}
