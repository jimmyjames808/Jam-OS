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
 *   - V8's and PartitionAlloc's patterns (mempat.c);
 *   - mmap's address hint, as Linux takes one: a free range is where the
 *     mapping goes (an unaligned hint rounded down; above 4 GiB, near the
 *     top of the user range; Chromium's zero segment, 64 KiB to 4,097 MiB,
 *     which must be free: posixtest is linked at 1 TiB, as Chromium's
 *     programs are, and placement is top down); one over a mapping
 *     (libc's or the program's), past
 *     the user range or wrapping is a mapping elsewhere;
 *     MAP_FIXED_NOREPLACE where something is is EEXIST, where nothing is
 *     exactly there;
 *   - the edges: MAP_FIXED_NOREPLACE inside a reservation, MAP_FIXED at
 *     its first and last pages, across two of libc's mappings, and
 *     reaching past its end; mprotect past its end;
 *   - the limits: a 64 TiB reservation, a shared anonymous mapping past
 *     64 GiB, ENOMEM past what a VMO or the address space holds.
 *
 * Nothing between a count and the check of it may allocate (printf can):
 * the counts are the job's, so they see every page the program takes. */
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <sys/mman.h>
#include <time.h>
#include "memres.h"
#include "pt.h"

/* Handle units charged to our job now: a handle each, and one for every
 * VMO object. */
uint64_t mr_job_handles(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_HANDLES] : 0;
}

uint32_t mr_mappings(void)
{
    struct process_info pi;
    return jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) == OK ? pi.mappings : 0;
}

uint64_t mr_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* len bytes of address space, no access, nothing committed: V8's and
 * PartitionAlloc's reservation. */
char *mr_reserve(uint64_t len)
{
    return mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

/* MAP_FIXED private anonymous memory at p, with prot. */
bool mr_fixed_anon(char *p, uint64_t len, int prot)
{
    return mmap(p, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) ==
           p;
}

/* A byte written in every page of [p, p + len): each page committed. */
void mr_touch(char *p, uint64_t len, char v)
{
    for (uint64_t o = 0; o < len; o += PG)
        ((volatile char *)p)[o] = v;
}

/* Does every page of [p, p + len) read zero at its start? (Reading
 * commits the pages.) */
bool mr_zero(const char *p, uint64_t len)
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
    char *r = mr_reserve(64 * MIB);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    bool rw = mprotect(r, 64 * MIB, PROT_READ | PROT_WRITE) == 0;
    CHECK(rw);
    if (rw)
        mr_touch(r + 16 * MIB, 4 * MIB, 1);
    uint64_t before = pt_job_pages();
    CHECK(munmap(r + 16 * MIB, 2 * MIB) == 0);
    CHECK(pt_job_pages() + 2 * MIB / PG <= before + SLACK);
    before = pt_job_pages();
    char *s = mmap(r + 18 * MIB, 2 * MIB, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(s == r + 18 * MIB);
    CHECK(pt_job_pages() + 2 * MIB / PG <= before + SLACK);
    CHECK(s == MAP_FAILED || mr_zero(s, 2 * MIB));
    CHECK(munmap(r, 64 * MIB) == 0);
}

/* A 16 GiB reservation: nothing committed by the reservation or by
 * mprotect, the pages a touch commits given back by MADV_DONTNEED and by
 * MAP_FIXED PROT_NONE, and MAP_FIXED pieces inside it (each over the
 * reservation's own VMO: no VMO made). */
static void check_reserve_cheap(void)
{
    uint64_t pages = pt_job_pages(), handles = mr_job_handles();
    char *r = mr_reserve(16 * GIB);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(mr_job_handles() <= handles + 2);   /* its VMO: the handle and the object */

    char *piece = r + 5 * GIB;
    uint64_t p0 = pt_job_pages();
    CHECK(mprotect(piece, 2 * MIB, PROT_READ | PROT_WRITE) == 0);
    CHECK(pt_job_pages() <= p0 + SLACK);   /* commits nothing */
    mr_touch(piece, 2 * MIB, 1);
    uint64_t p1 = pt_job_pages();
    CHECK(p1 >= p0 + 2 * MIB / PG && p1 <= p0 + 2 * MIB / PG + TABLES);
    CHECK(madvise(piece, 2 * MIB, MADV_DONTNEED) == 0);
    CHECK(pt_job_pages() + 2 * MIB / PG <= p1 + SLACK);
    CHECK(mr_zero(piece, 2 * MIB));
    mr_touch(piece, 2 * MIB, 2);
    p1 = pt_job_pages();
    CHECK(mr_fixed_anon(piece, 2 * MIB, PROT_NONE));
    CHECK(pt_job_pages() + 2 * MIB / PG <= p1 + SLACK);
    CHECK(mr_job_handles() <= handles + 2);   /* in place: no VMO made */
    CHECK(mprotect(piece, 2 * MIB, PROT_READ | PROT_WRITE) == 0 && mr_zero(piece, 2 * MIB));

    bool placed = true;
    for (unsigned i = 0; i < 64 && placed; i++) {
        char *q = r + 8 * GIB + i * 64 * MIB;
        placed = mr_fixed_anon(q, 64 * KIB, PROT_READ | PROT_WRITE);
        if (placed)
            q[0] = (char)(i + 1);
    }
    CHECK(placed);
    bool kept = placed;
    for (unsigned i = 0; i < 64 && kept; i++)
        kept = r[8 * GIB + i * 64 * MIB] == (char)(i + 1);
    CHECK(kept);
    CHECK(mr_job_handles() <= handles + 2);
    char *q = r + 8 * GIB;   /* over a piece written: fresh pages, read-only */
    CHECK(mr_fixed_anon(q, 64 * KIB, PROT_READ) && q[0] == 0);
    CHECK(munmap(r, 16 * GIB) == 0);
    CHECK(pt_job_pages() <= pages + SLACK);
    CHECK(mr_job_handles() <= handles);
}

/* check_reserve_edges' steps on r (16 pages, a hole after it): false at
 * a step that failed, since what follows would touch memory that isn't
 * there. */
static bool reserve_edges(char *r)
{
    errno = 0;
    CHECK(mmap(r + 4 * PG, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
               0) == MAP_FAILED && errno == EEXIST);
    if (!mr_fixed_anon(r, PG, PROT_READ | PROT_WRITE) ||
        !mr_fixed_anon(r + 15 * PG, PG, PROT_READ | PROT_WRITE))
        return false;
    CHECK(r[0] == 0 && r[15 * PG] == 0);
    r[0] = 1;
    r[15 * PG] = 2;
    errno = 0;
    CHECK(mprotect(r + 15 * PG, 2 * PG, PROT_READ) == -1 && errno == ENOMEM);   /* past its end */
    CHECK(r[0] == 1 && r[15 * PG] == 2);
    if (munmap(r + 12 * PG, 4 * PG) || !mr_fixed_anon(r + 8 * PG, 4 * PG, PROT_READ | PROT_WRITE))
        return false;
    mr_touch(r + 8 * PG, 4 * PG, 9);
    /* Over its end into the hole just made: a replace, with a VMO of its own. */
    if (!mr_fixed_anon(r + 10 * PG, 4 * PG, PROT_READ | PROT_WRITE))
        return false;
    CHECK(r[9 * PG] == 9 && mr_zero(r + 10 * PG, 4 * PG));
    mr_touch(r + 10 * PG, 4 * PG, 3);
    /* Across two of libc's mappings (two VMOs): in place in both. */
    uint64_t handles = mr_job_handles();
    if (!mr_fixed_anon(r + 9 * PG, 2 * PG, PROT_READ))
        return false;
    CHECK(mr_job_handles() <= handles);
    CHECK(r[8 * PG] == 9 && mr_zero(r + 9 * PG, 2 * PG) && r[11 * PG] == 3);
    return true;
}

/* MAP_FIXED and mprotect at a reservation's edges. The reservation is 16
 * pages with a hole of 4 after it (made from 20: whatever first fit puts
 * after a reservation must not be what mprotect reaches). */
static void check_reserve_edges(void)
{
    char *r = mr_reserve(20 * PG);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    bool hole = munmap(r + 16 * PG, 4 * PG) == 0;
    CHECK(hole);
    if (!hole) {
        (void)munmap(r, 20 * PG);
        return;
    }
    CHECK(reserve_edges(r));
    CHECK(munmap(r, 16 * PG) == 0);
}

/* Reservations as big as the address space allows (the kernel's sparse
 * big VMOs): 64 TiB, half the user half, costs no page and one mapping;
 * a piece committed at its far end works, and is given back with it; a
 * shared anonymous mapping past 64 GiB is made too. Refused with ENOMEM, Linux's answer when the address
 * space can't hold it (V8 and PartitionAlloc then reserve less): past a
 * VMO's most (VMO_MAX_SIZE, 128 TiB), and the whole user half (which
 * the address space can't hold: its first page and last pages aren't
 * the user's). */
static void check_reserve_limits(void)
{
    uint64_t pages = pt_job_pages(), huge = 64ul << 40;
    uint32_t maps = mr_mappings();
    char *r = mr_reserve(huge);
    CHECK(r != MAP_FAILED && pt_job_pages() <= pages + SLACK && mr_mappings() <= maps + 1);
    if (r != MAP_FAILED) {
        char *p = r + huge - 2 * MIB;
        bool rw = mr_fixed_anon(p, 2 * MIB, PROT_READ | PROT_WRITE);
        CHECK(rw);
        if (rw) {
            mr_touch(p, 2 * MIB, 4);
            CHECK(p[2 * MIB - PG] == 4 && pt_job_pages() >= pages + 2 * MIB / PG);
        }
        CHECK(munmap(r, huge) == 0);
        CHECK(pt_job_pages() <= pages + SLACK);
    }
    char *s = mmap(NULL, 64 * GIB + PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1,
                   0);
    CHECK(s != MAP_FAILED);
    if (s != MAP_FAILED) {
        s[64 * GIB] = 5;
        CHECK(s[64 * GIB] == 5 && munmap(s, 64 * GIB + PG) == 0);
    }
    errno = 0;
    CHECK(mr_reserve((1ul << 47) + PG) == MAP_FAILED && errno == ENOMEM);
    errno = 0;
    CHECK(mr_reserve(1ul << 47) == MAP_FAILED && errno == ENOMEM);
}

/* mmap of len bytes at a without MAP_FIXED: the hint. */
static char *hinted(uint64_t a, uint64_t len, int prot)
{
    return mmap((void *)a, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

/* A hint at a that nothing holds (MAP_FIXED_NOREPLACE finds it free, and
 * gives it back) is where the mapping goes; a that something holds is
 * skipped (the address space's layout is not this test's). */
static void hint_free_at(uint64_t a, uint64_t len)
{
    char *p = mmap((void *)a, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED)
        return;
    CHECK(p == (char *)a && munmap(p, len) == 0);
    p = hinted(a, len, PROT_READ | PROT_WRITE);
    CHECK(p == (char *)a);
    if (p != MAP_FAILED) {
        p[len - 1] = 6;
        CHECK(p[len - 1] == 6 && munmap(p, len) == 0);
    }
}

/* A hint that can't be taken: a mapping elsewhere, never a refusal. */
static void hint_elsewhere(uint64_t a, uint64_t len)
{
    char *p = hinted(a, len, PROT_READ | PROT_WRITE);
    CHECK(p != MAP_FAILED && p != (char *)a);
    if (p != MAP_FAILED) {
        p[0] = 7;
        CHECK(p[0] == 7 && munmap(p, len) == 0);
    }
}

extern char __ehdr_start[];   /* the program's first page (posix.ld) */

/* The end of PartitionAlloc's zero segment: Chromium 155's
 * USER_SPACE_ZERO_SEGMENT_SIZE_MB, 4,097 MiB, from 64 KiB. */
#define ZERO_END (4097 * MIB)

/* mmap's address hint (Linux's rule): a free one is where the mapping
 * goes, unaligned ones rounded down to their page; one over a mapping (of
 * libc's, or the program itself), past the user range or wrapping goes
 * elsewhere, and MAP_FIXED_NOREPLACE there is EEXIST. Above 4 GiB, near
 * the top of the user range, and Chromium's zero segment (64 KiB to
 * ZERO_END: all free in a program linked above it, as posixtest is; over
 * the program in one linked at 0x400000). */
static void check_hints(void)
{
    char *r = mr_reserve(64 * PG);   /* room known to be free once given back */
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    CHECK(munmap(r + 8 * PG, 16 * PG) == 0);
    char *p = hinted((uint64_t)r + 8 * PG, 16 * PG, PROT_READ | PROT_WRITE);
    CHECK(p == r + 8 * PG);   /* the hole, exactly */
    if (p != MAP_FAILED)
        CHECK(munmap(p, 16 * PG) == 0);
    p = hinted((uint64_t)r + 8 * PG + 123, 4 * PG, PROT_READ | PROT_WRITE);
    CHECK(p == r + 8 * PG);   /* rounded down to its page */
    if (p != MAP_FAILED)
        CHECK(munmap(p, 4 * PG) == 0);
    hint_elsewhere((uint64_t)r + 4 * PG, 8 * PG);   /* half over the reservation */
    hint_elsewhere((uint64_t)r, PG);
    errno = 0;
    CHECK(mmap(r, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
              MAP_FAILED && errno == EEXIST);
    p = mmap(r + 8 * PG, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
             0);
    CHECK(p == r + 8 * PG);   /* NOREPLACE where nothing is: exactly there */
    CHECK(munmap(r, 64 * PG) == 0);

    uint64_t image = (uint64_t)__ehdr_start;   /* not libc's: the kernel says it's taken */
    hint_elsewhere(image, PG);
    errno = 0;
    CHECK(mmap(__ehdr_start, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
               -1, 0) == MAP_FAILED && errno == EEXIST);
    uint64_t top = 0x00007fffffffe000ul;   /* the user range's end (the kernel's USER_TOP) */
    hint_elsewhere(top, PG);
    hint_elsewhere(top - PG, 2 * PG);      /* reaching past it */
    hint_elsewhere(0xffff800000000000ul, PG);
    hint_elsewhere(~0ul - PG + 1, 2 * PG);   /* wrapping */
    hint_free_at(top - 48 * GIB, 16 * PG);   /* above the stack (STACK_TOP, libjam's spawn.c) */
    hint_free_at(ZERO_END + 64 * KIB, 16 * PG);
    if (image < ZERO_END) {
        hint_elsewhere(64 * KIB, ZERO_END - 64 * KIB);   /* over the program */
        return;
    }
    /* Linked at 1 TiB (posix.mk), as Chromium's programs are: nothing is
     * placed below it (placement is top down: first fit from the user
     * range's first page put the first mappings at 0x1000), so the first
     * pages are free and PartitionAlloc's zero segment is made. */
    char *low = mmap((void *)PG, 64 * KIB - PG, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(low == (char *)PG);
    if (low != MAP_FAILED)
        CHECK(munmap(low, 64 * KIB - PG) == 0);
    char *z = hinted(64 * KIB, ZERO_END - 64 * KIB, PROT_NONE);
    CHECK(z == (char *)(64 * KIB));
    if (z != MAP_FAILED)
        CHECK(munmap(z, ZERO_END - 64 * KIB) == 0);
}

void pt_mem_reserve(void)
{
    check_hints();
    check_reserve_limits();
    check_drop_gives_back();
    check_reserve_cheap();
    pt_mem_patterns();
    check_reserve_edges();
}
