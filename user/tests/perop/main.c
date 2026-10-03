/* perop: the per-operation lines (docs/M11.5-PLAN.md, "Per-operation
 * lines"): what one file operation costs a program, beside `bench`'s
 * per-call lines, timed in ring 3 with bench's method (the header of
 * kernel/test/bench.c): the TSC fenced with lfence on both sides, the
 * timestamp's own cost measured first and subtracted, untimed warm-up,
 * the median and the 99th percentile of every sample (no mean, nothing
 * trimmed), nothing printed while measuring.
 *
 * Run it from the shell: `perop [-q] [dir]`. dir is a writable mount,
 * /data by default (the boot stick) or a /usb* one (another stick,
 * `mount -w` first); -q takes a tenth of the samples (the QEMU test). It
 * writes a scratch file, dir/perop.tmp (about 3.3 MiB), times, and
 * deletes it. Every line names dir:
 *   stat           fs_stat of the file
 *   open + close   file_open (read only) and file_close: the fs.open call,
 *                  the transfer buffer mapped and unmapped
 *   read 4 KiB     the file's first 4 KiB, again and again: from fat's
 *                  block cache (one file.read call)
 *   read 64 KiB    the first 64 KiB the same way (one call: fat's
 *                  transfer buffer is 64 KiB)
 *   block read     a 512-byte read at a place of the file fat has never
 *                  read: its cache reads the 4 KiB around it, one block
 *                  read through usb-storage (user/services/fat/cache.c,
 *                  FILL_MIN); the next sample reads STRIDE further on, so
 *                  no sample finds the last one's block, nor reads right
 *                  where it ended (which would double the read-ahead)
 *   write 64 KiB   the first 64 KiB written again, write-through: fat's
 *                  write returns once the stick took it (no FS_GATHER)
 * The two that reach the stick take DEV_SAMPLES samples after DEV_WARM
 * untimed operations; the others SAMPLES after WARM_NS. The file is
 * written once, before, with FS_GATHER; a write never puts sectors in
 * fat's cache, so the block reads find none of the file there (unless a
 * line is left from an earlier run's file in the same clusters: a few
 * samples at most, in the low tail).
 *
 * How it differs from bench's user lines: it is not pinned (no system
 * call pins a thread from user space; bench's are pinned by the kernel
 * that starts them), and it runs on the live system, as `bench` from the
 * shell does. fat is a separate process wherever the scheduler puts it.
 * The TSC's rate is measured against the clock (the kernel knows it; a
 * program doesn't).
 *
 * The lines are printed as any program's output is: to the terminal and
 * into the kernel log, where they read "[perop] bench: ...", so
 * `grep '] bench:'` on the stick's log finds them with bench's (in a
 * pipe, `perop | grep bench`, they go to the pipe instead). Exit 0 if
 * every operation worked, 1 if one failed (its line says so), 2 for a
 * usage error. */
#include <os.h>
#include <wants.h>

/* The mounts it may be pointed at. */
JAM_WANTS("mount /data rw\n"
          "mount /usb* rw\n");

#define SAMPLES     4000
#define DEV_SAMPLES 400
#define DEV_WARM    8                  /* untimed operations before a device line */
#define WARM_NS     (20 * NS_PER_MS)
#define SMALL       4096u              /* the cached small read */
#define BIG         65536u             /* the cached big read and the write */
#define PROBE       512u               /* bytes a block-read sample reads */
#define STRIDE      8192u              /* between two block-read samples */
#define FILE_NAME   "perop.tmp"

/* One line's operation. */
struct op {
    const char *what;                  /* the line, before " (dir)" */
    status_t  (*fn)(uint64_t k);       /* the k-th call: 0, 1, 2, ... (warm-up first) */
    bool        dev;                   /* reaches the stick: DEV_SAMPLES after DEV_WARM */
};

static uint64_t ps_per_cycle_x1024;    /* picoseconds per TSC cycle, << 10 */
static uint64_t tsc_mhz;               /* the TSC's rate, measured */
static uint64_t stamp_cost;            /* cycles, median of a back-to-back pair */
static uint64_t samples[SAMPLES];      /* picoseconds per sample */
static unsigned n_samples = SAMPLES, n_dev = DEV_SAMPLES;
static uint8_t io[BIG];                /* what the reads read into, the write writes from */
static char path[FS_PATH_MAX];         /* dir/perop.tmp */
static uint64_t file_size;
static struct jfile rfile, wfile;      /* opened for reading; for writing */

/* ---- output -------------------------------------------------------------------- */

/* As bench's: "123.4 ns" under 10 us, else whole us. */
static void fmt_ps(char *buf, size_t n, uint64_t ps)
{
    if (ps < 10000000)
        snprintf(buf, n, "%lu.%lu ns", (unsigned long)(ps / 1000), (unsigned long)(ps / 100 % 10));
    else
        snprintf(buf, n, "%lu us", (unsigned long)(ps / 1000000));
}

static void sort(uint64_t *a, unsigned n)
{
    static const unsigned gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
        for (unsigned i = gaps[g]; i < n; i++) {
            uint64_t v = a[i];
            unsigned j = i;
            for (; j >= gaps[g] && a[j - gaps[g]] > v; j -= gaps[g])
                a[j] = a[j - gaps[g]];
            a[j] = v;
        }
}

/* The line for n samples, in bench's format. */
static void result(const char *what, const char *dir, unsigned n)
{
    char name[96], med[24], p99[24];
    sort(samples, n);
    snprintf(name, sizeof(name), "%s (%s)", what, dir);
    fmt_ps(med, sizeof(med), samples[(n - 1) / 2]);
    fmt_ps(p99, sizeof(p99), samples[(n - 1) * 99 / 100]);
    printf("bench: %-44s median %-10s p99 %s\n", name, med, p99);
}

/* ---- timing ------------------------------------------------------------------------ */

static uint64_t span_ps(uint64_t t0, uint64_t t1)
{
    uint64_t c = t1 - t0;
    c = c > stamp_cost ? c - stamp_cost : 0;
    return c * ps_per_cycle_x1024 >> 10;
}

/* The TSC's rate against the clock over WARM_NS (which also brings the
 * core up to speed), then the cost of a timestamp. */
static void calibrate(void)
{
    uint64_t t0 = now(), c0 = cpu_tsc(), t1, c1;
    do {
        t1 = now();
        c1 = cpu_tsc();
    } while (t1 - t0 < WARM_NS);
    uint64_t hz = (c1 - c0) * NS_PER_S / (t1 - t0);
    if (!hz)
        hz = 1;
    tsc_mhz = hz / 1000000;
    ps_per_cycle_x1024 = (1000000000000ull << 10) / hz;
    for (unsigned i = 0; i < SAMPLES; i++) {
        uint64_t a = cpu_tsc(), b = cpu_tsc();
        samples[i] = b - a;
    }
    sort(samples, SAMPLES);
    stamp_cost = samples[(SAMPLES - 1) / 2];
}

/* Time one line: the warm-up, then n samples of one call each. */
static status_t time_op(const struct op *o, unsigned *n_out)
{
    unsigned n = o->dev ? n_dev : n_samples;
    uint64_t k = 0;
    status_t st = OK;
    if (o->dev) {
        for (; k < DEV_WARM && st == OK; k++)
            st = o->fn(k);
    } else {
        for (uint64_t end = now() + WARM_NS; st == OK && now() < end; k++)
            st = o->fn(k);
    }
    for (unsigned i = 0; i < n && st == OK; i++, k++) {
        uint64_t t0 = cpu_tsc();
        st = o->fn(k);
        samples[i] = span_ps(t0, cpu_tsc());
    }
    *n_out = n;
    return st;
}

/* ---- the file and the operations -------------------------------------------------- */

/* Every 8-byte word of the file holds its own offset: a read checks the
 * first word it got, so a sample can't pass on the wrong bytes. */
static void pattern(uint64_t offset, size_t n)
{
    uint64_t *w = (uint64_t *)(void *)io;
    for (size_t i = 0; i < n / 8; i++)
        w[i] = offset + i * 8;
}

static bool holds(uint64_t offset)
{
    uint64_t w;
    memcpy(&w, io, sizeof(w));
    return w == offset;
}

/* dir/perop.tmp, written with FS_GATHER (big disk writes) and synced. */
static status_t make_file(void)
{
    struct jfile f;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_GATHER, &f);
    if (st != OK)
        return st;
    for (uint64_t off = 0; off < file_size && st == OK; off += BIG) {
        size_t n = file_size - off < BIG ? (size_t)(file_size - off) : BIG, done = 0;
        pattern(off, n);
        st = file_write(&f, off, io, n, &done);
        if (st == OK && done != n)
            st = ERR_IO;
    }
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    return st;
}

static status_t read_at(uint64_t offset, size_t n)
{
    size_t done = 0;
    status_t st = file_read(&rfile, offset, io, n, &done);
    if (st == OK && (done != n || !holds(offset)))
        st = ERR_IO;
    return st;
}

static status_t op_stat(uint64_t k)
{
    (void)k;
    uint64_t size = 0;
    bool is_dir = true;
    status_t st = fs_stat(path, &size, &is_dir, NULL);
    return st == OK && (is_dir || size != file_size) ? ERR_IO : st;
}

static status_t op_open(uint64_t k)
{
    (void)k;
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st == OK)
        file_close(&f);
    return st;
}

static status_t op_small(uint64_t k)
{
    (void)k;
    return read_at(0, SMALL);
}

static status_t op_big(uint64_t k)
{
    (void)k;
    return read_at(0, BIG);
}

/* The k-th block read: past the first BIG bytes (the cached reads' and
 * the write's), STRIDE apart. */
static status_t op_block(uint64_t k)
{
    return read_at(BIG + k * STRIDE, PROBE);
}

static status_t op_write(uint64_t k)
{
    (void)k;
    size_t done = 0;
    status_t st = file_write(&wfile, 0, io, BIG, &done);
    return st == OK && done != BIG ? ERR_IO : st;
}

static const struct op reads[] = {
    { "user: stat of a file", op_stat, false },
    { "user: open + close a file", op_open, false },
    { "user: 4 KiB block read via usb-storage", op_block, true },
    { "user: read 4 KiB, cached in fat", op_small, false },
    { "user: read 64 KiB, cached in fat", op_big, false },
};

static const struct op write_op = { "user: write 64 KiB through to the stick", op_write, true };

/* One line, or why it has none. */
static bool line(const struct op *o, const char *dir)
{
    unsigned n;
    status_t st = time_op(o, &n);
    if (st != OK) {
        printf("bench: %s (%s): FAILED: %s\n", o->what, dir, status_str(st));
        return false;
    }
    result(o->what, dir, n);
    return true;
}

/* Every line: the reads on one open file, then the write on another. */
static bool run_all(const char *dir)
{
    bool ok = true;
    status_t st = file_open(path, FS_READ, &rfile);
    if (st != OK) {
        printf("bench: perop: can't open %s: %s\n", path, status_str(st));
        return false;
    }
    for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); i++)
        ok &= line(&reads[i], dir);
    file_close(&rfile);
    pattern(0, BIG);   /* what the file holds there already */
    st = file_open(path, FS_READ | FS_WRITE, &wfile);
    if (st == OK) {
        ok &= line(&write_op, dir);
        file_close(&wfile);
    } else {
        printf("bench: %s (%s): FAILED: can't open: %s\n", write_op.what, dir, status_str(st));
        ok = false;
    }
    return ok;
}

static int usage(void)
{
    printf("usage: perop [-q] [dir]   (dir: a writable mount, /data by default)\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *dir = "/data";
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-q")) {
        n_samples = SAMPLES / 10;
        n_dev = DEV_SAMPLES / 10;
        i++;
    }
    if (i < argc)
        dir = argv[i++];
    if (i < argc || dir[0] != '/')
        return usage();
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')
        len--;
    if ((size_t)snprintf(path, sizeof(path), "%.*s/%s", (int)len, dir, FILE_NAME) >= sizeof(path))
        return usage();

    status_t pr = jam_thread_set_priority(startup_handle(SR_SELF_THREAD), THREAD_PRIO_USER_MAX);
    calibrate();
    char stamp[24];
    fmt_ps(stamp, sizeof(stamp), stamp_cost * ps_per_cycle_x1024 >> 10);
    printf("bench: per-operation lines (bin/perop): ring 3 from the shell, not pinned, "
           "priority %s, TSC %lu MHz (measured), timestamp %s (subtracted)\n",
           pr == OK ? "24" : "16 (24 refused)", (unsigned long)tsc_mhz, stamp);
    printf("bench: median and p99 of %u samples, the stick's lines of %u; block read = a %u-byte "
           "read fat has no cache for, write = through to the stick\n", n_samples, n_dev, PROBE);
    file_size = BIG + (uint64_t)(n_dev + DEV_WARM) * STRIDE;
    printf("perop: writing %s (%lu KiB)\n", path, (unsigned long)(file_size >> 10));
    status_t st = make_file();
    if (st != OK) {
        printf("bench: perop: can't write %s: %s\n", path, status_str(st));
        (void)fs_unlink(path);   /* whatever was made of it */
        return 1;
    }
    char shown[FS_PATH_MAX];
    snprintf(shown, sizeof(shown), "%.*s", (int)len, dir);
    bool ok = run_all(shown);
    st = fs_unlink(path);
    if (st != OK)
        printf("perop: can't delete %s: %s\n", path, status_str(st));
    printf("perop: %s\n", ok ? "done" : "done, with failures");
    return ok ? 0 : 1;
}
