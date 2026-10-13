/* posixtest's mem section, V8's and PartitionAlloc's patterns over big
 * sparse reservations (memres.c runs them; docs/CR-PLAN.md section 2).
 * Pages are counted on the job (pt_job_pages), VMOs on its handle count.
 *
 *   - V8's pattern: 2,048 pieces of 256 KiB committed by mprotect at
 *     scattered places in a 32 GiB reservation (V8's sandbox when the
 *     1 TiB one is refused), a byte written in each, half decommitted by
 *     MAP_FIXED PROT_NONE and half by MADV_DONTNEED, then committed
 *     again: the pages follow, no VMO is made, and the time a piece takes
 *     is printed for the first quarter and the last (the kernel's
 *     mapping tree keeps them close);
 *   - PartitionAlloc's: a 16 GiB pool aligned to its size (twice that
 *     reserved, head and tail given back), 64 super pages of 2 MiB each
 *     with a metadata page and eight 64 KiB slot spans committed, the odd
 *     spans decommitted (no access, then MADV_DONTNEED) and committed
 *     again, then every super page freed by MAP_FIXED PROT_NONE.
 *
 * Nothing between a count and the check of it may allocate (printf can). */
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include "memres.h"
#include "pt.h"

#define V8_RESERVE (32 * GIB)
#define V8_PIECES  2048u   /* each in a 16 MiB slot of its own */
#define V8_PIECE   (256 * KIB)

#define PA_POOL   (16 * GIB)
#define PA_SUPER  (2 * MIB)
#define PA_SUPERS 64u
#define PA_FIRST  (16 * KIB)   /* the first slot span, after the first partition page */
#define PA_SPAN   (64 * KIB)
#define PA_SPANS  8u

/* V8's piece i: its slot in the reservation, scattered (1237 is odd, so
 * i * 1237 runs through every slot once). */
static char *v8_piece(char *r, unsigned i)
{
    return r + (uint64_t)((i * 1237u) % V8_PIECES) * (V8_RESERVE / V8_PIECES);
}

/* V8's commit: every piece made read-write and a byte written in its
 * last page. The ns the first quarter and the last took go to ns[0] and
 * ns[1]. */
static bool v8_commit(char *r, uint64_t ns[2])
{
    uint64_t t = mr_now_ns();
    for (unsigned i = 0; i < V8_PIECES; i++) {
        char *p = v8_piece(r, i);
        if (mprotect(p, V8_PIECE, PROT_READ | PROT_WRITE))
            return false;
        p[V8_PIECE - 1] = (char)(i | 1);
        if (i == V8_PIECES / 4 - 1)
            ns[0] = mr_now_ns() - t;
        if (i == V8_PIECES - V8_PIECES / 4 - 1)
            t = mr_now_ns();
    }
    ns[1] = mr_now_ns() - t;
    return true;
}

/* V8's decommit: even pieces by MAP_FIXED PROT_NONE (its DecommitPages),
 * odd ones by MADV_DONTNEED (its DiscardSystemPages). */
static bool v8_decommit(char *r)
{
    bool ok = true;
    for (unsigned i = 0; i < V8_PIECES && ok; i++) {
        char *p = v8_piece(r, i);
        ok = i % 2 ? madvise(p, V8_PIECE, MADV_DONTNEED) == 0 : mr_fixed_anon(p, V8_PIECE, PROT_NONE);
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

static void check_v8_pattern(void)
{
    uint64_t pages = pt_job_pages(), handles = mr_job_handles(), ns[2] = { 0, 0 };
    char *r = mr_reserve(V8_RESERVE);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    bool ok = v8_commit(r, ns);
    CHECK(ok);
    uint64_t p1 = pt_job_pages();
    CHECK(p1 >= pages + V8_PIECES);
    bool same = ok;
    for (unsigned i = 0; i < V8_PIECES && same; i++)
        same = v8_piece(r, i)[V8_PIECE - 1] == (char)(i | 1);
    CHECK(same);
    uint64_t t = mr_now_ns();
    ok = ok && v8_decommit(r);
    uint64_t decommit_ns = mr_now_ns() - t;
    CHECK(ok);
    CHECK(pt_job_pages() + V8_PIECES <= p1 + SLACK);
    CHECK(mr_job_handles() <= handles + 2);   /* in place: no VMO made */
    CHECK(ok && v8_recommit(r));
    CHECK(pt_job_pages() <= p1 + SLACK);
    CHECK(munmap(r, V8_RESERVE) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(mr_job_handles() <= handles);
    printf("posixtest: mem: V8's pattern, %u pieces: commit %lu ns a piece in the first "
           "quarter, %lu in the last; decommit %lu ns a piece\n", V8_PIECES,
           (unsigned long)(ns[0] / (V8_PIECES / 4)), (unsigned long)(ns[1] / (V8_PIECES / 4)),
           (unsigned long)(decommit_ns / V8_PIECES));
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

static void check_pa_pattern(void)
{
    uint64_t pages = pt_job_pages(), handles = mr_job_handles();
    char *pool = pa_pool();
    CHECK(pool != MAP_FAILED);
    if (pool == MAP_FAILED)
        return;
    CHECK(pt_job_pages() <= pages + SLACK);
    bool ok = true;
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)
        ok = pa_super_commit(pool, s);
    CHECK(ok);
    uint64_t p1 = pt_job_pages(), spans = PA_SUPERS * (PA_SPANS / 2) * (PA_SPAN / PG);
    CHECK(p1 >= pages + PA_SUPERS * (1 + PA_SPANS * PA_SPAN / PG));
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
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)   /* freed: its DecommitAndZeroSystemPages */
        ok = mr_fixed_anon(pool + s * PA_SUPER, PA_SUPER, PROT_NONE);
    CHECK(ok);
    CHECK(pt_job_pages() <= pages + PA_SUPERS * 3 + SLACK);   /* tables and records stay */
    CHECK(mr_job_handles() <= handles + 2);   /* in place: no VMO made */
    char *span = pool + PA_FIRST;   /* a freed super page used again */
    CHECK(mprotect(span, PA_SPAN, PROT_READ | PROT_WRITE) == 0 && mr_zero(span, PA_SPAN));
    CHECK(munmap(pool, PA_POOL) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(mr_job_handles() <= handles);
}

void pt_mem_patterns(void)
{
    check_v8_pattern();
    check_pa_pattern();
}
