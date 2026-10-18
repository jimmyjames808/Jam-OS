/* vmar_resident (mincore's kernel half; abi/syscalls.def): one byte per
 * page of a range of the caller's own address space, 1 when the VMO mapped
 * there holds the page (aspace_resident, vmo_resident).
 *
 *   vmar_resident_anon
 *       anonymous memory: nothing touched reads 0; a fault, a vmo_write
 *       and a commit put pages there, a decommit takes one away; memory
 *       protected to none keeps its pages; a second mapping at an offset of
 *       the VMO reads that VMO's pages; past a shrunk VMO's size is 0; a
 *       kept VMO and a contiguous one hold every page.
 *   vmar_resident_holes
 *       a range over two touching mappings of different VMOs is told whole;
 *       one that runs into a hole stops there (the count says where), and
 *       one starting in a hole is 0.
 *   vmar_resident_cow
 *       a clone (vmar_clone's aspace_clone) and a child (vmo_create_child):
 *       a page shared copy-on-write is there in each; a write that
 *       separates it leaves it there in both; the source's decommit of a
 *       shared page leaves the copy's.
 *   vmar_resident_pager
 *       a writable pager VMO through a mapping that may write it: absent 0,
 *       supplied (clean), dirty and writing 1, invalidated 0 again; the
 *       same VMO through a read-only mapping, and a read-only pager VMO,
 *       read 1 for every page (the cache isn't told); a pager VMO's child
 *       reads 1 for its window and its own pages, 0 past its size and for
 *       its untouched pages past the window (shrunk and grown back).
 *   vmar_resident_sparse
 *       a 256 GiB reservation with three pages touched (one past the VMO's
 *       inline 64 GiB table root) swept whole in VMAR_RESIDENT_MAX chunks:
 *       exactly those three, in well under a second on an idle machine.
 *   vmar_resident_sys
 *       the call through handles: the arguments it refuses (unaligned,
 *       empty, longer than VMAR_RESIDENT_MAX pages, outside the user range),
 *       a handle without RIGHT_READ, a VMO handle, a closed one, and an
 *       address space not the caller's own (a parent's handle of its
 *       child's) refused; the caller's own answered.
 *   vmar_resident_fuzz
 *       four fixed seeds: random mappings of plain VMOs and a child over a
 *       window with holes, random touches, writes, decommits and protects,
 *       and random ranges asked: every count and byte as the VMOs' tables
 *       say. */
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/process.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/vmar.h>
#include <jam/vmo.h>

#define PG     PAGE_SIZE
#define RW     (ASPACE_READ | ASPACE_WRITE)
#define BASE   0x30000000ull
#define ANSWER (600 * NS_PER_S)

static struct vmo *vmo_of(uint64_t pages, uint32_t flags)
{
    struct vmo *v;
    KT_EQ(vmo_create(pages * PG, flags, &v), OK);
    return v;
}

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static void map_at(struct aspace *as, struct vmo *v, uint64_t off_pages, uint64_t pages,
                   uint64_t va, unsigned flags)
{
    uint64_t at = va;
    KT_EQ(aspace_map(as, v, off_pages * PG, pages * PG, flags | ASPACE_FIXED, &at), OK);
    KT_EQ(at, va);
}

/* The n (at most 64) bytes for [va, va + n pages), all mapped, as bits:
 * bit i for page i. Every byte must be 0 or 1. */
static uint64_t res_bits(struct aspace *as, uint64_t va, unsigned n)
{
    uint8_t b[64];
    memset(b, 0xee, sizeof(b));
    KT_EQ(aspace_resident(as, va, n, b), n);
    uint64_t m = 0;
    for (unsigned i = 0; i < n; i++) {
        KT_ASSERT(b[i] <= 1);
        m |= (uint64_t)b[i] << i;
    }
    return m;
}

KTEST(vmar_resident_anon)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *a = vmo_of(16, 0);
    map_at(as, a, 0, 16, BASE, RW);
    KT_EQ(res_bits(as, BASE, 16), 0);

    /* A write fault, a read fault, a vmo_write and a commit: each a page. */
    KT_EQ(aspace_fault(as, BASE + 3 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, BASE + 9 * PG, ASPACE_READ), OK);
    KT_EQ(vmo_write(a, 12 * PG + 5, "x", 1), OK);
    KT_EQ(vmo_commit(a, 14 * PG, PG), OK);
    KT_EQ(res_bits(as, BASE, 16), 0x5208);
    KT_EQ(res_bits(as, BASE + 9 * PG, 1), 1);   /* one page asked on its own */
    KT_EQ(res_bits(as, BASE + 4 * PG, 5), 0);

    /* A decommit takes one away; a protect to none keeps its page. */
    KT_EQ(vmo_decommit(a, 3 * PG, PG), OK);
    KT_EQ(aspace_protect(as, BASE + 8 * PG, 2 * PG, 0), OK);
    KT_EQ(res_bits(as, BASE, 16), 0x5200);

    /* A second mapping of a from its page 8: its pages 9, 12 and 14. */
    map_at(as, a, 8, 8, BASE + 32 * PG, ASPACE_READ);
    KT_EQ(res_bits(as, BASE + 32 * PG, 8), 0x52);

    /* Shrunk to 10 pages: the mappings' pages past it are 0. */
    KT_EQ(vmo_set_size(a, 10 * PG), OK);
    KT_EQ(res_bits(as, BASE, 16), 0x0200);
    KT_EQ(res_bits(as, BASE + 32 * PG, 8), 0x2);

    /* A kept VMO and a contiguous one hold every page. */
    struct vmo *k = vmo_of(4, VMO_KEEP_PAGES), *c = vmo_of(4, VMO_CONTIGUOUS);
    map_at(as, k, 0, 4, BASE + 48 * PG, ASPACE_READ);
    map_at(as, c, 1, 3, BASE + 56 * PG, RW);
    KT_EQ(res_bits(as, BASE + 48 * PG, 4), 0xf);
    KT_EQ(res_bits(as, BASE + 56 * PG, 3), 0x7);

    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(k);
    put(c);
}

KTEST(vmar_resident_holes)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    /* a over pages [0, 4), a hole [4, 6), b over [6, 10) touching c over
     * [10, 12) (another VMO: two mappings), then nothing. */
    struct vmo *a = vmo_of(4, 0), *b = vmo_of(4, 0), *c = vmo_of(2, 0);
    map_at(as, a, 0, 4, BASE, RW);
    map_at(as, b, 0, 4, BASE + 6 * PG, RW);
    map_at(as, c, 0, 2, BASE + 10 * PG, RW);
    KT_EQ(aspace_fault(as, BASE + 3 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, BASE + 9 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, BASE + 10 * PG, ASPACE_WRITE), OK);

    uint8_t v[16];
    memset(v, 0xee, sizeof(v));
    KT_EQ(aspace_resident(as, BASE + 2 * PG, 4, v), 2);   /* runs into the hole at 4 */
    KT_EQ(v[0], 0);
    KT_EQ(v[1], 1);
    KT_EQ(v[2], 0xee);   /* nothing written past the count */
    KT_EQ(aspace_resident(as, BASE + 4 * PG, 2, v), 0);
    KT_EQ(aspace_resident(as, BASE + 5 * PG, 1, v), 0);
    KT_EQ(res_bits(as, BASE + 6 * PG, 6), 0x18);   /* across b and c */
    memset(v, 0xee, sizeof(v));
    KT_EQ(aspace_resident(as, BASE + 6 * PG, 10, v), 6);   /* the hole after c */
    KT_EQ(v[3], 1);
    KT_EQ(v[4], 1);
    KT_EQ(v[6], 0xee);
    KT_EQ(aspace_resident(as, BASE + 12 * PG, 1, v), 0);
    KT_EQ(aspace_resident(as, BASE - PG, 4, v), 0);   /* a hole, then a */

    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(b);
    put(c);
}

KTEST(vmar_resident_cow)
{
    struct job *j = kt_fresh_job();
    struct aspace *src, *dst;
    KT_EQ(aspace_create_charged(j, &src), OK);
    KT_EQ(aspace_create_charged(j, &dst), OK);
    struct vmo *a = vmo_of(8, 0);
    map_at(src, a, 0, 8, BASE, RW);   /* a copy in a clone */
    KT_EQ(aspace_fault(src, BASE, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(src, BASE + 2 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(src, BASE + 5 * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_clone(src, dst, NULL, 0), OK);
    KT_EQ(res_bits(src, BASE, 8), 0x25);
    KT_EQ(res_bits(dst, BASE, 8), 0x25);   /* shared: there in each */

    /* The copy's write separates page 0, and it makes page 7: both sides
     * keep page 0, only the copy has 7. */
    KT_EQ(aspace_fault(dst, BASE, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(dst, BASE + 7 * PG, ASPACE_WRITE), OK);
    KT_EQ(res_bits(src, BASE, 8), 0x25);
    KT_EQ(res_bits(dst, BASE, 8), 0xa5);

    /* The source gives page 2 back; the copy keeps its share of it. */
    KT_EQ(vmo_decommit(a, 2 * PG, PG), OK);
    KT_EQ(res_bits(src, BASE, 8), 0x21);
    KT_EQ(res_bits(dst, BASE, 8), 0xa5);

    /* A child of a made directly holds what a holds now. */
    struct vmo *c;
    KT_EQ(vmo_create_child(j, a, VMO_CHILD_SNAPSHOT, 0, 8 * PG, &c), OK);
    map_at(src, c, 0, 8, BASE + 16 * PG, ASPACE_READ);
    KT_EQ(res_bits(src, BASE + 16 * PG, 8), 0x21);

    aspace_unref(src);
    aspace_unref(dst);
    put(c);
    put(a);
    kt_job_is_empty(j);
    job_unref(j);
}

/* Pages [first, first + n) of key's file, supplied. */
static void supply(struct pager *pg, uint64_t key, uint64_t first, uint64_t n)
{
    uint8_t *buf = kzalloc(n * PG);
    KT_ASSERT(buf);
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    KT_EQ(pager_supply(pg, key, first * PG, &src, n * PG), OK);
    kfree(buf);
}

KTEST(vmar_resident_pager)
{
    struct job *j = kt_fresh_job();
    struct pager *pg;
    KT_EQ(pager_create(j, 64, ANSWER, &pg), OK);
    struct vmo *w, *r, *c;
    KT_EQ(pager_create_vmo_flags(pg, 1, 8 * PG, PAGER_VMO_WRITABLE, &w), OK);
    KT_EQ(pager_create_vmo(pg, 2, 4 * PG, &r), OK);
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    map_at(as, w, 0, 8, BASE, RW);                     /* may write: told */
    map_at(as, w, 0, 8, BASE + 16 * PG, ASPACE_READ);   /* may not: not told */
    map_at(as, r, 0, 4, BASE + 32 * PG, ASPACE_READ);   /* read-only VMO: not told */
    KT_EQ(res_bits(as, BASE, 8), 0);
    KT_EQ(res_bits(as, BASE + 16 * PG, 8), 0xff);
    KT_EQ(res_bits(as, BASE + 32 * PG, 4), 0xf);

    /* Supplied (clean), dirty, writing: all there. */
    supply(pg, 1, 0, 4);
    KT_EQ(res_bits(as, BASE, 8), 0xf);
    KT_EQ(aspace_fault(as, BASE + PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, BASE + 2 * PG, ASPACE_WRITE), OK);
    KT_EQ(vmo_pager_page_state(w, 1), PAGER_PAGE_DIRTY);
    uint8_t *buf = kzalloc(PG);
    KT_ASSERT(buf);
    struct pager_run run = { .key = 1, .offset = 2 * PG, .len = PG };
    struct pager_dst dst = { .addr = (uint64_t)(uintptr_t)buf, .user = false };
    KT_EQ(pager_dirty(pg, 1, PAGER_DIRTY_BEGIN, &run, &dst), OK);
    KT_EQ(run.len, PG);
    KT_EQ(vmo_pager_page_state(w, 2), PAGER_PAGE_WRITING);
    KT_EQ(res_bits(as, BASE, 8), 0xf);
    KT_EQ(pager_op(pg, 1, PAGER_OP_WRITTEN, 2 * PG, PG, OK), OK);
    KT_EQ(vmo_pager_page_state(w, 2), 0);   /* clean again */
    KT_EQ(res_bits(as, BASE, 8), 0xf);

    /* Invalidated (a clean page): gone from the cache, 0 again. */
    KT_EQ(pager_op(pg, 1, PAGER_OP_INVALIDATE, 0, PG, 0), OK);
    KT_EQ(res_bits(as, BASE, 8), 0xe);
    KT_EQ(res_bits(as, BASE + 16 * PG, 8), 0xff);   /* still not told */

    /* A child of r, 8 pages over r's 4: its window is all of it (the
     * pages past r's end read as zero), and every page there reads 1 (it
     * would come from r). Shrunk to 4 pages and grown back, its window
     * stays 4: the pages past it are the child's own, told as they are. */
    KT_EQ(vmo_create_child(j, r, VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE, 0, 8 * PG, &c), OK);
    map_at(as, c, 0, 8, BASE + 48 * PG, RW);
    KT_EQ(res_bits(as, BASE + 48 * PG, 8), 0xff);
    KT_EQ(vmo_set_size(c, 4 * PG), OK);
    KT_EQ(res_bits(as, BASE + 48 * PG, 8), 0xf);   /* past the size */
    KT_EQ(vmo_set_size(c, 8 * PG), OK);
    KT_EQ(res_bits(as, BASE + 48 * PG, 8), 0xf);
    KT_EQ(aspace_fault(as, BASE + 54 * PG, ASPACE_WRITE), OK);   /* past the window */
    KT_EQ(res_bits(as, BASE + 48 * PG, 8), 0x4f);

    kfree(buf);
    aspace_unref(as);
    put(c);
    put(w);
    put(r);
    kt_pager_close(pg);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(vmar_resident_sparse)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    const uint64_t pages = 1ull << 26;   /* 256 GiB */
    struct vmo *v = vmo_of(pages, 0);
    uint64_t at = 0;
    KT_EQ(aspace_map(as, v, 0, pages * PG, RW, &at), OK);
    const uint64_t far = 200ull << 18;   /* 200 GiB in: past the inline root's 64 GiB */
    KT_EQ(aspace_fault(as, at, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, at + far * PG, ASPACE_WRITE), OK);
    KT_EQ(aspace_fault(as, at + (pages - 1) * PG, ASPACE_WRITE), OK);

    uint64_t *b = kmalloc(VMAR_RESIDENT_MAX);
    KT_ASSERT(b);
    uint64_t in = 0, start = uptime_ns();
    for (uint64_t p = 0; p < pages; p += VMAR_RESIDENT_MAX) {
        KT_EQ(aspace_resident(as, at + p * PG, VMAR_RESIDENT_MAX, (uint8_t *)b),
              VMAR_RESIDENT_MAX);
        for (unsigned w = 0; w < VMAR_RESIDENT_MAX / 8; w++) {
            if (!b[w])
                continue;   /* eight pages not there */
            for (unsigned i = w * 8; i < w * 8 + 8; i++) {
                if (((const uint8_t *)b)[i]) {
                    KT_ASSERT(p + i == 0 || p + i == far || p + i == pages - 1);
                    in++;
                }
            }
        }
    }
    uint64_t ns = uptime_ns() - start;
    KT_EQ(in, 3);
    KT_IDLE_ASSERT(ns < NS_PER_S);   /* 16384 calls, almost all over tables that aren't there */

    kfree(b);
    aspace_unref(as);
    put(v);
    kt_job_is_empty(j);
    job_unref(j);
}

static struct aspace *aspace_of(struct handle_table *t, handle_t vmar)
{
    struct kobject *o;
    KT_EQ(handle_get(t, vmar, OBJ_VMAR, 0, &o, NULL), OK);
    struct aspace *as = vmar_aspace(vmar_from_kobject(o));
    kobject_unref(o);   /* the handle keeps it */
    return as;
}

KTEST(vmar_resident_sys)
{
    struct handle_table t;
    handle_table_init(&t);
    handle_t vmar, other, h, noread;
    KT_EQ(sys_vmar_create(&t, &vmar), OK);
    KT_EQ(sys_vmar_create(&t, &other), OK);
    KT_EQ(sys_vmo_create(&t, 4 * PG, 0, &h), OK);
    struct aspace *self = aspace_of(&t, vmar), *child = aspace_of(&t, other);
    /* At a fixed place with room on both sides: placed top down, the
     * fresh address space's one mapping sits at the very top, and the
     * probes past it below would be out of the user range. */
    uint64_t a = 1ull << 40;
    KT_EQ(sys_vmar_map(&t, vmar, h, 0, 4 * PG, VMAR_READ | VMAR_WRITE | VMAR_FIXED, &a), OK);
    KT_EQ(aspace_fault(self, a + PG, ASPACE_WRITE), OK);
    uint8_t *b = kmalloc(VMAR_RESIDENT_MAX + 1);
    KT_ASSERT(b);

    /* The caller's own, answered. */
    memset(b, 0xee, 8);
    KT_EQ(sys_vmar_resident(&t, self, vmar, a, 4 * PG, b), 4);
    KT_EQ(b[0], 0);
    KT_EQ(b[1], 1);
    KT_EQ(b[3], 0);
    KT_EQ(sys_vmar_resident(&t, self, vmar, a + 4 * PG, PG, b), 0);   /* nothing there */
    KT_EQ(sys_vmar_resident(&t, self, vmar, a, (uint64_t)VMAR_RESIDENT_MAX * PG, b), 4);

    /* The arguments. */
    KT_EQ(sys_vmar_resident(&t, self, vmar, a + 1, PG, b), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_resident(&t, self, vmar, a, PG + 1, b), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_resident(&t, self, vmar, a, 0, b), ERR_INVALID_ARGS);
    KT_EQ(sys_vmar_resident(&t, self, vmar, a, (VMAR_RESIDENT_MAX + 1ull) * PG, b),
          ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmar_resident(&t, self, vmar, 0, PG, b), ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmar_resident(&t, self, vmar, USER_TOP, PG, b), ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmar_resident(&t, self, vmar, USER_TOP - PG, 2 * PG, b), ERR_OUT_OF_RANGE);
    KT_EQ(sys_vmar_resident(&t, self, vmar, -PG, PG, b), ERR_OUT_OF_RANGE);

    /* The handle: RIGHT_READ, a vmar, open, and the caller's own. */
    KT_EQ(handle_duplicate(&t, vmar, RIGHTS_BASIC | RIGHT_WRITE, &noread), OK);
    KT_EQ(sys_vmar_resident(&t, self, noread, a, PG, b), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_resident(&t, self, h, a, PG, b), ERR_WRONG_TYPE);
    KT_EQ(sys_vmar_resident(&t, child, vmar, a, PG, b), ERR_ACCESS_DENIED);   /* a parent's look */
    KT_EQ(sys_vmar_resident(&t, self, other, a, PG, b), ERR_ACCESS_DENIED);
    KT_EQ(sys_vmar_resident(&t, child, other, a, PG, b), 0);   /* its own: nothing mapped */
    KT_EQ(handle_close(&t, noread), OK);
    KT_EQ(sys_vmar_resident(&t, self, noread, a, PG, b), ERR_BAD_HANDLE);

    kfree(b);
    handle_table_destroy(&t);
}

/* The fuzz's layout: up to SLOTS mappings over a window of WIN pages. */
#define WIN   512u
#define SLOTS 10u
#define VMOS  5u

struct fz_map {
    uint64_t    first, pages;   /* window pages */
    struct vmo *v;
    uint64_t    off;            /* VMO page at first */
};

/* What the byte for window page p should be, and whether p is mapped. */
static bool fz_expect(const struct fz_map *m, unsigned n, uint64_t p, uint8_t *want)
{
    for (unsigned i = 0; i < n; i++) {
        if (p >= m[i].first && p < m[i].first + m[i].pages) {
            *want = vmo_page_phys(m[i].v, (m[i].off + p - m[i].first) * PG) != 0;
            return true;
        }
    }
    return false;
}

/* One layout and 300 rounds from seed s (a failure names it). */
static void fuzz_one(uint64_t s)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    uint64_t seed = s;
    struct vmo *v[VMOS];
    for (unsigned i = 0; i < VMOS - 1; i++)
        v[i] = vmo_of(64, 0);
    for (unsigned i = 0; i < 32; i++)   /* a child sharing some of v[0]'s pages */
        if (kt_rng(&seed) & 1)
            KT_EQ(vmo_write(v[0], i * PG, "s", 1), OK);
    KT_EQ(vmo_create_child(j, v[0], VMO_CHILD_SNAPSHOT, 0, 64 * PG, &v[VMOS - 1]), OK);

    /* Mappings one after another with random gaps (0 often: touching). */
    struct fz_map m[SLOTS];
    unsigned n = 0;
    for (uint64_t p = kt_rng(&seed) % 8; n < SLOTS && p < WIN; n++) {
        struct fz_map *x = &m[n];
        x->v = v[kt_rng(&seed) % VMOS];
        x->pages = 1 + kt_rng(&seed) % 48;
        x->off = kt_rng(&seed) % (64 - x->pages + 1);
        if (p + x->pages > WIN)
            break;
        x->first = p;
        map_at(as, x->v, x->off, x->pages, BASE + p * PG, RW);
        p += x->pages + (kt_rng(&seed) % 3 ? 0 : kt_rng(&seed) % 16);
    }
    KT_ASSERT(n > 0);

    uint8_t b[WIN + 64];
    for (unsigned round = 0; round < 300; round++) {
        /* Change something: touch, write, decommit or protect. */
        struct fz_map *x = &m[kt_rng(&seed) % n];
        uint64_t k = kt_rng(&seed) % x->pages, va = BASE + (x->first + k) * PG;
        switch (kt_rng(&seed) % 5) {
        case 0:
        case 1:
            KT_EQ(aspace_protect(as, va, PG, RW), OK);
            KT_EQ(aspace_fault(as, va, kt_rng(&seed) & 1 ? ASPACE_WRITE : ASPACE_READ), OK);
            break;
        case 2:
            KT_EQ(vmo_write(x->v, (x->off + k) * PG, "w", 1), OK);
            break;
        case 3:
            KT_EQ(vmo_decommit(x->v, (x->off + k) * PG, PG), OK);
            break;
        default:
            KT_EQ(aspace_protect(as, va, PG, kt_rng(&seed) & 1 ? ASPACE_READ : 0), OK);
            break;
        }
        /* Ask a random range (it may start or run into a hole). */
        uint64_t first = kt_rng(&seed) % (WIN + 8), len = 1 + kt_rng(&seed) % (WIN + 64 - 1);
        memset(b, 0xee, sizeof(b));
        uint64_t got = aspace_resident(as, BASE + first * PG, len, b);
        uint64_t want_n = 0;
        uint8_t want;
        while (want_n < len && fz_expect(m, n, first + want_n, &want)) {
            if (b[want_n] != want)
                ktest_fail("seed %lx round %u: page %lu of the window reads %u, not %u", s,
                           round, first + want_n, b[want_n], want);
            want_n++;
        }
        if (got != want_n || (got < len && b[got] != 0xee))
            ktest_fail("seed %lx round %u: %lu pages from window page %lu, not %lu", s, round,
                       got, first, want_n);
    }

    aspace_unref(as);
    for (unsigned i = 0; i < VMOS; i++)
        put(v[i]);
    kt_job_is_empty(j);
    job_unref(j);
}

KTEST(vmar_resident_fuzz)
{
    for (uint64_t s = 1; s <= 4; s++)
        fuzz_one(0x6d696e636f7265ull * s);
}
