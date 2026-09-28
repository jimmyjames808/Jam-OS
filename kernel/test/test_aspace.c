/* Tests for user address spaces (mm/aspace.c) and the VMO reverse map.
 *
 * Nothing here enters ring 3. Entries are checked by walking the address
 * space's tables (aspace_pte, vmm_translate on aspace_pml4), and data
 * through the HHDM alias of the page an entry names. Two tests load an
 * address space's CR3 on a CPU and read user addresses directly, to catch
 * a stale TLB entry for real; those reads all go through user_peek(), the
 * one place that touches a user address (with stac/clac, so it keeps
 * working once SMAP is on). */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/cpu.h>
#include <jam/dbghook.h>
#include <jam/ipi.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/vmo.h>
#include <jam/x86.h>

#define PG PAGE_SIZE
#define RW (ASPACE_READ | ASPACE_WRITE)
#define RX (ASPACE_READ | ASPACE_EXEC)
#define R  ASPACE_READ

#define PTE_P  (1ull << 0)
#define PTE_W  (1ull << 1)
#define PTE_U  (1ull << 2)
#define PTE_NX (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull

static uint64_t free_now(void)
{
    uint64_t total, free;
    pmm_stats(&total, &free);
    return free;
}

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct aspace *new_as(void)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    return as;
}

static uint64_t pte_pa(struct aspace *as, uint64_t va)
{
    uint64_t e = aspace_pte(as, va);
    return (e & PTE_P) ? (e & PTE_ADDR) : 0;
}

/* Every level of the walk to va is present and user-accessible. */
static bool user_walk_ok(struct aspace *as, uint64_t va)
{
    uint64_t *t = phys_to_virt(aspace_pml4(as));
    for (int l = 4; l >= 1; l--) {
        uint64_t e = t[(va >> (12 + 9 * (l - 1))) & 511];
        if (!(e & PTE_P) || !(e & PTE_U))
            return false;
        t = phys_to_virt(e & PTE_ADDR);
    }
    return true;
}

/* THE only direct access to a user address in the tests. The caller has
 * this CPU on the address space (aspace_switch) and knows the page is
 * mapped. stac/clac when the CPU has SMAP, so this keeps working once
 * Track A turns SMAP on (without SMAP they would #UD). */
static uint64_t user_peek(uint64_t va)
{
    uint32_t a, b, c, d;
    cpuid(7, 0, &a, &b, &c, &d);
    bool smap = b & (1u << 20);
    if (smap)
        __asm__ volatile("stac" ::: "memory");
    uint64_t v = *(volatile uint64_t *)va;
    if (smap)
        __asm__ volatile("clac" ::: "memory");
    return v;
}

static uint32_t cur_cpu(void)
{
    preempt_disable();
    uint32_t c = this_cpu()->index;
    preempt_enable_no_resched();
    return c;
}

static void pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
    KT_EQ(cur_cpu(), cpu);
}

static void unpin_self(void)
{
    cpumask_t all;
    cpumask_all(&all);
    thread_set_affinity(current_thread(), &all);
}

/* ---- map / unmap / protect edges ------------------------------------------ */

KTEST(aspace_map_args)
{
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(8 * PG, 0, &v), OK);
    uint64_t a = 0x400000;

    /* Permissions: W^X, and no W or X without R. */
    KT_EQ(aspace_map(as, v, 0, PG, RW | ASPACE_EXEC, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_WRITE, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_EXEC, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, v, 0, PG, R | (1u << 12), &a), ERR_INVALID_ARGS);
    /* Alignment and length. */
    KT_EQ(aspace_map(as, v, 0, 0, R, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, v, 0, PG + 1, R, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, v, 100, PG, R, &a), ERR_INVALID_ARGS);
    a = 0x400010;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &a), ERR_INVALID_ARGS);
    /* Outside the user range: page 0, the last page, the kernel half. */
    a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &a), ERR_INVALID_ARGS);
    a = USER_TOP;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &a), ERR_INVALID_ARGS);
    a = USER_TOP - PG;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, R | ASPACE_FIXED, &a), ERR_INVALID_ARGS);
    a = 0xffff800000000000ull;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &a), ERR_INVALID_ARGS);
    /* The VMO range. */
    KT_EQ(aspace_map(as, v, 8 * PG, PG, R, &a), ERR_OUT_OF_RANGE);
    KT_EQ(aspace_map(as, v, 4 * PG, 5 * PG, R, &a), ERR_OUT_OF_RANGE);
    KT_EQ(aspace_map(as, v, ~0xfffull, 2 * PG, R, &a), ERR_OUT_OF_RANGE);
    KT_EQ(aspace_mapping_count(as), 0);
    KT_EQ(vmo_kobject(v)->refs, 1);

    /* The very last page and the very first are fine. */
    a = USER_TOP - PG;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &a), OK);
    KT_EQ(a, USER_TOP - PG);
    /* First fit starts at USER_BASE, then packs. */
    uint64_t b1, b2, b3, b4;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, R, &b1), OK);
    KT_EQ(b1, USER_BASE);
    KT_EQ(aspace_map(as, v, 0, 3 * PG, RW, &b2), OK);
    KT_EQ(b2, USER_BASE + 2 * PG);
    /* FIXED overlapping anything is refused, adjacent is fine. */
    b3 = b2 + 2 * PG;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &b3), ERR_ALREADY_BOUND);
    b3 = b1 - PG;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &b3), ERR_INVALID_ARGS);   /* page 0 */
    b3 = b2 + 3 * PG + 4 * PG;
    KT_EQ(aspace_map(as, v, 0, PG, R | ASPACE_FIXED, &b3), OK);
    /* A 4-page hole after b2: a 5-page first fit skips it, 4 fits it. */
    KT_EQ(aspace_map(as, v, 0, 5 * PG, R, &b4), OK);
    KT_EQ(b4, b3 + PG);
    uint64_t b5;
    KT_EQ(aspace_map(as, v, 0, 4 * PG, R, &b5), OK);
    KT_EQ(b5, b2 + 3 * PG);
    /* A no-access mapping (guard) is allowed. */
    uint64_t g;
    KT_EQ(aspace_map(as, v, 0, PG, 0, &g), OK);
    KT_EQ(aspace_fault(as, g, ASPACE_READ), ERR_ACCESS_DENIED);

    KT_EQ(aspace_mapping_count(as), 7);
    KT_EQ(vmo_kobject(v)->refs, 8);   /* every mapping holds one */
    aspace_unref(as);                 /* unmaps everything */
    KT_EQ(vmo_kobject(v)->refs, 1);
    put(v);
}

KTEST(aspace_unmap_split)
{
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(16 * PG, 0, &v), OK);
    uint64_t base = 0x10000000;
    uint64_t a = base;
    KT_EQ(aspace_map(as, v, 4 * PG, 10 * PG, RW | ASPACE_FIXED, &a), OK);
    for (unsigned i = 0; i < 10; i++)
        KT_EQ(aspace_fault(as, base + i * PG, ASPACE_WRITE), OK);
    KT_EQ(vmo_committed(v), 10 * PG);
    uint64_t tables = aspace_pt_pages(as);
    KT_EQ(tables, 3);   /* one PDPT, PD and PT */

    /* A hole in the middle: two mappings, the tail keeps its VMO offsets. */
    KT_EQ(aspace_unmap(as, base + 3 * PG, 2 * PG), OK);
    KT_EQ(aspace_mapping_count(as), 2);
    KT_EQ(pte_pa(as, base + 3 * PG), 0);
    KT_EQ(pte_pa(as, base + 4 * PG), 0);
    KT_EQ(aspace_fault(as, base + 3 * PG, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(aspace_fault(as, base + 4 * PG + 123, ASPACE_READ), ERR_NOT_FOUND);
    for (unsigned i = 0; i < 10; i++) {
        if (i == 3 || i == 4)
            continue;
        KT_EQ(pte_pa(as, base + i * PG), vmo_page_phys(v, (4 + i) * PG));
    }
    KT_EQ(vmo_committed(v), 10 * PG);   /* unmapping never decommits */
    KT_EQ(vmo_kobject(v)->refs, 3);

    /* Head and tail cuts, then one unmap across a hole and two mappings. */
    KT_EQ(aspace_unmap(as, base, PG), OK);
    KT_EQ(aspace_unmap(as, base + 9 * PG, PG), OK);
    KT_EQ(aspace_mapping_count(as), 2);
    KT_EQ(aspace_fault(as, base, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(aspace_fault(as, base + PG, ASPACE_READ), OK);
    KT_EQ(aspace_fault(as, base + 8 * PG, ASPACE_READ), OK);
    KT_EQ(aspace_fault(as, base + 9 * PG, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(pte_pa(as, base + 5 * PG), vmo_page_phys(v, 9 * PG));
    KT_EQ(aspace_unmap(as, base + 2 * PG, 5 * PG), OK);   /* [2,3) and [5,7) */
    KT_EQ(aspace_mapping_count(as), 2);                   /* [1,2) and [7,9) */
    KT_EQ(pte_pa(as, base + 7 * PG), vmo_page_phys(v, 11 * PG));
    KT_EQ(aspace_unmap(as, base + 2 * PG, 5 * PG), ERR_NOT_FOUND);   /* nothing left there */

    /* Arguments. */
    KT_EQ(aspace_unmap(as, base, 0), ERR_INVALID_ARGS);
    KT_EQ(aspace_unmap(as, base + 1, PG), ERR_INVALID_ARGS);
    KT_EQ(aspace_unmap(as, 0, PG), ERR_INVALID_ARGS);
    KT_EQ(aspace_unmap(as, USER_TOP - PG, 2 * PG), ERR_INVALID_ARGS);

    /* Unmapping everything frees the page tables too. */
    KT_EQ(aspace_unmap(as, USER_BASE, USER_TOP - USER_BASE), OK);
    KT_EQ(aspace_mapping_count(as), 0);
    KT_EQ(aspace_pt_pages(as), 0);
    KT_EQ(vmo_kobject(v)->refs, 1);
    aspace_unref(as);
    put(v);
}

KTEST(aspace_protect)
{
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(8 * PG, 0, &v), OK);
    uint64_t base = 0x20000000, a = base;
    KT_EQ(aspace_map(as, v, 0, 6 * PG, RW | ASPACE_CAN_EXEC | ASPACE_FIXED, &a), OK);
    for (unsigned i = 0; i < 6; i++)
        KT_EQ(aspace_fault(as, base + i * PG, ASPACE_WRITE), OK);
    KT_ASSERT(aspace_pte(as, base + 2 * PG) & PTE_W);

    /* Read-only in the middle: three mappings, entries rewritten in place. */
    KT_EQ(aspace_protect(as, base + 2 * PG, 2 * PG, R), OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_ASSERT(aspace_pte(as, base + PG) & PTE_W);
    KT_ASSERT(!(aspace_pte(as, base + 2 * PG) & PTE_W));
    KT_ASSERT(!(aspace_pte(as, base + 3 * PG) & PTE_W));
    KT_ASSERT(aspace_pte(as, base + 4 * PG) & PTE_W);
    KT_EQ(pte_pa(as, base + 3 * PG), vmo_page_phys(v, 3 * PG));   /* same page */
    KT_EQ(aspace_fault(as, base + 2 * PG, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, base + 2 * PG, ASPACE_READ), OK);
    KT_EQ(aspace_fault(as, base + 2 * PG, ASPACE_EXEC), ERR_ACCESS_DENIED);

    /* W+X never; EXEC only within the mapping's CAN bits. */
    KT_EQ(aspace_protect(as, base, PG, RW | ASPACE_EXEC), ERR_INVALID_ARGS);
    KT_EQ(aspace_protect(as, base + 2 * PG, PG, RX), OK);
    KT_ASSERT(!(aspace_pte(as, base + 2 * PG) & (PTE_NX | PTE_W)));
    KT_ASSERT((aspace_pte(as, base + 3 * PG) & PTE_NX) || !cpu_features.nx);
    KT_EQ(aspace_fault(as, base + 2 * PG, ASPACE_EXEC), OK);
    KT_EQ(aspace_mapping_count(as), 4);

    /* Across mappings; a gap or an unmapped end is refused, changing nothing. */
    KT_EQ(aspace_protect(as, base, 6 * PG, R), OK);
    KT_EQ(aspace_protect(as, base, 7 * PG, RW), ERR_NOT_FOUND);
    KT_EQ(aspace_protect(as, base - PG, 2 * PG, RW), ERR_NOT_FOUND);
    KT_ASSERT(!(aspace_pte(as, base) & PTE_W));
    KT_EQ(aspace_unmap(as, base + PG, PG), OK);
    KT_EQ(aspace_protect(as, base, 3 * PG, RW), ERR_NOT_FOUND);
    KT_EQ(aspace_protect(as, base + 2 * PG, PG, RW), OK);

    /* No access: the entries go; every access is then denied. */
    KT_EQ(aspace_protect(as, base + 4 * PG, 2 * PG, 0), OK);
    KT_EQ(pte_pa(as, base + 4 * PG), 0);
    KT_EQ(aspace_fault(as, base + 5 * PG, ASPACE_READ), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, base + 4 * PG, 2 * PG, R), OK);
    KT_EQ(aspace_fault(as, base + 5 * PG, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, base + 5 * PG), vmo_page_phys(v, 5 * PG));

    /* A mapping without CAN_EXEC can't become executable. */
    uint64_t b = 0;
    KT_EQ(aspace_map(as, v, 0, PG, RW, &b), OK);
    KT_EQ(aspace_protect(as, b, PG, RX), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, b, PG, R), OK);
    KT_EQ(aspace_protect(as, b, PG, RW), OK);   /* back up to what it had */
    KT_EQ(aspace_protect(as, b, PG + 1, R), ERR_INVALID_ARGS);

    aspace_unref(as);
    KT_EQ(vmo_kobject(v)->refs, 1);
    put(v);
}

/* ---- faults ---------------------------------------------------------------- */

KTEST(aspace_fault_commits_one_page)
{
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(64 * PG, 0, &v), OK);
    uint64_t magic = 0x5eed5eed5eedull;
    KT_EQ(vmo_write(v, 7 * PG + 8, &magic, 8), OK);
    KT_EQ(vmo_committed(v), PG);

    uint64_t ro = 0x7f0000000000ull, rw = 0x1000000, rx = 0x2000000;
    KT_EQ(aspace_map(as, v, 0, 64 * PG, R | ASPACE_FIXED, &ro), OK);
    KT_EQ(aspace_map(as, v, 0, 64 * PG, RW | ASPACE_FIXED, &rw), OK);
    KT_EQ(aspace_map(as, v, 0, 64 * PG, RX | ASPACE_FIXED, &rx), OK);
    KT_EQ(vmo_committed(v), PG);   /* mapping commits nothing */
    KT_EQ(aspace_pte(as, rw), 0);

    /* A write fault commits exactly the one page and maps it U, W, NX. */
    KT_EQ(aspace_fault(as, rw + 3 * PG + 100, ASPACE_WRITE), OK);
    KT_EQ(vmo_committed(v), 2 * PG);
    uint64_t e = aspace_pte(as, rw + 3 * PG);
    KT_ASSERT((e & PTE_P) && (e & PTE_U) && (e & PTE_W));
    KT_ASSERT((e & PTE_NX) || !cpu_features.nx);
    KT_EQ(e & PTE_ADDR, vmo_page_phys(v, 3 * PG));
    KT_ASSERT(user_walk_ok(as, rw + 3 * PG));
    KT_EQ(vmm_translate(aspace_pml4(as), rw + 3 * PG + 100), vmo_page_phys(v, 3 * PG) + 100);
    KT_EQ(aspace_pte(as, rw + 2 * PG), 0);   /* neighbours untouched */
    KT_EQ(aspace_pte(as, rw + 4 * PG), 0);

    /* Faulting again (or a read of an RW mapping) commits nothing more. */
    KT_EQ(aspace_fault(as, rw + 3 * PG, ASPACE_READ), OK);
    KT_EQ(vmo_committed(v), 2 * PG);

    /* The same data is behind every mapping of the page. */
    KT_EQ(aspace_fault(as, ro + 7 * PG, ASPACE_READ), OK);
    KT_EQ(aspace_fault(as, rw + 7 * PG, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, ro + 7 * PG), pte_pa(as, rw + 7 * PG));
    KT_EQ(*(uint64_t *)phys_to_virt(pte_pa(as, ro + 7 * PG) + 8), magic);
    e = aspace_pte(as, ro + 7 * PG);
    KT_ASSERT(!(e & PTE_W) && (e & PTE_U));
    KT_EQ(vmo_committed(v), 2 * PG);

    /* Read-only: a write fault is denied and commits nothing. */
    KT_EQ(aspace_fault(as, ro + 9 * PG, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(aspace_fault(as, ro + 9 * PG, ASPACE_EXEC), ERR_ACCESS_DENIED);
    KT_EQ(aspace_pte(as, ro + 9 * PG), 0);
    KT_EQ(vmo_committed(v), 2 * PG);
    /* Executable: no NX, no W. */
    KT_EQ(aspace_fault(as, rx + 5 * PG, ASPACE_EXEC), OK);
    e = aspace_pte(as, rx + 5 * PG);
    KT_ASSERT(!(e & PTE_NX) && !(e & PTE_W) && (e & PTE_U));
    KT_EQ(aspace_fault(as, rx + 5 * PG, ASPACE_WRITE), ERR_ACCESS_DENIED);

    /* Nothing mapped, or not a user address. */
    KT_EQ(aspace_fault(as, rw + 64 * PG, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(aspace_fault(as, 0, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(aspace_fault(as, USER_TOP, ASPACE_READ), ERR_NOT_FOUND);
    KT_EQ(aspace_fault(as, (uint64_t)&magic, ASPACE_READ), ERR_NOT_FOUND);

    /* The kernel half is shared with the kernel's own tables. */
    KT_EQ(vmm_translate(aspace_pml4(as), (uint64_t)&magic),
          vmm_translate(vmm_kernel_pml4(), (uint64_t)&magic));
    KT_EQ(vmm_translate(aspace_pml4(as), (uint64_t)v),
          vmm_translate(vmm_kernel_pml4(), (uint64_t)v));
    KT_EQ(aspace_pml4(NULL), vmm_kernel_pml4());

    aspace_unref(as);
    put(v);
}

KTEST(aspace_physical_vmo_cache_type)
{
    struct aspace *as = new_as();
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    KT_ASSERT(pa);
    struct vmo *wc, *uc, *wb;
    KT_EQ(vmo_create_physical(pa, PG, VM_WC, &wc), OK);
    KT_EQ(vmo_create_physical(pa, PG, VM_UC, &uc), OK);
    KT_EQ(vmo_create(PG, 0, &wb), OK);
    uint64_t a1 = 0, a2 = 0, a3 = 0;
    KT_EQ(aspace_map(as, wc, 0, PG, RW, &a1), OK);
    KT_EQ(aspace_map(as, uc, 0, PG, RW, &a2), OK);
    KT_EQ(aspace_map(as, wb, 0, PG, RW, &a3), OK);
    KT_EQ(aspace_fault(as, a1, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, a2, ASPACE_READ), OK);
    KT_EQ(aspace_fault(as, a3, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, a1), pa);
    KT_EQ(pte_pa(as, a2), pa);
    uint64_t pml4 = aspace_pml4(as);
    KT_ASSERT(!strcmp(vmm_cache_type(pml4, a1), "WC"));
    KT_ASSERT(!strcmp(vmm_cache_type(pml4, a2), "UC"));
    KT_ASSERT(!strcmp(vmm_cache_type(pml4, a3), "WB"));
    /* Physical VMOs can't be decommitted, mapped or not. */
    KT_EQ(vmo_decommit(wc, 0, PG), ERR_NOT_SUPPORTED);
    aspace_unref(as);
    put(wc);
    put(uc);
    put(wb);
    pmm_free_page_phys(pa);   /* physical VMOs never free their pages */
}

/* ---- running on an address space ------------------------------------------ */

KTEST(aspace_switch_reads_user_memory)
{
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, 0, &v), OK);
    uint64_t val = 0xabcdef0123ull;
    KT_EQ(vmo_write(v, PG + 16, &val, 8), OK);
    uint64_t a = 0x40000000;
    KT_EQ(aspace_map(as, v, 0, 2 * PG, R | ASPACE_FIXED, &a), OK);
    KT_EQ(aspace_fault(as, a + PG, ASPACE_READ), OK);

    uint64_t f = irq_save();
    uint64_t cr3 = read_cr3();
    aspace_switch(NULL, as);
    KT_EQ(read_cr3() & PTE_ADDR, aspace_pml4(as));
    aspace_switch(as, as);   /* no-op */
    uint64_t seen = user_peek(a + PG + 16);
    uint64_t kernel_seen = *(volatile uint64_t *)&val;   /* the kernel half still works */
    aspace_switch(as, NULL);
    KT_EQ(read_cr3(), cr3);
    irq_restore(f);
    KT_EQ(seen, val);
    KT_EQ(kernel_seen, val);

    aspace_unref(as);
    put(v);
}

/* Decommit of a page mapped in two address spaces, each loaded on its own
 * CPU: both entries go, both CPUs (and only they) are shot down before the
 * page is freed, and neither CPU still reads the old page afterwards. */
static struct aspace *two_as[2];
static uint64_t two_addr[2];
static volatile int two_ready, two_phase;
static volatile uint64_t two_seen[2][2];
static struct page *two_victim;
static uint64_t two_count_before[2];
static uint32_t two_cpu[2];
static volatile int two_hook_ran, two_hook_ok;

static void two_runner(void *arg)
{
    int i = (int)(uintptr_t)arg;
    preempt_disable();   /* stay on this CPU with the CR3 loaded; IRQs stay on */
    uint64_t f = irq_save();
    aspace_switch(NULL, two_as[i]);
    irq_restore(f);
    two_seen[i][0] = user_peek(two_addr[i]);   /* caches the translation */
    __atomic_add_fetch(&two_ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&two_phase, __ATOMIC_ACQUIRE) < 1)
        cpu_relax();   /* the shootdown IPIs land here */
    two_seen[i][1] = user_peek(two_addr[i]);
    f = irq_save();
    aspace_switch(two_as[i], NULL);
    irq_restore(f);
    preempt_enable();
}

static void two_hook(void *arg)
{
    struct tlb_gather *g = arg;
    bool listed = false;
    for (struct list_node *n = g->pages.next; n != &g->pages; n = n->next)
        listed |= container_of(n, struct page, node) == two_victim;
    two_hook_ok = listed && two_victim->refcount == 1 &&
                  tlb_mask_flush_count(two_cpu[0]) > two_count_before[0] &&
                  tlb_mask_flush_count(two_cpu[1]) > two_count_before[1];
    two_hook_ran++;
}

KTEST(aspace_decommit_two_cpus)
{
    if (cpu_count < 3) {
        kprintf("ktest: %s skipped (needs >= 3 CPUs)\n", ktest_current);
        return;
    }
    pin_self(0);
    two_cpu[0] = 1;
    two_cpu[1] = 2;
    struct vmo *v;
    KT_EQ(vmo_create(PG, 0, &v), OK);
    uint64_t old = 0x1111111111111111ull;
    KT_EQ(vmo_write(v, 0, &old, 8), OK);
    for (int i = 0; i < 2; i++) {
        two_as[i] = new_as();
        two_addr[i] = 0x600000 + (uint64_t)i * 0x1000000;
        KT_EQ(aspace_map(two_as[i], v, 0, PG, RW | ASPACE_FIXED, &two_addr[i]), OK);
        KT_EQ(aspace_fault(two_as[i], two_addr[i], ASPACE_WRITE), OK);
    }
    uint64_t pa = vmo_page_phys(v, 0);
    KT_EQ(pte_pa(two_as[0], two_addr[0]), pa);
    KT_EQ(pte_pa(two_as[1], two_addr[1]), pa);

    two_ready = two_phase = 0;
    struct thread *t[2];
    for (int i = 0; i < 2; i++) {
        cpumask_t m;
        cpumask_one(&m, two_cpu[i]);
        t[i] = thread_create_on("as-runner", two_runner, (void *)(uintptr_t)i, PRIO_DEFAULT, &m);
    }
    while (__atomic_load_n(&two_ready, __ATOMIC_ACQUIRE) < 2)
        thread_yield();

    two_victim = pfn_to_page(pa >> PAGE_SHIFT);
    for (int i = 0; i < 2; i++)
        two_count_before[i] = tlb_mask_flush_count(two_cpu[i]);
    uint64_t me_before = tlb_mask_flush_count(0);
    uint64_t other_before = cpu_count > 3 ? tlb_mask_flush_count(3) : 0;
    two_hook_ran = two_hook_ok = 0;
    dbg_hooks[DBG_GATHER_PRE_FREE] = two_hook;
    KT_EQ(vmo_decommit(v, 0, PG), OK);
    dbg_hooks[DBG_GATHER_PRE_FREE] = NULL;

    KT_EQ(two_hook_ran, 1);
    KT_ASSERT(two_hook_ok);   /* both shot down while the page was still held */
    KT_EQ(two_victim->refcount, 0);   /* and freed after */
    KT_EQ(aspace_pte(two_as[0], two_addr[0]), 0);
    KT_EQ(aspace_pte(two_as[1], two_addr[1]), 0);
    KT_EQ(vmm_translate(aspace_pml4(two_as[0]), two_addr[0]), UINT64_MAX);
    KT_EQ(vmm_translate(aspace_pml4(two_as[1]), two_addr[1]), UINT64_MAX);
    KT_EQ(vmo_committed(v), 0);
    /* CPUs that weren't using either address space were left alone. */
    KT_EQ(tlb_mask_flush_count(0), me_before);
    if (cpu_count > 3)
        KT_EQ(tlb_mask_flush_count(3), other_before);

    /* Grab the freed page and scribble on it, so a stale TLB entry would
     * read garbage; then fault a fresh zero page in behind both mappings. */
    uint64_t decoy = pmm_alloc_page_phys(0);
    KT_ASSERT(decoy);
    *(volatile uint64_t *)phys_to_virt(decoy) = 0x2222222222222222ull;
    KT_EQ(aspace_fault(two_as[0], two_addr[0], ASPACE_READ), OK);
    KT_EQ(aspace_fault(two_as[1], two_addr[1], ASPACE_READ), OK);
    KT_EQ(vmo_committed(v), PG);
    KT_ASSERT(pte_pa(two_as[0], two_addr[0]) != decoy);
    __atomic_store_n(&two_phase, 1, __ATOMIC_RELEASE);
    thread_join(t[0]);
    thread_join(t[1]);
    for (int i = 0; i < 2; i++) {
        KT_EQ(two_seen[i][0], old);
        KT_EQ(two_seen[i][1], 0);   /* the new page, not a stale translation */
    }
    pmm_free_page_phys(decoy);
    aspace_unref(two_as[0]);
    aspace_unref(two_as[1]);
    put(v);
    unpin_self();
}

/* ---- decommit and shrink under mappings ----------------------------------- */

KTEST(aspace_decommit_and_shrink_unmap)
{
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(8 * PG, 0, &v), OK);
    uint64_t a = 0x800000, b = 0x900000;
    KT_EQ(aspace_map(as, v, 0, 8 * PG, RW | ASPACE_FIXED, &a), OK);
    KT_EQ(aspace_map(as, v, 2 * PG, 4 * PG, R | ASPACE_FIXED, &b), OK);   /* pages 2..5 */
    for (unsigned i = 0; i < 8; i++)
        KT_EQ(aspace_fault(as, a + i * PG, ASPACE_WRITE), OK);
    for (unsigned i = 0; i < 4; i++)
        KT_EQ(aspace_fault(as, b + i * PG, ASPACE_READ), OK);
    KT_EQ(vmo_committed(v), 8 * PG);

    /* Decommit of mapped pages is allowed now: both mappings lose them. */
    KT_EQ(vmo_decommit(v, 3 * PG, 2 * PG), OK);
    KT_EQ(vmo_committed(v), 6 * PG);
    KT_EQ(aspace_pte(as, a + 3 * PG), 0);
    KT_EQ(aspace_pte(as, a + 4 * PG), 0);
    KT_EQ(aspace_pte(as, b + PG), 0);
    KT_EQ(aspace_pte(as, b + 2 * PG), 0);
    KT_EQ(pte_pa(as, a + 2 * PG), vmo_page_phys(v, 2 * PG));
    KT_EQ(pte_pa(as, b), vmo_page_phys(v, 2 * PG));
    KT_EQ(pte_pa(as, b + 3 * PG), vmo_page_phys(v, 5 * PG));
    /* A fault brings back a zero page. */
    KT_EQ(aspace_fault(as, b + PG, ASPACE_READ), OK);
    KT_EQ(*(uint64_t *)phys_to_virt(pte_pa(as, b + PG)), 0);
    KT_EQ(vmo_committed(v), 7 * PG);

    /* A kernel mapping (or pin) still blocks it. */
    void *kva;
    KT_EQ(vmo_map_kernel(v, 5 * PG, PG, 0, &kva), OK);
    KT_EQ(vmo_decommit(v, 0, 8 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 4 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_committed(v), 7 * PG);
    KT_EQ(vmo_unmap_kernel(v, kva), OK);

    /* Shrink under both mappings: the tail's entries go, faults past the
     * end fail, and growing back gives zero pages. */
    KT_EQ(vmo_set_size(v, 3 * PG), OK);
    KT_EQ(vmo_committed(v), 3 * PG);
    for (unsigned i = 3; i < 8; i++)
        KT_EQ(aspace_pte(as, a + i * PG), 0);
    KT_EQ(pte_pa(as, a + 2 * PG), vmo_page_phys(v, 2 * PG));
    KT_EQ(pte_pa(as, b), vmo_page_phys(v, 2 * PG));
    KT_EQ(aspace_pte(as, b + PG), 0);
    KT_EQ(aspace_fault(as, a + 5 * PG, ASPACE_READ), ERR_OUT_OF_RANGE);
    KT_EQ(aspace_fault(as, b + PG, ASPACE_READ), ERR_OUT_OF_RANGE);
    uint64_t c = 0;
    KT_EQ(aspace_map(as, v, 2 * PG, 2 * PG, R, &c), ERR_OUT_OF_RANGE);
    /* Splitting a mapping that now reaches past the end still works. */
    KT_EQ(aspace_protect(as, a + 2 * PG, 3 * PG, R), OK);
    KT_EQ(vmo_set_size(v, 8 * PG), OK);
    KT_EQ(aspace_fault(as, a + 5 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, a + 4 * PG, ASPACE_WRITE), ERR_ACCESS_DENIED);
    KT_EQ(*(uint64_t *)phys_to_virt(pte_pa(as, a + 5 * PG)), 0);
    KT_EQ(vmo_committed(v), 4 * PG);

    aspace_unref(as);
    put(v);
}

/* Big decommits and shrinks go a leaf table at a time (O8): across several
 * leaves, sparse and dense, with some of it mapped. */
KTEST(aspace_large_decommit_batches)
{
    uint64_t base = free_now();
    struct aspace *as = new_as();
    struct vmo *v;
    enum { PAGES = 3000 };   /* ~6 leaves */
    KT_EQ(vmo_create(PAGES * PG, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, PAGES * PG), OK);
    uint64_t a = 0x10000000;
    KT_EQ(aspace_map(as, v, 0, PAGES * PG, RW | ASPACE_FIXED, &a), OK);
    for (unsigned i = 0; i < PAGES; i += 7)
        KT_EQ(aspace_fault(as, a + i * PG, ASPACE_READ), OK);
    KT_EQ(vmo_decommit(v, 100 * PG, 2500 * PG), OK);
    KT_EQ(vmo_committed(v), 500 * PG);
    for (unsigned i = 0; i < PAGES; i += 7) {
        bool gone = i >= 100 && i < 2600;
        KT_EQ(pte_pa(as, a + i * PG), gone ? 0 : vmo_page_phys(v, i * PG));
    }
    KT_EQ(vmo_set_size(v, 50 * PG), OK);
    KT_EQ(vmo_committed(v), 50 * PG);
    KT_EQ(pte_pa(as, a + 2604 * PG), 0);
    KT_EQ(pte_pa(as, a + 49 * PG), vmo_page_phys(v, 49 * PG));
    aspace_unref(as);
    put(v);
    KT_ASSERT(base - free_now() <= 2);
}

KTEST(aspace_destroy_frees_tables)
{
    struct vmo *keep;
    KT_EQ(vmo_create(PG, 0, &keep), OK);   /* keeps the slabs warm */
    struct aspace *warm = new_as();
    uint64_t w = 0;
    KT_EQ(aspace_map(warm, keep, 0, PG, R, &w), OK);

    uint64_t base = free_now();
    struct aspace *as = new_as();
    struct vmo *v;
    KT_EQ(vmo_create(4 * PG, 0, &v), OK);
    /* Far apart: separate PML4 slots, PDPTs and page tables. */
    static const uint64_t where[] = { 0x1000, 0x40000000, 0x8000000000ull, 0x400000000000ull,
                                      0x7fffffffc000ull - 0x4000 };
    for (unsigned i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        uint64_t a = where[i];
        KT_EQ(aspace_map(as, v, 0, 4 * PG, RW | ASPACE_FIXED, &a), OK);
        for (unsigned p = 0; p < 4; p++)
            KT_EQ(aspace_fault(as, a + p * PG, ASPACE_WRITE), OK);
    }
    /* PML4 slots 0, 1, 128, 255: 4 PDPTs; 5 PDs (slot 0 has two); 5 PTs. */
    KT_EQ(aspace_pt_pages(as), 14);
    KT_EQ(vmo_committed(v), 4 * PG);
    KT_EQ(vmo_kobject(v)->refs, 6);
    uint64_t before = free_now();
    aspace_unref(as);
    KT_ASSERT(free_now() - before >= 14 + 1);   /* the tables and the PML4 */
    KT_EQ(vmo_kobject(v)->refs, 1);
    put(v);
    KT_ASSERT(base - free_now() <= 1);   /* at most a slab page */

    aspace_unref(warm);
    put(keep);
}

/* ---- stress ------------------------------------------------------------------ */

enum { ST_AS = 3, ST_PAGES = 64, ST_SLOTS = 12, ST_MAX_THREADS = 8 };
static struct aspace *st_as[ST_AS];
static struct vmo *st_vmo;
static volatile int st_stop, st_started;
static volatile uint64_t st_ops[ST_MAX_THREADS];
static volatile int st_bad;

struct st_map {
    struct aspace *as;
    uint64_t       addr, off, len;
};
static struct st_map st_maps[ST_MAX_THREADS][ST_SLOTS];

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void st_bad_result(const char *what, status_t st)
{
    kprintf("aspace stress: %s returned %d\n", what, st);
    __atomic_add_fetch(&st_bad, 1, __ATOMIC_RELAXED);
}

static void st_worker(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    uint64_t s = 0x9e3779b97f4a7c15ull * (id + 1);
    struct st_map *maps = st_maps[id];
    __atomic_add_fetch(&st_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&st_stop, __ATOMIC_ACQUIRE)) {
        struct st_map *m = &maps[rnd(&s) % ST_SLOTS];
        status_t st;
        switch (rnd(&s) % 12) {
        case 0: case 1:   /* map into an empty slot */
            if (m->as)
                break;
            m->as = st_as[rnd(&s) % ST_AS];
            m->len = (1 + rnd(&s) % 8) * PG;
            m->off = (rnd(&s) % (ST_PAGES - 8)) * PG;
            m->addr = 0;
            st = aspace_map(m->as, st_vmo, m->off, m->len,
                            (rnd(&s) & 1 ? RW : R) | ASPACE_CAN_WRITE, &m->addr);
            if (st != OK) {
                if (st != ERR_OUT_OF_RANGE)   /* the VMO may be shrunk right now */
                    st_bad_result("map", st);
                m->as = NULL;
            }
            break;
        case 2: case 3: case 4: case 5:   /* fault a page of one of ours */
            if (!m->as)
                break;
            st = aspace_fault(m->as, m->addr + (rnd(&s) % (m->len / PG)) * PG + 8,
                              rnd(&s) & 1 ? ASPACE_WRITE : ASPACE_READ);
            if (st != OK && st != ERR_ACCESS_DENIED && st != ERR_OUT_OF_RANGE)
                st_bad_result("fault", st);
            break;
        case 6:   /* protect part of one of ours */
            if (!m->as)
                break;
            st = aspace_protect(m->as, m->addr + (rnd(&s) % (m->len / PG)) * PG, PG,
                                rnd(&s) & 1 ? RW : R);
            if (st != OK)
                st_bad_result("protect", st);
            break;
        case 7:   /* unmap one of ours */
            if (!m->as)
                break;
            st = aspace_unmap(m->as, m->addr, m->len);
            if (st != OK)
                st_bad_result("unmap", st);
            m->as = NULL;
            break;
        case 8: {   /* decommit a random range */
            uint64_t first = rnd(&s) % ST_PAGES, n = 1 + rnd(&s) % 16;
            st = vmo_decommit(st_vmo, first * PG, n * PG);
            if (st != OK && st != ERR_OUT_OF_RANGE)
                st_bad_result("decommit", st);
            break;
        }
        case 9: {   /* commit through a write */
            uint64_t val = rnd(&s);
            st = vmo_write(st_vmo, (rnd(&s) % ST_PAGES) * PG + 16, &val, 8);
            if (st != OK && st != ERR_OUT_OF_RANGE)
                st_bad_result("write", st);
            break;
        }
        case 10:   /* shrink and grow back (thread 0 only) */
            if (id != 0 || rnd(&s) % 8)
                break;
            if (vmo_set_size(st_vmo, (ST_PAGES / 2) * PG) != OK ||
                vmo_set_size(st_vmo, ST_PAGES * PG) != OK)
                st_bad_result("set_size", ERR_INTERNAL);
            break;
        case 11: {   /* run on an address space for a moment, so shootdowns hit us */
            struct aspace *as = st_as[rnd(&s) % ST_AS];
            preempt_disable();
            uint64_t f = irq_save();
            aspace_switch(NULL, as);
            irq_restore(f);
            uint64_t until = uptime_ns() + 20000;
            while (uptime_ns() < until)
                cpu_relax();
            f = irq_save();
            aspace_switch(as, NULL);
            irq_restore(f);
            preempt_enable();
            break;
        }
        }
        st_ops[id]++;
    }
}

KTEST(aspace_stress)
{
    uint64_t base = free_now();
    KT_EQ(vmo_create(ST_PAGES * PG, 0, &st_vmo), OK);
    for (int i = 0; i < ST_AS; i++)
        st_as[i] = new_as();
    uint32_t n = cpu_count < ST_MAX_THREADS ? cpu_count : ST_MAX_THREADS;
    if (n < 2)
        n = 2;
    memset(st_maps, 0, sizeof(st_maps));
    st_stop = st_started = st_bad = 0;
    struct thread *t[ST_MAX_THREADS];
    for (uint32_t i = 0; i < n; i++) {
        st_ops[i] = 0;
        cpumask_t m;
        cpumask_one(&m, i % cpu_count);
        t[i] = thread_create_on("as-stress", st_worker, (void *)(uintptr_t)i, PRIO_DEFAULT, &m);
    }
    thread_sleep_ms(3000);
    __atomic_store_n(&st_stop, 1, __ATOMIC_RELEASE);
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        thread_join(t[i]);
        total += st_ops[i];
    }
    kprintf("aspace stress: %u threads, %lu operations\n", n, total);
    KT_EQ(st_bad, 0);
    KT_ASSERT(total > 1000);

    /* Every entry still present names the VMO's current page for it. */
    uint64_t present = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (unsigned k = 0; k < ST_SLOTS; k++) {
            struct st_map *m = &st_maps[i][k];
            if (!m->as)
                continue;
            for (uint64_t p = 0; p < m->len; p += PG) {
                uint64_t pa = pte_pa(m->as, m->addr + p);
                if (pa) {
                    KT_EQ(pa, vmo_page_phys(st_vmo, m->off + p));
                    present++;
                }
            }
        }
    }
    kprintf("aspace stress: %lu entries checked\n", present);

    for (int i = 0; i < ST_AS; i++) {
        aspace_unmap(st_as[i], USER_BASE, USER_TOP - USER_BASE);
        KT_EQ(aspace_mapping_count(st_as[i]), 0);
        KT_EQ(aspace_pt_pages(st_as[i]), 0);
        aspace_unref(st_as[i]);
    }
    KT_EQ(vmo_kobject(st_vmo)->refs, 1);
    put(st_vmo);
    thread_sleep_ms(10);
    /* Thread stacks may have joined the scheduler's cache; the harness
     * accounts those. Page tables and VMO pages must all be back. */
    KT_ASSERT(base - free_now() <= n * 16 + 4);
}
