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
 * The run-time switch (the benchmark) bumps an epoch; each CPU compares it
 * on every load and, when it changed, forgets every slot and flush-loads,
 * so entries cached under the other setting are never used. */
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/x86.h>

#define PCID_SLOTS 8

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
    uint64_t kept, flushed;    /* statistics */
} __attribute__((aligned(64)));

static struct pcid_cpu pcpu[MAX_CPUS];
static volatile int usable = -1;          /* -1: not decided yet */
static volatile bool on;
static uint32_t epoch = 1;                /* pcpu[].epoch starts at 0: a reset */
static uint64_t next_id = 1;

bool pcid_usable(void)
{
    if (usable < 0) {
        usable = cpu_features.pcid && cpu_features.pge && !cmdline_has("nopcid");
        on = usable;
    }
    return usable;
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
    return pcid_usable() && on;
}

/* The decision for one load on the CPU whose slots are *pc: the PCID to
 * use and whether its entries may be kept. `gen` is only read (after the
 * caller's active-bit RMW) when a user slot is involved. */
static uint32_t decide(struct pcid_cpu *pc, bool sw, uint64_t id, const volatile uint64_t *gen,
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
    unsigned slot = PCID_SLOTS;
    for (unsigned i = 0; i < PCID_SLOTS; i++)
        if (pc->id[i] == id)
            slot = i;
    *keep = slot < PCID_SLOTS && pc->gen[slot] == g;
    if (slot == PCID_SLOTS) {
        slot = pc->victim++ % PCID_SLOTS;
        pc->id[slot] = id;
    }
    pc->gen[slot] = g;
    if (*keep)
        pc->kept++;
    else
        pc->flushed++;
    return slot + 1;
}

void pcid_load(uint64_t pml4, uint64_t id, const volatile uint64_t *gen)
{
    if (usable <= 0) {
        write_cr3(pml4);
        return;
    }
    bool keep;
    bool sw = __atomic_load_n(&on, __ATOMIC_SEQ_CST);
    uint32_t pcid = decide(&pcpu[this_cpu()->index], sw, id, gen, &keep);
    write_cr3(pml4 | pcid | (keep ? CR3_NOFLUSH : 0));
}

#ifndef JAM_NO_KTESTS
/* QEMU's TCG has no PCIDs, so the bookkeeping is tested on made-up CPUs:
 * what pcid_load would load (the PCID, plus PCID_TEST_KEEP when it keeps
 * the entries) on fake CPU `cpu` (< 4), as if PCIDs were in use and the
 * switch were `sw`. */
uint32_t pcid_test_decide(uint32_t cpu, uint64_t id, uint64_t gen, bool sw)
{
    static struct pcid_cpu fake[4];
    bool keep;
    uint32_t pcid = decide(&fake[cpu % 4], sw, id, &gen, &keep);
    return pcid | (keep ? PCID_TEST_KEEP : 0);
}
#endif

uint64_t pcid_kept_loads(uint32_t cpu)
{
    return cpu < MAX_CPUS ? __atomic_load_n(&pcpu[cpu].kept, __ATOMIC_RELAXED) : 0;
}

uint64_t pcid_flushed_loads(uint32_t cpu)
{
    return cpu < MAX_CPUS ? __atomic_load_n(&pcpu[cpu].flushed, __ATOMIC_RELAXED) : 0;
}
