/* Tests for copy-on-write child VMOs (vmo_create_child; the rules are at
 * the top of kernel/object/vmo.c, the design in docs/M13-PLAN.md's "F1a
 * design"): pages shared until either side writes, separated by vmo_write,
 * by a fault, by a pin or a kernel mapping; children of children and every
 * order of their deaths; the charge at creation; children of pager VMOs
 * across eviction, invalidation, a size change and a detach; writers on
 * every CPU while children are made, written and dropped; the refusals.
 *
 * Faults are made two ways: aspace_fault called directly, to look at the
 * entries it leaves (aspace_pte), and kernel threads that enter an address
 * space and copy to and from its user addresses (struct cpy), which goes
 * through the real page-fault path (trap_page_fault) on whatever CPU they
 * run. Bytes are checked through vmo_read and, for a page an entry names,
 * through its HHDM alias. Counts of copies (vmo_cow_copies) are only
 * checked on an idle machine. The harness checks every test for leaked
 * pages; the destruction-order test counts them itself as well. */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>
#include <jam/x86.h>

#define PG     PAGE_SIZE
#define RW     (ASPACE_READ | ASPACE_WRITE)
#define SNAP   VMO_CHILD_SNAPSHOT
#define ALOW   VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE
#define U_A    0x20000000ull   /* where the tests map */
#define ANSWER (600 * NS_PER_S)

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_ADDR 0x000ffffffffff000ull

/* ---- helpers ------------------------------------------------------------- */

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct vmo *mk(uint64_t pages)
{
    struct vmo *v;
    KT_EQ(vmo_create(pages * PG, 0, &v), OK);
    return v;
}

static struct vmo *child(struct vmo *p, uint32_t flags, uint64_t off, uint64_t size)
{
    struct vmo *c;
    KT_EQ(vmo_create_child(NULL, p, flags, off, size, &c), OK);
    return c;
}

/* Fill page `page` of v with the word w (vmo_write: a separation if the
 * page is shared). */
static void stamp(struct vmo *v, uint64_t page, uint64_t w)
{
    uint64_t *buf = kmalloc(PG);
    KT_ASSERT(buf);
    for (unsigned i = 0; i < PG / 8; i++)
        buf[i] = w;
    KT_EQ(vmo_write(v, page * PG, buf, PG), OK);
    kfree(buf);
}

/* Every word of page `page` of v is w. */
static bool page_is(struct vmo *v, uint64_t page, uint64_t w)
{
    uint64_t *buf = kmalloc(PG);
    KT_ASSERT(buf);
    KT_EQ(vmo_read(v, page * PG, buf, PG), OK);
    bool ok = true;
    for (unsigned i = 0; i < PG / 8 && ok; i++)
        ok = buf[i] == w;
    kfree(buf);
    return ok;
}

/* The first word of page `page` of v. */
static uint64_t w0(struct vmo *v, uint64_t page)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(v, page * PG, &w, 8), OK);
    return w;
}

static uint64_t ph(struct vmo *v, uint64_t page)
{
    return vmo_page_phys(v, page * PG);
}

static uint64_t copies(void)
{
    return __atomic_load_n(&vmo_cow_copies, __ATOMIC_RELAXED);
}

static struct aspace *new_as(void)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    return as;
}

static void map_at(struct aspace *as, struct vmo *v, uint64_t pages, unsigned perms)
{
    uint64_t a = U_A;
    KT_EQ(aspace_map(as, v, 0, pages * PG, perms | ASPACE_FIXED, &a), OK);
}

static uint64_t pte(struct aspace *as, uint64_t page)
{
    return aspace_pte(as, U_A + page * PG);
}

static uint64_t pte_pa(struct aspace *as, uint64_t page)
{
    uint64_t e = pte(as, page);
    return e & PTE_P ? e & PTE_ADDR : 0;
}

/* The first word of the physical page at pa. */
static uint64_t at(uint64_t pa)
{
    return *(const uint64_t *)phys_to_virt(pa);
}

/* Move the current kernel thread into address space `as` (NULL: out of
 * `was`), as a system call's copies run. */
static void enter_aspace(struct aspace *as, struct aspace *was)
{
    struct thread *t = current_thread();
    if (as)
        aspace_ref(as);
    irq_disable();
    t->aspace = as;
    aspace_switch(was, as);
    irq_enable();
    if (was)
        aspace_unref(was);
}

/* A kernel thread that enters an address space and copies one word to
 * or from a user address there: the real fault path, which may wait for
 * a pager. */
struct cpy {
    struct aspace *as;
    uint64_t       addr;
    bool           to;       /* copy_to_user (a write), else copy_from_user */
    uint64_t       word;     /* written, or read */
    status_t       st;       /* ERR_INTERNAL until it returns */
    struct thread *t;
};

static void cpy_main(void *arg)
{
    struct cpy *c = arg;
    enter_aspace(c->as, NULL);
    status_t st = c->to ? copy_to_user(c->addr, &c->word, 8) : copy_from_user(&c->word, c->addr, 8);
    enter_aspace(NULL, c->as);
    __atomic_store_n(&c->st, st, __ATOMIC_RELEASE);
}

static void cpy_start(struct cpy *c, struct aspace *as, uint64_t addr, bool to, uint64_t word)
{
    *c = (struct cpy){ .as = as, .addr = addr, .to = to, .word = word, .st = ERR_INTERNAL };
    c->t = thread_create("cow-copy", cpy_main, c, PRIO_DEFAULT);
}

static status_t cpy_end(struct cpy *c)
{
    thread_join(c->t);
    return c->st;
}

/* ---- the basics ------------------------------------------------------------ */

/* A child shares every page until a write: then only the writer's page
 * changes, whichever side writes, and a page left in one table is written
 * in place (no copy) by its holder from then on, the survivor of the
 * other's death included. */
KTEST(vmo_child_shares_until_write)
{
    struct vmo *p = mk(8);
    for (uint64_t i = 0; i < 8; i++)
        stamp(p, i, 0x100 + i);
    uint64_t c0 = copies();
    struct vmo *c = child(p, SNAP, 0, 8 * PG);
    KT_EQ(vmo_size(c), 8 * PG);
    KT_EQ(vmo_committed(c), 8 * PG);   /* each table counts (and pays for) every page */
    KT_EQ(vmo_committed(p), 8 * PG);
    for (uint64_t i = 0; i < 8; i++) {
        KT_EQ(ph(c, i), ph(p, i));
        KT_ASSERT(page_is(c, i, 0x100 + i));
    }

    uint64_t old2 = ph(p, 2), old5 = ph(p, 5);
    stamp(p, 2, 0x222);   /* the parent writes: its page alone changes */
    KT_ASSERT(ph(p, 2) != old2);
    KT_EQ(ph(c, 2), old2);
    KT_ASSERT(page_is(p, 2, 0x222));
    KT_ASSERT(page_is(c, 2, 0x102));
    stamp(c, 5, 0x555);   /* the child writes: the same the other way */
    KT_ASSERT(ph(c, 5) != old5);
    KT_EQ(ph(p, 5), old5);
    KT_ASSERT(page_is(c, 5, 0x555));
    KT_ASSERT(page_is(p, 5, 0x105));
    KT_IDLE_EQ(copies() - c0, 2);

    /* Separated pages are their holder's alone: written in place. */
    uint64_t new2 = ph(p, 2);
    stamp(p, 2, 0x2222);
    KT_EQ(ph(p, 2), new2);
    stamp(c, 2, 0x1222);   /* the old page, which the parent let go */
    KT_EQ(ph(c, 2), old2);
    stamp(p, 5, 0x5555);
    KT_EQ(ph(p, 5), old5);
    KT_IDLE_EQ(copies() - c0, 2);
    for (uint64_t i = 0; i < 8; i++)
        if (i != 2 && i != 5)
            KT_EQ(ph(c, i), ph(p, i));

    /* The parent dies: every page is the child's alone. */
    put(p);
    for (uint64_t i = 0; i < 8; i++) {
        uint64_t was = ph(c, i);
        stamp(c, i, 0xc00 + i);
        KT_EQ(ph(c, i), was);
    }
    KT_IDLE_EQ(copies() - c0, 2);
    put(c);
}

/* A snapshot holds the parent's bytes as at the call, both ways: what the
 * parent commits, decommits or shrinks afterwards never shows in the child
 * and the other way; a child may start at an offset and reach past the
 * parent (zeros there), and decommitting a shared page only drops it from
 * the decommitting side. */
KTEST(vmo_child_snapshot_isolation)
{
    struct vmo *p = mk(8);
    for (uint64_t i = 0; i < 4; i++)
        stamp(p, i, 0x200 + i);   /* pages 4-7 never committed */
    struct vmo *c = child(p, SNAP, 0, 12 * PG);
    KT_EQ(vmo_size(c), 12 * PG);
    KT_EQ(vmo_committed(c), 4 * PG);
    stamp(p, 5, 0x205);   /* after the call: not the child's */
    KT_ASSERT(page_is(c, 5, 0));
    KT_EQ(ph(c, 5), 0);
    for (uint64_t i = 8; i < 12; i++)
        KT_ASSERT(page_is(c, i, 0));
    KT_EQ(vmo_decommit(p, PG, PG), OK);
    KT_ASSERT(page_is(c, 1, 0x201));
    KT_ASSERT(page_is(p, 1, 0));
    KT_EQ(vmo_set_size(p, 2 * PG), OK);
    KT_ASSERT(page_is(c, 2, 0x202) && page_is(c, 3, 0x203));
    KT_EQ(vmo_decommit(c, 0, PG), OK);
    KT_ASSERT(page_is(c, 0, 0));
    KT_ASSERT(page_is(p, 0, 0x200));
    stamp(c, 9, 0x909);   /* past the parent: an ordinary page of its own */
    KT_ASSERT(page_is(c, 9, 0x909));

    /* At an offset: the child's page 0 is the parent's page 1. */
    struct vmo *d = child(c, SNAP, PG, 3 * PG);
    KT_EQ(ph(d, 0), ph(c, 1));
    KT_ASSERT(page_is(d, 0, 0x201) && page_is(d, 1, 0x202) && page_is(d, 2, 0x203));
    KT_EQ(vmo_committed(d), 3 * PG);
    put(p);
    put(c);
    KT_ASSERT(page_is(d, 1, 0x202));
    put(d);
}

/* Through mappings: the creation takes the write bit off the parent's
 * entries (they stay present); a read fault maps a shared page read-only
 * even in a writable mapping; a write fault copies it and maps the copy
 * writable, leaving the other side's entry and page alone; once the other
 * side let go, its write fault maps the same page writable without a
 * copy; vmar_protect never adds the write bit. */
KTEST(vmo_child_faults)
{
    struct aspace *a = new_as(), *b = new_as();
    struct vmo *p = mk(4);
    for (uint64_t i = 0; i < 4; i++)
        stamp(p, i, 0x300 + i);
    map_at(a, p, 4, RW);
    for (uint64_t i = 0; i < 4; i++)
        KT_EQ(aspace_fault(a, U_A + i * PG, ASPACE_WRITE), OK);
    KT_ASSERT(pte(a, 3) & PTE_W);

    uint64_t c0 = copies();
    struct vmo *c = child(p, ALOW, 0, 4 * PG);
    for (uint64_t i = 0; i < 4; i++) {
        KT_ASSERT(pte(a, i) & PTE_P);
        KT_ASSERT(!(pte(a, i) & PTE_W));
        KT_EQ(pte_pa(a, i), ph(p, i));
    }
    map_at(b, c, 4, RW);

    KT_EQ(aspace_fault(b, U_A, ASPACE_READ), OK);   /* shared: read-only */
    KT_EQ(pte_pa(b, 0), ph(p, 0));
    KT_ASSERT(!(pte(b, 0) & PTE_W));
    KT_EQ(aspace_fault(b, U_A, ASPACE_WRITE), OK);   /* the child's copy */
    KT_ASSERT(pte(b, 0) & PTE_W);
    KT_EQ(pte_pa(b, 0), ph(c, 0));
    KT_ASSERT(ph(c, 0) != ph(p, 0));
    KT_EQ(at(ph(c, 0)), 0x300);
    KT_EQ(pte_pa(a, 0), ph(p, 0));   /* the parent's entry and page stay */
    KT_ASSERT(!(pte(a, 0) & PTE_W));

    uint64_t p0 = ph(p, 0);   /* alone in the parent now: no copy */
    KT_EQ(aspace_fault(a, U_A, ASPACE_WRITE), OK);
    KT_EQ(ph(p, 0), p0);
    KT_EQ(pte_pa(a, 0), p0);
    KT_ASSERT(pte(a, 0) & PTE_W);
    KT_IDLE_EQ(copies() - c0, 1);

    uint64_t old1 = ph(p, 1);   /* the parent separates page 1 */
    KT_EQ(aspace_fault(a, U_A + PG, ASPACE_WRITE), OK);
    KT_ASSERT(ph(p, 1) != old1);
    KT_EQ(pte_pa(a, 1), ph(p, 1));
    KT_EQ(ph(c, 1), old1);
    KT_EQ(aspace_fault(b, U_A + PG, ASPACE_READ), OK);   /* alone in the child: writable at once */
    KT_EQ(pte_pa(b, 1), old1);
    KT_ASSERT(pte(b, 1) & PTE_W);
    KT_IDLE_EQ(copies() - c0, 2);

    /* vmar_protect up to write keeps a shared page's entry read-only; the
     * write that follows separates it. */
    KT_EQ(aspace_fault(a, U_A + 2 * PG, ASPACE_READ), OK);
    KT_EQ(aspace_protect(a, U_A + 2 * PG, PG, ASPACE_READ), OK);
    KT_EQ(aspace_protect(a, U_A + 2 * PG, PG, RW), OK);
    KT_ASSERT(pte(a, 2) & PTE_P);
    KT_ASSERT(!(pte(a, 2) & PTE_W));
    KT_EQ(aspace_fault(a, U_A + 2 * PG, ASPACE_WRITE), OK);
    KT_ASSERT(ph(p, 2) != ph(c, 2));
    KT_ASSERT(pte(a, 2) & PTE_W);
    /* And for a page alone in its VMO: read-only after it, the write
     * fault maps the same page writable again. */
    KT_EQ(aspace_protect(a, U_A, PG, ASPACE_READ), OK);
    KT_EQ(aspace_protect(a, U_A, PG, RW), OK);
    KT_ASSERT(!(pte(a, 0) & PTE_W));
    KT_EQ(aspace_fault(a, U_A, ASPACE_WRITE), OK);
    KT_EQ(pte_pa(a, 0), p0);
    KT_IDLE_EQ(copies() - c0, 3);

    aspace_unref(a);
    aspace_unref(b);
    put(p);
    put(c);
}

/* ---- children of children, and every order of their deaths ----------------- */

/* p, its child c1 and c1's child c2, each written after the next was made:
 * every page is shared by the tables that hadn't written it, and each
 * holds its own bytes. Then the three die in each of the six orders; the
 * survivors' bytes never change, a page left in one table is written in
 * place, and every page comes back. */
KTEST(vmo_child_chains_and_orders)
{
    static const uint8_t orders[6][3] = {
        { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 },
    };
    struct vmo *keep = mk(1);   /* the slab for struct vmo stays across the counts */
    uint64_t base = kt_free_pages();
    for (unsigned o = 0; o < 6; o++) {
        struct vmo *v[3];
        v[0] = mk(4);
        for (uint64_t i = 0; i < 4; i++)
            stamp(v[0], i, 0x400 + i);
        v[1] = child(v[0], SNAP, 0, 4 * PG);
        stamp(v[1], 1, 0x411);
        v[2] = child(v[1], ALOW, 0, 4 * PG);
        stamp(v[2], 2, 0x422);
        stamp(v[0], 3, 0x403 + 0x10);

        /* What each holds: want[k][i]. */
        uint64_t want[3][4] = {
            { 0x400, 0x401, 0x402, 0x413 },
            { 0x400, 0x411, 0x402, 0x403 },
            { 0x400, 0x411, 0x422, 0x403 },
        };
        KT_EQ(ph(v[0], 0), ph(v[1], 0));
        KT_EQ(ph(v[1], 0), ph(v[2], 0));
        KT_EQ(ph(v[1], 1), ph(v[2], 1));
        KT_ASSERT(ph(v[0], 1) != ph(v[1], 1));
        KT_EQ(ph(v[0], 2), ph(v[1], 2));
        KT_ASSERT(ph(v[2], 2) != ph(v[1], 2));
        KT_EQ(ph(v[1], 3), ph(v[2], 3));
        KT_ASSERT(ph(v[0], 3) != ph(v[1], 3));
        for (unsigned k = 0; k < 3; k++)
            for (uint64_t i = 0; i < 4; i++)
                KT_ASSERT(page_is(v[k], i, want[k][i]));

        bool dead[3] = { false, false, false };
        for (unsigned step = 0; step < 3; step++) {
            unsigned d = orders[o][step];
            put(v[d]);
            dead[d] = true;
            for (unsigned k = 0; k < 3; k++)
                for (uint64_t i = 0; i < 4 && !dead[k]; i++)
                    KT_ASSERT(page_is(v[k], i, want[k][i]));
        }
        (void)dead;
    }
    KT_GLOBAL_EQ(kt_free_pages(), base);

    /* The last one standing writes in place. */
    struct vmo *a = mk(2);
    stamp(a, 0, 1);
    stamp(a, 1, 2);
    struct vmo *b = child(a, SNAP, 0, 2 * PG);
    struct vmo *c = child(b, SNAP, 0, 2 * PG);
    put(b);
    put(a);
    uint64_t was = ph(c, 0);
    stamp(c, 0, 3);
    KT_EQ(ph(c, 0), was);
    put(c);
    KT_GLOBAL_EQ(kt_free_pages(), base);
    put(keep);
}

/* ---- the charge ------------------------------------------------------------- */

/* The child's job pays at the call for every page it shares (and its
 * tables): a job that can't is refused up front, charged nothing; one that
 * can is charged exactly that, and then every page written on both sides
 * (vmo_write and faults) charges nothing more, so no separation can fail
 * for the job. A page the child never shared is charged when first touched,
 * as any. */
KTEST(vmo_child_charges)
{
    enum { N = 64 };
    struct vmo *p = mk(N);
    KT_EQ(vmo_commit(p, 0, N * PG), OK);
    struct job *j = kt_fresh_job();
    struct vmo *c;
    /* N pages, a mid table and a leaf table. */
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, N + 2 - 1), OK);
    KT_EQ(vmo_create_child(j, p, SNAP, 0, (N + 1) * PG, &c), ERR_NO_MEMORY);
    kt_job_is_empty(j);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, N + 2), OK);
    KT_EQ(vmo_create_child(j, p, SNAP, 0, (N + 1) * PG, &c), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), N + 2);

    struct aspace *as = new_as();
    map_at(as, c, N + 1, RW);
    for (uint64_t i = 0; i < N; i += 2) {
        uint64_t w = 0x500 + i;
        KT_EQ(vmo_write(c, i * PG, &w, 8), OK);             /* separates */
        KT_EQ(aspace_fault(as, U_A + (i + 1) * PG, ASPACE_WRITE), OK);   /* separates */
        KT_EQ(vmo_write(p, (i + 1) * PG, &w, 8), OK);       /* the parent, likewise */
    }
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), N + 2);
    for (uint64_t i = 0; i < N; i++)
        KT_ASSERT(ph(c, i) != ph(p, i));   /* all separated, nothing more charged */
    /* Page N was never the parent's: a fresh page, charged now, refused. */
    KT_EQ(aspace_fault(as, U_A + N * PG, ASPACE_WRITE), ERR_NO_MEMORY);
    aspace_unref(as);
    put(c);
    kt_job_is_empty(j);

    /* A child of a pager VMO is charged its whole range at the call (every
     * page may become its copy) and its tables; touching it charges
     * nothing. */
    struct pager *pg;
    struct vmo *f;
    KT_EQ(pager_create(NULL, 1u << 20, ANSWER, &pg), OK);
    KT_EQ(pager_create_vmo(pg, 71, 16 * PG, &f), OK);
    uint64_t *buf = kmalloc(16 * PG);
    KT_ASSERT(buf);
    memset(buf, 0x5c, 16 * PG);
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    KT_EQ(pager_supply(pg, 71, 0, &src, 16 * PG), OK);
    kfree(buf);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 16 + 2 - 1), OK);
    KT_EQ(vmo_create_child(j, f, SNAP, 0, 16 * PG, &c), ERR_NOT_SUPPORTED);
    KT_EQ(vmo_create_child(j, f, ALOW, 0, 16 * PG, &c), ERR_NO_MEMORY);
    kt_job_is_empty(j);
    KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, 16 + 2), OK);
    KT_EQ(vmo_create_child(j, f, ALOW, 0, 16 * PG, &c), OK);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 16 + 2);
    KT_EQ(vmo_committed(c), 0);
    as = new_as();
    map_at(as, c, 16, RW);
    for (uint64_t i = 0; i < 16; i++)
        KT_EQ(aspace_fault(as, U_A + i * PG, i & 1 ? ASPACE_WRITE : ASPACE_READ), OK);
    KT_EQ(vmo_committed(c), 16 * PG);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 16 + 2);
    KT_EQ(vmo_decommit(c, 0, 4 * PG), OK);   /* the window stays paid for */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 16 + 2);
    KT_EQ(vmo_set_size(c, 8 * PG), OK);   /* a shrink gives its share back */
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), 8 + 2);
    aspace_unref(as);
    put(c);
    kt_job_is_empty(j);
    job_unref(j);
    put(f);
    put(p);
    struct handle_table t;   /* the pager's handle, closed: its VMO detached */
    handle_table_init(&t);
    struct khandle kh = khandle_from_new(pager_kobject(pg), PAGER_RIGHTS);
    handle_t h;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    handle_table_destroy(&t);
}

/* ---- children of pager VMOs ------------------------------------------------ */

/* The word every 8 bytes of page `page` hold in version gen of key's file. */
static uint64_t gword(uint64_t key, uint64_t page, uint64_t gen)
{
    return key << 32 | page | gen << 48 | 0x5a00000000000000ull;
}

static status_t supply_gen(struct pager *pg, uint64_t key, uint64_t first, uint64_t n,
                           uint64_t gen)
{
    uint64_t *buf = kmalloc(n * PG);
    KT_ASSERT(buf);
    for (uint64_t p = 0; p < n; p++)
        for (uint64_t i = 0; i < PG / 8; i++)
            buf[p * PG / 8 + i] = gword(key, first + p, gen);
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    status_t st = pager_supply(pg, key, first * PG, &src, n * PG);
    kfree(buf);
    return st;
}

/* The next request, waiting for one (a broken kernel fails, not hangs). */
static void next(struct pager *pg, struct pager_request *r)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    for (;;) {
        status_t st = pager_next(pg, 0, r);
        if (st == OK)
            return;
        KT_EQ(st, ERR_SHOULD_WAIT);
        KT_EQ(object_wait_one(pager_kobject(pg), SIG_READABLE, deadline, NULL), OK);
    }
}

static void pg_close(struct pager *pg)
{
    struct handle_table t;
    handle_table_init(&t);
    struct khandle kh = khandle_from_new(pager_kobject(pg), PAGER_RIGHTS);
    handle_t h;
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    handle_table_destroy(&t);
}

/* A child of a pager VMO: an untouched page reads the file as it is (an
 * eviction, an invalidation, a shrink and a growth all show through it);
 * a touched page (a write, a commit, a fault) is the child's own copy that
 * none of them reaches. A fault on an absent page waits for the pager's
 * answer like the pager VMO's own, without the region lock. A child of
 * the child shares its copies and takes its untouched pages from the same
 * pager VMO. A detach fails the untouched pages only. The child is never
 * sealed (so never executable), and holds the file open. */
KTEST(vmo_child_pager)
{
    struct pager *pg;
    struct vmo *f, *x;
    KT_EQ(pager_create(NULL, 4 + 2, ANSWER, &pg), OK);   /* 4 pages (and the VMO's 2 tables) */
    KT_EQ(pager_create_vmo(pg, 61, 8 * PG, &f), OK);
    KT_EQ(vmo_create_child(NULL, f, SNAP, 0, 8 * PG, &x), ERR_NOT_SUPPORTED);
    struct vmo *c = child(f, ALOW, 0, 8 * PG);
    KT_EQ(vmo_committed(c), 0);
    KT_EQ(vmo_seal(c), ERR_BAD_STATE);
    KT_EQ(vmo_create_child(NULL, c, SNAP, 0, PG, &x), ERR_NOT_SUPPORTED);

    KT_EQ(supply_gen(pg, 61, 0, 4, 0), OK);
    KT_EQ(w0(c, 0), gword(61, 0, 0));   /* read through: nothing copied */
    KT_EQ(vmo_committed(c), 0);
    uint64_t w = 0xaaaa;
    KT_EQ(vmo_write(c, PG + 8, &w, 8), OK);   /* page 1: the file's bytes, then ours */
    KT_EQ(w0(c, 1), gword(61, 1, 0));
    KT_EQ(vmo_read(c, PG + 8, &w, 8), OK);
    KT_EQ(w, 0xaaaa);
    KT_EQ(vmo_commit(c, 2 * PG, PG), OK);   /* page 2: a copy */
    KT_EQ(vmo_committed(c), 2 * PG);
    KT_ASSERT(ph(c, 2) && ph(c, 2) != ph(f, 2));

    /* Eviction: pages 0-3 go for 4-7; the copies stay, the rest re-read. */
    KT_EQ(supply_gen(pg, 61, 4, 4, 0), OK);
    KT_EQ(ph(f, 1), 0);
    KT_EQ(w0(c, 1), gword(61, 1, 0));
    KT_EQ(w0(c, 2), gword(61, 2, 0));
    /* Invalidation and a new version: untouched pages show it. */
    KT_EQ(pager_op(pg, 61, PAGER_OP_INVALIDATE, 0, 8 * PG, 0), OK);
    KT_EQ(supply_gen(pg, 61, 0, 4, 1), OK);
    KT_EQ(w0(c, 0), gword(61, 0, 1));
    KT_EQ(w0(c, 3), gword(61, 3, 1));
    KT_EQ(w0(c, 1), gword(61, 1, 0));
    KT_EQ(w0(c, 2), gword(61, 2, 0));
    /* A shrink: an untouched page past the file's end is zero (the child
     * is its own VMO); a growth shows the file again. */
    KT_EQ(pager_op(pg, 61, PAGER_OP_SET_SIZE, 2 * PG, 0, 0), OK);
    KT_EQ(w0(c, 3), 0);
    KT_EQ(w0(c, 2), gword(61, 2, 0));
    KT_EQ(vmo_size(c), 8 * PG);
    KT_EQ(pager_op(pg, 61, PAGER_OP_SET_SIZE, 8 * PG, 0, 0), OK);
    KT_EQ(supply_gen(pg, 61, 3, 1, 2), OK);
    KT_EQ(w0(c, 3), gword(61, 3, 2));

    /* Faults: a read of an absent page waits for the pager VMO's request
     * (the region lock free meanwhile), then holds a copy that an
     * invalidation doesn't reach; a write the same, then its word. The
     * file's resident pages here: 0, 1, 3 (budget 4). */
    struct aspace *as = new_as();
    map_at(as, c, 8, RW);
    struct cpy r;
    struct pager_request q;
    cpy_start(&r, as, U_A + 5 * PG, false, 0);
    next(pg, &q);
    KT_EQ(q.key, 61);
    KT_EQ(q.offset, 5 * PG);
    KT_EQ(aspace_mapping_count(as), 1);   /* the region lock is free while it waits */
    KT_EQ(supply_gen(pg, 61, 5, q.len / PG, 3), OK);   /* resident: 3, 5, 6, 7 */
    KT_EQ(cpy_end(&r), OK);
    KT_EQ(r.word, gword(61, 5, 3));
    KT_ASSERT(ph(c, 5) && ph(c, 5) != ph(f, 5));
    KT_EQ(pager_op(pg, 61, PAGER_OP_INVALIDATE, 5 * PG, 3 * PG, 0), OK);   /* resident: 3 */
    KT_EQ(w0(c, 5), gword(61, 5, 3));
    cpy_start(&r, as, U_A + 6 * PG + 16, true, 0xbbbb);
    next(pg, &q);
    KT_EQ(q.offset, 6 * PG);
    KT_EQ(supply_gen(pg, 61, 6, q.len / PG, 3), OK);   /* resident: 3, 6, 7 */
    KT_EQ(cpy_end(&r), OK);
    KT_EQ(w0(c, 6), gword(61, 6, 3));
    KT_EQ(vmo_read(c, 6 * PG + 16, &w, 8), OK);
    KT_EQ(w, 0xbbbb);
    KT_ASSERT(pte(as, 6) & PTE_W);

    /* A child of the child (c's pages 2-5): c's copies (2, 5) shared, its
     * untouched pages the same pager VMO's. */
    struct vmo *g = child(c, ALOW, 2 * PG, 4 * PG);
    KT_EQ(ph(g, 0), ph(c, 2));
    KT_EQ(ph(g, 3), ph(c, 5));
    KT_EQ(w0(g, 1), gword(61, 3, 2));
    KT_EQ(w0(c, 3), gword(61, 3, 2));
    KT_EQ(supply_gen(pg, 61, 4, 1, 4), OK);   /* resident: 3, 6, 7, 4 */
    KT_EQ(w0(g, 2), gword(61, 4, 4));
    KT_EQ(vmo_committed(g), 2 * PG);

    /* The pager VMO's last reference but the children's goes: they keep
     * the file open (no PAGER_REQ_GONE while they live). */
    put(f);
    KT_EQ(pager_next(pg, 0, &q), ERR_SHOULD_WAIT);

    /* A detach: untouched pages fail, copies don't. */
    KT_EQ(pager_op(pg, 61, PAGER_OP_DETACH, 0, 0, 0), OK);
    KT_EQ(vmo_read(c, 7 * PG, &w, 8), ERR_IO);
    KT_EQ(vmo_read(g, 2 * PG, &w, 8), ERR_IO);
    cpy_start(&r, as, U_A + 7 * PG, false, 0);
    KT_EQ(cpy_end(&r), ERR_IO);
    KT_EQ(w0(c, 1), gword(61, 1, 0));
    KT_EQ(w0(c, 5), gword(61, 5, 3));
    KT_EQ(w0(g, 0), gword(61, 2, 0));
    KT_EQ(w0(g, 3), gword(61, 5, 3));
    aspace_unref(as);
    put(g);
    put(c);
    pg_close(pg);
}

/* ---- pins and kernel mappings ------------------------------------------------ */

/* A pin of either kind and a kernel mapping separate every shared page of
 * their range first, so the device or the kernel reaches the VMO's own
 * page; while they last, no child shares that range. */
KTEST(vmo_child_pins)
{
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    struct vmo *p = mk(6);
    for (uint64_t i = 0; i < 6; i++)
        stamp(p, i, 0x600 + i);
    struct vmo *c = child(p, SNAP, 0, 6 * PG), *x;
    uint64_t phys[4], pin, ppin;
    KT_EQ(vmo_pin(c, cap, 0, 4 * PG, phys, 4, &pin), OK);
    for (uint64_t i = 0; i < 4; i++) {
        KT_EQ(phys[i], ph(c, i));
        KT_ASSERT(phys[i] != ph(p, i));
        KT_EQ(at(phys[i]), 0x600 + i);
    }
    KT_EQ(ph(c, 4), ph(p, 4));   /* outside the pin: still shared */
    uint64_t was = ph(p, 0);   /* the parent's are its own again: no copy */
    KT_EQ(vmo_pin_flags(p, cap, 0, 2 * PG, VMO_PIN_READ_ONLY, phys, 4, &ppin), OK);
    KT_EQ(phys[0], was);
    KT_EQ(vmo_create_child(NULL, c, SNAP, 0, 6 * PG, &x), ERR_BAD_STATE);
    KT_EQ(vmo_create_child(NULL, p, SNAP, PG, 2 * PG, &x), ERR_BAD_STATE);
    x = child(p, SNAP, 4 * PG, 2 * PG);   /* outside the pins */
    KT_EQ(ph(x, 0), ph(p, 4));
    put(x);
    KT_EQ(vmo_unpin(c, cap, pin), OK);
    KT_EQ(vmo_unpin(p, cap, ppin), OK);
    x = child(c, SNAP, 0, 6 * PG);
    KT_EQ(ph(x, 0), ph(c, 0));
    put(x);

    /* A read-only pin of a shared page separates it as well: a later write
     * by its VMO must not move the page from under the device. */
    struct vmo *d = child(p, SNAP, 0, 6 * PG);
    KT_EQ(vmo_pin_flags(d, cap, 0, PG, VMO_PIN_READ_ONLY, phys, 4, &pin), OK);
    KT_ASSERT(phys[0] != ph(p, 0));
    stamp(d, 0, 0x6d0);
    KT_EQ(ph(d, 0), phys[0]);
    KT_EQ(at(phys[0]), 0x6d0);
    KT_EQ(vmo_unpin(d, cap, pin), OK);

    /* A kernel mapping: the same, and its writes are the VMO's alone. */
    void *va;
    KT_EQ(ph(d, 1), ph(p, 1));
    KT_EQ(vmo_map_kernel(d, PG, PG, VM_WRITE, &va), OK);
    KT_ASSERT(ph(d, 1) != ph(p, 1));
    *(volatile uint64_t *)va = 0x6d1;
    KT_EQ(w0(d, 1), 0x6d1);
    KT_ASSERT(page_is(p, 1, 0x601));
    KT_EQ(vmo_create_child(NULL, d, SNAP, 0, 2 * PG, &x), ERR_BAD_STATE);
    KT_EQ(vmo_unmap_kernel(d, va), OK);
    put(d);
    put(c);
    put(p);
    kobject_unref(cap);
}

/* ---- the refusals and the kinds that never share ------------------------------ */

KTEST(vmo_child_refusals)
{
    struct vmo *p = mk(4), *x;
    stamp(p, 0, 0x700);
    KT_EQ(vmo_create_child(NULL, p, 0, 0, PG, &x), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_child(NULL, p, SNAP | ALOW, 0, PG, &x), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_child(NULL, p, SNAP | (1u << 7), 0, PG, &x), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_child(NULL, p, SNAP, PG + 1, PG, &x), ERR_INVALID_ARGS);
    KT_EQ(vmo_create_child(NULL, p, SNAP, VMO_MAX_SIZE + PG, PG, &x), ERR_OUT_OF_RANGE);
    KT_EQ(vmo_create_child(NULL, p, SNAP, 0, VMO_MAX_SIZE + 1, &x), ERR_OUT_OF_RANGE);
    x = child(p, SNAP, VMO_MAX_SIZE, 0);   /* empty, past everything: allowed */
    KT_EQ(vmo_size(x), 0);
    put(x);

    struct vmo *cont, *phys;
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS, &cont), OK);
    KT_EQ(vmo_create_child(NULL, cont, SNAP, 0, PG, &x), ERR_NOT_SUPPORTED);
    put(cont);
    KT_EQ(vmo_create_physical(0x100000, PG, 0, &phys), OK);
    KT_EQ(vmo_create_child(NULL, phys, ALOW, 0, PG, &x), ERR_NOT_SUPPORTED);
    put(phys);

    /* A kept VMO is copied, never shared; its child is an ordinary VMO. */
    struct vmo *k;
    KT_EQ(vmo_create(2 * PG, VMO_KEEP_PAGES, &k), OK);
    stamp(k, 1, 0x711);
    x = child(k, SNAP, 0, 2 * PG);
    KT_ASSERT(ph(x, 0) && ph(x, 0) != ph(k, 0));
    KT_ASSERT(ph(x, 1) != ph(k, 1));
    KT_ASSERT(page_is(x, 1, 0x711) && page_is(x, 0, 0));
    KT_ASSERT(!vmo_is_kept(x));
    KT_EQ(vmo_decommit(x, 0, 2 * PG), OK);
    put(x);
    put(k);

    /* A sealed parent shares, and its child is writable (and not sealed). */
    KT_EQ(vmo_seal(p), OK);
    x = child(p, SNAP, 0, 4 * PG);
    KT_EQ(ph(x, 0), ph(p, 0));
    stamp(x, 0, 0x7a0);
    KT_ASSERT(page_is(p, 0, 0x700));
    KT_EQ(vmo_seal(x), OK);   /* its own seal: an ordinary VMO's */
    put(x);

    /* The handle layer: READ and DUPLICATE on the parent; the child's
     * handle is a new VMO's, never with EXEC. */
    struct handle_table t;
    handle_table_init(&t);
    handle_t h, hc, hn;
    kobject_ref(vmo_kobject(p));
    struct khandle kh = khandle_from_new(vmo_kobject(p), RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP |
                                                         RIGHT_EXEC);
    KT_EQ(handle_insert(&t, &kh, &h), OK);
    KT_EQ(sys_vmo_create_child(&t, h, SNAP, 0, PG, &hc), OK);
    struct kobject *obj;
    rights_t r;
    KT_EQ(handle_get(&t, hc, OBJ_VMO, 0, &obj, &r), OK);
    KT_EQ(r, RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_RESIZE);
    kobject_unref(obj);
    KT_EQ(handle_duplicate(&t, h, RIGHT_READ | RIGHT_MAP, &hn), OK);
    KT_EQ(sys_vmo_create_child(&t, hn, SNAP, 0, PG, &hc), ERR_ACCESS_DENIED);   /* no DUPLICATE */
    KT_EQ(handle_duplicate(&t, h, RIGHT_DUPLICATE | RIGHT_MAP, &hn), OK);
    KT_EQ(sys_vmo_create_child(&t, hn, SNAP, 0, PG, &hc), ERR_ACCESS_DENIED);   /* no READ */
    handle_t he;
    KT_EQ(sys_event_create(&t, &he), OK);
    KT_EQ(sys_vmo_create_child(&t, he, SNAP, 0, PG, &hc), ERR_WRONG_TYPE);
    handle_table_destroy(&t);
    put(p);
}

/* ---- many CPUs -------------------------------------------------------------- */

/* Writers on every CPU, half in an address space mapping the parent, half
 * in one mapping its child, each with a word of its own on every page,
 * many rounds, while the main thread makes and drops children of both:
 * each side ends with exactly its own writers' last words and the other
 * side's starting words, and no write is ever lost. */
enum { MC_PAGES = 64, MC_THREADS = 8, MC_ROUNDS = 40 };

struct mc_writer {
    struct aspace *as;
    unsigned       slot;       /* its word on each page: slot * 8 */
    uint64_t       errors;     /* reads that didn't see its own last write */
    bool           finished;   /* atomic */
    struct thread *t;
};

static uint64_t mc_start(uint64_t page, unsigned slot)
{
    return 0x8800000000000000ull | page << 16 | slot;
}

static uint64_t mc_word(unsigned slot, uint64_t round, uint64_t page)
{
    return (uint64_t)slot << 56 | round << 24 | page;
}

static void mc_main(void *arg)
{
    struct mc_writer *w = arg;
    enter_aspace(w->as, NULL);
    for (uint64_t round = 1; round <= MC_ROUNDS; round++) {
        for (uint64_t pg = 0; pg < MC_PAGES; pg++) {
            uint64_t addr = U_A + pg * PG + w->slot * 8, now = 0;
            uint64_t want = round == 1 ? mc_start(pg, w->slot) : mc_word(w->slot, round - 1, pg);
            if (copy_from_user(&now, addr, 8) != OK || now != want)
                w->errors++;
            uint64_t v = mc_word(w->slot, round, pg);
            if (copy_to_user(addr, &v, 8) != OK)
                w->errors++;
        }
    }
    enter_aspace(NULL, w->as);
    __atomic_store_n(&w->finished, true, __ATOMIC_RELEASE);
}

KTEST(vmo_child_many_writers)
{
    struct vmo *p = mk(MC_PAGES);
    uint64_t *buf = kmalloc(PG);
    KT_ASSERT(buf);
    for (uint64_t pg = 0; pg < MC_PAGES; pg++) {
        for (unsigned s = 0; s < PG / 8; s++)
            buf[s] = mc_start(pg, s);
        KT_EQ(vmo_write(p, pg * PG, buf, PG), OK);
    }
    struct vmo *c = child(p, SNAP, 0, MC_PAGES * PG);
    struct aspace *a = new_as(), *b = new_as();
    map_at(a, p, MC_PAGES, RW);
    map_at(b, c, MC_PAGES, RW);
    uint64_t c0 = copies();
    struct mc_writer *w = kzalloc(MC_THREADS * sizeof(*w));
    KT_ASSERT(w);
    for (unsigned k = 0; k < MC_THREADS; k++) {
        w[k] = (struct mc_writer){ .as = k & 1 ? b : a, .slot = k };
        cpumask_t m;
        cpumask_one(&m, k % cpu_count);
        w[k].t = thread_create_on("cow-writer", mc_main, &w[k], PRIO_DEFAULT, &m);
    }
    /* Meanwhile: children of both sides made, written and dropped. */
    unsigned made = 0;
    for (bool running = true; running;) {
        struct vmo *x = child(made & 1 ? c : p, made & 2 ? ALOW : SNAP, 0, MC_PAGES * PG);
        uint64_t v = made;
        KT_EQ(vmo_write(x, (made % MC_PAGES) * PG + 63 * 8, &v, 8), OK);
        put(x);
        made++;
        running = false;
        for (unsigned k = 0; k < MC_THREADS; k++)
            running |= !__atomic_load_n(&w[k].finished, __ATOMIC_ACQUIRE);
    }
    for (unsigned k = 0; k < MC_THREADS; k++) {
        thread_join(w[k].t);
        KT_EQ(w[k].errors, 0);
    }
    for (uint64_t pg = 0; pg < MC_PAGES; pg++) {
        KT_EQ(vmo_read(p, pg * PG, buf, PG), OK);
        for (unsigned k = 0; k < MC_THREADS; k++)
            KT_EQ(buf[k], k & 1 ? mc_start(pg, k) : mc_word(k, MC_ROUNDS, pg));
        KT_EQ(vmo_read(c, pg * PG, buf, PG), OK);
        for (unsigned k = 0; k < MC_THREADS; k++)
            KT_EQ(buf[k], k & 1 ? mc_word(k, MC_ROUNDS, pg) : mc_start(pg, k));
    }
    kprintf("vmo_child_many_writers: %u children made meanwhile, %lu copies\n", made,
            copies() - c0);
    kfree(buf);
    kfree(w);
    aspace_unref(a);
    aspace_unref(b);
    put(c);
    put(p);
}

/* Writers on every CPU but one write a sequence on every page of the
 * parent while the main thread makes a child again and again: each child
 * holds, for each writer, a value it had written between the start and
 * the end of the call, and keeps it (a later read finds the same); and no
 * writer ever reads back anything but its own last write (a write through
 * an entry the creation hadn't shot down yet, landing in a page the
 * writer's own side had copied already, would be lost: the creation's
 * `protecting` wait is what stops it). At least CR_CHILDREN children back
 * to back, and on until every writer has finished CR_SWEEPS sweeps, one a
 * millisecond: a writer whose CPU is shared (a live machine, a busy host)
 * would otherwise race creation after creation, every page of every sweep
 * copied again, and could take minutes over its sweeps (2026-10-17: over
 * 60 s in a soak on a loaded PC, 1.2 million children in another). */
enum { CR_PAGES = 256, CR_MAX_WRITERS = 7, CR_CHILDREN = 150, CR_SWEEPS = 20 };

struct cr_writer {
    struct aspace *as;
    unsigned       slot;
    bool          *stop;     /* atomic */
    uint64_t       done;     /* atomic: whole sweeps finished */
    uint64_t       errors;
    struct thread *t;
};

static uint64_t cr_word(unsigned slot, uint64_t sweep)
{
    return (uint64_t)(slot + 1) << 48 | sweep;
}

static void cr_main(void *arg)
{
    struct cr_writer *w = arg;
    enter_aspace(w->as, NULL);
    for (uint64_t sweep = 1; !__atomic_load_n(w->stop, __ATOMIC_ACQUIRE); sweep++) {
        for (uint64_t pg = 0; pg < CR_PAGES; pg++) {
            uint64_t addr = U_A + pg * PG + w->slot * 8, now = 0, v = cr_word(w->slot, sweep);
            if (copy_from_user(&now, addr, 8) != OK || now != cr_word(w->slot, sweep - 1))
                w->errors++;
            if (copy_to_user(addr, &v, 8) != OK)
                w->errors++;
        }
        __atomic_store_n(&w->done, sweep, __ATOMIC_RELEASE);
    }
    enter_aspace(NULL, w->as);
}

KTEST(vmo_child_creation_races_writers)
{
    unsigned n = cpu_count > 1 ? cpu_count - 1 : 1;
    if (n > CR_MAX_WRITERS)
        n = CR_MAX_WRITERS;
    struct vmo *p = mk(CR_PAGES);   /* every word 0: sweep 0's (cr_word(k, 0) is not 0) */
    uint64_t *buf = kmalloc(PG);
    KT_ASSERT(buf);
    for (unsigned s = 0; s < PG / 8; s++)
        buf[s] = cr_word(s, 0);
    for (uint64_t pg = 0; pg < CR_PAGES; pg++)
        KT_EQ(vmo_write(p, pg * PG, buf, PG), OK);
    struct aspace *a = new_as();
    map_at(a, p, CR_PAGES, RW);
    struct cr_writer *w = kzalloc(CR_MAX_WRITERS * sizeof(*w));
    KT_ASSERT(w);
    bool stop = false;
    for (unsigned k = 0; k < n; k++) {
        w[k] = (struct cr_writer){ .as = a, .slot = k, .stop = &stop };
        cpumask_t m;
        cpumask_one(&m, (k + 1) % cpu_count);
        w[k].t = thread_create_on("cow-sweeper", cr_main, &w[k], PRIO_DEFAULT, &m);
    }
    uint64_t before[CR_MAX_WRITERS], after[CR_MAX_WRITERS], least = 0;
    uint64_t waits = __atomic_load_n(&vmo_cow_waits, __ATOMIC_RELAXED);
    uint64_t deadline = uptime_ns() + kt_patience_ms(60000) * NS_PER_MS;
    unsigned i = 0;
    for (; i < CR_CHILDREN || least < CR_SWEEPS; i++) {
        KT_ASSERT(uptime_ns() < deadline);
        for (unsigned k = 0; k < n; k++)
            before[k] = __atomic_load_n(&w[k].done, __ATOMIC_ACQUIRE);
        struct vmo *c = child(p, i & 1 ? ALOW : SNAP, 0, CR_PAGES * PG);
        for (unsigned k = 0; k < n; k++)
            after[k] = __atomic_load_n(&w[k].done, __ATOMIC_ACQUIRE) + 1;   /* + the sweep under way */
        /* A few pages, each read twice with the writers going on between:
         * a value written in the call, then the same one. */
        for (uint64_t pg = i % 7; pg < CR_PAGES; pg += 97) {
            uint64_t first[CR_MAX_WRITERS];
            KT_EQ(vmo_read(c, pg * PG, first, n * 8), OK);
            for (unsigned k = 0; k < n; k++) {
                KT_EQ(first[k] >> 48, k + 1);
                KT_ASSERT((first[k] & 0xffffffffffffull) >= before[k]);
                KT_ASSERT((first[k] & 0xffffffffffffull) <= after[k]);
            }
            thread_yield();
            KT_EQ(vmo_read(c, pg * PG, buf, n * 8), OK);
            for (unsigned k = 0; k < n; k++)
                KT_EQ(buf[k], first[k]);
        }
        put(c);
        least = UINT64_MAX;
        for (unsigned k = 0; k < n; k++) {
            uint64_t d = __atomic_load_n(&w[k].done, __ATOMIC_ACQUIRE);
            least = d < least ? d : least;
        }
        if (i + 1 >= CR_CHILDREN && least < CR_SWEEPS)
            thread_sleep_ms(1);
    }
    __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
    for (unsigned k = 0; k < n; k++) {
        thread_join(w[k].t);
        KT_EQ(w[k].errors, 0);
    }
    for (uint64_t pg = 0; pg < CR_PAGES; pg += 31) {
        KT_EQ(vmo_read(p, pg * PG, buf, n * 8), OK);
        for (unsigned k = 0; k < n; k++)
            KT_EQ(buf[k], cr_word(k, w[k].done));
    }
    kprintf("vmo_child_creation_races_writers: %u writers, %u children, %lu sweeps each at "
            "least, %lu separations waited for a creation\n", n, i, least,
            __atomic_load_n(&vmo_cow_waits, __ATOMIC_RELAXED) - waits);
    kfree(buf);
    kfree(w);
    aspace_unref(a);
    put(p);
}

/* The window itself, held open: a writer keeps writing a page of the
 * parent through the writable entry its CPU still caches, while the
 * creation (in DBG_VMO_CHILD_SHOOT) has made the entry read-only but not
 * yet shot it down, and another thread asks to separate that page
 * (vmo_write). The separation must wait for the shootdown: a copy taken
 * now would miss the writes still landing in the old page, and the
 * writer, reading its word back after its own fault, would find an older
 * one. */
struct sw {
    struct aspace *as;
    struct vmo    *p;
    bool           stop;      /* atomic: the writer ends */
    bool           open;      /* atomic: the hook is holding the window */
    bool           asking;    /* atomic: the separating thread is about to write */
    uint64_t       last;      /* the writer's last word */
    uint64_t       errors;    /* reads that didn't find the writer's last word */
};

static struct sw *sw_now;   /* the hook's test (set and cleared by the test) */

static void sw_hook(void *arg)
{
    struct sw *s = __atomic_load_n(&sw_now, __ATOMIC_ACQUIRE);
    if (!s || arg != s->p)
        return;
    __atomic_store_n(&s->open, true, __ATOMIC_RELEASE);
    uint64_t deadline = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (!__atomic_load_n(&s->asking, __ATOMIC_ACQUIRE) && uptime_ns() < deadline)
        cpu_relax();
    thread_sleep_ms(3);   /* the separation (and the writer) run meanwhile */
}

static void sw_writer(void *arg)
{
    struct sw *s = arg;
    enter_aspace(s->as, NULL);
    for (uint64_t v = 1; !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE); v++) {
        uint64_t now = ~0ull;
        if (copy_from_user(&now, U_A, 8) != OK || now != v - 1 || copy_to_user(U_A, &v, 8) != OK)
            s->errors++;
        s->last = v;
    }
    enter_aspace(NULL, s->as);
}

static void sw_separator(void *arg)
{
    struct sw *s = arg;
    uint64_t deadline = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (!__atomic_load_n(&s->open, __ATOMIC_ACQUIRE) && uptime_ns() < deadline)
        cpu_relax();
    __atomic_store_n(&s->asking, true, __ATOMIC_RELEASE);
    uint64_t w = 0x5e9;
    KT_EQ(vmo_write(s->p, 8, &w, 8), OK);
}

KTEST(vmo_child_separation_waits_for_shootdown)
{
    if (cpu_count < 3) {
        kprintf("vmo_child_separation_waits_for_shootdown: needs 3 CPUs, skipped\n");
        return;
    }
    struct sw *s = kzalloc(sizeof(*s));
    KT_ASSERT(s);
    s->as = new_as();
    s->p = mk(1);
    map_at(s->as, s->p, 1, RW);
    cpumask_t m1, m2;
    cpumask_one(&m1, 1);
    cpumask_one(&m2, 2);
    struct thread *wt = thread_create_on("cow-stale", sw_writer, s, PRIO_DEFAULT, &m1);
    thread_sleep_ms(2);   /* writing through its cached entry by now */
    uint64_t waits = __atomic_load_n(&vmo_cow_waits, __ATOMIC_RELAXED);
    __atomic_store_n(&sw_now, s, __ATOMIC_RELEASE);
    __atomic_store_n(&dbg_hooks[DBG_VMO_CHILD_SHOOT], sw_hook, __ATOMIC_RELEASE);
    struct thread *st = thread_create_on("cow-separate", sw_separator, s, PRIO_DEFAULT, &m2);
    struct vmo *c = child(s->p, SNAP, 0, PG);
    __atomic_store_n(&dbg_hooks[DBG_VMO_CHILD_SHOOT], NULL, __ATOMIC_RELEASE);
    thread_join(st);
    thread_sleep_ms(2);
    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
    thread_join(wt);
    __atomic_store_n(&sw_now, NULL, __ATOMIC_RELEASE);
    KT_EQ(s->errors, 0);
    KT_EQ(w0(s->p, 0), s->last);   /* the parent has the writer's last word */
    uint64_t w = 0;
    KT_EQ(vmo_read(s->p, 8, &w, 8), OK);
    KT_EQ(w, 0x5e9);
    KT_ASSERT(w0(c, 0) <= s->last);
    KT_IDLE_ASSERT(__atomic_load_n(&vmo_cow_waits, __ATOMIC_RELAXED) > waits);
    put(c);
    aspace_unref(s->as);
    put(s->p);
    kfree(s);
}
