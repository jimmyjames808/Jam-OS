/* disktest: a file storm for a writable disk, and the check that finds what
 * a power cut left (docs/M12.7-PLAN.md, stage 2; tools/nvme-write-test.sh
 * runs it, and the owner types it on the PC in P2).
 *
 *   disktest <dir> write [count [max_kib [pace_ms]]]
 *       writes numbered files <dir>/dt-00000.bin, dt-00001.bin, ... each of
 *       seeded content (the same bytes every time for the same number: a
 *       16-byte header, then a xorshift stream seeded by the number), 64
 *       bytes to max_kib KiB (default 64) long. Each file is synced,
 *       read back and compared, and only then does the line
 *           synced N
 *       say that files 0..N-1 are on the medium. It starts at the first
 *       file that is not whole (an earlier storm's, or one a power cut
 *       left half-written), so storms add up, and N goes on counting.
 *       Stops after `count` files (default 100), a failed call, or a full
 *       disk; pace_ms waits between files (default 0).
 *   disktest <dir> check
 *       looks at every dt-NNNNN.bin in <dir> and says what it found:
 *           disktest: check <dir>: files=F whole=W damaged=D missing=M prefix=P
 *       P is how many files from 0 on are whole in a row: after a power
 *       cut that follows a "synced N" line, P is at least N, M is 0 (no
 *       file below the highest is gone) and D is at most 1 (the file that
 *       was being written). Each damaged file gets a line of its own.
 *       Exit 0 if all were whole.
 *
 * It wants every mount, writable where the mount is (`mount -w` first). Exit
 * 0 if all went well, 1 if a file was damaged or a call failed, 2 for a
 * usage error. */
#include <os.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("mount * rw\n");

#define MAGIC      "JAMDTST1"
#define HDR        16u                    /* magic, the number, the size: 32 bits each */
#define MAX_FILES  100000u
#define MAX_KIB    512u
#define NAME_FMT   "%s/dt-%05u.bin"

static uint8_t wbuf[MAX_KIB * 1024], rbuf[MAX_KIB * 1024];
static char    dir[FS_PATH_MAX];

static uint64_t rnd(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static uint64_t seed_of(uint32_t n)
{
    return ((uint64_t)n + 1) * 0x9E3779B97F4A7C15ull;
}

/* The bytes of file n, size bytes of them, into p. */
static void content(uint32_t n, uint32_t size, uint8_t *p)
{
    memcpy(p, MAGIC, 8);
    memcpy(p + 8, &n, 4);
    memcpy(p + 12, &size, 4);
    uint64_t s = seed_of(n);
    for (uint32_t i = HDR; i < size; i += 8) {
        uint64_t v = rnd(&s);
        memcpy(p + i, &v, size - i < 8 ? size - i : 8);
    }
}

/* The size file n gets in a storm of files up to max bytes: seeded too, so a
 * run with another max_kib still writes a whole file for the number. */
static uint32_t size_of(uint32_t n, uint32_t max)
{
    uint64_t s = seed_of(n) ^ 0xD1B54A32D192ED03ull;
    rnd(&s);
    uint32_t span = max - 64;
    uint32_t size = 64 + (uint32_t)(rnd(&s) % span);
    return size < HDR + 8 ? HDR + 8 : size;
}

/* Is file n whole? 0 yes, 1 absent, 2 damaged (*why says how). */
static int examine(uint32_t n, const char **why)
{
    char path[FS_PATH_MAX + 16];
    snprintf(path, sizeof(path), NAME_FMT, dir, n);
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st == ERR_NOT_FOUND)
        return 1;
    if (st != OK) {
        *why = "can't be opened";
        return 2;
    }
    int r = 2;
    uint64_t size = 0;
    size_t done = 0;
    if (file_stat(&f, &size, NULL) != OK) {
        *why = "can't be sized";
    } else if (size < HDR || size > MAX_KIB * 1024u) {
        *why = size ? "short or too long" : "empty";
    } else if (file_read(&f, 0, rbuf, (size_t)size, &done) != OK || done != size) {
        *why = "can't be read";
    } else {
        uint32_t hn, hsize;
        memcpy(&hn, rbuf + 8, 4);
        memcpy(&hsize, rbuf + 12, 4);
        content(n, (uint32_t)size, wbuf);
        if (memcmp(rbuf, MAGIC, 8) || hn != n || hsize != size)
            *why = memcmp(rbuf, MAGIC, 8) ? "no header" : "another file's or size's header";
        else if (memcmp(rbuf + HDR, wbuf + HDR, (size_t)size - HDR))
            *why = "content differs";
        else
            r = 0;
    }
    file_close(&f);
    return r;
}

/* name is "dt-NNNNN.bin" (five digits): *n gets the number. */
static bool file_number(const char *name, uint32_t *n)
{
    if (strncmp(name, "dt-", 3) || strlen(name) != 12 || strcmp(name + 8, ".bin"))
        return false;
    uint32_t v = 0;
    for (int i = 3; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    *n = v;
    return v < MAX_FILES;
}

/* Every number with a file in dir: the highest, and how many files. */
static status_t scan(uint32_t *highest, uint32_t *files, bool *any)
{
    struct fs_entry e;
    *any = false;
    *highest = *files = 0;
    for (uint32_t i = 0;; i++) {
        status_t st = fs_readdir(dir, i, &e);
        if (st == ERR_NOT_FOUND)
            return OK;
        if (st != OK)
            return st;
        uint32_t n;
        if (e.is_dir || !file_number(e.name, &n))
            continue;
        (*files)++;
        if (!*any || n > *highest)
            *highest = n;
        *any = true;
    }
}

static int check(void)
{
    uint32_t highest, files, whole = 0, damaged = 0, missing = 0, prefix = 0;
    bool any, run = true;
    status_t st = scan(&highest, &files, &any);
    if (st != OK) {
        printf("disktest: can't list %s: %s\n", dir, status_str(st));
        return 1;
    }
    for (uint32_t n = 0; any && n <= highest; n++) {
        const char *why = "";
        int r = examine(n, &why);
        if (r == 0) {
            whole++;
            prefix += run;
        } else {
            run = false;
            if (r == 1) {
                missing++;
            } else {
                damaged++;
                printf("disktest: " NAME_FMT " is damaged: %s\n", dir, n, why);
            }
        }
    }
    printf("disktest: check %s: files=%u whole=%u damaged=%u missing=%u prefix=%u\n", dir, files,
           whole, damaged, missing, prefix);
    return damaged || missing ? 1 : 0;
}

static int write_files(uint32_t count, uint32_t max_kib, uint32_t pace_ms)
{
    uint32_t highest, files, start = 0;
    bool any;
    status_t st = scan(&highest, &files, &any);
    if (st != OK) {
        printf("disktest: can't list %s: %s\n", dir, status_str(st));
        return 1;
    }
    for (const char *why; any && start <= highest && examine(start, &why) == 0;)
        start++;   /* the whole files already there */
    printf("disktest: writing %s from file %u, %u file(s) of up to %u KiB\n", dir, start, count,
           max_kib);
    uint32_t max = max_kib * 1024u;
    for (uint32_t n = start; n < start + count && n < MAX_FILES; n++) {
        uint32_t size = size_of(n, max);
        char path[FS_PATH_MAX + 16];
        snprintf(path, sizeof(path), NAME_FMT, dir, n);
        content(n, size, wbuf);
        struct jfile f;
        size_t done = 0;
        st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
        if (st != OK) {
            printf("disktest: can't open %s: %s\n", path, status_str(st));
            return 1;
        }
        st = file_write(&f, 0, wbuf, size, &done);
        if (st == OK && done != size)
            st = ERR_NO_SPACE;
        if (st == OK)
            st = file_sync(&f);
        if (st == OK) {   /* and read back through the same open file */
            st = file_read(&f, 0, rbuf, size, &done);
            if (st == OK && (done != size || memcmp(rbuf, wbuf, size))) {
                printf("disktest: %s read back different\n", path);
                file_close(&f);
                return 1;
            }
        }
        file_close(&f);
        if (st != OK) {
            printf("disktest: %s: %s\n", path, status_str(st));
            return 1;
        }
        printf("synced %u\n", n + 1);
        if (pace_ms)
            jam_sleep_until(now() + (uint64_t)pace_ms * NS_PER_MS);
    }
    return 0;
}

static int usage(void)
{
    printf("usage: disktest <dir> write [count [max_kib [pace_ms]]] | disktest <dir> check\n");
    return 2;
}

/* argv[i] as a number in lo..hi, else *ok false. */
static uint32_t num(int argc, char **argv, int i, uint32_t dflt, uint32_t lo, uint32_t hi, bool *ok)
{
    if (i >= argc)
        return dflt;
    uint64_t v = 0;
    for (const char *p = argv[i]; *p; p++) {
        if (*p < '0' || *p > '9' || v > hi) {
            *ok = false;
            return dflt;
        }
        v = v * 10 + (uint64_t)(*p - '0');
    }
    if (!argv[i][0] || v < lo || v > hi)
        *ok = false;
    return (uint32_t)v;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argv[1][0] != '/' || strlen(argv[1]) >= sizeof(dir))
        return usage();
    size_t len = strlen(argv[1]);
    while (len > 1 && argv[1][len - 1] == '/')
        len--;
    snprintf(dir, sizeof(dir), "%.*s", (int)len, argv[1]);
    if (!strcmp(argv[2], "check"))
        return argc == 3 ? check() : usage();
    if (strcmp(argv[2], "write") || argc > 6)
        return usage();
    bool ok = true;
    uint32_t count = num(argc, argv, 3, 100, 1, MAX_FILES, &ok);
    uint32_t max_kib = num(argc, argv, 4, 64, 1, MAX_KIB, &ok);
    uint32_t pace_ms = num(argc, argv, 5, 0, 0, 10000, &ok);
    return ok ? write_files(count, max_kib, pace_ms) : usage();
}
