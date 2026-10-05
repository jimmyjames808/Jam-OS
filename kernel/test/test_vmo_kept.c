/* Tests for kept VMOs (VMO_KEEP_PAGES) and kept mappings (ASPACE_KEPT_ONLY,
 * VMAR_KEPT_ONLY): every page committed and charged at creation and at
 * growth, every way to take a page away refused for every holder, and a
 * kept mapping's entries filled at map time, so that a reader never
 * faults whatever the VMO's other holders do.
 *
 * "Never faults" is measured, not assumed: a kernel thread takes on the
 * test's address space the way a user thread does (t->aspace and its CR3)
 * and reads through it with copy_from_user, whose faults go to
 * aspace_fault like a user thread's; aspace_fault_count counts them. A
 * plain mapping read the same way is the control (one fault per page). */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/cpu.h>
#include <jam/handle.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/usercopy.h>
#include <jam/vmar.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define R     ASPACE_READ
#define RW    (ASPACE_READ | ASPACE_WRITE)
#define KEPT  ASPACE_KEPT_ONLY
#define MAGIC 0x6b65707470616765ull   /* "keptpage" */

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_U    (1ull << 2)
#define PTE_NX   (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

/* Page i of v starts with MAGIC ^ i. */
static void stamp(struct vmo *v, uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t w = MAGIC ^ i;
        KT_EQ(vmo_write(v, i * PG, &w, sizeof(w)), OK);
    }
}

/* ---- reading through an address space, counting its faults --------------- */

struct reader {
    struct aspace *as;       /* the address space to read through */
    uint64_t       addr;     /* first page */
    uint64_t       pages;    /* how many, each read at its first word */
    uint64_t       first;    /* VMO page index at addr (for the stamp) */
    status_t       st;       /* the first failed copy, or OK */
    uint64_t       wrong;    /* pages whose word wasn't MAGIC ^ index */
    uint64_t       faults;   /* aspace_fault calls during the reads */
};

/* A kernel thread that takes on r->as as a user thread does (uthread_main),
 * reads, and leaves it before it ends. */
static void reader_main(void *arg)
{
    struct reader *r = arg;
    struct thread *t = current_thread();
    aspace_ref(r->as);
    irq_disable();
    t->aspace = r->as;
    aspace_switch(NULL, r->as);
    irq_enable();
    uint64_t before = aspace_fault_count(r->as);
    r->st = OK;
    r->wrong = 0;
    for (uint64_t i = 0; i < r->pages && r->st == OK; i++) {
        uint64_t w = 0;
        r->st = copy_from_user(&w, r->addr + i * PG, sizeof(w));
        r->wrong += r->st == OK && w != (MAGIC ^ (r->first + i));
    }
    r->faults = aspace_fault_count(r->as) - before;
    irq_disable();
    t->aspace = NULL;
    aspace_switch(r->as, NULL);
    irq_enable();
    aspace_unref(r->as);
}

static void read_through(struct reader *r)
{
    struct thread *t = thread_create_on("kt-kept-reader", reader_main, r, PRIO_DEFAULT, NULL);
    KT_ASSERT(t);
    thread_join(t);
}

/* Every page of [addr, addr + pages) has a present, user, read-only, NX
 * entry naming VMO page first + i. */
static void entries_filled(struct aspace *as, struct vmo *v, uint64_t addr, uint64_t first,
                           uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t e = aspace_pte(as, addr + i * PG);
        KT_ASSERT((e & PTE_P) && (e & PTE_U));
        KT_ASSERT(!(e & PTE_W));
        if (cpu_features.nx)
            KT_ASSERT(e & PTE_NX);
        KT_EQ(e & PTE_ADDR, vmo_page_phys(v, (first + i) * PG));
    }
}

/* ---- creation --------------------------------------------------------------- */

/* What vmo_create refuses, and a kept VMO's pages are all there at once. */
KTEST(vmo_kept_create)
{
    struct vmo *v;
    KT_EQ(vmo_create(4 * PG, VMO_KEEP_PAGES | VMO_CONTIGUOUS, &v), ERR_INVALID_ARGS);
    KT_EQ(vmo_create(4 * PG, VMO_KEEP_PAGES | VMO_DMA32, &v), ERR_INVALID_ARGS);
    KT_EQ(vmo_create(4 * PG, 1u << 3, &v), ERR_INVALID_ARGS);
    KT_EQ(vmo_create(VMO_MAX_SIZE + PG, VMO_KEEP_PAGES, &v), ERR_OUT_OF_RANGE);

    KT_EQ(vmo_create(5 * PG - 100, VMO_KEEP_PAGES, &v), OK);
    KT_ASSERT(vmo_is_kept(v));
    KT_EQ(vmo_size(v), 5 * PG);
    KT_EQ(vmo_committed(v), 5 * PG);
    for (uint64_t i = 0; i < 5; i++)
        KT_ASSERT(vmo_page_phys(v, i * PG) != 0);
    put(v);

    KT_EQ(vmo_create(0, VMO_KEEP_PAGES, &v), OK);   /* empty: grows later */
    KT_EQ(vmo_committed(v), 0);
    put(v);
    KT_EQ(vmo_create(PG, 0, &v), OK);
    KT_ASSERT(!vmo_is_kept(v));
    put(v);
}

/* Every page (and its tables) is charged to the creator's job before it
 * exists; a job that can't pay for all of them gets no VMO and keeps no
 * charge, and the memory comes back. */
KTEST(vmo_kept_charges_creator)
{
    struct job *j = kt_fresh_job();
    struct vmo *v;
    KT_EQ(vmo_create_for(j, 10 * PG, VMO_KEEP_PAGES, &v), OK);
    /* 10 pages, a mid and a leaf table; the struct is a handle unit. */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 12);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 1);
    put(v);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);

    uint64_t before = kt_free_pages();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 11), OK);   /* one short */
    KT_EQ(vmo_create_for(j, 10 * PG, VMO_KEEP_PAGES, &v), ERR_NO_MEMORY);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), 0);
    KT_GLOBAL_ASSERT(kt_free_pages() + 2 >= before);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 12), OK);   /* exactly enough */
    KT_EQ(vmo_create_for(j, 10 * PG, VMO_KEEP_PAGES, &v), OK);
    put(v);

    /* A plain VMO still commits nothing at creation. */
    KT_EQ(vmo_create_for(j, 10 * PG, 0, &v), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 0);
    put(v);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- the refusals ------------------------------------------------------------- */

/* Decommit and shrink are refused for every holder, through the object and
 * through handles with every right; what stays allowed still works. */
KTEST(vmo_kept_refuses_page_drops)
{
    struct vmo *v;
    KT_EQ(vmo_create(4 * PG, VMO_KEEP_PAGES, &v), OK);
    stamp(v, 4);
    uint64_t pa = vmo_page_phys(v, 2 * PG);
    KT_EQ(vmo_decommit(v, 0, 4 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_decommit(v, 2 * PG, PG), ERR_BAD_STATE);
    KT_EQ(vmo_decommit(v, 0, 0), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 3 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 0), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 4 * PG - 1), OK);   /* rounds up to the same size */
    KT_EQ(vmo_commit(v, 0, 4 * PG), OK);       /* nothing to do */
    KT_EQ(vmo_size(v), 4 * PG);
    KT_EQ(vmo_committed(v), 4 * PG);
    KT_EQ(vmo_page_phys(v, 2 * PG), pa);

    /* Through handles: the creator's full one and a second holder's. */
    struct handle_table t;
    handle_table_init(&t);
    kobject_ref(vmo_kobject(v));
    struct khandle kh = khandle_from_new(vmo_kobject(v), RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE |
                                                           RIGHT_MAP | RIGHT_RESIZE);
    handle_t h, h2;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    KT_EQ(handle_duplicate(&t, h, RIGHT_SAME, &h2), OK);
    KT_EQ(sys_vmo_decommit(&t, h, 0, PG), ERR_BAD_STATE);
    KT_EQ(sys_vmo_decommit(&t, h2, PG, 3 * PG), ERR_BAD_STATE);
    KT_EQ(sys_vmo_set_size(&t, h2, PG), ERR_BAD_STATE);
    KT_EQ(sys_vmo_write(&t, h2, 8, "x", 1), OK);
    handle_table_destroy(&t);

    KT_EQ(vmo_committed(v), 4 * PG);
    uint64_t w;
    for (uint64_t i = 0; i < 4; i++) {
        KT_EQ(vmo_read(v, i * PG, &w, sizeof(w)), OK);
        KT_EQ(w, MAGIC ^ i);
    }
    put(v);
}

/* Growing commits the new pages (charged) before the size shows them; a
 * growth the job can't pay for changes nothing: size, pages, charges. */
KTEST(vmo_kept_grow_commits)
{
    struct job *j = kt_fresh_job();
    struct vmo *v;
    KT_EQ(vmo_create_for(j, 2 * PG, VMO_KEEP_PAGES, &v), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 4);   /* 2 pages, mid, leaf */
    KT_EQ(vmo_set_size(v, 6 * PG), OK);
    KT_EQ(vmo_committed(v), 6 * PG);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 8);
    for (uint64_t i = 0; i < 6; i++)
        KT_ASSERT(vmo_page_phys(v, i * PG) != 0);

    /* To 518 pages: 506 more in this leaf, then a new leaf and 6 pages in
     * it. The job pays for the 506, the new leaf and 3 of its pages, so the
     * failure has a leaf of its own to give back too. */
    uint64_t before = kt_free_pages();
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 8 + 506 + 1 + 3), OK);
    KT_EQ(vmo_set_size(v, 518 * PG), ERR_NO_MEMORY);
    KT_EQ(vmo_size(v), 6 * PG);
    KT_EQ(vmo_committed(v), 6 * PG);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 8);
    KT_GLOBAL_ASSERT(kt_free_pages() + 2 >= before);
    uint64_t w = 1;
    KT_EQ(vmo_read(v, 6 * PG, &w, sizeof(w)), ERR_OUT_OF_RANGE);

    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    KT_EQ(vmo_set_size(v, 518 * PG), OK);
    KT_EQ(vmo_committed(v), 518 * PG);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 518 + 3);   /* pages, mid, two leaves */
    put(v);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- kept mappings -------------------------------------------------------------- */

/* What a kept mapping refuses: any VMO that isn't kept, any permission but
 * read; and once made, any change of its permissions. Through the handle
 * layer too: rights and handle types as for any map. */
KTEST(vmo_kept_map_refusals)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *plain, *kept, *contig;
    KT_EQ(vmo_create(2 * PG, 0, &plain), OK);
    KT_EQ(vmo_create(2 * PG, VMO_KEEP_PAGES, &kept), OK);
    KT_EQ(vmo_create(2 * PG, VMO_CONTIGUOUS, &contig), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, plain, 0, PG, R | KEPT, &a), ERR_WRONG_TYPE);
    KT_EQ(aspace_map(as, contig, 0, PG, R | KEPT, &a), ERR_WRONG_TYPE);
    KT_EQ(aspace_map(as, kept, 0, PG, RW | KEPT, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, kept, 0, PG, R | ASPACE_EXEC | KEPT, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, kept, 0, PG, KEPT, &a), ERR_INVALID_ARGS);
    KT_EQ(aspace_map(as, kept, 0, 3 * PG, R | KEPT, &a), ERR_OUT_OF_RANGE);
    KT_EQ(aspace_mapping_count(as), 0);
    KT_EQ(aspace_pt_pages(as), 0);

    /* The CAN bits don't apply, and no protect changes it, not even to
     * "no access" (which would clear its entries). */
    KT_EQ(aspace_map(as, kept, 0, 2 * PG, R | KEPT | ASPACE_CAN_WRITE, &a), OK);
    KT_EQ(aspace_protect(as, a, 2 * PG, RW), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, a, PG, 0), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(as, a, 2 * PG, R), OK);
    entries_filled(as, kept, a, 0, 2);
    /* What an unmap leaves of it stays kept. */
    KT_EQ(aspace_unmap(as, a, PG), OK);
    KT_EQ(aspace_protect(as, a + PG, PG, 0), ERR_ACCESS_DENIED);
    entries_filled(as, kept, a + PG, 1, 1);
    aspace_unref(as);

    /* sys_vmar_map: the flag passes; rights and types are checked first. */
    struct handle_table t;
    handle_table_init(&t);
    handle_t vh, kh, ph, nomap, ro;
    KT_EQ(sys_vmar_create(&t, &vh), OK);
    KT_EQ(sys_vmo_create(&t, 2 * PG, VMO_KEEP_PAGES, HANDLE_INVALID, &kh), OK);
    KT_EQ(sys_vmo_create(&t, 2 * PG, 0, HANDLE_INVALID, &ph), OK);
    KT_EQ(handle_duplicate(&t, kh, RIGHTS_BASIC | RIGHT_READ, &nomap), OK);
    KT_EQ(handle_duplicate(&t, kh, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP, &ro), OK);
    KT_EQ(sys_vmar_map(&t, vh, ph, 0, PG, R | KEPT, &a), ERR_WRONG_TYPE);
    KT_EQ(sys_vmar_map(&t, vh, nomap, 0, PG, R | KEPT, &a), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_map(&t, vh, vh, 0, PG, R | KEPT, &a), ERR_WRONG_TYPE);
    KT_EQ(sys_vmar_map(&t, vh, kh, 0, PG, R | KEPT | (1u << 5), &a), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_map(&t, vh, kh, 0, PG, RW | KEPT, &a), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_map(&t, vh, ro, 0, 2 * PG, R | KEPT, &a), OK);
    KT_EQ(sys_vmar_protect(&t, vh, a, PG, 0), ERR_ACCESS_DENIED);
    handle_table_destroy(&t);

    put(plain);
    put(kept);
    put(contig);
}

/* A kept mapping has every entry at map time, its page tables charged to
 * the mapper then, and a read of every page takes no fault; the same
 * reads through a plain mapping take one per page (the control). */
KTEST(vmo_kept_map_never_faults)
{
    enum { N = 40 };
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *v, *plain;
    KT_EQ(vmo_create(N * PG, VMO_KEEP_PAGES, &v), OK);
    KT_EQ(vmo_create(N * PG, 0, &plain), OK);
    stamp(v, N);
    stamp(plain, N);

    /* Across a 2 MiB line, so two page tables. */
    uint64_t a = 0x40000000 + (1ull << 21) - 8 * PG;
    uint64_t used = job_used(j, JOB_LIMIT_PAGES);
    KT_EQ(aspace_map(as, v, 0, N * PG, R | KEPT | ASPACE_FIXED, &a), OK);
    KT_EQ(aspace_pt_pages(as), 4);   /* PDPT, PD, two PTs */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), used + 4 + 1);   /* + the mapping struct page */
    entries_filled(as, v, a, 0, N);
    KT_EQ(aspace_fault_count(as), 0);

    struct reader r = { .as = as, .addr = a, .pages = N };
    read_through(&r);
    KT_EQ(r.st, OK);
    KT_EQ(r.wrong, 0);
    KT_EQ(r.faults, 0);

    uint64_t b = 0;
    KT_EQ(aspace_map(as, plain, 0, N * PG, R, &b), OK);
    struct reader c = { .as = as, .addr = b, .pages = N };
    read_through(&c);
    KT_EQ(c.st, OK);
    KT_EQ(c.wrong, 0);
    KT_EQ(c.faults, N);   /* the counter does count */
    kprintf("vmo_kept_map_never_faults: %d pages: kept mapping %lu faults, plain %lu\n", N,
            r.faults, c.faults);

    aspace_unref(as);
    put(v);
    put(plain);
    kt_job_is_empty(j);
    job_unref(j);
}

/* What another holder of the VMO can do while it is mapped kept: decommit
 * and shrink are refused, growth leaves the mapping as it was (same
 * length, same pages), and the mapper still reads every page with no
 * fault; a new map of the grown VMO is filled too. */
KTEST(vmo_kept_holder_cannot_make_mapper_fault)
{
    enum { N = 8 };
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v;
    KT_EQ(vmo_create(N * PG, VMO_KEEP_PAGES, &v), OK);
    stamp(v, N);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, N * PG, R | KEPT, &a), OK);
    uint64_t pa[N];
    for (unsigned i = 0; i < N; i++)
        pa[i] = vmo_page_phys(v, i * PG);

    /* The "client" tries everything. */
    KT_EQ(vmo_decommit(v, 0, N * PG), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, PG), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 2 * N * PG), OK);
    for (uint64_t i = N; i < 2 * N; i++) {
        uint64_t w = MAGIC ^ i;
        KT_EQ(vmo_write(v, i * PG, &w, sizeof(w)), OK);
    }
    KT_EQ(aspace_mapping_count(as), 1);
    KT_EQ(aspace_pte(as, a + N * PG), 0);   /* the mapping didn't grow */
    for (unsigned i = 0; i < N; i++)
        KT_EQ(aspace_pte(as, a + i * PG) & PTE_ADDR, pa[i]);

    struct reader r = { .as = as, .addr = a, .pages = N };
    read_through(&r);
    KT_EQ(r.st, OK);
    KT_EQ(r.wrong, 0);
    KT_EQ(r.faults, 0);

    /* The compositor's resize: map the bigger range, drop the old one. */
    uint64_t b = 0;
    KT_EQ(aspace_map(as, v, 0, 2 * N * PG, R | KEPT, &b), OK);
    KT_EQ(aspace_unmap(as, a, N * PG), OK);
    entries_filled(as, v, b, 0, 2 * N);
    struct reader r2 = { .as = as, .addr = b, .pages = 2 * N };
    read_through(&r2);
    KT_EQ(r2.st, OK);
    KT_EQ(r2.wrong, 0);
    KT_EQ(r2.faults, 0);
    aspace_unref(as);
    put(v);
}

/* A map whose page tables the mapper's job can't pay for fails whole,
 * there and then: no mapping, no entries, no tables, no charge left. */
KTEST(vmo_kept_map_charge_refused)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *v;
    KT_EQ(vmo_create(16 * PG, VMO_KEEP_PAGES, &v), OK);
    uint64_t used = job_used(j, JOB_LIMIT_PAGES);   /* the PML4 */
    /* The mapping struct, PDPT, PD and the first PT fit; the second PT not. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, used + 4), OK);
    uint64_t a = 0x40000000 + (1ull << 21) - 8 * PG;
    KT_EQ(aspace_map(as, v, 0, 16 * PG, R | KEPT | ASPACE_FIXED, &a), ERR_NO_MEMORY);
    KT_EQ(aspace_mapping_count(as), 0);
    KT_EQ(aspace_pt_pages(as), 0);
    KT_EQ(aspace_pte(as, a), 0);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), used);

    /* One page more and it fits. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, used + 5), OK);
    KT_EQ(aspace_map(as, v, 0, 16 * PG, R | KEPT | ASPACE_FIXED, &a), OK);
    entries_filled(as, v, a, 0, 16);
    aspace_unref(as);
    put(v);
    kt_job_is_empty(j);
    job_unref(j);
}
