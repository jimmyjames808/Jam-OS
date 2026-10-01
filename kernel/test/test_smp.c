/* The application processors' startup (arch/x86_64/smp.c, apboot.c).
 *
 *   smp_trampoline_parked
 *       The trampoline's page is below 640 KiB and at or above 64 KiB,
 *       page-aligned, never handed to the allocator; after a startup it
 *       holds the halt stub and its transition page table is freed. The
 *       kernel's PML4 has nothing in its user half, so the trampoline is
 *       not identity-mapped there.
 *
 *   smp_cpus_match_bsp
 *       Every online CPU, however it was started, runs with the BSP's
 *       CR0 (but TS, which follows the FPU's owner), CR4, EFER, PAT and
 *       MTRR default type, its own APIC ID and its own struct cpu in GS. */
#include <jam/cpu.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/smp.h>
#include <jam/x86.h>

KTEST(smp_trampoline_parked)
{
    uint64_t pa = smp_trampoline_page();
    kprintf("ktest %s: trampoline at %lx, %s, %u table pages\n", ktest_current, pa,
            smp_own_startup() ? "used" : "not used", smp_trampoline_tables());
    if (pa) {
        KT_ASSERT(pa >= 64 * 1024 && pa < 640 * 1024);
        KT_EQ(pa & (PAGE_SIZE - 1), 0);
        KT_ASSERT(pfn_to_page(pa >> PAGE_SHIFT)->flags & PG_RESERVED);
    }
    KT_ASSERT(pa || !smp_own_startup());
    if (smp_own_startup())
        KT_ASSERT(smp_trampoline_parked());
    KT_EQ(smp_trampoline_tables(), 0);
    const uint64_t *pml4 = phys_to_virt(vmm_kernel_pml4());
    for (int i = 0; i < 256; i++)
        KT_EQ(pml4[i], 0);
    if (pa)
        KT_EQ(vmm_translate(vmm_kernel_pml4(), pa), UINT64_MAX);
}

#define MSR_MTRR_DEFTYPE 0x2ff

struct cpu_regs {
    uint64_t cr0, cr4, efer, pat, mtrr_def, gs;   /* as read on that CPU */
    uint32_t apic_id;                             /* lapic_id() there */
};

static void read_regs(void *arg)
{
    struct cpu_regs *r = arg;
    r->cr0 = read_cr0() & ~CR0_TS;
    r->cr4 = read_cr4();
    r->efer = rdmsr(MSR_EFER);
    r->pat = cpu_features.pat ? rdmsr(MSR_PAT) : 0;
    r->mtrr_def = cpu_features.mtrr ? rdmsr(MSR_MTRR_DEFTYPE) : 0;
    r->gs = rdmsr(MSR_GS_BASE);
    r->apic_id = lapic_id();
}

KTEST(smp_cpus_match_bsp)
{
    struct cpu_regs bsp, r;
    smp_call_on(0, read_regs, &bsp);
    uint32_t checked = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (!cpu_online(cpus[i]))
            continue;
        smp_call_on(i, read_regs, &r);
        KT_EQ(r.cr0, bsp.cr0);
        KT_EQ(r.cr4, bsp.cr4);
        KT_EQ(r.efer, bsp.efer);
        KT_EQ(r.pat, bsp.pat);
        KT_EQ(r.mtrr_def, bsp.mtrr_def);
        KT_EQ(r.gs, (uint64_t)cpus[i]);
        KT_EQ(r.apic_id, cpus[i]->lapic_id);
        checked++;
    }
    kprintf("ktest %s: %u CPUs, cr0 %lx cr4 %lx efer %lx pat %lx mtrr default %lx\n",
            ktest_current, checked, bsp.cr0, bsp.cr4, bsp.efer, bsp.pat, bsp.mtrr_def);
}
