/* posixtest's mem section, its second half (mem.c runs it): big sparse
 * reservations, the way V8 and PartitionAlloc use the address space
 * (docs/CR-PLAN.md section 2). Pages are counted on the job
 * (pt_job_pages).
 *
 *   - a munmap or a MAP_FIXED over part of private anonymous memory gives
 *     that part's pages back at once, though the rest still maps the VMO.
 *
 * Nothing between a count and the check of it may allocate (printf can):
 * the counts are the job's, so they see every page the program takes. */
#include <stdint.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <sys/mman.h>
#include "pt.h"

#define PG    4096ul
#define MIB   (1ul << 20)
#define SLACK 4ul   /* pages: a page of mapping records, a page table made or freed */

/* len bytes of address space, no access, nothing committed: V8's and
 * PartitionAlloc's reservation. */
static char *reserve(uint64_t len)
{
    return mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
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

void pt_mem_reserve(void)
{
    check_drop_gives_back();
}
