/* posixtest's mem section, V8's and PartitionAlloc's patterns over big
 * sparse reservations, at their real sizes (memres.c runs them;
 * docs/CR-PLAN.md section 2). Pages are counted on the job
 * (pt_job_pages), VMOs on its handle count, mappings on process_info.
 *
 *   - V8's pattern: its sandbox, 1 TiB with 32 GiB of guards, reserved;
 *     10,000 pieces of 256 KiB committed by mprotect at scattered places
 *     in it, a byte written in each; half decommitted by MAP_FIXED
 *     PROT_NONE and half by MADV_DONTNEED; all committed again, then all
 *     released by MAP_FIXED PROT_NONE. The pages follow every step, no
 *     VMO is made, and the mapping count follows what is committed: each
 *     piece taken back merges into the reservation again, so after the
 *     release the sandbox is one mapping. A commit's time (the median of
 *     the first 1,000 against the last 1,000, so a busy host's moments
 *     don't count) stays flat as the count grows to 20,000.
 *   - PartitionAlloc's: its regular and BackupRef pools, 16 GiB each,
 *     each aligned to its size (twice that reserved, head and tail given
 *     back); 64 super pages of 2 MiB in each with a metadata page and
 *     eight 64 KiB slot spans committed, the odd spans decommitted (no
 *     access, then MADV_DONTNEED) and committed again; a direct-map
 *     allocation of 5 MiB in 3 super pages; then every super page freed
 *     by MAP_FIXED PROT_NONE, each pool one mapping again.
 *
 * Nothing between a count and the check of it may allocate (printf can). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "memres.h"
#include "pt.h"

#define MAP_SLACK 2u   /* mappings another thread of ours may make meanwhile */

#define V8_CAGE   (1024 * GIB)
#define V8_GUARDS (32 * GIB)
#define V8_SLOTS  16384u   /* 64 MiB each */
#define V8_PIECES 10000u
#define V8_PIECE  (256 * KIB)
#define V8_WINDOW 1000u    /* pieces in a median of commit times */

#define PA_POOL    (16 * GIB)
#define PA_SUPER   (2 * MIB)
#define PA_SUPERS  64u
#define PA_FIRST   (16 * KIB)   /* the first slot span, after the first partition page */
#define PA_SPAN    (64 * KIB)
#define PA_SPANS   8u
#define PA_DIRECT  (5 * MIB)    /* a direct-map allocation, after the slot spans' super pages */

static uint32_t v8_ns[V8_PIECES];   /* each piece's commit time */
static uint32_t median_buf[V8_WINDOW];

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* The median of t[0 .. V8_WINDOW). */
static uint32_t median(const uint32_t *t)
{
    for (unsigned i = 0; i < V8_WINDOW; i++)
        median_buf[i] = t[i];
    qsort(median_buf, V8_WINDOW, sizeof(median_buf[0]), cmp_u32);
    return median_buf[V8_WINDOW / 2];
}

/* V8's piece i: its slot in the cage, scattered (10007 is odd, so
 * i * 10007 runs through every slot once). */
static char *v8_piece(char *r, unsigned i)
{
    return r + (uint64_t)((i * 10007u) % V8_SLOTS) * (V8_CAGE / V8_SLOTS);
}

/* V8's commit: every piece made read-write and a byte written in its
 * last page, each one's time in v8_ns. */
static bool v8_commit(char *r)
{
    for (unsigned i = 0; i < V8_PIECES; i++) {
        char *p = v8_piece(r, i);
        uint64_t t = mr_now_ns();
        if (mprotect(p, V8_PIECE, PROT_READ | PROT_WRITE))
            return false;
        p[V8_PIECE - 1] = (char)(i | 1);
        v8_ns[i] = (uint32_t)(mr_now_ns() - t);
    }
    return true;
}

/* V8's decommit: even pieces by MAP_FIXED PROT_NONE (its DecommitPages),
 * odd ones by MADV_DONTNEED (its DiscardSystemPages); or, with all, every
 * piece by MAP_FIXED PROT_NONE (the release). */
static bool v8_decommit(char *r, bool all)
{
    bool ok = true;
    for (unsigned i = 0; i < V8_PIECES && ok; i++) {
        char *p = v8_piece(r, i);
        ok = i % 2 && !all ? madvise(p, V8_PIECE, MADV_DONTNEED) == 0
                           : mr_fixed_anon(p, V8_PIECE, PROT_NONE);
    }
    return ok;
}

/* Every piece back: an even one made read-write again; each reads zero
 * where its byte was, and gets the byte again. */
static bool v8_recommit(char *r)
{
    bool ok = true;
    for (unsigned i = 0; i < V8_PIECES && ok; i++) {
        char *p = v8_piece(r, i);
        if (!(i % 2))
            ok = mprotect(p, V8_PIECE, PROT_READ | PROT_WRITE) == 0;
        ok = ok && p[V8_PIECE - 1] == 0;
        if (ok)
            p[V8_PIECE - 1] = (char)(i | 1);
    }
    return ok;
}

/* V8's pattern's counts after its commit: pages, then mappings. */
static void v8_check_committed(char *r, uint64_t pages, uint32_t maps)
{
    CHECK(pt_job_pages() >= pages + V8_PIECES);
    CHECK(mr_mappings() <= maps + 1 + 2 * V8_PIECES + MAP_SLACK);
    bool same = true;
    for (unsigned i = 0; i < V8_PIECES && same; i++)
        same = v8_piece(r, i)[V8_PIECE - 1] == (char)(i | 1);
    CHECK(same);
}

static void check_v8_pattern(void)
{
    uint64_t pages = pt_job_pages(), handles = mr_job_handles();
    uint32_t maps = mr_mappings();
    char *r = mr_reserve(V8_CAGE + V8_GUARDS);
    CHECK(r != MAP_FAILED && pt_job_pages() <= pages + SLACK);
    if (r == MAP_FAILED)
        return;
    bool ok = v8_commit(r);
    CHECK(ok);
    uint64_t p1 = pt_job_pages();
    uint32_t m1 = mr_mappings();
    if (ok)
        v8_check_committed(r, pages, maps);
    uint64_t t = mr_now_ns();
    ok = ok && v8_decommit(r, false);
    uint64_t decommit_ns = mr_now_ns() - t;
    CHECK(ok);
    CHECK(pt_job_pages() + V8_PIECES <= p1 + SLACK);
    uint32_t m2 = mr_mappings();   /* the even pieces merged back */
    CHECK(m2 <= maps + 1 + V8_PIECES + MAP_SLACK);
    CHECK(mr_job_handles() <= handles + 2);   /* in place: no VMO made */
    CHECK(ok && v8_recommit(r));
    CHECK(pt_job_pages() <= p1 + SLACK);
    ok = ok && v8_decommit(r, true);
    CHECK(ok);
    /* Released: one mapping again; only the VMO's tables stay (a leaf
     * for each piece, a table for each GiB and above). */
    CHECK(mr_mappings() <= maps + 1 + MAP_SLACK);
    CHECK(pt_job_pages() <= pages + V8_PIECES + V8_CAGE / GIB + 64);
    CHECK(munmap(r, V8_CAGE + V8_GUARDS) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(mr_job_handles() <= handles);
    CHECK(mr_mappings() <= maps + MAP_SLACK);
    uint32_t first = median(v8_ns), last = median(v8_ns + V8_PIECES - V8_WINDOW);
    CHECK(last <= 2 * first + 2000);   /* flat as the mappings grow */
    printf("posixtest: mem: V8's pattern, %u pieces in a 1 TiB cage: commit %u ns a piece (median) "
           "for the first %u, %u for the last; decommit %lu ns a piece; mappings %u, %u "
           "committed, %u half decommitted\n", V8_PIECES, (unsigned)first, V8_WINDOW,
           (unsigned)last, (unsigned long)(decommit_ns / V8_PIECES), (unsigned)maps,
           (unsigned)m1, (unsigned)m2);
}

/* A PA_POOL pool aligned to its size, as PartitionAlloc reserves one:
 * twice the size reserved, the head and the tail given back. */
static char *pa_pool(void)
{
    char *big = mr_reserve(2 * PA_POOL);
    if (big == MAP_FAILED)
        return big;
    uint64_t b = (uint64_t)(uintptr_t)big;
    uint64_t a = (b + PA_POOL - 1) & ~(PA_POOL - 1);
    bool ok = (a == b || munmap(big, a - b) == 0) &&
              munmap((char *)(uintptr_t)(a + PA_POOL), b + 2 * PA_POOL - (a + PA_POOL)) == 0;
    return ok ? (char *)(uintptr_t)a : MAP_FAILED;
}

/* Super page s's metadata page (after a guard page) and its slot spans
 * made read-write, each page of span k written k + 1. */
static bool pa_super_commit(char *pool, unsigned s)
{
    char *sp = pool + s * PA_SUPER;
    if (mprotect(sp + PG, PG, PROT_READ | PROT_WRITE))
        return false;
    sp[PG] = (char)s;
    for (unsigned k = 0; k < PA_SPANS; k++) {
        char *span = sp + PA_FIRST + k * PA_SPAN;
        if (mprotect(span, PA_SPAN, PROT_READ | PROT_WRITE))
            return false;
        mr_touch(span, PA_SPAN, (char)(k + 1));
    }
    return true;
}

/* Super page s's odd spans decommitted as PartitionAlloc does (no
 * access, then discarded); or, again, committed, checked zero and
 * written. */
static bool pa_odd_spans(char *pool, unsigned s, bool again)
{
    bool ok = true;
    for (unsigned k = 1; k < PA_SPANS && ok; k += 2) {
        char *span = pool + s * PA_SUPER + PA_FIRST + k * PA_SPAN;
        if (!again) {
            ok = mprotect(span, PA_SPAN, PROT_NONE) == 0 &&
                 madvise(span, PA_SPAN, MADV_DONTNEED) == 0;
            continue;
        }
        ok = mprotect(span, PA_SPAN, PROT_READ | PROT_WRITE) == 0 && mr_zero(span, PA_SPAN);
        if (ok)
            mr_touch(span, PA_SPAN, (char)(k + 1));
    }
    return ok;
}

/* A direct-map allocation in pool: PA_DIRECT bytes after a guard page,
 * in the super pages after the slot spans', committed and written. */
static bool pa_direct(char *pool)
{
    char *d = pool + PA_SUPERS * PA_SUPER + PG;
    if (mprotect(d, PA_DIRECT, PROT_READ | PROT_WRITE))
        return false;
    mr_touch(d, PA_DIRECT, 7);
    return d[PA_DIRECT - PG] == 7;
}

/* One pool's life: its super pages and a direct map committed, the odd
 * spans decommitted and committed again, then everything freed by
 * MAP_FIXED PROT_NONE (DecommitAndZeroSystemPages). */
static void pa_pool_cycle(char *pool)
{
    uint64_t pages = pt_job_pages();
    bool ok = true;
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)
        ok = pa_super_commit(pool, s);
    ok = ok && pa_direct(pool);
    CHECK(ok);
    uint64_t p1 = pt_job_pages(), spans = PA_SUPERS * (PA_SPANS / 2) * (PA_SPAN / PG);
    CHECK(p1 >= pages + PA_SUPERS * (1 + PA_SPANS * PA_SPAN / PG) + PA_DIRECT / PG);
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)
        ok = pa_odd_spans(pool, s, false);
    CHECK(ok);
    /* The kernel merged each super page's spans into one read-write
     * mapping; taking access from the odd ones splits it again, two more
     * mappings a span, charged a page per 16. */
    uint64_t splits = (2 * PA_SUPERS * (PA_SPANS / 2) + 15) / 16;
    CHECK(pt_job_pages() + spans <= p1 + splits + SLACK);
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)
        ok = pa_odd_spans(pool, s, true) && pool[s * PA_SUPER + PG] == (char)s &&
             pool[s * PA_SUPER + PA_FIRST + 2 * PA_SPAN] == 3;
    CHECK(ok);
    CHECK(pt_job_pages() <= p1 + SLACK);
    for (unsigned s = 0; s < PA_SUPERS + 3 && ok; s++)
        ok = mr_fixed_anon(pool + s * PA_SUPER, PA_SUPER, PROT_NONE);
    CHECK(ok);
    CHECK(pt_job_pages() <= pages + (PA_SUPERS + 3) * 3 + SLACK);   /* the VMO's tables stay */
    char *span = pool + PA_FIRST;   /* a freed super page used again */
    CHECK(ok && mprotect(span, PA_SPAN, PROT_READ | PROT_WRITE) == 0 && mr_zero(span, PA_SPAN));
    CHECK(ok && mr_fixed_anon(span, PA_SPAN, PROT_NONE));
}

static void check_pa_pattern(void)
{
    uint64_t pages = pt_job_pages(), handles = mr_job_handles();
    uint32_t maps = mr_mappings();
    char *pool[2] = { pa_pool(), pa_pool() };   /* the regular pool, the BackupRef pool */
    CHECK(pool[0] != MAP_FAILED && pool[1] != MAP_FAILED);
    CHECK(pt_job_pages() <= pages + SLACK);
    for (unsigned i = 0; i < 2; i++)
        if (pool[i] != MAP_FAILED)
            pa_pool_cycle(pool[i]);
    CHECK(mr_mappings() <= maps + 2 + MAP_SLACK);   /* each pool one mapping again */
    CHECK(mr_job_handles() <= handles + 4);   /* in place: no VMO made */
    for (unsigned i = 0; i < 2; i++)
        CHECK(pool[i] == MAP_FAILED || munmap(pool[i], PA_POOL) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(mr_job_handles() <= handles);
}

void pt_mem_patterns(void)
{
    check_v8_pattern();
    check_pa_pattern();
}
