/* Clone modes (mm/aspace.c's "Clone modes", docs/FORK-PLAN.md section 1.1,
 * track FK1): what a clone of the address space (vmar_clone, FK2) makes of
 * each mapping. aspace_map decides it once, and nothing after changes it.
 *
 *   aspace_clone_defaults
 *       without a flag, a copy where the maximum has write, else shared
 *       (read-only through a handle that can't write, execute-only, a
 *       guard, a kept mapping); a contiguous or physical VMO shared where
 *       it would be copied; the flags win otherwise; both together, or
 *       either with ASPACE_KEPT_ONLY, refused with nothing mapped.
 *   aspace_clone_jit_never_reaches
 *       a JIT VMO's mapping whose maximum has write is none whatever the
 *       caller asks, and stays none through protect (to RX and back to
 *       RWX); one through a maximum without write is shared, or none.
 *   aspace_clone_kept_by_every_operation
 *       the mode survives protect splits and merges back, an unmap that
 *       splits, and a replace that cuts; neighbours merge only with the
 *       same mode (on map, protect and replace), and aspace_check finds
 *       every mapping's mode allowed and no two neighbours that could be
 *       one after each step.
 *   aspace_clone_sys_flags
 *       through the system call's layer: the maximum comes from the
 *       handle's rights (a read-only mapping through a handle that can
 *       write is copied, through one that can't is shared), the flags
 *       reach aspace_map, vmar_protect refuses them and keeps the mode.
 *
 * The names start "aspace_clone", so `ktest=aspace` runs them. */
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sys.h>
#include <jam/vmar.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define R     ASPACE_READ
#define RW    (ASPACE_READ | ASPACE_WRITE)
#define RX    (ASPACE_READ | ASPACE_EXEC)
#define RWX   (ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC)
#define CAN_R ASPACE_CAN_READ
#define CAN_RW  (ASPACE_CAN_READ | ASPACE_CAN_WRITE)
#define CAN_RX  (ASPACE_CAN_READ | ASPACE_CAN_EXEC)
#define CAN_ALL (ASPACE_CAN_READ | ASPACE_CAN_WRITE | ASPACE_CAN_EXEC)
#define SHARE ASPACE_CLONE_SHARE
#define NONE  ASPACE_CLONE_NONE
#define COPY  0u
#define FIX   ASPACE_FIXED
#define BASE  0x30000000ull

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct vmo *vmo_of(unsigned pages, uint32_t flags)
{
    struct vmo *v;
    KT_EQ(vmo_create(pages * PG, flags, &v), OK);
    return v;
}

/* The mapping over va: its clone mode (map_info.flags), and its base and
 * length if asked. */
static uint32_t mode_at(struct aspace *as, uint64_t va, uint64_t *base, uint64_t *len)
{
    struct map_info mi;
    KT_EQ(aspace_get_maps(as, va, &mi, 1), 1);
    KT_ASSERT(mi.base <= va && va < mi.base + mi.len);
    if (base)
        *base = mi.base;
    if (len)
        *len = mi.len;
    return mi.flags;
}

/* Map pages [off, off + n) of v at page `at` of BASE, fixed. */
static status_t map_at(struct aspace *as, struct vmo *v, unsigned off, unsigned n, unsigned at,
                       unsigned flags)
{
    uint64_t a = BASE + at * PG;
    return aspace_map(as, v, off * PG, n * PG, flags | FIX, &a);
}

static void check(struct aspace *as, uint32_t maps)
{
    KT_EQ(aspace_check(as), maps);
    KT_EQ(aspace_mapping_count(as), maps);
}

KTEST(aspace_clone_defaults)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *a = vmo_of(16, 0), *c = vmo_of(1, VMO_CONTIGUOUS), *k = vmo_of(1, VMO_KEEP_PAGES);
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    KT_ASSERT(pa);
    struct vmo *p;
    KT_EQ(vmo_create_physical(pa, PG, 0, &p), OK);

    /* Each its own page of a, a page apart, so none merge. Without a flag
     * the maximum decides: write in it (even unused) is a copy. */
    KT_EQ(map_at(as, a, 0, 1, 0, RW | CAN_RW), OK);
    KT_EQ(map_at(as, a, 1, 1, 2, R | CAN_RW), OK);
    KT_EQ(map_at(as, a, 2, 1, 4, R | CAN_R), OK);
    KT_EQ(map_at(as, a, 3, 1, 6, RX | CAN_RX), OK);
    KT_EQ(map_at(as, a, 4, 1, 8, 0), OK);                  /* a guard: no maximum at all */
    KT_EQ(map_at(as, a, 5, 1, 10, RW), OK);                /* the kernel's own: max = perms */
    /* The flags win over the default, either way. */
    KT_EQ(map_at(as, a, 6, 1, 12, RW | CAN_RW | NONE), OK);
    KT_EQ(map_at(as, a, 7, 1, 14, RW | CAN_RW | SHARE), OK);
    KT_EQ(map_at(as, a, 8, 1, 16, R | CAN_R | NONE), OK);
    KT_EQ(map_at(as, a, 9, 1, 18, R | CAN_R | SHARE), OK);
    /* Device memory can't be copied: shared where it would be, none kept. */
    KT_EQ(map_at(as, c, 0, 1, 20, RW | CAN_RW), OK);
    KT_EQ(map_at(as, c, 0, 1, 22, RW | CAN_RW | NONE), OK);
    KT_EQ(map_at(as, p, 0, 1, 24, RW | CAN_RW), OK);
    /* A kept mapping is shared, and takes no flag. */
    KT_EQ(map_at(as, k, 0, 1, 26, ASPACE_KEPT_ONLY | R), OK);
    check(as, 14);

    static const uint32_t want[14] = { COPY, COPY,  SHARE, SHARE, SHARE, COPY, NONE,
                                       SHARE, NONE, SHARE, SHARE, NONE,  SHARE, SHARE };
    for (unsigned i = 0; i < 14; i++)
        KT_EQ(mode_at(as, BASE + 2 * i * PG, NULL, NULL), want[i]);

    /* Refused, nothing mapped: both flags; either with a kept mapping;
     * a bad flag combination comes before the kind of VMO. */
    KT_EQ(map_at(as, a, 10, 1, 30, RW | CAN_RW | SHARE | NONE), ERR_INVALID_ARGS);
    KT_EQ(map_at(as, k, 0, 1, 30, ASPACE_KEPT_ONLY | R | SHARE), ERR_INVALID_ARGS);
    KT_EQ(map_at(as, k, 0, 1, 30, ASPACE_KEPT_ONLY | R | NONE), ERR_INVALID_ARGS);
    KT_EQ(map_at(as, a, 0, 1, 30, ASPACE_KEPT_ONLY | R | NONE), ERR_INVALID_ARGS);
    check(as, 14);

    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(c);
    put(k);
    put(p);
    pmm_free_page_phys(pa);   /* physical VMOs never free their pages */
}

KTEST(aspace_clone_jit_never_reaches)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *v = vmo_of(8, 0);
    KT_EQ(vmo_make_jit(v), OK);

    /* A maximum with write: none, whatever is asked or mapped now. */
    KT_EQ(map_at(as, v, 0, 1, 0, RWX | CAN_ALL), OK);
    KT_EQ(map_at(as, v, 1, 1, 2, RX | CAN_ALL), OK);
    KT_EQ(map_at(as, v, 2, 1, 4, RX | CAN_ALL | SHARE), OK);
    KT_EQ(map_at(as, v, 3, 1, 6, RW | CAN_RW), OK);
    KT_EQ(map_at(as, v, 4, 1, 8, R | CAN_RW | SHARE), OK);
    /* Without write in the maximum (a handle that can't write): the
     * default, shared, or none if asked. */
    KT_EQ(map_at(as, v, 5, 1, 10, RX | CAN_RX), OK);
    KT_EQ(map_at(as, v, 6, 1, 12, RX | CAN_RX | NONE), OK);
    check(as, 7);
    static const uint32_t want[7] = { NONE, NONE, NONE, NONE, NONE, SHARE, NONE };
    for (unsigned i = 0; i < 7; i++)
        KT_EQ(mode_at(as, BASE + 2 * i * PG, NULL, NULL), want[i]);

    /* Protect never changes it: the maximum it came from stays. */
    KT_EQ(aspace_fault(as, BASE, ASPACE_WRITE), OK);
    KT_EQ(aspace_protect(as, BASE, PG, RX), OK);
    KT_EQ(mode_at(as, BASE, NULL, NULL), NONE);
    KT_EQ(aspace_protect(as, BASE, PG, R), OK);
    KT_EQ(mode_at(as, BASE, NULL, NULL), NONE);
    KT_EQ(aspace_protect(as, BASE, PG, RWX), OK);
    KT_EQ(mode_at(as, BASE, NULL, NULL), NONE);
    KT_EQ(aspace_protect(as, BASE + 10 * PG, PG, R), OK);
    KT_EQ(mode_at(as, BASE + 10 * PG, NULL, NULL), SHARE);
    check(as, 7);

    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(v);
}

KTEST(aspace_clone_kept_by_every_operation)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *a = vmo_of(16, 0), *b = vmo_of(8, 0), *c = vmo_of(8, 0);
    uint64_t base, len;

    /* a over pages [0, 16), none: a protect in the middle splits it three
     * ways, each piece none; protected back it is one mapping again. */
    KT_EQ(map_at(as, a, 0, 16, 0, RW | CAN_RW | NONE), OK);
    check(as, 1);
    KT_EQ(aspace_protect(as, BASE + 4 * PG, 4 * PG, R), OK);
    check(as, 3);
    for (unsigned pg = 0; pg < 16; pg += 4)
        KT_EQ(mode_at(as, BASE + pg * PG, NULL, NULL), NONE);
    KT_EQ(aspace_protect(as, BASE + 4 * PG, 4 * PG, RW), OK);
    check(as, 1);
    KT_EQ(mode_at(as, BASE, &base, &len), NONE);
    KT_EQ(len, 16 * PG);

    /* An unmap in the middle: two pieces, each none. Mapped again with
     * another mode the hole doesn't merge; with the same mode it does. */
    KT_EQ(aspace_unmap(as, BASE + 6 * PG, PG), OK);
    check(as, 2);
    KT_EQ(mode_at(as, BASE, NULL, NULL), NONE);
    KT_EQ(mode_at(as, BASE + 7 * PG, NULL, NULL), NONE);
    KT_EQ(map_at(as, a, 6, 1, 6, RW | CAN_RW), OK);   /* a copy */
    check(as, 3);
    KT_EQ(mode_at(as, BASE + 6 * PG, &base, &len), COPY);
    KT_EQ(base, BASE + 6 * PG);
    KT_EQ(len, PG);
    KT_EQ(aspace_unmap(as, BASE + 6 * PG, PG), OK);
    KT_EQ(map_at(as, a, 6, 1, 6, RW | CAN_RW | NONE), OK);
    check(as, 1);

    /* b: [0, 4) a copy and [4, 8) none, both read-only with a maximum that
     * writes, at contiguous addresses and offsets: two mappings. A protect
     * over both to read-write leaves two; over the first's tail it splits
     * it, the piece kept a copy and merged back after. */
    KT_EQ(map_at(as, b, 0, 4, 20, R | CAN_RW), OK);
    KT_EQ(map_at(as, b, 4, 4, 24, R | CAN_RW | NONE), OK);
    check(as, 3);
    KT_EQ(aspace_protect(as, BASE + 20 * PG, 8 * PG, RW), OK);
    check(as, 3);
    KT_EQ(mode_at(as, BASE + 23 * PG, NULL, NULL), COPY);
    KT_EQ(mode_at(as, BASE + 24 * PG, NULL, NULL), NONE);
    KT_EQ(aspace_protect(as, BASE + 22 * PG, 2 * PG, R), OK);
    check(as, 4);
    KT_EQ(mode_at(as, BASE + 22 * PG, &base, &len), COPY);
    KT_EQ(base, BASE + 22 * PG);
    KT_EQ(len, 2 * PG);
    KT_EQ(aspace_protect(as, BASE + 22 * PG, 2 * PG, RW), OK);
    check(as, 3);
    KT_EQ(mode_at(as, BASE + 20 * PG, &base, &len), COPY);
    KT_EQ(len, 4 * PG);

    /* c over [0, 8), shared: a replace of [2, 4) with the same pages as a
     * copy cuts it in three (the pieces kept shared, the middle not merged);
     * replaced again shared, it is one mapping; replaced whole with none, one. */
    KT_EQ(map_at(as, c, 0, 8, 40, RW | CAN_RW | SHARE), OK);
    check(as, 4);
    KT_EQ(map_at(as, c, 2, 2, 42, RW | CAN_RW | ASPACE_REPLACE), OK);
    check(as, 6);
    KT_EQ(mode_at(as, BASE + 40 * PG, NULL, NULL), SHARE);
    KT_EQ(mode_at(as, BASE + 42 * PG, NULL, NULL), COPY);
    KT_EQ(mode_at(as, BASE + 44 * PG, &base, &len), SHARE);
    KT_EQ(base, BASE + 44 * PG);
    KT_EQ(len, 4 * PG);
    KT_EQ(map_at(as, c, 2, 2, 42, RW | CAN_RW | SHARE | ASPACE_REPLACE), OK);
    check(as, 4);
    KT_EQ(mode_at(as, BASE + 40 * PG, NULL, &len), SHARE);
    KT_EQ(len, 8 * PG);
    KT_EQ(map_at(as, c, 0, 8, 40, RW | CAN_RW | NONE | ASPACE_REPLACE), OK);
    check(as, 4);
    KT_EQ(mode_at(as, BASE + 47 * PG, NULL, &len), NONE);
    KT_EQ(len, 8 * PG);
    /* A replace that only cuts the end of a mapping of another mode. */
    KT_EQ(map_at(as, c, 6, 4, 46, R | CAN_R | ASPACE_REPLACE), ERR_OUT_OF_RANGE);
    KT_EQ(map_at(as, b, 0, 4, 46, R | CAN_R | ASPACE_REPLACE), OK);
    check(as, 5);
    KT_EQ(mode_at(as, BASE + 45 * PG, &base, &len), NONE);
    KT_EQ(len, 6 * PG);
    KT_EQ(mode_at(as, BASE + 46 * PG, NULL, NULL), SHARE);

    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(b);
    put(c);
}

/* The aspace a vmar handle names (the handle keeps it alive). */
static struct aspace *aspace_of(struct handle_table *t, handle_t vmar)
{
    struct kobject *o;
    KT_EQ(handle_get(t, vmar, OBJ_VMAR, 0, &o, NULL), OK);
    struct aspace *as = vmar_aspace(vmar_from_kobject(o));
    kobject_unref(o);
    return as;
}

KTEST(aspace_clone_sys_flags)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t vmar, h, ro;
    KT_EQ(sys_vmar_create(&t, &vmar), OK);
    KT_EQ(sys_vmo_create(&t, 8 * PG, 0, &h), OK);
    KT_EQ(handle_duplicate(&t, h, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP, &ro), OK);
    struct aspace *as = aspace_of(&t, vmar);

    uint64_t a[6] = { 0 };
    /* Read-only through a handle that can write: the maximum writes, a
     * copy; through one that can't: shared. */
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, PG, VMAR_READ, &a[0]), OK);
    KT_EQ(sys_vmar_map(&t, vmar, ro, PG, PG, VMAR_READ, &a[1]), OK);
    KT_EQ(sys_vmar_map(&t, vmar, h, 2 * PG, PG, VMAR_READ | VMAR_WRITE | VMAR_CLONE_NONE, &a[2]),
          OK);
    KT_EQ(sys_vmar_map(&t, vmar, h, 3 * PG, PG, VMAR_READ | VMAR_WRITE | VMAR_CLONE_SHARE, &a[3]),
          OK);
    KT_EQ(sys_vmar_map(&t, vmar, ro, 4 * PG, PG, VMAR_READ | VMAR_CLONE_NONE, &a[4]), OK);
    KT_EQ(sys_vmar_map(&t, vmar, h, 5 * PG, PG,
                       VMAR_READ | VMAR_WRITE | VMAR_CLONE_SHARE | VMAR_CLONE_NONE, &a[5]),
          ERR_INVALID_ARGS);
    static const uint32_t want[5] = { COPY, SHARE, NONE, SHARE, NONE };
    for (unsigned i = 0; i < 5; i++)
        KT_EQ(mode_at(as, a[i], NULL, NULL), want[i]);
    KT_EQ(aspace_check(as), 5);

    /* vmar_protect takes no mode: refused, the mode as it was. */
    KT_EQ(sys_vmar_protect(&t, vmar, a[2], PG, VMAR_READ | VMAR_CLONE_SHARE), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_protect(&t, vmar, a[0], PG, VMAR_READ | VMAR_CLONE_NONE), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_protect(&t, vmar, a[2], PG, VMAR_READ), OK);
    KT_EQ(mode_at(as, a[2], NULL, NULL), NONE);
    KT_EQ(mode_at(as, a[0], NULL, NULL), COPY);

    handle_table_destroy(&t);
}
