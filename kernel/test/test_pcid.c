/* PCIDs (kernel/arch/x86_64/pcid.c): the per-CPU slot bookkeeping, and no
 * stale translation after a CPU switched away from an address space. */
#include <jam/aspace.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/vmo.h>
#include <jam/x86.h>

/* ---- PCIDs ------------------------------------------------------------------ */

#define KEEP PCID_TEST_KEEP

/* The per-CPU slot bookkeeping, on made-up CPUs (QEMU's TCG has no PCIDs,
 * so this is the only way it runs there). */
KTEST(pcid_slot_bookkeeping)
{
    /* First load of an address space on a CPU: a slot, flushed. */
    KT_EQ(pcid_test_decide(0, 1000, 1, true), 1);
    KT_EQ(pcid_test_decide(0, 1000, 1, true), 1 | KEEP);   /* same generation: kept */
    KT_EQ(pcid_test_decide(0, 1000, 2, true), 1);          /* entries changed: flushed */
    KT_EQ(pcid_test_decide(0, 1000, 2, true), 1 | KEEP);
    KT_EQ(pcid_test_decide(0, 1001, 7, true), 2);          /* another one, another slot */
    KT_EQ(pcid_test_decide(0, 1000, 2, true), 1 | KEEP);   /* the first kept its entries */
    KT_EQ(pcid_test_decide(0, 0, 0, true), 0 | KEEP);      /* the kernel's tables: PCID 0 */
    /* Slots are per CPU. */
    KT_EQ(pcid_test_decide(1, 1000, 2, true), 1);
    /* Eight more address spaces take every slot round robin: 1000 is gone
     * and comes back flushed, in a slot of its own. */
    for (uint64_t id = 2000; id < 2000 + PCID_SLOTS_PER_CPU; id++)
        KT_EQ(pcid_test_decide(0, id, 1, true) & KEEP, 0);
    uint32_t again = pcid_test_decide(0, 1000, 2, true);
    KT_EQ(again & KEEP, 0);
    KT_ASSERT(again >= 1 && again <= PCID_SLOTS_PER_CPU);
    /* Switched off: PCID 0, flushed on every load, as in M5. */
    KT_EQ(pcid_test_decide(2, 1000, 2, false), 0);
    KT_EQ(pcid_test_decide(2, 1000, 2, false), 0);
    KT_EQ(pcid_test_decide(2, 0, 0, false), 0);
    /* Switched back on: the first kernel-table load flushes PCID 0, which
     * held the user address space's entries; the next one keeps. */
    KT_EQ(pcid_test_decide(2, 0, 0, true), 0);
    KT_EQ(pcid_test_decide(2, 0, 0, true), 0 | KEEP);
    /* Flipping the real switch makes every CPU forget its slots. */
    if (pcid_usable()) {
        KT_EQ(pcid_test_decide(3, 1000, 2, true), 1);
        KT_EQ(pcid_test_decide(3, 1000, 2, true), 1 | KEEP);
        bool was = pcid_is_on();
        pcid_set(!was);
        pcid_set(was);
        /* Forgotten: flushed, in whichever slot comes next (the
         * round-robin position isn't reset, and needn't be: a flushing
         * load clears the slot's old entries). */
        uint32_t after = pcid_test_decide(3, 1000, 2, true);
        KT_EQ(after & KEEP, 0);
        KT_ASSERT(after >= 1 && after <= PCID_SLOTS_PER_CPU);
    }
}

/* A page mapped in two address spaces, each loaded and read on its own CPU,
 * which then switch AWAY (to another address space, then the kernel's
 * tables). A decommit interrupts neither CPU (neither is running them),
 * but when they load the address spaces again they must not use the old
 * translation: with PCIDs the TLB still holds it under their PCIDs, and
 * the generation check has to flush it (on the PC; in QEMU, without PCIDs,
 * every CR3 load flushes and this checks the M5 path). */
static struct aspace *pc_as[2], *pc_other;
static uint64_t pc_addr[2], pc_other_addr;
static volatile int pc_ready, pc_phase;
static volatile uint64_t pc_seen[2][2];
static uint64_t pc_flushes_before[2];

static void pcid_runner(void *arg)
{
    int i = (int)(uintptr_t)arg;
    preempt_disable();   /* stay on this CPU; interrupts stay on while waiting */
    uint64_t f = irq_save();
    aspace_switch(NULL, pc_as[i]);
    pc_seen[i][0] = kt_user_peek(pc_addr[i]);   /* caches the translation */
    aspace_switch(pc_as[i], pc_other);
    (void)kt_user_peek(pc_other_addr);
    aspace_switch(pc_other, NULL);
    irq_restore(f);
    pc_flushes_before[i] = pcid_flushed_loads(this_cpu()->index);
    __atomic_add_fetch(&pc_ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&pc_phase, __ATOMIC_ACQUIRE) < 1)
        cpu_relax();
    f = irq_save();
    aspace_switch(NULL, pc_as[i]);
    pc_seen[i][1] = kt_user_peek(pc_addr[i]);
    aspace_switch(pc_as[i], NULL);
    irq_restore(f);
    preempt_enable();
}

KTEST(pcid_no_stale_translation_after_switching_away)
{
    if (cpu_count < 3)
        return;
    kt_pin_self(0);
    struct vmo *v, *w;
    KT_EQ(vmo_create(PAGE_SIZE, 0, &v), OK);
    KT_EQ(vmo_create(PAGE_SIZE, 0, &w), OK);
    uint64_t old = 0x1111111111111111ull;
    KT_EQ(vmo_write(v, 0, &old, 8), OK);
    for (int i = 0; i < 2; i++) {
        KT_EQ(aspace_create(&pc_as[i]), OK);
        pc_addr[i] = 0x700000 + (uint64_t)i * 0x1000000;
        KT_EQ(aspace_map(pc_as[i], v, 0, PAGE_SIZE, ASPACE_READ | ASPACE_WRITE | ASPACE_FIXED,
                         &pc_addr[i]), OK);
        KT_EQ(aspace_fault(pc_as[i], pc_addr[i], ASPACE_READ), OK);
    }
    KT_EQ(aspace_create(&pc_other), OK);
    pc_other_addr = 0x900000;
    KT_EQ(aspace_map(pc_other, w, 0, PAGE_SIZE, ASPACE_READ | ASPACE_FIXED, &pc_other_addr), OK);
    KT_EQ(aspace_fault(pc_other, pc_other_addr, ASPACE_READ), OK);

    pc_ready = pc_phase = 0;
    struct thread *t[2];
    uint32_t cpu[2] = { 1, 2 };
    for (int i = 0; i < 2; i++) {
        cpumask_t m;
        cpumask_one(&m, cpu[i]);
        t[i] = thread_create_on("kt-pcid", pcid_runner, (void *)(uintptr_t)i, PRIO_DEFAULT,
                                &m);
    }
    while (__atomic_load_n(&pc_ready, __ATOMIC_ACQUIRE) < 2)
        thread_yield();
    uint64_t ipis[2] = { tlb_mask_flush_count(cpu[0]), tlb_mask_flush_count(cpu[1]) };
    KT_EQ(vmo_decommit(v, 0, PAGE_SIZE), OK);
    /* Neither CPU runs either address space now: nobody was interrupted. */
    KT_EQ(tlb_mask_flush_count(cpu[0]), ipis[0]);
    KT_EQ(tlb_mask_flush_count(cpu[1]), ipis[1]);
    /* Scribble on the freed page and fault a fresh zero page in behind
     * both mappings, so a stale translation reads garbage. */
    uint64_t decoy = pmm_alloc_page_phys(0);
    KT_ASSERT(decoy);
    *(volatile uint64_t *)phys_to_virt(decoy) = 0x2222222222222222ull;
    KT_EQ(aspace_fault(pc_as[0], pc_addr[0], ASPACE_READ), OK);
    KT_EQ(aspace_fault(pc_as[1], pc_addr[1], ASPACE_READ), OK);
    __atomic_store_n(&pc_phase, 1, __ATOMIC_RELEASE);
    thread_join(t[0]);
    thread_join(t[1]);
    bool on = pcid_is_on();
    for (int i = 0; i < 2; i++) {
        kprintf("pcid: cpu %u read %lx before, %lx after (PCIDs %s, %lu flushing loads since)\n",
                cpu[i], pc_seen[i][0], pc_seen[i][1], on ? "on" : "off",
                pcid_flushed_loads(cpu[i]) - pc_flushes_before[i]);
        KT_EQ(pc_seen[i][0], old);
        KT_EQ(pc_seen[i][1], 0);   /* the new zero page, not the old one */
        if (on)
            KT_ASSERT(pcid_flushed_loads(cpu[i]) > pc_flushes_before[i]);
    }
    pmm_free_page_phys(decoy);
    aspace_unref(pc_as[0]);
    aspace_unref(pc_as[1]);
    aspace_unref(pc_other);
    kobject_unref(vmo_kobject(v));
    kobject_unref(vmo_kobject(w));
    kt_unpin_self();
}
