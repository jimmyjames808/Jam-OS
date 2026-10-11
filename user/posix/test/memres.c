/* posixtest's mem section, its second half (mem.c runs it): big sparse
 * reservations, the way V8 and PartitionAlloc use the address space
 * (docs/CR-PLAN.md section 2). Pages are counted on the job
 * (pt_job_pages), and VMOs on its handle count, which holds a unit for
 * every VMO object besides the handle itself.
 *
 *   - a munmap or a MAP_FIXED over part of private anonymous memory gives
 *     that part's pages back at once, though the rest still maps the VMO;
 *   - a 16 GiB PROT_NONE reservation costs no page; mprotect commits
 *     nothing until touched; MADV_DONTNEED and MAP_FIXED PROT_NONE give
 *     the pages back; MAP_FIXED pieces inside it make no VMO;
 *   - V8's pattern: 2,048 pieces of 256 KiB committed by mprotect at
 *     scattered places in a 32 GiB reservation (V8's sandbox when the
 *     1 TiB one is refused), a byte written in each, half decommitted by
 *     MAP_FIXED PROT_NONE and half by MADV_DONTNEED, then committed
 *     again: the pages follow, no VMO is made, and the time a piece takes
 *     is printed for the first quarter and the last (it grows with the
 *     kernel's mapping count until its mappings merge: docs/CR-PLAN.md's
 *     CK2; 2,048 pieces stay well inside today's 16,384 mappings);
 *   - PartitionAlloc's: a 16 GiB pool aligned to its size (twice that
 *     reserved, head and tail given back), 64 super pages of 2 MiB each
 *     with a metadata page and eight 64 KiB slot spans committed, the odd
 *     spans decommitted (no access, then MADV_DONTNEED) and committed
 *     again, then every super page freed by MAP_FIXED PROT_NONE;
 *   - the edges: MAP_FIXED_NOREPLACE inside a reservation, MAP_FIXED at
 *     its first and last pages, across two of libc's mappings, and
 *     reaching past its end; mprotect past its end.
 *
 * Nothing between a count and the check of it may allocate (printf can):
 * the counts are the job's, so they see every page the program takes. */
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <sys/mman.h>
#include <time.h>
#include "pt.h"

#define PG     4096ul
#define KIB    1024ul
#define MIB    (1ul << 20)
#define GIB    (1ul << 30)
#define SLACK  4ul   /* pages: a page of mapping records, a page table made or freed */
#define TABLES 12ul  /* pages: the tables a first touch in a new GiB may make (VMO, address space) */

#define V8_RESERVE (32 * GIB)
#define V8_PIECES  2048u   /* each in a 16 MiB slot of its own */
#define V8_PIECE   (256 * KIB)

#define PA_POOL   (16 * GIB)
#define PA_SUPER  (2 * MIB)
#define PA_SUPERS 64u
#define PA_FIRST  (16 * KIB)   /* the first slot span, after the first partition page */
#define PA_SPAN   (64 * KIB)
#define PA_SPANS  8u

/* Handle units charged to our job now: a handle each, and one for every
 * VMO object. */
static uint64_t job_handles(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_HANDLES] : 0;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* len bytes of address space, no access, nothing committed: V8's and
 * PartitionAlloc's reservation. */
static char *reserve(uint64_t len)
{
    return mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

/* MAP_FIXED private anonymous memory at p, with prot. */
static bool fixed_anon(char *p, uint64_t len, int prot)
{
    return mmap(p, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) ==
           p;
}

/* A byte written in every page of [p, p + len): each page committed. */
static void touch(char *p, uint64_t len, char v)
{
    for (uint64_t o = 0; o < len; o += PG)
        ((volatile char *)p)[o] = v;
}

/* Does every page of [p, p + len) read zero at its start? (Reading
 * commits the pages.) */
static bool zero(const char *p, uint64_t len)
{
    bool z = true;
    for (uint64_t o = 0; o < len; o += PG)
        z &= ((const volatile char *)p)[o] == 0;
    return z;
}

/* A hole unmapped in the middle of a mapping, and a MAP_SHARED mapping
 * put over another part: their pages are back on the job at once. */
static void check_drop_gives_back(void)
{
    char *r = reserve(64 * MIB);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    CHECK(mprotect(r, 64 * MIB, PROT_READ | PROT_WRITE) == 0);
    touch(r + 16 * MIB, 4 * MIB, 1);
    uint64_t before = pt_job_pages();
    CHECK(munmap(r + 16 * MIB, 2 * MIB) == 0);
    CHECK(pt_job_pages() + 2 * MIB / PG <= before + SLACK);
    before = pt_job_pages();
    char *s = mmap(r + 18 * MIB, 2 * MIB, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(s == r + 18 * MIB);
    CHECK(pt_job_pages() + 2 * MIB / PG <= before + SLACK);
    CHECK(s == MAP_FAILED || zero(s, 2 * MIB));
    CHECK(munmap(r, 64 * MIB) == 0);
}

/* A 16 GiB reservation: nothing committed by the reservation or by
 * mprotect, the pages a touch commits given back by MADV_DONTNEED and by
 * MAP_FIXED PROT_NONE, and MAP_FIXED pieces inside it (each over the
 * reservation's own VMO: no VMO made). */
static void check_reserve_cheap(void)
{
    uint64_t pages = pt_job_pages(), handles = job_handles();
    char *r = reserve(16 * GIB);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(job_handles() <= handles + 2);   /* its VMO: the handle and the object */

    char *piece = r + 5 * GIB;
    uint64_t p0 = pt_job_pages();
    CHECK(mprotect(piece, 2 * MIB, PROT_READ | PROT_WRITE) == 0);
    CHECK(pt_job_pages() <= p0 + SLACK);   /* commits nothing */
    touch(piece, 2 * MIB, 1);
    uint64_t p1 = pt_job_pages();
    CHECK(p1 >= p0 + 2 * MIB / PG && p1 <= p0 + 2 * MIB / PG + TABLES);
    CHECK(madvise(piece, 2 * MIB, MADV_DONTNEED) == 0);
    CHECK(pt_job_pages() + 2 * MIB / PG <= p1 + SLACK);
    CHECK(zero(piece, 2 * MIB));
    touch(piece, 2 * MIB, 2);
    p1 = pt_job_pages();
    CHECK(fixed_anon(piece, 2 * MIB, PROT_NONE));
    CHECK(pt_job_pages() + 2 * MIB / PG <= p1 + SLACK);
    CHECK(job_handles() <= handles + 2);   /* in place: no VMO made */
    CHECK(mprotect(piece, 2 * MIB, PROT_READ | PROT_WRITE) == 0 && zero(piece, 2 * MIB));

    bool placed = true;
    for (unsigned i = 0; i < 64 && placed; i++) {
        char *q = r + 8 * GIB + i * 64 * MIB;
        placed = fixed_anon(q, 64 * KIB, PROT_READ | PROT_WRITE);
        if (placed)
            q[0] = (char)(i + 1);
    }
    CHECK(placed);
    bool kept = placed;
    for (unsigned i = 0; i < 64 && kept; i++)
        kept = r[8 * GIB + i * 64 * MIB] == (char)(i + 1);
    CHECK(kept);
    CHECK(job_handles() <= handles + 2);
    char *q = r + 8 * GIB;   /* over a piece written: fresh pages, read-only */
    CHECK(fixed_anon(q, 64 * KIB, PROT_READ) && q[0] == 0);
    CHECK(munmap(r, 16 * GIB) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(job_handles() <= handles);
}

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
    uint64_t t = now_ns();
    for (unsigned i = 0; i < V8_PIECES; i++) {
        char *p = v8_piece(r, i);
        if (mprotect(p, V8_PIECE, PROT_READ | PROT_WRITE))
            return false;
        p[V8_PIECE - 1] = (char)(i | 1);
        if (i == V8_PIECES / 4 - 1)
            ns[0] = now_ns() - t;
        if (i == V8_PIECES - V8_PIECES / 4 - 1)
            t = now_ns();
    }
    ns[1] = now_ns() - t;
    return true;
}

/* V8's decommit: even pieces by MAP_FIXED PROT_NONE (its DecommitPages),
 * odd ones by MADV_DONTNEED (its DiscardSystemPages). */
static bool v8_decommit(char *r)
{
    bool ok = true;
    for (unsigned i = 0; i < V8_PIECES && ok; i++) {
        char *p = v8_piece(r, i);
        ok = i % 2 ? madvise(p, V8_PIECE, MADV_DONTNEED) == 0 : fixed_anon(p, V8_PIECE, PROT_NONE);
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
    uint64_t pages = pt_job_pages(), handles = job_handles(), ns[2] = { 0, 0 };
    char *r = reserve(V8_RESERVE);
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
    uint64_t t = now_ns();
    ok = ok && v8_decommit(r);
    uint64_t decommit_ns = now_ns() - t;
    CHECK(ok);
    CHECK(pt_job_pages() + V8_PIECES <= p1 + SLACK);
    CHECK(job_handles() <= handles + 2);   /* in place: no VMO made */
    CHECK(ok && v8_recommit(r));
    CHECK(pt_job_pages() <= p1 + SLACK);
    CHECK(munmap(r, V8_RESERVE) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(job_handles() <= handles);
    printf("posixtest: mem: V8's pattern, %u pieces: commit %lu ns a piece in the first "
           "quarter, %lu in the last; decommit %lu ns a piece\n", V8_PIECES,
           (unsigned long)(ns[0] / (V8_PIECES / 4)), (unsigned long)(ns[1] / (V8_PIECES / 4)),
           (unsigned long)(decommit_ns / V8_PIECES));
}

/* A PA_POOL pool aligned to its size, as PartitionAlloc reserves one:
 * twice the size reserved, the head and the tail given back. */
static char *pa_pool(void)
{
    char *big = reserve(2 * PA_POOL);
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
        touch(span, PA_SPAN, (char)(k + 1));
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
        ok = mprotect(span, PA_SPAN, PROT_READ | PROT_WRITE) == 0 && zero(span, PA_SPAN);
        if (ok)
            touch(span, PA_SPAN, (char)(k + 1));
    }
    return ok;
}

static void check_pa_pattern(void)
{
    uint64_t pages = pt_job_pages(), handles = job_handles();
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
    CHECK(pt_job_pages() + spans <= p1 + SLACK);
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)
        ok = pa_odd_spans(pool, s, true) && pool[s * PA_SUPER + PG] == (char)s &&
             pool[s * PA_SUPER + PA_FIRST + 2 * PA_SPAN] == 3;
    CHECK(ok);
    CHECK(pt_job_pages() <= p1 + SLACK);
    for (unsigned s = 0; s < PA_SUPERS && ok; s++)   /* freed: its DecommitAndZeroSystemPages */
        ok = fixed_anon(pool + s * PA_SUPER, PA_SUPER, PROT_NONE);
    CHECK(ok);
    CHECK(pt_job_pages() <= pages + PA_SUPERS * 3 + SLACK);   /* tables and records stay */
    CHECK(job_handles() <= handles + 2);   /* in place: no VMO made */
    char *span = pool + PA_FIRST;   /* a freed super page used again */
    CHECK(mprotect(span, PA_SPAN, PROT_READ | PROT_WRITE) == 0 && zero(span, PA_SPAN));
    CHECK(munmap(pool, PA_POOL) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(job_handles() <= handles);
}

/* MAP_FIXED and mprotect at a reservation's edges. The reservation is 16
 * pages with a hole of 4 after it (made from 20: whatever first fit puts
 * after a reservation must not be what mprotect reaches). */
static void check_reserve_edges(void)
{
    char *r = reserve(20 * PG);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    bool hole = munmap(r + 16 * PG, 4 * PG) == 0;
    CHECK(hole);
    if (!hole) {
        (void)munmap(r, 20 * PG);
        return;
    }
    errno = 0;
    CHECK(mmap(r + 4 * PG, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
               0) == MAP_FAILED && errno == EEXIST);
    CHECK(fixed_anon(r, PG, PROT_READ | PROT_WRITE) && r[0] == 0);
    r[0] = 1;
    CHECK(fixed_anon(r + 15 * PG, PG, PROT_READ | PROT_WRITE) && r[15 * PG] == 0);
    r[15 * PG] = 2;
    CHECK(r[0] == 1);
    errno = 0;
    CHECK(mprotect(r + 15 * PG, 2 * PG, PROT_READ) == -1 && errno == ENOMEM);   /* past its end */
    CHECK(munmap(r + 12 * PG, 4 * PG) == 0);
    CHECK(fixed_anon(r + 8 * PG, 4 * PG, PROT_READ | PROT_WRITE));
    touch(r + 8 * PG, 4 * PG, 9);
    /* Over its end into the hole just made: a replace, with a VMO of its own. */
    CHECK(fixed_anon(r + 10 * PG, 4 * PG, PROT_READ | PROT_WRITE));
    CHECK(r[9 * PG] == 9 && zero(r + 10 * PG, 4 * PG));
    touch(r + 10 * PG, 4 * PG, 3);
    /* Across two of libc's mappings (two VMOs): in place in both. */
    uint64_t handles = job_handles();
    CHECK(fixed_anon(r + 9 * PG, 2 * PG, PROT_READ));
    CHECK(job_handles() == handles);
    CHECK(r[8 * PG] == 9 && zero(r + 9 * PG, 2 * PG) && r[11 * PG] == 3);
    CHECK(munmap(r, 16 * PG) == 0);
}

void pt_mem_reserve(void)
{
    check_drop_gives_back();
    check_reserve_cheap();
    check_v8_pattern();
    check_pa_pattern();
    check_reserve_edges();
}
