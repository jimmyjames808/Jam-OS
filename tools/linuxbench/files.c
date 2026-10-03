/* linuxbench: the per-operation lines, the Linux side of Jam OS's perop
 * (user/tests/perop/main.c), in the directory given with --dir: the
 * SanDisk's FAT32 partition, mounted, so both measure the same stick.
 * A scratch file, dir/linuxbench.tmp, is written, timed and deleted:
 *   stat, open + close, read 4 KiB, read 64 KiB
 *                  as perop's; the reads come from the page cache
 *   block read     pread of 4 KiB with O_DIRECT, STRIDE apart past the
 *                  first 64 KiB: one 4 KiB read from the stick each
 *   write 64 KiB   pwrite of the first 64 KiB with O_DIRECT: returns when
 *                  the stick took it, no cache flush (as fat's
 *                  write-through)
 * The stick's lines take DEV_SAMPLES samples after DEV_WARM untimed
 * operations, the others SAMPLES after WARM_NS, all on P. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SMALL  4096u
#define BIG    65536u
#define STRIDE 8192u
#define NAME   "linuxbench.tmp"

static char path[4096];
static uint64_t file_size;
static uint8_t *io;            /* BIG bytes, page-aligned (O_DIRECT) */
static int fd_read, fd_direct, fd_write;

/* Every 8-byte word holds its own offset in the file. */
static void pattern(uint64_t offset, size_t n)
{
    for (size_t i = 0; i < n / 8; i++) {
        uint64_t v = offset + i * 8;
        memcpy(io + i * 8, &v, 8);
    }
}

static bool holds(uint64_t offset)
{
    uint64_t w;
    memcpy(&w, io, 8);
    return w == offset;
}

static bool make_file(void)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = true;
    for (uint64_t off = 0; off < file_size && ok; off += BIG) {
        size_t n = file_size - off < BIG ? (size_t)(file_size - off) : BIG;
        pattern(off, n);
        ok = pwrite(fd, io, n, (off_t)off) == (ssize_t)n;
    }
    ok = ok && fsync(fd) == 0;
    /* Out of the page cache, so the O_DIRECT reads find nothing to flush. */
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    return close(fd) == 0 && ok;
}

/* One line's operation: the k-th call (warm-up first) is OK. */
typedef bool (*op_fn)(uint64_t k);

static bool op_stat(uint64_t k)
{
    (void)k;
    struct stat st;
    return stat(path, &st) == 0 && (uint64_t)st.st_size == file_size;
}

static bool op_open(uint64_t k)
{
    (void)k;
    int fd = open(path, O_RDONLY);
    return fd >= 0 && close(fd) == 0;
}

static bool read_at(int fd, uint64_t offset, size_t n)
{
    return pread(fd, io, n, (off_t)offset) == (ssize_t)n && holds(offset);
}

static bool op_small(uint64_t k)
{
    (void)k;
    return read_at(fd_read, 0, SMALL);
}

static bool op_big(uint64_t k)
{
    (void)k;
    return read_at(fd_read, 0, BIG);
}

static bool op_block(uint64_t k)
{
    return read_at(fd_direct, BIG + k * STRIDE, SMALL);
}

static bool op_write(uint64_t k)
{
    (void)k;
    return pwrite(fd_write, io, BIG, 0) == (ssize_t)BIG;
}

static struct {
    op_fn fn;
    bool dev;       /* reaches the stick */
    bool ok;        /* every call worked */
    unsigned n;     /* samples taken */
} job;

static void *op_thread(void *arg)
{
    (void)arg;
    uint64_t k = 0;
    job.ok = true;
    if (job.dev) {
        for (; k < DEV_WARM && job.ok; k++)
            job.ok = job.fn(k);
    } else {
        uint64_t end;
        warm_until(&end);
        for (; mono_ns() < end && job.ok; k++)
            job.ok = job.fn(k);
    }
    job.n = job.dev ? DEV_SAMPLES : SAMPLES;
    for (unsigned i = 0; i < job.n && job.ok; i++, k++) {
        uint64_t t0 = stamp();
        job.ok = job.fn(k);
        samples[i] = span_ps(t0, stamp(), 1);
    }
    return NULL;
}

static void line(const char *what, const char *dir, op_fn fn, bool dev, const char *how)
{
    char name[160];
    snprintf(name, sizeof(name), "%s (%s)", what, dir);
    job.fn = fn;
    job.dev = dev;
    run_on(cpu_p, op_thread, NULL);
    if (job.ok)
        result(name, how, job.n);
    else
        skipped(name, "FAILED (an operation failed: O_DIRECT not supported here?)");
}

void bench_files(const char *dir)
{
    file_size = BIG + (uint64_t)(DEV_SAMPLES + DEV_WARM) * STRIDE;
    snprintf(path, sizeof(path), "%s/%s", dir, NAME);
    io = aligned_alloc(4096, BIG);
    if (!io || !make_file()) {
        skipped("per-operation lines", "can't write the scratch file (is --dir writable?)");
        free(io);
        return;
    }
    fd_read = open(path, O_RDONLY);
    fd_direct = open(path, O_RDONLY | O_DIRECT);
    line("user: stat of a file", dir, op_stat, false, "stat()");
    line("user: open + close a file", dir, op_open, false, "open(O_RDONLY) + close()");
    if (fd_direct >= 0)
        line("user: 4 KiB block read via usb-storage", dir, op_block, true,
             "pread 4 KiB, O_DIRECT, where nothing was read: one read from the stick");
    else
        skipped("user: 4 KiB block read via usb-storage", "O_DIRECT refused here");
    line("user: read 4 KiB, cached", dir, op_small, false,
         "pread 4 KiB from the page cache");
    line("user: read 64 KiB, cached", dir, op_big, false,
         "pread 64 KiB from the page cache");
    pattern(0, BIG);   /* what the file holds there already */
    fd_write = open(path, O_WRONLY | O_DIRECT);
    if (fd_write >= 0)
        line("user: write 64 KiB through to the stick", dir, op_write, true,
             "pwrite 64 KiB, O_DIRECT: the stick took it, no cache flush");
    else
        skipped("user: write 64 KiB through to the stick", "O_DIRECT refused here");
    if (fd_write >= 0)
        close(fd_write);
    if (fd_direct >= 0)
        close(fd_direct);
    close(fd_read);
    unlink(path);
    free(io);
}
