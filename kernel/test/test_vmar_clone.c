/* vmar_clone, the address space copied for fork (docs/FORK-PLAN.md section
 * 1.1, track FK2; mm/aspace.c's "Clones", vmo.c's "Clones"):
 *
 *   vmar_clone_modes_and_bytes
 *       every kind of mapping cloned and looked at from both sides: a copy
 *       holds the parent's bytes and writes separate both ways, two copied
 *       mappings of one VMO map one child holding only the pages they map,
 *       share stays shared, none is absent; permissions, maximum and mode
 *       kept; the child's first faults draw on the table reserve.
 *   vmar_clone_charges_exact
 *       the charge is the children's pages and tables, the mapping structs
 *       and the table reserve, to the page; a job one page short at any
 *       point gets ERR_NO_MEMORY with nothing left: no charge, no mapping,
 *       no page shared; the child's faults charge nothing until the reserve
 *       is used, and its death gives everything back.
 *   vmar_clone_sparse_cage
 *       a 1 TiB VMO with a page per GiB (touched by the parent) cloned in a
 *       few milliseconds, holding those 1,024 pages; 60,000 mappings of
 *       one VMO cloned whole.
 *   vmar_clone_writers_not_torn
 *       writers on every CPU write a counter to a page of VMO A, then to a
 *       page of VMO B, while clone after clone is made: no clone ever holds
 *       a B ahead of its A, and no writer ever loses a write.
 *   vmar_clone_storm
 *       maps, unmaps, protects, vmo_writes and decommits from other threads
 *       while clones are made: each clone is whole (aspace_check), each
 *       call waits its turn.
 *   vmar_clone_jit_never_reaches
 *       a JIT VMO's writable mapping is absent in the clone; its read and
 *       execute view is shared, can't be made writable there, and marks
 *       the clone as mapping JIT code.
 *   vmar_clone_kept
 *       a kept mapping is shared with every entry filled at once; a
 *       writable mapping of a kept VMO is copied, page by page.
 *   vmar_clone_refusals
 *       a destination that isn't empty, the source itself, a VMO that
 *       isn't mapped as a copy, a kernel mapping over a page to copy: each
 *       refused with nothing made, and the VMOs writable again at once.
 *   vmar_clone_sys
 *       through the handle layer: the destination is a NEW process, held
 *       so (process_clone_begin), its table's generations random after;
 *       the children's handles have a fresh VMO's rights; the rights and
 *       types asked for; every refusal gives the reserved slots back.
 *   vmar_clone_write_in_flight
 *       a vmo_write stopped between its page and its copy: a clone, or a
 *       child's creation, meanwhile waits for the copy, so the child's
 *       bytes never change after it returned (docs/FORK-REVIEW-KERNEL.md,
 *       finding 1).
 *
 * Faults go through the real page-fault path: the test thread enters an
 * address space and copies to and from its user addresses, as a system
 * call does. The names start "vmar_clone", so `ktest=vmar_clone` runs
 * them. */
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>

#define PG     PAGE_SIZE
#define R      ASPACE_READ
#define RW     (ASPACE_READ | ASPACE_WRITE)
#define RX     (ASPACE_READ | ASPACE_EXEC)
#define RWX    (ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC)
#define CAN_R  ASPACE_CAN_READ
#define CAN_RW (ASPACE_CAN_READ | ASPACE_CAN_WRITE)
#define CAN_RX (ASPACE_CAN_READ | ASPACE_CAN_EXEC)
#define CAN_ALL (ASPACE_CAN_READ | ASPACE_CAN_WRITE | ASPACE_CAN_EXEC)
#define SHARE  ASPACE_CLONE_SHARE
#define NONE   ASPACE_CLONE_NONE
#define BASE   0x40000000ull   /* 1 GiB-aligned: a few pages here need one table of each level */
#define GIB    (1ull << 30)

/* ---- helpers --------------------------------------------------------------- */

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

static struct aspace *new_as(struct job *j)
{
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    return as;
}

static void stamp(struct vmo *v, uint64_t page, uint64_t w)
{
    KT_EQ(vmo_write(v, page * PG, &w, 8), OK);
}

static uint64_t word(struct vmo *v, uint64_t page)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(v, page * PG, &w, 8), OK);
    return w;
}

/* Map pages [off, off + n) of v at `at`, fixed. */
static void map_at(struct aspace *as, struct vmo *v, uint64_t off, uint64_t n, uint64_t at,
                   unsigned flags)
{
    uint64_t a = at;
    KT_EQ(aspace_map(as, v, off * PG, n * PG, flags | ASPACE_FIXED, &a), OK);
}

/* Move the current thread into address space `as` (NULL: out of `was`),
 * as a system call's copies run. */
static void enter(struct aspace *as, struct aspace *was)
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

static status_t uget(struct aspace *as, uint64_t va, uint64_t *w)
{
    enter(as, NULL);
    status_t st = copy_from_user(w, va, 8);
    enter(NULL, as);
    return st;
}

static uint64_t uword(struct aspace *as, uint64_t va)
{
    uint64_t w = ~0ull;
    KT_EQ(uget(as, va, &w), OK);
    return w;
}

static status_t uput(struct aspace *as, uint64_t va, uint64_t w)
{
    enter(as, NULL);
    status_t st = copy_to_user(va, &w, 8);
    enter(NULL, as);
    return st;
}

/* Clone src into dst asking for v's child (NULL: none); the child, a
 * reference of the caller's, in *child. */
static status_t clone1(struct aspace *src, struct aspace *dst, struct vmo *v, struct vmo **child)
{
    struct aspace_clone_req r = { .v = v };
    status_t st = aspace_clone(src, dst, &r, v ? 1 : 0);
    if (child)
        *child = r.child;
    else if (r.child)
        put(r.child);
    return st;
}

/* The mapping over va: its map_info. */
static struct map_info info_at(struct aspace *as, uint64_t va)
{
    struct map_info mi = { 0 };
    KT_EQ(aspace_get_maps(as, va, &mi, 1), 1);
    KT_ASSERT(mi.base <= va && va < mi.base + mi.len);
    return mi;
}

/* ---- every kind of mapping --------------------------------------------------- */

KTEST(vmar_clone_modes_and_bytes)
{
    struct vmo *a = mk(8), *s = mk(2), *n = mk(2), *o = mk(1);
    for (uint64_t i = 0; i < 8; i++)
        stamp(a, i, 0xa00 + i);
    stamp(s, 0, 0x5);
    stamp(o, 0, 0x0);
    struct aspace *src = new_as(NULL), *dst = new_as(NULL);
    map_at(src, a, 0, 4, BASE, RW | CAN_RW);              /* copy */
    map_at(src, a, 6, 2, BASE + 5 * PG, R | CAN_RW);      /* copy (the maximum writes): the same child */
    map_at(src, s, 0, 2, BASE + 8 * PG, RW | CAN_RW | SHARE);
    map_at(src, n, 0, 2, BASE + 10 * PG, RW | CAN_RW | NONE);
    map_at(src, o, 0, 1, BASE + 12 * PG, R | CAN_R);      /* shared: nothing can write it */
    KT_EQ(uput(src, BASE + 10 * PG, 0x77), OK);           /* tables there too */
    uint64_t tables = aspace_pt_pages(src);
    KT_EQ(tables, 3);

    struct vmo *c;
    KT_EQ(clone1(src, dst, a, &c), OK);
    KT_ASSERT(c && c != a);
    KT_EQ(aspace_check(src), 5);
    KT_EQ(aspace_check(dst), 4);
    KT_EQ(vmo_size(c), vmo_size(a));
    KT_EQ(vmo_committed(c), 6 * PG);   /* pages 4 and 5 are mapped nowhere: not copied */
    KT_EQ(aspace_pt_reserve(dst), tables);
    KT_EQ(aspace_pt_pages(dst), 0);

    /* The same mappings, but none's, mapping the child or the same VMO. */
    static const uint64_t at[4] = { BASE, BASE + 5 * PG, BASE + 8 * PG, BASE + 12 * PG };
    for (unsigned i = 0; i < 4; i++) {
        struct map_info x = info_at(src, at[i]), y = info_at(dst, at[i]);
        KT_EQ(x.base, y.base);
        KT_EQ(x.len, y.len);
        KT_EQ(x.vmo_off, y.vmo_off);
        KT_EQ(x.perms, y.perms);
        KT_EQ(x.flags, y.flags);
        KT_EQ(y.vmo_koid, i < 2 ? vmo_kobject(c)->koid : x.vmo_koid);
    }
    uint64_t w;
    KT_EQ(uget(dst, BASE + 10 * PG, &w), ERR_INVALID_ARGS);   /* none: nothing there */
    KT_EQ(uget(dst, BASE + 4 * PG, &w), ERR_INVALID_ARGS);    /* never mapped */

    /* The bytes, and the first faults' tables come from the reserve. */
    for (uint64_t i = 0; i < 4; i++)
        KT_EQ(uword(dst, BASE + i * PG), 0xa00 + i);
    KT_EQ(aspace_pt_pages(dst), tables);
    KT_EQ(aspace_pt_reserve(dst), 0);
    KT_EQ(uword(dst, BASE + 5 * PG), 0xa06);
    KT_EQ(uword(dst, BASE + 6 * PG), 0xa07);
    KT_EQ(uword(dst, BASE + 12 * PG), 0);

    /* Writes separate both ways. */
    KT_EQ(uput(src, BASE, 1), OK);
    KT_EQ(uput(dst, BASE + PG, 2), OK);
    KT_EQ(uword(src, BASE), 1);
    KT_EQ(uword(dst, BASE), 0xa00);
    KT_EQ(uword(dst, BASE + PG), 2);
    KT_EQ(uword(src, BASE + PG), 0xa01);
    KT_EQ(word(a, 0), 1);
    KT_EQ(word(c, 1), 2);
    /* The read-only copy keeps its maximum: it may be made writable, and
     * then writes only the child. */
    KT_EQ(uput(dst, BASE + 5 * PG, 3), ERR_INVALID_ARGS);
    KT_EQ(aspace_protect(dst, BASE + 5 * PG, 2 * PG, RW), OK);
    KT_EQ(uput(dst, BASE + 5 * PG, 3), OK);
    KT_EQ(word(a, 6), 0xa06);
    KT_EQ(word(c, 6), 3);
    /* Shared both ways. */
    KT_EQ(uput(dst, BASE + 8 * PG, 4), OK);
    KT_EQ(uword(src, BASE + 8 * PG), 4);
    KT_EQ(uput(src, BASE + 9 * PG, 5), OK);
    KT_EQ(uword(dst, BASE + 9 * PG), 5);
    KT_EQ(aspace_protect(dst, BASE + 12 * PG, PG, RW), ERR_ACCESS_DENIED);

    KT_EQ(aspace_check(src), 5);
    KT_EQ(aspace_check(dst), 4);
    aspace_unref(dst);
    KT_EQ(word(c, 1), 2);   /* our reference keeps the child */
    put(c);
    aspace_unref(src);
    put(a);
    put(s);
    put(n);
    put(o);
}

/* ---- charges -------------------------------------------------------------------- */

KTEST(vmar_clone_charges_exact)
{
    struct job *j = kt_fresh_job();
    struct vmo *a = mk(8), *s = mk(1);
    for (uint64_t i = 0; i < 5; i++)
        stamp(a, i, 0xc0 + i);
    struct aspace *src = new_as(NULL);
    map_at(src, a, 0, 8, BASE, RW | CAN_RW);
    map_at(src, s, 0, 1, BASE + 8 * PG, RW | CAN_RW | SHARE);
    KT_EQ(uword(src, BASE), 0xc0);   /* the source's three tables */
    KT_EQ(aspace_pt_pages(src), 3);
    uint64_t pa0 = vmo_page_phys(a, 0);

    /* The child: 5 pages and its 2 tables (a mid and a leaf), one
     * handle unit for its struct; the structs of 2 mappings, 1 page; the
     * reserve, 3. */
    const uint64_t want = 5 + 2 + 1 + 3;
    for (uint64_t room = 0; room <= want; room++) {
        struct aspace *dst = new_as(j);
        uint64_t pages = job_used(j, JOB_LIMIT_PAGES), handles = job_used(j, JOB_LIMIT_HANDLES);
        KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, pages + room), OK);
        struct vmo *c = NULL;
        status_t st = clone1(src, dst, a, &c);
        KT_EQ(job_set_limit(j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
        if (room < want) {
            KT_EQ(st, ERR_NO_MEMORY);
            KT_ASSERT(!c);
            KT_EQ(job_used(j, JOB_LIMIT_PAGES), pages);
            KT_EQ(job_used(j, JOB_LIMIT_HANDLES), handles);
            KT_EQ(aspace_check(dst), 0);
            KT_EQ(aspace_pt_reserve(dst), 0);
            /* Nothing shared: a write lands in place, and the VMO isn't
             * frozen any more (no wait). */
            KT_EQ(uput(src, BASE, 0xc0), OK);
            KT_EQ(vmo_page_phys(a, 0), pa0);
            stamp(a, 1, 0xc1);
            aspace_unref(dst);
            continue;
        }
        KT_EQ(st, OK);
        KT_EQ(job_used(j, JOB_LIMIT_PAGES), pages + want);
        KT_EQ(job_used(j, JOB_LIMIT_HANDLES), handles + 1);
        KT_EQ(vmo_committed(c), 5 * PG);
        /* The child's faults draw on the reserve: no charge. */
        KT_EQ(uword(dst, BASE + 4 * PG), 0xc4);
        KT_EQ(uword(dst, BASE + 7 * PG), 0);   /* a zero page of the child's own: charged */
        KT_EQ(job_used(j, JOB_LIMIT_PAGES), pages + want + 1);
        KT_EQ(aspace_pt_reserve(dst), 0);
        KT_EQ(aspace_check(dst), 2);
        put(c);
        aspace_unref(dst);   /* its mappings held the child: everything back */
        KT_EQ(job_used(j, JOB_LIMIT_PAGES), pages - 1);   /* (the PML4 too) */
        KT_EQ(job_used(j, JOB_LIMIT_HANDLES), handles);
    }
    KT_EQ(aspace_check(src), 2);
    aspace_unref(src);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(s);
}

/* ---- big ones -------------------------------------------------------------------- */

KTEST(vmar_clone_sparse_cage)
{
    struct vmo *v;
    KT_EQ(vmo_create(1024 * GIB, 0, &v), OK);
    struct aspace *src = new_as(NULL), *dst = new_as(NULL);
    uint64_t base = 0;
    KT_EQ(aspace_map(src, v, 0, 1024 * GIB, RW | CAN_RW, &base), OK);
    enter(src, NULL);
    for (uint64_t g = 0; g < 1024; g++) {
        uint64_t w = 0xca9e0000 + g;
        KT_EQ(copy_to_user(base + g * GIB + g * PG % GIB, &w, 8), OK);
    }
    enter(NULL, src);
    uint64_t tables = aspace_pt_pages(src);
    uint64_t t0 = uptime_ns();
    struct vmo *c;
    KT_EQ(clone1(src, dst, v, &c), OK);
    uint64_t ns = uptime_ns() - t0;
    kprintf("vmar_clone_sparse_cage: 1 TiB, 1024 pages, %lu tables: %lu us\n", tables, ns / 1000);
    KT_IDLE_ASSERT(ns < 20 * NS_PER_MS);
    KT_EQ(vmo_committed(c), 1024 * PG);
    KT_EQ(aspace_pt_reserve(dst), tables);
    for (uint64_t g = 0; g < 1024; g += 101)
        KT_EQ(uword(dst, base + g * GIB + g * PG % GIB), 0xca9e0000 + g);
    KT_EQ(aspace_check(src), 1);
    KT_EQ(aspace_check(dst), 1);
    put(c);
    aspace_unref(dst);
    aspace_unref(src);
    put(v);

    /* 60,000 one-page mappings of one VMO, every other one read-only so
     * none merge: one child, every mapping made again. */
    enum { MANY = 60000 };
    KT_EQ(vmo_create(MANY * PG, 0, &v), OK);
    src = new_as(NULL);
    dst = new_as(NULL);
    for (uint64_t i = 0; i < MANY; i++)
        map_at(src, v, i, 1, BASE + i * PG, (i & 1 ? R : RW) | CAN_RW);
    stamp(v, MANY - 1, 0x600d);
    t0 = uptime_ns();
    KT_EQ(clone1(src, dst, v, &c), OK);
    ns = uptime_ns() - t0;
    kprintf("vmar_clone_sparse_cage: %u mappings: %lu us\n", MANY, ns / 1000);
    KT_EQ(aspace_mapping_count(dst), MANY);
    KT_EQ(aspace_check(dst), MANY);
    KT_EQ(aspace_check(src), MANY);
    KT_EQ(vmo_committed(c), PG);
    KT_EQ(uword(dst, BASE + (MANY - 1) * PG), 0x600d);
    put(c);
    aspace_unref(dst);
    aspace_unref(src);
    put(v);
}

/* ---- one instant ------------------------------------------------------------------ */

enum { NT_WRITERS = 8, NT_CLONES = 300 };

struct nt_writer {
    struct aspace *as;
    unsigned       slot;      /* its page in each VMO */
    bool          *stop;      /* atomic */
    uint64_t       errors;    /* reads that didn't find its own last write */
    uint64_t       last;
    struct thread *t;
};

#define NT_A BASE
#define NT_B (BASE + 64 * PG)

static void nt_main(void *arg)
{
    struct nt_writer *w = arg;
    enter(w->as, NULL);
    for (uint64_t v = 1; !__atomic_load_n(w->stop, __ATOMIC_ACQUIRE); v++) {
        uint64_t a = NT_A + w->slot * PG, b = NT_B + w->slot * PG, now = ~0ull;
        if (copy_from_user(&now, a, 8) != OK || now != v - 1)
            w->errors++;
        if (copy_to_user(a, &v, 8) != OK || copy_to_user(b, &v, 8) != OK)
            w->errors++;
        w->last = v;
    }
    enter(NULL, w->as);
}

KTEST(vmar_clone_writers_not_torn)
{
    struct vmo *a = mk(NT_WRITERS), *b = mk(NT_WRITERS);
    struct aspace *src = new_as(NULL);
    map_at(src, a, 0, NT_WRITERS, NT_A, RW | CAN_RW);
    map_at(src, b, 0, NT_WRITERS, NT_B, RW | CAN_RW);
    bool stop = false;
    struct nt_writer *w = kzalloc(NT_WRITERS * sizeof(*w));
    KT_ASSERT(w);
    for (unsigned k = 0; k < NT_WRITERS; k++) {
        w[k] = (struct nt_writer){ .as = src, .slot = k, .stop = &stop };
        cpumask_t m;
        cpumask_one(&m, k % cpu_count);
        w[k].t = thread_create_on("clone-writer", nt_main, &w[k], PRIO_DEFAULT, &m);
    }
    thread_sleep_ms(2);
    uint64_t ahead = 0;
    for (unsigned i = 0; i < NT_CLONES; i++) {
        struct aspace *dst = new_as(NULL);
        struct aspace_clone_req r[2] = { { .v = a }, { .v = b } };
        KT_EQ(aspace_clone(src, dst, r, 2), OK);
        struct vmo *ca = r[0].v == a ? r[0].child : r[1].child;
        struct vmo *cb = r[0].v == a ? r[1].child : r[0].child;
        for (unsigned k = 0; k < NT_WRITERS; k++) {
            uint64_t x = word(ca, k), y = word(cb, k);
            if (x != y && x != y + 1)
                ktest_fail("clone %u, writer %u: A %lu, B %lu: torn", i, k, x, y);
            ahead += x != y;
        }
        KT_EQ(aspace_check(dst), 2);
        put(ca);
        put(cb);
        aspace_unref(dst);
    }
    __atomic_store_n(&stop, true, __ATOMIC_RELEASE);
    for (unsigned k = 0; k < NT_WRITERS; k++) {
        thread_join(w[k].t);
        KT_EQ(w[k].errors, 0);
        KT_EQ(word(a, k), w[k].last);
        KT_EQ(word(b, k), w[k].last);
    }
    kprintf("vmar_clone_writers_not_torn: %u clones, %lu caught between A and B\n", NT_CLONES,
            ahead);
    KT_EQ(aspace_check(src), 2);
    kfree(w);
    aspace_unref(src);
    put(a);
    put(b);
}

/* Other threads change the source and its VMOs while clones are made. */
struct st_ctx {
    struct aspace *src;
    struct vmo    *a, *x;
    bool           stop;   /* atomic */
    uint64_t       rounds[3];
};

#define ST_X (BASE + 64 * PG)

static void st_mapper(void *arg)
{
    struct st_ctx *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        map_at(s->src, s->x, 0, 4, ST_X, RW | CAN_RW);
        KT_EQ(aspace_unmap(s->src, ST_X + PG, 2 * PG), OK);   /* split, then gone */
        KT_EQ(aspace_unmap(s->src, ST_X, 4 * PG), OK);
        s->rounds[0]++;
    }
}

static void st_protector(void *arg)
{
    struct st_ctx *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        KT_EQ(aspace_protect(s->src, BASE + 8 * PG, 8 * PG, R), OK);
        KT_EQ(aspace_protect(s->src, BASE + 12 * PG, 2 * PG, RW), OK);
        KT_EQ(aspace_protect(s->src, BASE + 8 * PG, 8 * PG, RW), OK);
        s->rounds[1]++;
    }
}

static void st_writer(void *arg)
{
    struct st_ctx *s = arg;
    for (uint64_t v = 1; !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE); v++) {
        KT_EQ(vmo_write(s->a, (v % 4) * PG, &v, 8), OK);
        KT_EQ(vmo_decommit(s->a, ((v + 2) % 4) * PG, PG), OK);
        s->rounds[2]++;
    }
}

KTEST(vmar_clone_storm)
{
    struct st_ctx *s = kzalloc(sizeof(*s));
    KT_ASSERT(s);
    s->a = mk(16);
    s->x = mk(4);
    s->src = new_as(NULL);
    map_at(s->src, s->a, 0, 16, BASE, RW | CAN_RW);
    struct thread *t[3] = {
        thread_create("clone-mapper", st_mapper, s, PRIO_DEFAULT),
        thread_create("clone-protect", st_protector, s, PRIO_DEFAULT),
        thread_create("clone-vmo", st_writer, s, PRIO_DEFAULT),
    };
    for (unsigned i = 0; i < 200; i++) {
        struct aspace *dst = new_as(NULL);
        KT_EQ(clone1(s->src, dst, s->a, NULL), OK);
        /* Whole: one of the states the others pass through, not between. */
        int64_t n = aspace_check(dst);
        KT_ASSERT(n >= 1 && n <= 6);
        aspace_unref(dst);
    }
    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 3; i++)
        thread_join(t[i]);
    kprintf("vmar_clone_storm: rounds meanwhile %lu map, %lu protect, %lu vmo\n", s->rounds[0],
            s->rounds[1], s->rounds[2]);
    KT_EQ(aspace_check(s->src), 1);
    aspace_unref(s->src);
    put(s->a);
    put(s->x);
    kfree(s);
}

/* ---- JIT and kept memory ------------------------------------------------------------ */

KTEST(vmar_clone_jit_never_reaches)
{
    struct vmo *jv = mk(2), *a = mk(1);
    KT_EQ(vmo_make_jit(jv), OK);
    struct aspace *src = new_as(NULL), *dst = new_as(NULL);
    map_at(src, jv, 0, 2, BASE, RWX | CAN_ALL);                /* writable: none, always */
    map_at(src, jv, 0, 2, BASE + 4 * PG, RX | CAN_RX);         /* read and execute: shared */
    map_at(src, jv, 0, 2, BASE + 8 * PG, RWX | CAN_ALL | SHARE);   /* asked share: still none */
    map_at(src, a, 0, 1, BASE + 12 * PG, RW | CAN_RW);
    KT_EQ(uput(src, BASE, 0x717), OK);
    struct aspace_clone_req r = { .v = jv };
    KT_EQ(aspace_clone(src, dst, &r, 1), ERR_INVALID_ARGS);   /* never a copy */
    KT_ASSERT(!r.child);
    KT_EQ(aspace_mapping_count(dst), 0);
    KT_EQ(clone1(src, dst, NULL, NULL), OK);
    KT_EQ(aspace_check(dst), 2);
    uint64_t w;
    KT_EQ(uget(dst, BASE, &w), ERR_INVALID_ARGS);
    KT_EQ(uget(dst, BASE + 8 * PG, &w), ERR_INVALID_ARGS);
    struct map_info mi = info_at(dst, BASE + 4 * PG);
    KT_EQ(mi.vmo_koid, vmo_kobject(jv)->koid);
    KT_EQ(mi.flags, SHARE);
    KT_EQ(uword(dst, BASE + 4 * PG), 0x717);   /* the parent's code, as it writes it */
    KT_EQ(uput(src, BASE + 8, 0x718), OK);
    KT_EQ(uword(dst, BASE + 4 * PG + 8), 0x718);
    KT_EQ(aspace_protect(dst, BASE + 4 * PG, 2 * PG, RWX), ERR_ACCESS_DENIED);
    KT_EQ(aspace_protect(dst, BASE + 4 * PG, 2 * PG, RW), ERR_ACCESS_DENIED);
    KT_ASSERT(aspace_maps_jit(dst));
    aspace_unref(dst);
    aspace_unref(src);
    put(jv);
    put(a);
}

KTEST(vmar_clone_kept)
{
    struct vmo *k;
    KT_EQ(vmo_create(4 * PG, VMO_KEEP_PAGES, &k), OK);
    for (uint64_t i = 0; i < 4; i++)
        stamp(k, i, 0x4e0 + i);
    struct aspace *src = new_as(NULL), *dst = new_as(NULL);
    map_at(src, k, 0, 4, BASE, R | ASPACE_KEPT_ONLY);
    map_at(src, k, 0, 2, BASE + 8 * PG, RW | CAN_RW);   /* a writable view: copied */
    KT_EQ(uput(src, BASE + 8 * PG + 8, 0x4e9), OK);
    struct vmo *c;
    KT_EQ(clone1(src, dst, k, &c), OK);
    KT_EQ(aspace_check(dst), 2);
    /* The kept mapping's entries are all there before any fault. */
    for (uint64_t i = 0; i < 4; i++)
        KT_ASSERT(aspace_pte(dst, BASE + i * PG) & 1);
    uint64_t faults = aspace_fault_count(dst);
    KT_EQ(uword(dst, BASE + 3 * PG), 0x4e3);
    KT_EQ(aspace_fault_count(dst), faults);
    KT_EQ(info_at(dst, BASE).vmo_koid, vmo_kobject(k)->koid);
    /* The copy: its own pages from the start (a kept VMO is never shared). */
    KT_EQ(vmo_committed(c), 2 * PG);
    KT_ASSERT(vmo_page_phys(c, 0) != vmo_page_phys(k, 0));
    KT_EQ(uword(dst, BASE + 8 * PG + 8), 0x4e9);
    KT_EQ(uput(dst, BASE + 8 * PG, 0x4ea), OK);
    KT_EQ(word(k, 0), 0x4e0);
    KT_EQ(uword(dst, BASE + 8 * PG), 0x4ea);
    put(c);
    aspace_unref(dst);
    aspace_unref(src);
    put(k);
}

/* ---- refusals ---------------------------------------------------------------------- */

KTEST(vmar_clone_refusals)
{
    struct job *j = kt_fresh_job();
    struct vmo *a = mk(8), *s = mk(1), *u = mk(1);
    struct aspace *src = new_as(NULL), *dst = new_as(j);
    map_at(src, a, 0, 4, BASE, RW | CAN_RW);
    map_at(src, s, 0, 1, BASE + 8 * PG, RW | CAN_RW | SHARE);
    KT_EQ(uput(src, BASE, 0xab), OK);
    uint64_t pages = job_used(j, JOB_LIMIT_PAGES), handles = job_used(j, JOB_LIMIT_HANDLES);

    KT_EQ(clone1(src, src, NULL, NULL), ERR_BAD_STATE);
    KT_EQ(clone1(src, dst, s, NULL), ERR_INVALID_ARGS);   /* shared, not copied */
    KT_EQ(clone1(src, dst, u, NULL), ERR_INVALID_ARGS);   /* not mapped at all */
    struct aspace_clone_req r[3] = { { .v = a }, { .v = u }, { .v = a } };
    KT_EQ(aspace_clone(src, dst, r, 3), ERR_INVALID_ARGS);
    for (unsigned i = 0; i < 3; i++)
        KT_ASSERT(!r[i].child);

    /* A kernel mapping over a copied page: a device or the kernel could
     * write it behind the copy. One over a page no copy maps is fine. */
    void *kva;
    KT_EQ(vmo_map_kernel(a, 2 * PG, PG, VM_WRITE, &kva), OK);
    KT_EQ(clone1(src, dst, a, NULL), ERR_BAD_STATE);
    KT_EQ(job_used(j, JOB_LIMIT_PAGES), pages);
    KT_EQ(job_used(j, JOB_LIMIT_HANDLES), handles);
    KT_EQ(aspace_check(dst), 0);
    uint64_t pa = vmo_page_phys(a, 0);
    stamp(a, 0, 0xac);   /* not frozen, not shared */
    KT_EQ(vmo_page_phys(a, 0), pa);
    KT_EQ(vmo_unmap_kernel(a, kva), OK);
    KT_EQ(vmo_map_kernel(a, 6 * PG, PG, VM_WRITE, &kva), OK);

    /* Duplicates get the same child. */
    struct aspace_clone_req d[2] = { { .v = a, .index = 7 }, { .v = a, .index = 9 } };
    KT_EQ(aspace_clone(src, dst, d, 2), OK);
    KT_ASSERT(d[0].child && d[0].child == d[1].child);
    KT_ASSERT(d[0].index + d[1].index == 16);
    put(d[0].child);
    put(d[1].child);
    KT_EQ(vmo_unmap_kernel(a, kva), OK);
    KT_EQ(clone1(src, dst, NULL, NULL), ERR_BAD_STATE);   /* not empty now */
    KT_EQ(aspace_check(dst), 2);

    aspace_unref(dst);
    aspace_unref(src);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(s);
    put(u);
}

/* ---- through the handle layer -------------------------------------------------------- */

static status_t sys_clone(struct handle_table *t, struct aspace *src, handle_t dst,
                          const handle_t *vmos, handle_t *children, uint32_t n)
{
    status_t st = n ? handle_reserve(t, n, children) : OK;
    return st == OK ? sys_vmar_clone(t, src, dst, vmos, children, n) : st;
}

KTEST(vmar_clone_sys)
{
    struct handle_table t;
    handle_table_init(&t);
    struct job *j = kt_fresh_job();
    t.job = j;
    struct process *p;
    KT_EQ(process_create(j, "clone-dst", &p), OK);
    struct khandle kp = khandle_from_new(process_kobject(p), PROCESS_RIGHTS);
    handle_t ph, ro_ph, vh, nodup;
    KT_EQ(handle_insert(&t, &kp, &ph), OK);
    KT_EQ(handle_duplicate(&t, ph, RIGHTS_BASIC | RIGHT_READ, &ro_ph), OK);
    KT_EQ(sys_vmo_create(&t, 4 * PG, 0, &vh), OK);
    KT_EQ(handle_duplicate(&t, vh, RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER, &nodup), OK);
    struct kobject *vo;
    KT_EQ(handle_get(&t, vh, OBJ_VMO, 0, &vo, NULL), OK);
    struct vmo *v = vmo_from_kobject(vo);
    stamp(v, 0, 0x5c);
    struct aspace *src = new_as(NULL);
    map_at(src, v, 0, 4, BASE, RW | CAN_RW);
    uint32_t used = t.used;

    handle_t ch[2];
    KT_EQ(sys_clone(&t, src, ro_ph, &vh, ch, 1), ERR_ACCESS_DENIED);    /* no RIGHT_WRITE */
    KT_EQ(sys_clone(&t, src, vh, &vh, ch, 1), ERR_WRONG_TYPE);          /* not a process */
    KT_EQ(sys_clone(&t, src, ph, &nodup, ch, 1), ERR_ACCESS_DENIED);    /* no RIGHT_DUPLICATE */
    KT_EQ(sys_clone(&t, src, ph, &ph, ch, 1), ERR_WRONG_TYPE);
    KT_EQ(t.used, used);
    KT_EQ(handle_info(&t, ch[0], &(struct handle_info){ 0 }), ERR_BAD_HANDLE);   /* given back */

    handle_t two[2] = { vh, vh };
    KT_EQ(sys_clone(&t, src, ph, two, ch, 2), OK);
    KT_EQ(t.used, used + 2);
    struct handle_info hi;
    KT_EQ(handle_info(&t, ch[1], &hi), OK);
    KT_EQ(hi.type, OBJ_VMO);
    KT_EQ(hi.rights, VMO_RIGHTS);
    struct aspace *das = process_aspace(p);
    KT_ASSERT(das);
    KT_EQ(info_at(das, BASE).vmo_koid, hi.koid);
    KT_EQ(uword(das, BASE), 0x5c);
    KT_EQ(aspace_check(das), 1);
    KT_EQ(handle_close(&t, ch[0]), OK);
    KT_EQ(handle_close(&t, ch[1]), OK);

    /* Still NEW, and its handle table random from now on: four new
     * handles' generations aren't all 0 (a fresh table's are, 2^-60). */
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.state, PROCESS_NEW);
    handle_t hs[4];
    uint32_t gens = 0;
    for (unsigned i = 0; i < 4; i++) {
        struct khandle kv = khandle_from_new(vo, VMO_RIGHTS);
        kobject_ref(vo);
        KT_EQ(handle_insert(process_handles(p), &kv, &hs[i]), OK);
        gens |= hs[i] & 0x7fff;
    }
    KT_ASSERT(gens != 0);

    /* Not empty now: refused, the slots given back. */
    KT_EQ(sys_clone(&t, src, ph, &vh, ch, 1), ERR_BAD_STATE);
    KT_EQ(t.used, used);
    aspace_unref(das);

    /* A process killed before it started can't be filled. */
    struct process *q;
    KT_EQ(process_create(j, "clone-dead", &q), OK);
    struct khandle kq = khandle_from_new(process_kobject(q), PROCESS_RIGHTS);
    handle_t qh;
    KT_EQ(handle_insert(&t, &kq, &qh), OK);
    process_kill(q, PROCESS_KILLED_CODE, true);
    KT_EQ(sys_clone(&t, src, qh, &vh, ch, 1), ERR_BAD_STATE);
    KT_EQ(handle_close(&t, qh), OK);

    process_kill(p, PROCESS_KILLED_CODE, true);   /* never started: torn down here */
    kobject_unref(vo);
    aspace_unref(src);
    handle_table_destroy(&t);
    kt_job_is_empty(j);
    job_unref(j);
}

/* ---- FR1 (docs/FORK-REVIEW-KERNEL.md) ------------------------------------------- */

/* Finding 1: a vmo_write that took its page before a freeze copies its
 * bytes after it. The writer stops between the two (DBG_VMO_WRITE_COPY)
 * while a clone, or a child's creation, runs. */
struct wf_ctx {
    struct vmo    *v, *c;
    struct aspace *src, *dst;
    int            how;         /* 0: aspace_clone, 1: vmo_create_child */
    int            in_copy, go, done;   /* atomic */
    status_t       write_st, copy_st;
    uint64_t       at_return;   /* the child's word as its copy returned */
};

static struct wf_ctx *wf;

static void wf_hook(void *arg)
{
    if (arg != wf->v || __atomic_exchange_n(&wf->in_copy, 1, __ATOMIC_ACQ_REL))
        return;   /* another VMO's write, or the second time */
    uint64_t end = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    while (!__atomic_load_n(&wf->go, __ATOMIC_ACQUIRE) && uptime_ns() < end)
        thread_sleep_ms(1);
}

static void wf_writer(void *arg)
{
    (void)arg;
    uint64_t w = 2;
    wf->write_st = vmo_write(wf->v, 0, &w, 8);
}

static void wf_copier(void *arg)
{
    (void)arg;
    if (wf->how == 0) {
        wf->dst = new_as(NULL);
        wf->copy_st = clone1(wf->src, wf->dst, wf->v, &wf->c);
    } else {
        wf->copy_st = vmo_create_child(NULL, wf->v, VMO_CHILD_SNAPSHOT, 0, PG, &wf->c);
    }
    if (wf->copy_st == OK)
        wf->at_return = word(wf->c, 0);
    __atomic_store_n(&wf->done, 1, __ATOMIC_RELEASE);
}

KTEST(vmar_clone_write_in_flight)
{
    for (int how = 0; how < 2; how++) {
        wf = kzalloc(sizeof(*wf));
        KT_ASSERT(wf);
        wf->how = how;
        wf->v = mk(1);
        stamp(wf->v, 0, 1);
        wf->src = new_as(NULL);
        map_at(wf->src, wf->v, 0, 1, BASE, RW | CAN_RW);
        __atomic_store_n(&dbg_hooks[DBG_VMO_WRITE_COPY], wf_hook, __ATOMIC_RELEASE);
        struct thread *w = thread_create("kt-wf-writer", wf_writer, NULL, PRIO_DEFAULT);
        uint64_t end = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
        while (!__atomic_load_n(&wf->in_copy, __ATOMIC_ACQUIRE) && uptime_ns() < end)
            thread_sleep_ms(1);
        KT_ASSERT(__atomic_load_n(&wf->in_copy, __ATOMIC_ACQUIRE));
        struct thread *c = thread_create("kt-wf-copier", wf_copier, NULL, PRIO_DEFAULT);
        /* Without waiting for the write the copy is done in microseconds;
         * with it, it waits for the go. */
        end = uptime_ns() + 20 * NS_PER_MS;
        while (!__atomic_load_n(&wf->done, __ATOMIC_ACQUIRE) && uptime_ns() < end)
            thread_sleep_ms(1);
        bool early = __atomic_load_n(&wf->done, __ATOMIC_ACQUIRE);
        __atomic_store_n(&wf->go, 1, __ATOMIC_RELEASE);
        thread_join(w);
        thread_join(c);
        __atomic_store_n(&dbg_hooks[DBG_VMO_WRITE_COPY], NULL, __ATOMIC_RELEASE);
        KT_EQ(wf->write_st, OK);
        KT_EQ(wf->copy_st, OK);
        uint64_t now = word(wf->c, 0);
        kprintf("vmar_clone_write_in_flight: %s %s the write, the child %lu then %lu\n",
                how ? "a child's creation" : "a clone", early ? "passed" : "waited for",
                wf->at_return, now);
        /* The child's bytes never change after its copy returned. */
        KT_EQ(now, wf->at_return);
        KT_EQ(word(wf->v, 0), 2);
        KT_EQ(uword(wf->src, BASE), 2);
        put(wf->c);
        if (wf->dst)
            aspace_unref(wf->dst);
        aspace_unref(wf->src);
        put(wf->v);
        kfree(wf);
        wf = NULL;
    }
}
