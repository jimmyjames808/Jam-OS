/* exttest: files bigger than 4 GiB, and sparse ones, written and read back
 * by a program, for tools/extfs-test.sh's `big` case (docs/M13.6-PLAN.md,
 * U9). Every line it prints starts with "exttest:", but a hash's.
 *
 *   exttest sums <dir>           the SHA-256 of every file of the directory
 *                                (not the ones below it), as the shell's
 *                                sha256sum prints it. Names made of bytes
 *                                over 127 can't be typed into the shell:
 *                                this reads them as they come. A name
 *                                whose path is over 255 bytes (FS_PATH_MAX)
 *                                is said, not read.
 *   exttest digest <dir>         one line for a directory of hundreds (the
 *                                log keeps few lines at a time): the
 *                                SHA-256 of the lines "<file's hash>  <name>"
 *                                of its files, names in byte order.
 *   exttest copy <dir> <prefix>  every file of the directory copied to a
 *                                new name, the prefix and the old name; a
 *                                copy that can't be made (a path over 255
 *                                bytes) is said and the rest go on.
 *   exttest open <dir> <count>   <count> files (at most 1024) made in the
 *                                directory and held open all at once, each
 *                                written and read back while the rest are
 *                                open (an extfs's open files and its job's
 *                                limits: docs/M13.6-REVIEW.md's R3).
 *
 *   exttest big <path> <MiB>     writes <MiB> MiB of the pattern below into
 *                                the file (made, or emptied), 1 MiB a write,
 *                                syncs it, closes it, opens it again and
 *                                reads it back whole. Says the SHA-256 of
 *                                what it wrote and of what it read.
 *   exttest sparse <path>        makes the file below: data in four places
 *                                of a 6 GiB-and-a-bit file, holes between,
 *                                a hole at the end (the size set by a
 *                                truncate that grows it); syncs, closes,
 *                                opens again and reads the whole file
 *                                (holes as zeros). Says the SHA-256 of the
 *                                file as it should be and as it read, and
 *                                the size and the 512-byte units the file
 *                                holds (the holes must not be counted).
 *
 * The pattern: a 1 MiB block of xorshift64 bytes (seed 0x9E3779B97F4A7C15),
 * 4 KiB at a time, the first eight bytes of each 4 KiB block replaced by its
 * number (little endian), so a block that lands in the wrong place is
 * seen. tools/extfs-test.sh makes the same bytes on the host, hashes them,
 * and compares with the hashes here and with the file read out of the
 * volume by Linux's debugfs.
 *
 * `sparse`'s file, in bytes:
 *   [0, 64 KiB)                     blocks 0 to 15 of the pattern, in place
 *   [5 GiB - 8 KiB, 5 GiB + 56 KiB) 16 blocks of the pattern, in place
 *   100 bytes at 5 GiB + 5 MiB + 77 byte i is (i * 7 + 3) & 255
 *   [6 GiB, 6 GiB + 1 MiB)          256 blocks of the pattern, in place
 *   size 6 GiB + 1 MiB + 8 KiB      zeros everywhere else
 *
 * Exit codes: 0 every check held; 1 a mismatch or a failed call; 2 usage.
 * Its list gives it every mount read-write. */
#include <os.h>
#include <sha256.h>
#include <wants.h>

JAM_WANTS("mount * rw\n");

#define BLK        4096u
#define PAT_BYTES  (1u << 20)
#define IO_BYTES   (1u << 20)     /* the size of one write or read */
#define GIB        ((uint64_t)1 << 30)

static uint8_t pat[PAT_BYTES];
static uint8_t io[IO_BYTES];

static void make_pattern(void)
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < PAT_BYTES; i += 8) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        memcpy(pat + i, &s, 8);
    }
}

/* Block number n of the pattern, into out (BLK bytes). */
static void block(uint64_t n, uint8_t *out)
{
    memcpy(out, pat + (n % (PAT_BYTES / BLK)) * BLK, BLK);
    memcpy(out, &n, 8);
}

static uint64_t ms_since(uint64_t t0)
{
    return (now() - t0) / NS_PER_MS;
}

static bool number(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (UINT64_MAX - 9) / 10)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

static void hex(const struct sha256 *s, char out[2 * SHA256_BYTES + 1])
{
    struct sha256 copy = *s;
    uint8_t d[SHA256_BYTES];
    sha256_done(&copy, d);
    sha256_hex(d, out);
}

/* Read the whole file (size bytes) through f, into h. */
static status_t read_all(struct jfile *f, uint64_t size, struct sha256 *h)
{
    for (uint64_t at = 0; at < size;) {
        size_t want = size - at < IO_BYTES ? (size_t)(size - at) : IO_BYTES, done = 0;
        status_t st = file_read(f, at, io, want, &done);
        if (st != OK)
            return st;
        if (done != want)
            return ERR_IO;
        sha256_add(h, io, done);
        at += done;
    }
    return OK;
}

/* Write the 1 MiB at `at` (blocks of the pattern) and add it to h. */
static status_t put_blocks(struct jfile *f, uint64_t at, uint64_t blocks, struct sha256 *h)
{
    for (uint64_t b = 0; b < blocks;) {
        uint32_t n = blocks - b < IO_BYTES / BLK ? (uint32_t)(blocks - b) : IO_BYTES / BLK;
        for (uint32_t i = 0; i < n; i++)
            block(at / BLK + b + i, io + (size_t)i * BLK);
        size_t done = 0;
        status_t st = file_write(f, at + b * BLK, io, (size_t)n * BLK, &done);
        if (st != OK)
            return st;
        if (done != (size_t)n * BLK)
            return ERR_NO_SPACE;
        if (h)
            sha256_add(h, io, done);
        b += n;
    }
    return OK;
}

static int cmd_big(const char *path, uint64_t mib)
{
    struct jfile f;
    struct sha256 w, r;
    char wh[2 * SHA256_BYTES + 1], rh[2 * SHA256_BYTES + 1];
    sha256_init(&w);
    sha256_init(&r);
    uint64_t t0 = now();
    status_t st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK) {
        printf("exttest: big %s: can't open: %s\n", path, status_str(st));
        return 1;
    }
    for (uint64_t m = 0; st == OK && m < mib; m++)
        st = put_blocks(&f, m << 20, 256, &w);
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    if (st != OK) {
        printf("exttest: big %s: the write failed: %s\n", path, status_str(st));
        return 1;
    }
    uint64_t wrote = ms_since(t0);
    t0 = now();
    st = file_open(path, FS_READ, &f);
    if (st == OK) {
        st = read_all(&f, mib << 20, &r);
        file_close(&f);
    }
    if (st != OK) {
        printf("exttest: big %s: the read failed: %s\n", path, status_str(st));
        return 1;
    }
    hex(&w, wh);
    hex(&r, rh);
    printf("exttest: big %s: %lu bytes written in %lu ms, read back in %lu ms\n", path,
           (unsigned long)(mib << 20), (unsigned long)wrote, (unsigned long)ms_since(t0));
    printf("exttest: big %s: wrote sha256 %s\n", path, wh);
    printf("exttest: big %s: read sha256 %s\n", path, rh);
    bool same = !strcmp(wh, rh);
    printf("exttest: big %s: %s\n", path, same ? "PASS" : "MISMATCH");
    return same ? 0 : 1;
}

/* The runs of sparse's file: where (bytes) and how many blocks. */
static const struct {
    uint64_t at, blocks;
} runs[] = {
    { 0, 16 },
    { 5 * GIB - 8192, 16 },
    { 6 * GIB, 256 },
};
#define ODD_AT   (5 * GIB + (5u << 20) + 77)
#define ODD_LEN  100u
#define SP_SIZE  (6 * GIB + (1u << 20) + 8192)

/* The file as it should be, hashed (zeros between the runs). */
static void expected(struct sha256 *h)
{
    static const uint8_t zeros[IO_BYTES];
    uint8_t odd[ODD_LEN];
    for (uint32_t i = 0; i < ODD_LEN; i++)
        odd[i] = (uint8_t)(i * 7 + 3);
    uint64_t at = 0;
    for (;;) {
        /* The next thing that isn't a zero: the start of a run, or the odd bytes. */
        uint64_t next = SP_SIZE;
        int which = -1;
        for (unsigned k = 0; k < sizeof(runs) / sizeof(runs[0]); k++)
            if (runs[k].at >= at && runs[k].at < next) {
                next = runs[k].at;
                which = (int)k;
            }
        if (ODD_AT >= at && ODD_AT < next) {
            next = ODD_AT;
            which = -2;
        }
        for (; at < next; at += next - at < IO_BYTES ? next - at : IO_BYTES)
            sha256_add(h, zeros, next - at < IO_BYTES ? (size_t)(next - at) : IO_BYTES);
        if (which == -1)
            return;
        if (which == -2) {
            sha256_add(h, odd, ODD_LEN);
            at += ODD_LEN;
            continue;
        }
        uint8_t b[BLK];
        for (uint64_t k = 0; k < runs[which].blocks; k++) {
            block(runs[which].at / BLK + k, b);
            sha256_add(h, b, BLK);
        }
        at += runs[which].blocks * BLK;
    }
}

static int cmd_sparse(const char *path)
{
    struct jfile f;
    struct sha256 want, got;
    char wh[2 * SHA256_BYTES + 1], gh[2 * SHA256_BYTES + 1];
    sha256_init(&want);
    sha256_init(&got);
    uint64_t t0 = now();
    status_t st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK) {
        printf("exttest: sparse %s: can't open: %s\n", path, status_str(st));
        return 1;
    }
    for (unsigned k = 0; st == OK && k < sizeof(runs) / sizeof(runs[0]); k++)
        st = put_blocks(&f, runs[k].at, runs[k].blocks, NULL);
    uint8_t odd[ODD_LEN];
    for (uint32_t i = 0; i < ODD_LEN; i++)
        odd[i] = (uint8_t)(i * 7 + 3);
    size_t done = 0;
    if (st == OK)
        st = file_write(&f, ODD_AT, odd, ODD_LEN, &done);
    if (st == OK && done != ODD_LEN)
        st = ERR_NO_SPACE;
    if (st == OK)
        st = file_truncate(&f, SP_SIZE);   /* grows: the end is a hole */
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    if (st != OK) {
        printf("exttest: sparse %s: the write failed: %s\n", path, status_str(st));
        return 1;
    }
    uint64_t wrote = ms_since(t0);
    t0 = now();
    struct fs_attr a;
    st = file_open(path, FS_READ, &f);
    if (st == OK)
        st = file_getattr(&f, &a);
    if (st == OK)
        st = read_all(&f, a.size, &got);
    file_close(&f);
    if (st != OK) {
        printf("exttest: sparse %s: the read failed: %s\n", path, status_str(st));
        return 1;
    }
    expected(&want);
    hex(&want, wh);
    hex(&got, gh);
    printf("exttest: sparse %s: written in %lu ms, read back in %lu ms\n", path,
           (unsigned long)wrote, (unsigned long)ms_since(t0));
    printf("exttest: sparse %s: size %lu, %lu units of 512 bytes held\n", path,
           (unsigned long)a.size, (unsigned long)a.blocks);
    printf("exttest: sparse %s: wanted sha256 %s\n", path, wh);
    printf("exttest: sparse %s: read sha256 %s\n", path, gh);
    bool same = !strcmp(wh, gh) && a.size == SP_SIZE;
    /* Holes aren't held: the data is about 1.4 MiB (2,900 units) and the
     * allocation's overhead (extent nodes) a little more. */
    bool small = a.blocks < (16u << 20) / 512;
    printf("exttest: sparse %s: %s%s\n", path, same ? "PASS" : "MISMATCH",
           small ? "" : " (not sparse: the holes are held)");
    return same && small ? 0 : 1;
}

/* ---- names ---------------------------------------------------------------------------- */

#define MAX_NAMES 512

static char names[MAX_NAMES][FS_NAME_MAX];

/* The files (not directories) of dir, their names into names[]. */
static status_t list_files(const char *dir, unsigned *count)
{
    struct fs_entry e;
    *count = 0;
    for (uint32_t i = 0;; i++) {
        status_t st = fs_readdir(dir, i, &e);
        if (st == ERR_NOT_FOUND)
            return OK;
        if (st != OK)
            return st;
        if (e.is_dir)
            continue;
        if (*count == MAX_NAMES)
            return ERR_OUT_OF_RANGE;
        memcpy(names[(*count)++], e.name, FS_NAME_MAX);
    }
}

/* "<dir>/<prefix><name>" into path; false if it doesn't fit. */
static bool join(char *path, size_t cap, const char *dir, const char *prefix, const char *name)
{
    int n = snprintf(path, cap, "%s/%s%s", dir, prefix, name);
    return n > 0 && (size_t)n < cap;
}

static int cmd_sums(const char *dir)
{
    unsigned count;
    status_t st = list_files(dir, &count);
    if (st != OK) {
        printf("exttest: sums %s: can't list it: %s\n", dir, status_str(st));
        return 1;
    }
    int bad = 0;
    for (unsigned i = 0; i < count; i++) {
        char path[FS_PATH_MAX], h[2 * SHA256_BYTES + 1];
        struct sha256 s;
        struct jfile f;
        sha256_init(&s);
        if (!join(path, sizeof(path), dir, "", names[i])) {
            printf("exttest: sums %s: a name of %u bytes: the path is too long\n", dir,
                   (unsigned)strlen(names[i]));
            continue;
        }
        st = file_open(path, FS_READ, &f);
        if (st == OK) {
            struct fs_attr a;
            st = file_getattr(&f, &a);
            if (st == OK)
                st = read_all(&f, a.size, &s);
            file_close(&f);
        }
        if (st != OK) {
            printf("exttest: sums %s: %s\n", path, status_str(st));
            bad = 1;
            continue;
        }
        hex(&s, h);
        printf("%s  %s\n", h, path);
    }
    return bad;
}

/* One digest for the whole directory (the log takes few lines at a time): the
 * SHA-256 of the lines "<hash of the file>  <name>\n" for every file, the
 * names in byte order. */
static int cmd_digest(const char *dir)
{
    static unsigned order[MAX_NAMES];
    unsigned count;
    status_t st = list_files(dir, &count);
    if (st != OK) {
        printf("exttest: digest %s: can't list it: %s\n", dir, status_str(st));
        return 1;
    }
    for (unsigned i = 0; i < count; i++) {   /* insertion sort of the names */
        unsigned j = i;
        for (; j > 0 && strcmp(names[order[j - 1]], names[i]) > 0; j--)
            order[j] = order[j - 1];
        order[j] = i;
    }
    struct sha256 all;
    sha256_init(&all);
    for (unsigned k = 0; k < count; k++) {
        char path[FS_PATH_MAX], h[2 * SHA256_BYTES + 1], line[FS_NAME_MAX + 2 * SHA256_BYTES + 4];
        struct sha256 s;
        struct jfile f;
        struct fs_attr a;
        sha256_init(&s);
        if (!join(path, sizeof(path), dir, "", names[order[k]]))
            continue;
        st = file_open(path, FS_READ, &f);
        if (st == OK) {
            st = file_getattr(&f, &a);
            if (st == OK)
                st = read_all(&f, a.size, &s);
            file_close(&f);
        }
        if (st != OK) {
            printf("exttest: digest %s: %s\n", path, status_str(st));
            return 1;
        }
        hex(&s, h);
        int n = snprintf(line, sizeof(line), "%s  %s\n", h, names[order[k]]);
        sha256_add(&all, line, (size_t)n);
    }
    char h[2 * SHA256_BYTES + 1];
    hex(&all, h);
    printf("exttest: digest %s: %u files, sha256 %s\n", dir, count, h);
    return 0;
}

/* Every file of dir copied to a new name, prefix and the old one (a name
 * made of bytes over 127 stays what it was). One that can't be made says
 * why and the rest go on. */
static int cmd_copy(const char *dir, const char *prefix)
{
    unsigned count;
    status_t st = list_files(dir, &count);
    if (st != OK) {
        printf("exttest: copy %s: can't list it: %s\n", dir, status_str(st));
        return 1;
    }
    unsigned made = 0, refused = 0;
    for (unsigned i = 0; i < count; i++) {
        char from[FS_PATH_MAX], to[FS_PATH_MAX];
        struct jfile in, outf;
        if (!join(from, sizeof(from), dir, "", names[i]) ||
            !join(to, sizeof(to), dir, prefix, names[i])) {
            printf("exttest: copy %s: a name of %u bytes: the path is too long\n", dir,
                   (unsigned)strlen(names[i]));
            refused++;
            continue;
        }
        st = file_open(from, FS_READ, &in);
        if (st != OK) {
            printf("exttest: copy %s: %s\n", from, status_str(st));
            return 1;
        }
        st = file_open(to, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &outf);
        if (st != OK) {
            printf("exttest: copy %s: %s\n", to, status_str(st));
            file_close(&in);
            refused++;
            continue;
        }
        struct fs_attr a;
        st = file_getattr(&in, &a);
        for (uint64_t at = 0; st == OK && at < a.size;) {
            size_t want = a.size - at < IO_BYTES ? (size_t)(a.size - at) : IO_BYTES, done = 0;
            st = file_read(&in, at, io, want, &done);
            if (st == OK && done != want)
                st = ERR_IO;
            if (st == OK)
                st = file_write(&outf, at, io, done, &done);
            at += want;
        }
        if (st == OK)
            st = file_sync(&outf);
        file_close(&in);
        file_close(&outf);
        if (st != OK) {
            printf("exttest: copy %s: %s\n", to, status_str(st));
            return 1;
        }
        made++;
    }
    printf("exttest: copy %s: %u made, %u refused\n", dir, made, refused);
    return 0;
}

/* n files made in dir and all held open at once, each written its own name
 * and read back while every other is open too; then closed. */
#define OPEN_MAX 1024u
static struct jfile opened[OPEN_MAX];

static int cmd_open(const char *dir, uint64_t n)
{
    unsigned i = 0;
    status_t st = OK;
    char path[FS_PATH_MAX], got[FS_PATH_MAX];
    for (; st == OK && i < n; i++) {
        size_t done = 0;
        snprintf(path, sizeof(path), "%s/o%04u", dir, i);
        st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &opened[i]);
        if (st == OK)
            st = file_write(&opened[i], 0, path, strlen(path), &done);
        if (st != OK)
            printf("exttest: open %s: file %u: %s\n", dir, i + 1, status_str(st));
    }
    unsigned held = st == OK ? i : i - 1;
    for (unsigned k = 0; st == OK && k < held; k++) {
        size_t done = 0;
        snprintf(path, sizeof(path), "%s/o%04u", dir, k);
        st = file_read(&opened[k], 0, got, strlen(path), &done);
        if (st == OK && (done != strlen(path) || memcmp(got, path, done)))
            st = ERR_IO;
        if (st != OK)
            printf("exttest: open %s: file %u read back: %s\n", dir, k + 1, status_str(st));
    }
    for (unsigned k = 0; k < held; k++)
        file_close(&opened[k]);
    if (st != OK)
        return 1;
    printf("exttest: open %s: %u files open at once\n", dir, held);
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t mib = 0;
    make_pattern();
    if (argc == 4 && !strcmp(argv[1], "big") && number(argv[3], &mib) && mib)
        return cmd_big(argv[2], mib);
    if (argc == 3 && !strcmp(argv[1], "sparse"))
        return cmd_sparse(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "sums"))
        return cmd_sums(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "digest"))
        return cmd_digest(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "copy"))
        return cmd_copy(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "open") && number(argv[3], &mib) && mib &&
        mib <= OPEN_MAX)
        return cmd_open(argv[2], mib);
    printf("usage: exttest big <path> <MiB> | sparse <path> | sums <dir> | digest <dir> | "
           "copy <dir> <prefix> | open <dir> <count>\n");
    return 2;
}
