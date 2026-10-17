/* posixtest's mapped files (docs/M13-PLAN.md section 12): mmap of a file
 * on its own pages where the mount's service is a pager (fat), msync, and
 * posix_spawn of a program on /data.
 *
 * The mmap section (by default, on /data):
 *   - MAP_SHARED read-only: the file's bytes, zero past its end in the
 *     last page, kept after the descriptor is closed; with PROT_WRITE
 *     refused (EACCES on a descriptor that can't write, ENODEV on one
 *     that can: the pager never writes pages back);
 *   - MAP_PRIVATE read-only, then mprotect adding write: our writes,
 *     never the file's (read and the shared mapping see the file);
 *   - MAP_PRIVATE writable: charged in full at mmap, its writes the
 *     mapping's alone, MADV_DONTNEED bringing the file's bytes back;
 *   - msync: Linux's checks (flags, alignment, ENOMEM over a hole), and a
 *     private mapping's writes never reaching the file;
 *   - 4 MiB mapped and read without a copy: the job's pages don't grow by
 *     the file's (a copy would add 1,024).
 * The checks that need the file's pages are made only when /data has a
 * pager (libjam's mapped_file_open says so); without one the section says
 * so on a line and checks what a copy gives (MAP_SHARED is ENODEV then).
 *
 * Asked for by name (tools/posix-mmap-test.sh):
 *   mmap-big <path> <sum>     the file at path (100 MiB on a second stick)
 *                             mapped shared and private, summed as 64-bit
 *                             little-endian words (sum in hex, the host's),
 *                             the job's pages counted: no copy
 *   mmap-coherent             a write through the file seen by a shared
 *                             mapping and by read, a private mapping's
 *                             touched page keeping its bytes and an
 *                             untouched one following the file (needs fat
 *                             to keep mappings coherent with writes: until
 *                             then the shared mapping is ENODEV)
 *   mmap-tmp                  the mmap section in /tmp instead of /data,
 *                             its pager required (init's /tmp, a volume's
 *                             view or the memory one: tools/layout-test.sh)
 *   spawn-data <path> allowed|refused
 *                             posix_spawn of a copy of posixtest on /data:
 *                             runs (and exits 7) once `allow`ed, else
 *                             EACCES; allowed, its self-image section
 *                             passes too (procfs.c), and a forked
 *                             child's execve of it gets our pipe and our
 *                             SIGTERM, forwarded (proc.c's term: exit 9)
 * and the child mode --child-mmap-pull <path>: maps the file, says it is
 * ready, waits for a line on descriptor 0 (the stick is pulled meanwhile),
 * then copies from the mapping through a pipe (EIO, and it lives) and
 * touches it (the kernel ends it: the file's pager is gone). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PG        4096ul
#define MF_PATH   mf_path               /* in the section's directory (mmap_in) */
#define MF_SIZE   (3 * PG + 100)        /* three pages and a bit */
#define MF_MAP    (4 * PG)              /* the file's pages, the last one part zero */
#define MF_BIG    mf_big
#define MF_BIGSZ  (4ul << 20)
#define MF_COH    "/data/pt-mmap-coh.bin"
#define SETTLE_MS 3000                  /* how long an unlink waits out EBUSY */

static char mf_path[64] = "/data/pt-mmap.bin", mf_big[64] = "/data/pt-mmap-big.bin";

/* The byte at offset i of every file this section writes. */
static uint8_t pat(uint64_t i)
{
    return (uint8_t)(i * 7 + (i >> 12) + 3);
}

/* Do p's n bytes hold the pattern from offset from? */
static bool same(const volatile uint8_t *p, uint64_t from, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != pat(from + i))
            return false;
    return true;
}

static bool zero(const volatile uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

static void nap_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* path made size bytes long, the pattern, through write(). */
static bool put_file(const char *path, uint64_t size)
{
    static uint8_t buf[65536];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = true;
    for (uint64_t at = 0; ok && at < size; at += sizeof(buf)) {
        uint64_t n = size - at < sizeof(buf) ? size - at : sizeof(buf);
        for (uint64_t i = 0; i < n; i++)
            buf[i] = pat(at + i);
        ok = write(fd, buf, n) == (ssize_t)n;
    }
    return close(fd) == 0 && ok;
}

/* unlink, waiting out EBUSY a while: fat lets a file go once the last
 * mapping of it has (its VMO's end reaches fat just after our munmap). */
static int unlink_settled(const char *path)
{
    int r = unlink(path);
    for (unsigned waited = 0; r && errno == EBUSY && waited < SETTLE_MS; waited += 20) {
        nap_ms(20);
        r = unlink(path);
    }
    return r;
}

/* Does path's mount have a pager (libjam's mapped file: file.map
 * answered)? */
static bool has_pager(const char *path)
{
    struct mapped_file mf;
    if (mapped_file_open(path, &mf) != OK)
        return false;
    mapped_file_close(&mf);
    return true;
}

/* MAP_SHARED with PROT_WRITE, before anything maps the file (a mapping
 * holds it in fat, which then refuses an open for writing). */
static void shared_write_refused(void)
{
    int rw = open(MF_PATH, O_RDWR), ro = open(MF_PATH, O_RDONLY);
    CHECK(rw >= 0 && ro >= 0);
    errno = 0;
    CHECK(mmap(NULL, MF_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED &&
          errno == EACCES);
    errno = 0;
    CHECK(mmap(NULL, MF_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, rw, 0) == MAP_FAILED &&
          errno == ENODEV);
    close(rw);
    close(ro);
}

/* MAP_SHARED read-only on the file's pages; *out the mapping (MAP_FAILED
 * without a pager: ENODEV checked). Kept after the descriptor's close. */
static uint8_t *shared_map(int fd, bool pager)
{
    errno = 0;
    uint8_t *s = mmap(NULL, MF_MAP, PROT_READ, MAP_SHARED, fd, 0);
    if (!pager) {
        CHECK(s == MAP_FAILED && errno == ENODEV);
        return MAP_FAILED;
    }
    CHECK(s != MAP_FAILED);
    if (s == MAP_FAILED)
        return s;
    CHECK(same(s, 0, MF_SIZE));
    CHECK(zero(s + MF_SIZE, MF_MAP - MF_SIZE));   /* the last page past the end */
    errno = 0;
    CHECK(mprotect(s, PG, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    return s;
}

/* MAP_PRIVATE read-only, then write added: a page of our own. */
static void private_then_write(int fd, const uint8_t *s)
{
    uint8_t *m = mmap(NULL, MF_MAP, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    CHECK(same(m, 0, MF_SIZE));
    CHECK(mprotect(m + PG, 2 * PG, PROT_READ | PROT_WRITE) == 0);
    m[PG + 10] = 0x5a;
    m[2 * PG + 10] = 0xa5;
    CHECK(m[PG + 10] == 0x5a && m[2 * PG + 10] == 0xa5);
    CHECK(same(m, 0, PG) && same(m + PG + 11, PG + 11, 100));   /* the rest as it was */
    uint8_t b = 0;
    CHECK(pread(fd, &b, 1, PG + 10) == 1 && b == pat(PG + 10));   /* never the file's */
    if (s != MAP_FAILED)
        CHECK(s[PG + 10] == pat(PG + 10));
    errno = 0;
    CHECK(mprotect(m, PG, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    CHECK(munmap(m, MF_MAP) == 0);
}

/* MAP_PRIVATE writable: charged at mmap, its writes its own, msync
 * writes nothing, MADV_DONTNEED gives the file's bytes back. */
static void private_writable(int fd, const uint8_t *s, bool pager)
{
    uint64_t before = pt_job_pages();
    uint8_t *w = mmap(NULL, MF_MAP, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(w != MAP_FAILED);
    if (w == MAP_FAILED)
        return;
    CHECK(pt_job_pages() >= before + MF_MAP / PG);   /* the whole mapping, now */
    CHECK(same(w, 0, MF_SIZE) && zero(w + MF_SIZE, MF_MAP - MF_SIZE));
    w[PG + 1] = 0xee;
    w[MF_SIZE + 1] = 0xee;   /* past the end, in the last page */
    CHECK(msync(w, MF_MAP, MS_SYNC) == 0);
    uint8_t b[2] = { 0 };
    CHECK(pread(fd, b, 1, PG + 1) == 1 && b[0] == pat(PG + 1));
    if (s != MAP_FAILED)
        CHECK(s[PG + 1] == pat(PG + 1) && s[MF_SIZE + 1] == 0);
    CHECK(madvise(w + PG, PG, MADV_DONTNEED) == 0);
    if (pager)   /* Linux's answer for a private file mapping; a copy reads zero */
        CHECK(w[PG + 1] == pat(PG + 1));
    CHECK(munmap(w, MF_MAP) == 0);
}

/* msync's own checks. */
static void msync_checks(const uint8_t *s)
{
    uint8_t *a = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED);
    if (a == MAP_FAILED)
        return;
    CHECK(msync(a, 4 * PG, MS_ASYNC) == 0 && msync(a, 4 * PG, MS_SYNC | MS_INVALIDATE) == 0);
    CHECK(msync(a, 0, MS_SYNC) == 0);
    errno = 0;
    CHECK(msync(a, PG, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(a, PG, 8) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(a + 1, PG, MS_SYNC) == -1 && errno == EINVAL);
    CHECK(munmap(a + PG, PG) == 0);   /* a hole */
    errno = 0;
    CHECK(msync(a, 4 * PG, MS_SYNC) == -1 && errno == ENOMEM);
    CHECK(msync(a + 2 * PG, 2 * PG, MS_SYNC) == 0);
    if (s != MAP_FAILED)
        CHECK(msync((void *)s, MF_MAP, MS_SYNC) == 0);
    CHECK(munmap(a, 4 * PG) == 0);
}

/* MF_BIGSZ mapped and read: the 64-bit words' sum, and the job's pages
 * no more than a mapping's tables more (a copy would be the file's). */
static void big_no_copy(bool pager)
{
    CHECK(put_file(MF_BIG, MF_BIGSZ));
    int fd = open(MF_BIG, O_RDONLY);
    CHECK(fd >= 0);
    uint64_t before = pt_job_pages(), want = 0, sum = 0;
    const uint64_t *m = mmap(NULL, MF_BIGSZ, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    for (uint64_t i = 0; i < MF_BIGSZ / 8; i++) {
        uint64_t w = 0;
        for (unsigned k = 0; k < 8; k++)
            w |= (uint64_t)pat(i * 8 + k) << (8 * k);
        want += w;
        sum += m[i];
    }
    uint64_t grew = pt_job_pages() - before;
    CHECK(sum == want);
    printf("posixtest: mmap: %lu MiB mapped and read (%s), the job grew %lu pages\n",
           MF_BIGSZ >> 20, pager ? "the file's pages" : "a copy: no pager", grew);
    if (pager)
        CHECK(grew < MF_BIGSZ / PG / 16);
    CHECK(munmap((void *)m, MF_BIGSZ) == 0);
    CHECK(unlink_settled(MF_BIG) == 0);
}

/* The mmap section's checks on files in dir; need_pager: its mount must
 * have one. */
static void mmap_in(const char *dir, bool need_pager)
{
    snprintf(mf_path, sizeof(mf_path), "%s/pt-mmap.bin", dir);
    snprintf(mf_big, sizeof(mf_big), "%s/pt-mmap-big.bin", dir);
    if (!put_file(MF_PATH, MF_SIZE)) {
        printf("posixtest: mmap: %s can't be written: %s\n", MF_PATH, strerror(errno));
        CHECK(!"no directory to test in");
        return;
    }
    bool pager = has_pager(MF_PATH);
    CHECK(pager || !need_pager);
    if (!pager)
        printf("posixtest: mmap: %s has no pager here: MAP_SHARED is ENODEV, and "
               "MAP_PRIVATE a copy\n", dir);
    shared_write_refused();
    int fd = open(MF_PATH, O_RDONLY);
    CHECK(fd >= 0);
    uint8_t *s = shared_map(fd, pager);
    private_then_write(fd, s);
    private_writable(fd, s, pager);
    uint8_t *o = mmap(NULL, PG, PROT_READ, MAP_PRIVATE, fd, 2 * PG);   /* at an offset */
    CHECK(o != MAP_FAILED && same(o, 2 * PG, PG));
    close(fd);
    if (s != MAP_FAILED)   /* the mapping outlives the descriptor */
        CHECK(same(s, 0, MF_SIZE));
    msync_checks(s);
    if (o != MAP_FAILED)
        CHECK(munmap(o, PG) == 0);
    if (s != MAP_FAILED)
        CHECK(munmap(s, MF_MAP) == 0);
    CHECK(unlink_settled(MF_PATH) == 0);
    /* The boot image's files have no pager: shared is ENODEV, private a copy. */
    int b = open("/boot/bin/posixtest", O_RDONLY);
    CHECK(b >= 0);
    errno = 0;
    CHECK(mmap(NULL, PG, PROT_READ, MAP_SHARED, b, 0) == MAP_FAILED && errno == ENODEV);
    const char *e = mmap(NULL, PG, PROT_READ, MAP_PRIVATE, b, 0);
    CHECK(e != MAP_FAILED && !memcmp(e, "\x7f" "ELF", 4));
    if (e != MAP_FAILED)
        munmap((void *)e, PG);
    close(b);
    big_no_copy(pager);
}

void pt_mmap(char **argv)
{
    (void)argv;
    mmap_in("/data", false);
}

/* The same in init's /tmp, which always has a pager (M13.6's DB1: the
 * memory volume's too). */
void pt_mmap_tmp(char **argv)
{
    (void)argv;
    mmap_in("/tmp", true);
}

/* ---- asked for by name --------------------------------------------------------- */

/* The 64-bit little-endian words' sum of n bytes at m (n a multiple of 8). */
static uint64_t sum_words(const volatile uint64_t *m, uint64_t n)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n / 8; i++)
        sum += m[i];
    return sum;
}

/* The file at path mapped with flags, summed against want; the job's
 * pages counted. */
static void big_one(const char *path, int flags, uint64_t want)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && !(st.st_size % 8));
    if (fd < 0)
        return;
    uint64_t size = (uint64_t)st.st_size, before = pt_job_pages();
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const uint64_t *m = mmap(NULL, size, PROT_READ, flags, fd, 0);
    close(fd);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    uint64_t sum = sum_words(m, size);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t grew = pt_job_pages() - before;
    long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("posixtest: mmap-big: %s %s: %lu bytes, sum %016lx (%s), the job grew %lu pages, "
           "%ld ms\n", path, flags == MAP_SHARED ? "shared" : "private", (unsigned long)size,
           (unsigned long)sum, sum == want ? "right" : "WRONG", (unsigned long)grew, ms);
    CHECK(sum == want);
    CHECK(grew < size / PG / 64);   /* tables only: a copy would be every page */
    CHECK(munmap((void *)m, size) == 0);
}

void pt_mmap_big(char **argv)
{
    const char *path = NULL, *sum = NULL;
    for (int i = 1; argv[i]; i++)
        if (!strcmp(argv[i], "mmap-big") && argv[i + 1] && argv[i + 2]) {
            path = argv[i + 1];
            sum = argv[i + 2];
        }
    CHECK(path && sum);
    if (!path || !sum)
        return;
    uint64_t want = strtoull(sum, NULL, 16);
    big_one(path, MAP_SHARED, want);
    big_one(path, MAP_PRIVATE, want);
}

void pt_mmap_coherent(char **argv)
{
    (void)argv;
    CHECK(put_file(MF_COH, 2 * PG + 8));
    int fd = open(MF_COH, O_RDWR);
    CHECK(fd >= 0);
    errno = 0;
    uint8_t *s = mmap(NULL, 3 * PG, PROT_READ, MAP_SHARED, fd, 0);
    uint8_t *p = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(s != MAP_FAILED && p != MAP_FAILED);
    if (s == MAP_FAILED || p == MAP_FAILED) {
        printf("posixtest: mmap-coherent: mmap of a file open for writing: %s\n",
               strerror(errno));
        close(fd);
        return;
    }
    CHECK(same(s, 0, 2 * PG + 8) && same(p, 0, PG));   /* p's page 0 touched: its own now */
    CHECK(pwrite(fd, "NEW!", 4, 16) == 4 && pwrite(fd, "new2", 4, PG + 16) == 4);
    char b[4] = { 0 };
    CHECK(pread(fd, b, 4, 16) == 4 && !memcmp(b, "NEW!", 4));
    CHECK(!memcmp(s + 16, "NEW!", 4) && !memcmp(s + PG + 16, "new2", 4));   /* the write */
    CHECK(same(p + 16, 16, 4));                 /* touched before the write: its own */
    CHECK(!memcmp(p + PG + 16, "new2", 4));     /* untouched: the file as it is now */
    CHECK(write(fd, b, 4) == 4);   /* at offset 0: through the file again */
    CHECK(!memcmp(s, "NEW!", 4));
    CHECK(munmap(s, 3 * PG) == 0 && munmap(p, 3 * PG) == 0);
    close(fd);
    CHECK(unlink_settled(MF_COH) == 0);
}

void pt_spawn_data(char **argv)
{
    const char *path = NULL, *want = NULL;
    for (int i = 1; argv[i]; i++)
        if (!strcmp(argv[i], "spawn-data") && argv[i + 1] && argv[i + 2]) {
            path = argv[i + 1];
            want = argv[i + 2];
        }
    CHECK(path && want && (!strcmp(want, "allowed") || !strcmp(want, "refused")));
    if (!path || !want)
        return;
    /* The shell's cp made it without the execute bit: on a mount with
     * modes (an ext /data, tools/posix-mmap-test.sh's MMAP_DATA=ext) that
     * alone is EACCES. FAT has no bits to set. */
    (void)chmod(path, 0755);
    char *const args[] = { "pt-prog", "--child-proc", "exit", "7", NULL };
    pid_t pid = 0;
    int r = posix_spawn(&pid, path, NULL, NULL, args, NULL);
    int status = 0;
    if (!r)
        CHECK(waitpid(pid, &status, 0) == pid);
    printf("posixtest: spawn-data: %s: %s", path, r ? strerror(r) : "started");
    if (!r)
        printf(", exited %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    printf("\n");
    if (!strcmp(want, "refused")) {
        CHECK(r == EACCES);
        return;
    }
    CHECK(r == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    /* Started by init, it has its own image and path: its self-image
     * section passes (procfs.c). */
    char *const self[] = { "pt-prog", "self-image", (char *)path, NULL };
    r = posix_spawn(&pid, path, NULL, NULL, self, NULL);
    status = -1;
    CHECK(r == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    printf("posixtest: spawn-data: %s self-image: %s\n", path,
           r ? strerror(r) : WIFEXITED(status) && !WEXITSTATUS(status) ? "PASS" : "FAIL");
    /* A forked child's execve of it (spawn, wait, exit: init starts it):
     * the pipe it says it is ready on is the program's, and our SIGTERM
     * to the forked child is forwarded to it by the child's signal thread,
     * whose wait ends with the program's code. */
    int p[2];
    char fd[12];
    CHECK(pipe(p) == 0);
    snprintf(fd, sizeof(fd), "%d", p[1]);
    char *const term[] = { "pt-prog", "--child-proc", "term", fd, NULL };
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        execv(path, term);
        _exit(98);
    }
    close(p[1]);
    char c = 0;
    CHECK(pid > 0 && read(p[0], &c, 1) == 1 && c == 'r');
    CHECK(pid > 0 && kill(pid, SIGTERM) == 0);
    status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 9);
    close(p[0]);
    printf("posixtest: spawn-data: %s fork-exec: wait status %#x\n", path, status);
}

int pt_child_mmap_pull(int argc, char **argv)
{
    if (argc < 3)
        return 64;
    setvbuf(stdout, NULL, _IOLBF, 0);   /* each line out before the next step */
    int fd = open(argv[2], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < (off_t)PG)
        return 2;
    uint64_t size = (uint64_t)st.st_size & ~(PG - 1);
    const volatile uint8_t *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        printf("posixtest: mmap-pull: mmap: %s\n", strerror(errno));
        return 3;
    }
    unsigned sum = 0;
    for (uint64_t i = 0; i < size; i += PG)   /* every page fetched once */
        sum += m[i];
    printf("posixtest: mmap-pull: mapped %s (%lu bytes, %u), ready\n", argv[2],
           (unsigned long)size, sum);
    char line[16];
    if (read(0, line, sizeof(line)) <= 0)
        return 4;
    int p[2];
    if (pipe(p))
        return 5;
    errno = 0;
    ssize_t n = write(p[1], (const void *)m, PG);   /* the kernel's copy from the mapping */
    int e = errno;
    printf("posixtest: mmap-pull: a copy from the mapping: %s\n",
           n == -1 && e == EIO ? "EIO" : "not refused (wrong)");
    if (!(n == -1 && e == EIO))
        printf("posixtest: mmap-pull: the copy returned %ld, errno %d (%s)\n", (long)n, e, strerror(e));
    printf("posixtest: mmap-pull: touching\n");
    unsigned b = m[0];   /* the stick is gone: the kernel ends us here */
    printf("posixtest: mmap-pull: touched: byte %u (wrong: the stick is gone)\n", b);
    return 1;
}
