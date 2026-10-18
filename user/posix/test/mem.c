/* posixtest's mem section: anonymous mappings, the break and malloc.
 *
 *   - mmap, a munmap in the middle of a mapping, a MAP_FIXED into the
 *     hole and over a live mapping, MAP_FIXED_NOREPLACE refused, mprotect
 *     (execute refused), madvise(MADV_DONTNEED) reading zero after, and
 *     the arguments Linux refuses refused the same way; MADV_DONTNEED on
 *     a MAP_SHARED mapping (a memfd's, an anonymous one) keeps its data;
 *   - mincore (none, all and some of a mapping's pages resident, each
 *     page told on its own: one in the middle, PROT_NONE, given back, a
 *     memfd's pages, a range longer than one kernel call; the
 *     arguments Linux refuses, EFAULT for vec), MADV_REMOVE over shared
 *     memory reading zero after for every holder, and pkey_alloc,
 *     pkey_free and pkey_mprotect without protection keys (ENOSPC,
 *     EINVAL);
 *   - MAP_FIXED in one step: a thread reads four pages without pause
 *     while they are mapped over again and again (a moment with nothing
 *     there would kill the program), and a MAP_FIXED whose range holds a
 *     mapping libc didn't make is ENOMEM with nothing changed;
 *   - brk moved up, written and moved back, and held where RLIMIT_DATA
 *     says;
 *   - malloc of 1 GiB in 1 MiB pieces, every page written and read back,
 *     then freed, and the job's pages back near where they were;
 *   - a program with a malloc of its own (bin/heaptest) that calls into
 *     the layer with its lock held, and a handler's calls made while it
 *     is inside that malloc: the layer never calls it (LH);
 *   - mlock and its kin: nothing is ever paged out, so they succeed (with
 *     Linux's checks of their arguments);
 *   - reservations (memres.c). */
#include <stdlib.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"

#define PG          4096ul
#define PIECE       (1ul << 20)        /* malloc's pieces */
#define PIECES      1024               /* 1 GiB */
#define SLACK_PAGES (8ul << 8)         /* 8 MiB malloc may keep for itself */
#define FIXED_ROUNDS 2000              /* MAP_FIXED over a range being read */

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

/* MADV_DONTNEED over shared memory: the data stays (Linux drops only the
 * caller's view of it). */
static void check_dontneed_shared(void)
{
    int fd = memfd_create("dontneed", 0);
    CHECK(fd >= 0 && ftruncate(fd, 2 * PG) == 0);
    char *m = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        m[0] = 5;
        m[PG] = 6;
        CHECK(madvise(m, 2 * PG, MADV_DONTNEED) == 0 && m[0] == 5 && m[PG] == 6);
        char b = 0;
        CHECK(pread(fd, &b, 1, PG) == 1 && b == 6);
        CHECK(munmap(m, 2 * PG) == 0);
    }
    CHECK(close(fd) == 0);
    char *a = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED);
    if (a == MAP_FAILED)
        return;
    a[0] = 7;
    CHECK(madvise(a, PG, MADV_FREE) == 0 && a[0] == 7);
    CHECK(munmap(a, PG) == 0);
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

static volatile char *fixed_area;
static volatile int fixed_stop;
static long fixed_reads;

static void *fixed_reader(void *arg)
{
    (void)arg;
    long sum = 0;
    while (!fixed_stop) {
        for (unsigned i = 0; i < 4; i++)
            sum += fixed_area[i * PG];
        fixed_reads++;
    }
    return (void *)sum;
}

static void check_fixed_one_step(void)
{
    char *p = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    fixed_area = p;
    fixed_stop = 0;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, fixed_reader, NULL) == 0);
    static const unsigned ranges[][2] = { { 0, 4 }, { 1, 3 }, { 0, 2 }, { 3, 4 } };
    bool placed = true;
    for (unsigned r = 0; r < FIXED_ROUNDS; r++) {
        const unsigned *g = ranges[r % 4];
        char *want = p + g[0] * PG;
        placed &= mmap(want, (g[1] - g[0]) * PG, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == want;
    }
    fixed_stop = 1;
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(placed && fixed_reads > 0);
    CHECK(munmap(p, 4 * PG) == 0);

    /* A hole holding a mapping that isn't libc's: refused, nothing changed. */
    p = mmap(NULL, 8 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    CHECK(munmap(p + 4 * PG, 2 * PG) == 0);
    handle_t v;
    uint64_t at = (uint64_t)(uintptr_t)(p + 4 * PG);
    CHECK(jam_vmo_create(2 * PG, 0, &v) == OK);
    CHECK(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, 2 * PG,
                       VMAR_READ | VMAR_WRITE | VMAR_FIXED, &at) == OK);
    p[0] = 7;
    p[4 * PG] = 9;
    p[7 * PG] = 11;
    errno = 0;
    CHECK(mmap(p, 8 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
               0) == MAP_FAILED && errno == ENOMEM);
    CHECK(p[0] == 7 && p[4 * PG] == 9 && p[7 * PG] == 11);
    CHECK(mmap(p, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
               0) == p && p[0] == 0 && p[4 * PG] == 9);
    CHECK(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), at, 2 * PG) == OK);
    CHECK(jam_handle_close(v) == OK);
    CHECK(munmap(p, 8 * PG) == 0);
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
    /* RLIMIT_DATA 0: the break does not grow (libc-test's memfill). */
    struct rlimit rl, was;
    CHECK(getrlimit(RLIMIT_DATA, &was) == 0);
    rl = was;
    rl.rlim_cur = 0;
    CHECK(setrlimit(RLIMIT_DATA, &rl) == 0);
    CHECK(syscall(SYS_brk, base + 64 * PG) == base);
    CHECK(setrlimit(RLIMIT_DATA, &was) == 0);
    CHECK(syscall(SYS_brk, base + 64 * PG) == base + 64 * (long)PG);
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

/* A program with a malloc of its own (bin/heaptest, user/tests/heaptest)
 * whose malloc calls into the layer with its lock held, and runs a
 * handler's calls while inside it: the layer never calls that malloc (its
 * heap is its own, user/posix/sys/heap.c), so neither waits for ever. */
static void check_own_malloc(void)
{
    static const char *const modes[] = { "own", "handler" };
    for (unsigned i = 0; i < 2; i++) {
        char *av[] = { "heaptest", (char *)modes[i], NULL };
        pid_t pid = -1;
        int status = -1;
        CHECK(posix_spawn(&pid, "/bin/heaptest", NULL, NULL, av, environ) == 0);
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status))
            printf("posixtest: mem: heaptest %s: wait status %#x\n", modes[i], status);
    }
}

static void check_mlock(void)
{
    char *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    CHECK(mlock(p, 2 * PG) == 0 && munlock(p, 2 * PG) == 0);
    CHECK(mlock(p + 1, 10) == 0);   /* rounded to its pages, as Linux */
    CHECK(mlockall(MCL_CURRENT | MCL_FUTURE) == 0 && munlockall() == 0);
    errno = 0;
    CHECK(mlockall(0) == -1 && errno == EINVAL);
    CHECK(mlock((void *)-PG, 2 * PG) == -1 && errno == EINVAL);   /* wraps */
    CHECK(munmap(p, 2 * PG) == 0);
}

/* Resident pages of p's n pages by mincore, or -1. */
static int resident_pages(void *p, size_t n)
{
    unsigned char vec[16];
    memset(vec, 0xff, sizeof(vec));
    if (mincore(p, n * PG, vec) != 0)
        return -1;
    int r = 0;
    for (size_t i = 0; i < n; i++)
        r += vec[i] & 1;
    return r;
}

/* mincore: a mapping with no page touched reads as none, all touched as
 * all, none again after madvise(MADV_DONTNEED); the length rounds up, a
 * length of 0 is fine, an unaligned start is EINVAL, and an unmapped
 * range ENOMEM (with what came before it filled in). Each page is told
 * on its own (the kernel's vmar_resident, docs/CR-PLAN.md "MC notes"). */
static void check_mincore(void)
{
    char *p = mmap(NULL, 8 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    CHECK(resident_pages(p, 8) == 0);
    for (unsigned i = 0; i < 3; i++)
        p[i * PG] = 1;
    CHECK(resident_pages(p, 8) == 3);
    CHECK(resident_pages(p, 3) == 3 && resident_pages(p + 3 * PG, 5) == 0);
    for (unsigned i = 0; i < 8; i++)
        p[i * PG] = 1;
    CHECK(resident_pages(p, 8) == 8);
    CHECK(resident_pages(p + 2 * PG, 1) == 1);
    unsigned char vec[16];
    memset(vec, 0, sizeof(vec));
    CHECK(mincore(p, 2 * PG + 1, vec) == 0 && vec[0] == 1 && vec[1] == 1 && vec[2] == 1);   /* 3 pages */
    CHECK(mincore(p, 0, vec) == 0);
    errno = 0;
    CHECK(mincore(p + 1, PG, vec) == -1 && errno == EINVAL);
    CHECK(madvise(p, 8 * PG, MADV_DONTNEED) == 0 && resident_pages(p, 8) == 0);
    p[PG] = 1;
    CHECK(resident_pages(p, 8) == 1 && resident_pages(p + PG, 1) == 1);
    /* a range running off the end of its mapping */
    for (unsigned i = 0; i < 8; i++)
        p[i * PG] = 1;
    CHECK(munmap(p + 4 * PG, 4 * PG) == 0);
    memset(vec, 0xff, sizeof(vec));
    errno = 0;
    CHECK(mincore(p + 2 * PG, 4 * PG, vec) == -1 && errno == ENOMEM);
    CHECK(vec[0] == 1 && vec[1] == 1);   /* the two pages that are there */
    errno = 0;
    CHECK(mincore(p + 4 * PG, PG, vec) == -1 && errno == ENOMEM);
    CHECK(munmap(p, 4 * PG) == 0);
    errno = 0;
    CHECK(mincore(p, PG, vec) == -1 && errno == ENOMEM);
}

/* mincore page by page, as Chromium's tests ask (PartitionAlloc's
 * CheckPageInCore, the heap profiler's residency): one page in the middle
 * of a mapping touched is the only one in memory; a page made PROT_NONE
 * keeps its memory (Linux's answer), one given back by MADV_DONTNEED
 * doesn't; a memfd's pages (all committed as it grows) are in memory in
 * a mapping that never touched them; a range longer than one kernel call
 * (4096 pages) is told whole; vec NULL, read-only or past the user range
 * is EFAULT (an unmapped range's ENOMEM first). */
static void check_mincore_pages(void)
{
    char *p = mmap(NULL, 8 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    unsigned char vec[8];
    p[5 * PG] = 1;
    memset(vec, 0xff, sizeof(vec));
    CHECK(mincore(p + 5 * PG, PG, vec) == 0 && vec[0] == 1);   /* CheckPageInCore */
    CHECK(mincore(p, PG, vec) == 0 && vec[0] == 0);
    CHECK(mincore(p, 8 * PG, vec) == 0);
    for (unsigned i = 0; i < 8; i++)
        CHECK(vec[i] == (i == 5));
    p[2 * PG] = 1;
    p[3 * PG] = 1;
    CHECK(mprotect(p + 2 * PG, PG, PROT_NONE) == 0);
    CHECK(madvise(p + 3 * PG, PG, MADV_DONTNEED) == 0);
    CHECK(mincore(p, 8 * PG, vec) == 0);
    CHECK(vec[2] == 1 && vec[3] == 0 && vec[5] == 1 && vec[4] == 0);
    errno = 0;
    CHECK(mincore(p, PG, NULL) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(mincore(p, PG, (unsigned char *)-PG) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(mincore(NULL, PG, vec) == -1 && errno == ENOMEM);   /* page 0: never mapped */
    unsigned char *ro = mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(ro != MAP_FAILED);
    errno = 0;
    CHECK(mincore(p, PG, ro) == -1 && errno == EFAULT);
    CHECK(munmap(ro, PG) == 0 && munmap(p, 8 * PG) == 0);
    errno = 0;
    CHECK(mincore(p, PG, NULL) == -1 && errno == ENOMEM);   /* the hole is found first */

    /* A memfd's pages are the memfd's, all of them in memory from the
     * moment it grows (a kept VMO, memfd.c): in memory in a new mapping
     * that never touched them. */
    int fd = memfd_create("mincore", 0);
    CHECK(fd >= 0 && ftruncate(fd, 4 * PG) == 0 && pwrite(fd, "x", 1, 2 * PG) == 1);
    char *s = mmap(NULL, 4 * PG, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(s != MAP_FAILED);
    CHECK(mincore(s, 4 * PG, vec) == 0 && vec[0] == 1 && vec[1] == 1 && vec[2] == 1 &&
          vec[3] == 1);
    CHECK(munmap(s, 4 * PG) == 0 && close(fd) == 0);

    /* Longer than one kernel call: 5000 pages, three touched, one past
     * the first call's 4096. */
    size_t n = 5000;
    char *big = mmap(NULL, n * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *bv = malloc(n);
    CHECK(big != MAP_FAILED && bv);
    big[0] = big[4095 * PG] = big[4999 * PG] = 1;
    memset(bv, 0xff, n);
    CHECK(mincore(big, n * PG, bv) == 0);
    size_t in = 0;
    for (size_t i = 0; i < n; i++)
        in += bv[i];
    CHECK(in == 3 && bv[0] == 1 && bv[4095] == 1 && bv[4999] == 1);
    free(bv);
    CHECK(munmap(big, n * PG) == 0);
}

/* MADV_REMOVE over shared memory frees its pages for every holder: they
 * read zero after, through the mapping, a second mapping and read() of the
 * memfd (Chromium's discardable memory purges so); private memory is
 * EINVAL, and a range nothing maps ENOMEM. */
static void check_madv_remove(void)
{
    int fd = memfd_create("remove", 0);
    CHECK(fd >= 0 && ftruncate(fd, 4 * PG) == 0);
    char *a = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *b = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED);
    if (a != MAP_FAILED && b != MAP_FAILED) {
        memset(a, 0xaa, 4 * PG);
        CHECK(b[0] == (char)0xaa && b[3 * PG] == (char)0xaa);
        CHECK(madvise(a + PG, 2 * PG, MADV_REMOVE) == 0);   /* pages 1 and 2 */
        bool zero_a = true, zero_b = true;
        for (unsigned i = PG; i < 3 * PG; i++) {
            zero_a &= a[i] == 0;
            zero_b &= b[i] == 0;
        }
        CHECK(zero_a);
        CHECK(zero_b);
        CHECK(a[0] == (char)0xaa && a[3 * PG + 7] == (char)0xaa);   /* the rest stays */
        char x = 1;
        CHECK(pread(fd, &x, 1, PG + 5) == 1 && x == 0);
        CHECK(madvise(a, 4 * PG, MADV_REMOVE) == 0 && b[0] == 0 && b[3 * PG] == 0);
        a[0] = 7;   /* the pages come back on touch */
        CHECK(b[0] == 7);
    }
    if (a != MAP_FAILED)
        munmap(a, 4 * PG);
    if (b != MAP_FAILED)
        munmap(b, 4 * PG);
    close(fd);
    char *s = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(s != MAP_FAILED);
    if (s != MAP_FAILED) {
        s[9] = 3;
        CHECK(madvise(s, PG, MADV_REMOVE) == 0 && s[9] == 0);
        munmap(s, PG);
    }
    char *q = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(q != MAP_FAILED);
    if (q != MAP_FAILED) {
        q[0] = 4;
        errno = 0;
        CHECK(madvise(q, PG, MADV_REMOVE) == -1 && errno == EINVAL && q[0] == 4);
        CHECK(munmap(q, PG) == 0);
        errno = 0;
        CHECK(madvise(q, PG, MADV_REMOVE) == -1 && errno == ENOMEM);
    }
}

/* Protection keys: none on Jam OS, answered as Linux does without them
 * (CBF2b: PartitionAlloc calls pkey_alloc whatever CPUID says). */
static void check_pkey(void)
{
    errno = 0;
    CHECK(syscall(SYS_pkey_alloc, 0, 0) == -1 && errno == ENOSPC);
    CHECK(syscall(SYS_pkey_alloc, 0, 2) == -1 && errno == ENOSPC);   /* PKEY_DISABLE_WRITE */
    CHECK(syscall(SYS_pkey_alloc, 1, 0) == -1 && errno == EINVAL);   /* no flags yet */
    CHECK(syscall(SYS_pkey_alloc, 0, 4) == -1 && errno == EINVAL);   /* an access right unknown */
    CHECK(syscall(SYS_pkey_free, 1) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_pkey_free, 0) == -1 && errno == EINVAL);
    char *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    p[0] = 1;
    CHECK(syscall(SYS_pkey_mprotect, p, PG, PROT_READ, 0) == 0);    /* the default key */
    CHECK(syscall(SYS_pkey_mprotect, p + PG, PG, PROT_READ, -1) == 0);   /* -1: none asked for */
    CHECK(p[0] == 1 && mprotect(p, 2 * PG, PROT_READ | PROT_WRITE) == 0);
    CHECK(syscall(SYS_pkey_mprotect, p, PG, PROT_READ, 1) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_pkey_mprotect, p, PG, PROT_READ, 15) == -1 && errno == EINVAL);
    /* the key is an int: 0xffffffff in a register of 64 bits is -1 */
    CHECK(syscall(SYS_pkey_mprotect, p, PG, PROT_READ, 0xffffffffl) == 0);
    CHECK(munmap(p, 2 * PG) == 0);
}

void pt_mem(char **argv __attribute__((unused)))
{
    check_mmap();
    check_dontneed_shared();
    check_mincore();
    check_mincore_pages();
    check_madv_remove();
    check_pkey();
    check_refusals();
    check_fixed_one_step();
    check_brk();
    check_malloc();
    check_own_malloc();
    check_mlock();
    pt_mem_reserve();
}
