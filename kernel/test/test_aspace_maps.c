/* aspace_get_maps (mm/aspace.c): the mappings of an address space in order,
 * with the VMO behind each and the pages present (process_get_maps's
 * source, which /proc/self/maps, stat and status are made from).
 *
 *   aspace_maps_shapes
 *       a read-write mapping, a read-only one and a guard (no access): the
 *       entries' bases, lengths, VMOs, offsets, permissions and clone
 *       modes; `resident`
 *       follows the faults (and is 0 for the guard); a protect splits one
 *       mapping into three and one to no access clears its count; the
 *       `from` cursor and the cap walk the same list in pieces; an unmap
 *       leaves the rest, and an empty address space has none. */
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/uentry.h>
#include <jam/vmo.h>

#define PG   PAGE_SIZE
#define RW   (ASPACE_READ | ASPACE_WRITE)
#define BASE 0x30000000ull

static struct vmo *vmo_of(unsigned pages)
{
    struct vmo *v;
    KT_EQ(vmo_create(pages * PG, 0, &v), OK);
    return v;
}

static void touch(struct aspace *as, unsigned page, unsigned access)
{
    KT_EQ(aspace_fault(as, BASE + page * PG, access), OK);
}

KTEST(aspace_maps_shapes)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct map_info m[8];
    KT_EQ(aspace_get_maps(as, 0, m, 8), 0);

    /* a: pages [0, 8) read-write; b: [10, 12) read-only from its page 1;
     * a guard over [14, 16). */
    struct vmo *a = vmo_of(8), *b = vmo_of(4), *g = vmo_of(2);
    uint64_t at = BASE;
    KT_EQ(aspace_map(as, a, 0, 8 * PG, RW | ASPACE_FIXED, &at), OK);
    at = BASE + 10 * PG;
    KT_EQ(aspace_map(as, b, PG, 2 * PG, ASPACE_READ | ASPACE_FIXED, &at), OK);
    at = BASE + 14 * PG;
    KT_EQ(aspace_map(as, g, 0, 2 * PG, ASPACE_FIXED, &at), OK);

    KT_EQ(aspace_get_maps(as, 0, m, 8), 3);
    KT_EQ(m[0].base, BASE);
    KT_EQ(m[0].len, 8 * PG);
    KT_EQ(m[0].vmo_koid, vmo_kobject(a)->koid);
    KT_EQ(m[0].vmo_off, 0);
    KT_EQ(m[0].perms, VMAR_READ | VMAR_WRITE);
    KT_EQ(m[0].resident, 0);   /* nothing touched yet */
    KT_EQ(m[1].base, BASE + 10 * PG);
    KT_EQ(m[1].len, 2 * PG);
    KT_EQ(m[1].vmo_koid, vmo_kobject(b)->koid);
    KT_EQ(m[1].vmo_off, PG);
    KT_EQ(m[1].perms, VMAR_READ);
    KT_EQ(m[2].base, BASE + 14 * PG);
    KT_EQ(m[2].perms, 0);
    KT_EQ(m[2].resident, 0);
    /* Clone modes (test_aspace_clone.c): a copy where the maximum writes. */
    KT_EQ(m[0].flags, 0);
    KT_EQ(m[1].flags, VMAR_CLONE_SHARE);
    KT_EQ(m[2].flags, VMAR_CLONE_SHARE);

    /* Pages present follow the faults: two of a's, one of b's. */
    touch(as, 1, ASPACE_WRITE);
    touch(as, 5, ASPACE_READ);
    touch(as, 11, ASPACE_READ);
    KT_EQ(aspace_get_maps(as, 0, m, 8), 3);
    KT_EQ(m[0].resident, 2);
    KT_EQ(m[1].resident, 1);
    KT_EQ(m[2].resident, 0);

    /* The cursor: the first mapping that ends after `from`. */
    KT_EQ(aspace_get_maps(as, BASE + 8 * PG, m, 8), 2);   /* a ends exactly there: not it */
    KT_EQ(m[0].base, BASE + 10 * PG);
    KT_EQ(aspace_get_maps(as, BASE + 8 * PG - 1, m, 8), 3);
    KT_EQ(m[0].base, BASE);
    KT_EQ(aspace_get_maps(as, BASE + 12 * PG, m, 8), 1);
    KT_EQ(m[0].base, BASE + 14 * PG);
    KT_EQ(aspace_get_maps(as, BASE + 16 * PG, m, 8), 0);
    KT_EQ(aspace_get_maps(as, UINT64_MAX, m, 8), 0);

    /* The cap: pieces of the list that put together are the whole of it. */
    uint64_t from = 0, seen = 0;
    for (unsigned rounds = 0; rounds < 4; rounds++) {
        uint32_t n = aspace_get_maps(as, from, m, 1);
        if (!n)
            break;
        KT_EQ(n, 1);
        seen = seen * 16 + (m[0].base - BASE) / PG;
        from = m[0].base + m[0].len;
    }
    KT_EQ(seen, 0x0ae);   /* pages 0, 10 and 14, in order */

    /* A protect in the middle of a splits it three ways, the pages it
     * keeps still present; one to no access takes its count away. */
    KT_EQ(aspace_protect(as, BASE + 4 * PG, 2 * PG, ASPACE_READ), OK);
    KT_EQ(aspace_get_maps(as, 0, m, 8), 5);
    KT_EQ(m[0].len, 4 * PG);
    KT_EQ(m[0].resident, 1);   /* page 1 */
    KT_EQ(m[1].base, BASE + 4 * PG);
    KT_EQ(m[1].len, 2 * PG);
    KT_EQ(m[1].perms, VMAR_READ);
    KT_EQ(m[1].resident, 1);   /* page 5 */
    KT_EQ(m[1].vmo_off, 4 * PG);
    KT_EQ(m[2].base, BASE + 6 * PG);
    KT_EQ(m[2].perms, VMAR_READ | VMAR_WRITE);
    KT_EQ(aspace_protect(as, BASE, 4 * PG, 0), OK);
    KT_EQ(aspace_get_maps(as, 0, m, 8), 5);
    KT_EQ(m[0].perms, 0);
    KT_EQ(m[0].resident, 0);

    /* Unmapped: the rest stays, and a whole address space of nothing is 0. */
    KT_EQ(aspace_unmap(as, BASE + 4 * PG, 8 * PG), OK);
    KT_EQ(aspace_get_maps(as, 0, m, 8), 2);
    KT_EQ(m[0].base, BASE);
    KT_EQ(m[1].base, BASE + 14 * PG);
    KT_EQ(aspace_unmap(as, USER_BASE, USER_TOP - USER_BASE), OK);
    KT_EQ(aspace_get_maps(as, 0, m, 8), 0);

    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    kobject_unref(vmo_kobject(a));
    kobject_unref(vmo_kobject(b));
    kobject_unref(vmo_kobject(g));
}
