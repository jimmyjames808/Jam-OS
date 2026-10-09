/* posixtest's mem section: anonymous mappings, the break and malloc.
 *
 *   - mmap, a munmap in the middle of a mapping, a MAP_FIXED into the
 *     hole and over a live mapping, MAP_FIXED_NOREPLACE refused, mprotect
 *     (execute refused), madvise(MADV_DONTNEED) reading zero after, and
 *     the arguments Linux refuses refused the same way;
 *   - brk moved up, written and moved back;
 *   - malloc of 1 GiB in 1 MiB pieces, every page written and read back,
 *     then freed, and the job's pages back near where they were. */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "pt.h"

#define PG          4096ul
#define PIECE       (1ul << 20)        /* malloc's pieces */
#define PIECES      1024               /* 1 GiB */
#define SLACK_PAGES (8ul << 8)         /* 8 MiB malloc may keep for itself */

static void check_mmap(void)
{
    char *p = mmap(NULL, 16 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    for (unsigned i = 0; i < 16; i++)
        p[i * PG] = (char)i;
    CHECK(munmap(p + 4 * PG, 4 * PG) == 0);   /* a hole in the middle */
    CHECK(p[3 * PG] == 3 && p[8 * PG] == 8);
    char *q = mmap(p + 4 * PG, 4 * PG, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(q == p + 4 * PG && q[0] == 0);   /* fresh pages in the hole */
    q = mmap(p + 2 * PG, 4 * PG, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);   /* over two mappings */
    CHECK(q == p + 2 * PG && q[0] == 0 && p[PG] == 1 && p[6 * PG] == 0);
    errno = 0;
    CHECK(mmap(p, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
              MAP_FAILED && errno == EEXIST);
    CHECK(mprotect(p, 2 * PG, PROT_READ) == 0 && p[PG] == 1);
    CHECK(mprotect(p, 2 * PG, PROT_READ | PROT_WRITE) == 0);
    p[PG] = 9;
    CHECK(mprotect(p, PG, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    CHECK(madvise(p + 8 * PG, 2 * PG, MADV_DONTNEED) == 0 && p[8 * PG] == 0 && p[9 * PG] == 0);
    CHECK(p[10 * PG] == 10);
    CHECK(munmap(p, 16 * PG) == 0);
}

static void check_refusals(void)
{
    errno = 0;
    CHECK(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
          errno == EINVAL);
    CHECK(mmap(NULL, PG, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1,
               0) == MAP_FAILED && errno == EACCES);
    CHECK(mmap(NULL, PG, PROT_READ, MAP_PRIVATE, 0, 0) == MAP_FAILED && errno == ENODEV);
    CHECK(mmap(NULL, PG, PROT_READ, MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap((void *)0x1001, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
              MAP_FAILED && errno == EINVAL);
    CHECK(munmap((void *)0x1001, PG) == -1 && errno == EINVAL);
    CHECK(madvise(NULL, PG, 9999) == -1 && errno == EINVAL);
    CHECK(mremap(NULL, PG, 2 * PG, MREMAP_MAYMOVE) == MAP_FAILED);
}

static void check_brk(void)
{
    long base = syscall(SYS_brk, 0);
    CHECK(base > 0);
    long top = syscall(SYS_brk, base + 64 * PG);
    CHECK(top == base + 64 * (long)PG);
    if (top == base + 64 * (long)PG) {
        memset((void *)base, 0x5a, 64 * PG);
        CHECK(((volatile char *)base)[63 * PG] == 0x5a);
    }
    CHECK(syscall(SYS_brk, base + (2l << 30)) == top);   /* past the reservation: unmoved */
    CHECK(syscall(SYS_brk, base) == base);
}

static void check_malloc(void)
{
    uint64_t before = pt_job_pages();
    static char *piece[PIECES];
    unsigned got = 0;
    for (; got < PIECES; got++) {
        piece[got] = malloc(PIECE);
        if (!piece[got])
            break;
        for (unsigned long o = 0; o < PIECE; o += PG)
            piece[got][o] = (char)(got + o / PG);
    }
    CHECK(got == PIECES);
    bool same = true;
    for (unsigned i = 0; i < got; i++)
        for (unsigned long o = 0; o < PIECE; o += PG)
            same &= piece[i][o] == (char)(i + o / PG);
    CHECK(same);
    uint64_t during = pt_job_pages();
    CHECK(during >= before + PIECES * (PIECE / PG));
    for (unsigned i = 0; i < got; i++)
        free(piece[i]);
    uint64_t after = pt_job_pages();
    CHECK(after <= before + SLACK_PAGES);
    char *r = NULL;
    for (size_t n = 16; n <= (64ul << 20); n *= 4) {   /* realloc copies as it grows */
        char *s = realloc(r, n);
        CHECK(s != NULL);
        if (!s)
            break;
        s[n - 1] = 1;
        r = s;
    }
    free(r);
}

void pt_mem(char **argv __attribute__((unused)))
{
    check_mmap();
    check_refusals();
    check_brk();
    check_malloc();
}
