/* pagertest rec: records stored through a shared writable mapping of a
 * file (FILE_MAP_WRITE: the file's service writes the stores back,
 * docs/WRITEMAP-PLAN.md), for the tests of tools/pager-test.sh's wmsync,
 * wcut and wpress.
 *
 * A record is rsz bytes (5000 unless a command says: it crosses pages, so a
 * torn write shows) that say who they are: an 8-byte tag, the record's
 * number, the run's salt, a body of bytes made from the two, and the sum
 * (CRC-32 as a 64-bit word) of all of it before the sum. The file holds records
 * 0 to n - 1 at i * rsz; a store makes record i whole in one go and is
 * never repeated (a crash in the middle of a store tears only the record
 * stored then, which nobody was told was safe). A record read back is
 *   whole    exactly what it should be,
 *   zero     all zero (never written),
 *   torn     some of its bytes right and the rest zero (the write-back was
 *            cut in the middle),
 *   foreign  a byte that is neither: another record's, or noise.
 *
 *   pagertest rec make <path> <records> <rsz>
 *       make the file (cut or grown to records * rsz bytes of zeros)
 *   pagertest rec put <path> <records> <salt> msync|nosync <batch> <rsz> [cue]
 *       map it writable and store the records in order; with msync after
 *       every <batch> records vmo_sync waits for the write-back, and the
 *       line "pagertest: rec: acked <k>" says the first k records are
 *       safe. With a cue (a file of the script's), the mapping is held
 *       after the last store until the cue appears, then every record is
 *       looked at again through it.
 *   pagertest rec check <path> <records> <salt> <acked> <rsz>
 *       the file's size is right and the first <acked> records are whole,
 *       none foreign, seen by read and then by a new mapping; the others
 *       may be whole, zero or torn.
 *   pagertest rec swarm <path> <records> <salt> <batch> <rsz>
 *       eight writers on one mapping of the file (docs/WRITEMAP-REVIEW.md's
 *       hostile test): it is mapped writable, then the process forks, and
 *       each process stores with four threads, writer w the records w,
 *       w + 8, ... (pages shared between writers: a record crosses them),
 *       the child through the mapping it inherited (an msync through a
 *       handle of its own: the same VMO). Each writer's msync (vmo_sync
 *       with the wait) covers its records since its last one, every
 *       <batch> of its records and after its last, and must be OK: what
 *       its pager's deaths do is the test's (tools/pager-test.sh's
 *       whostile kills it five times a second meanwhile). Then every
 *       record must be whole through the mapping: "pagertest: rec: swarm
 *       <path>: <k> whole of <records>: PASS".
 *
 * Exit codes: 0 done and right; 1 a record wrong; 2 the file could not be
 * made, opened or mapped (or the fork failed); 3 the cue never came; 4 an
 * msync or a store's system call failed (or the forked writers' process
 * ended badly). */
#include <fs_idl.h>
#include <os.h>
#include "pagertest.h"

#define REC_CUE_WAIT (120 * NS_PER_S)
#define REC_MAX      (64u << 10)   /* the biggest record */

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

static uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/* CRC-32 (zlib's: the host's python checks the records with zlib.crc32). */
static uint32_t crc32(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xffffffffu;
    while (n--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* Record i of the run salt, as it must be, into b (rsz bytes, rsz >= 32). */
static void record(uint8_t *b, uint32_t rsz, uint64_t i, uint64_t salt)
{
    memcpy(b, "JAMREC01", 8);
    put64(b + 8, i);
    put64(b + 16, salt);
    /* the body: 64-bit words (little endian) of a mixer run from the number
     * and the salt, cut at the sum */
    uint64_t s = mix(i * 0x2545f4914f6cdd1dull ^ salt);
    for (uint32_t at = 24; at < rsz - 8; at++) {
        if ((at - 24) % 8 == 0)
            s = mix(s);
        b[at] = (uint8_t)(s >> (8 * ((at - 24) % 8)));
    }
    put64(b + rsz - 8, crc32(b, rsz - 8));
}

enum { R_WHOLE, R_ZERO, R_TORN, R_FOREIGN, R_KINDS };

/* What got (rsz bytes) is as record i of the run. exp is scratch. */
static int classify(const uint8_t *got, uint8_t *exp, uint32_t rsz, uint64_t i, uint64_t salt)
{
    record(exp, rsz, i, salt);
    bool same = true, zero = true, torn = true;
    for (uint32_t k = 0; k < rsz; k++) {
        if (got[k] != exp[k]) {
            same = false;
            if (got[k] != 0)
                torn = false;
        }
        if (got[k] != 0)
            zero = false;
    }
    return same ? R_WHOLE : zero ? R_ZERO : torn ? R_TORN : R_FOREIGN;
}

static uint64_t up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* The file's shared VMO to write, mapped writable here. */
struct wmap {
    handle_t vmo;
    uint8_t *data;
    uint64_t size;   /* the file's bytes */
    uint64_t len;    /* mapped */
};

static status_t wmap_open(const char *path, struct wmap *w)
{
    struct jfile f;
    uint64_t addr = 0;
    status_t st = file_open(path, FS_READ | FS_WRITE, &f);
    if (st != OK)
        return st;
    st = file_map_within(f.ch, FS_CALL_TIMEOUT, FILE_MAP_WRITE, &w->vmo, &w->size);
    file_close(&f);   /* the mapping outlives the open */
    if (st != OK)
        return st;
    w->len = up(w->size);
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), w->vmo, 0, w->len, VMAR_READ | VMAR_WRITE,
                      &addr);
    if (st != OK) {
        jam_handle_close(w->vmo);
        return st;
    }
    w->data = (uint8_t *)(uintptr_t)addr;
    return OK;
}

static void wmap_close(struct wmap *w)
{
    jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)w->data, w->len);
    jam_handle_close(w->vmo);
}

static bool cue_comes(const char *cue)
{
    uint64_t end = now() + REC_CUE_WAIT;
    printf("pagertest: rec: holding, waiting for %s\n", cue);
    while (fs_stat(cue, NULL, NULL, NULL) != OK) {
        if (now() > end)
            return false;
        jam_sleep_until(now() + 50 * NS_PER_MS);
    }
    printf("pagertest: rec: %s is there\n", cue);
    return true;
}

static int rec_make(const char *path, uint64_t n, uint32_t rsz)
{
    struct jfile f;
    status_t st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st == OK)
        st = file_truncate(&f, n * rsz);
    if (st == OK)
        st = file_sync(&f);
    if (f.ch)
        file_close(&f);
    printf("pagertest: rec: make %s: %lu records of %u bytes: %s\n", path, (unsigned long)n, rsz,
           status_str(st));
    return st == OK ? 0 : 2;
}

/* Every record of the run through the mapping: how many of each kind. */
static void count_mapped(const uint8_t *data, uint64_t n, uint64_t salt, uint32_t rsz,
                         unsigned kinds[R_KINDS], uint64_t *first_bad)
{
    static uint8_t exp[REC_MAX];
    memset(kinds, 0, R_KINDS * sizeof(kinds[0]));
    *first_bad = n;
    for (uint64_t i = 0; i < n; i++) {
        int k = classify(data + i * rsz, exp, rsz, i, salt);
        kinds[k]++;
        if (k != R_WHOLE && i < *first_bad)
            *first_bad = i;
    }
}

static int rec_put(const char *path, uint64_t n, uint64_t salt, bool sync, uint64_t batch,
                   uint32_t rsz, const char *cue)
{
    struct wmap w;
    status_t st = wmap_open(path, &w);
    if (st != OK) {
        printf("pagertest: rec: put %s: map: %s\n", path, status_str(st));
        return 2;
    }
    if (w.size < n * rsz) {
        printf("pagertest: rec: put %s: the file is %lu bytes, %lu wanted\n", path,
               (unsigned long)w.size, (unsigned long)(n * rsz));
        wmap_close(&w);
        return 2;
    }
    printf("pagertest: rec: storing %lu records into %s (%s, batch %lu)\n", (unsigned long)n, path,
           sync ? "msync" : "no msync", (unsigned long)batch);
    static uint8_t rec[REC_MAX];
    uint64_t t0 = now(), slow = 0;
    int rc = 0;
    for (uint64_t i = 0; i < n && rc == 0; i++) {
        uint64_t t1 = now();
        record(rec, rsz, i, salt);
        memcpy(w.data + i * rsz, rec, rsz);   /* the store, which may wait for a write-back */
        uint64_t took = now() - t1;
        if (took > slow)
            slow = took;
        if (sync && ((i + 1) % batch == 0 || i + 1 == n)) {
            st = jam_vmo_sync(w.vmo, 0, w.len, VMO_SYNC_WAIT);
            if (st != OK) {
                printf("pagertest: rec: msync after record %lu: %s\n", (unsigned long)i,
                       status_str(st));
                rc = 4;
            } else {
                printf("pagertest: rec: acked %lu\n", (unsigned long)(i + 1));
            }
        }
    }
    if (rc == 0)
        printf("pagertest: rec: stored %lu records in %lu ms, the slowest store %lu ms\n",
               (unsigned long)n, (unsigned long)((now() - t0) / NS_PER_MS),
               (unsigned long)(slow / NS_PER_MS));
    if (rc == 0 && cue) {
        if (!cue_comes(cue)) {
            printf("pagertest: rec: the cue %s never came\n", cue);
            rc = 3;
        } else {
            unsigned kinds[R_KINDS];
            uint64_t bad;
            count_mapped(w.data, n, salt, rsz, kinds, &bad);
            bool ok = kinds[R_WHOLE] == n;
            printf("pagertest: rec: held mapping of %s: %u whole of %lu: %s\n", path,
                   kinds[R_WHOLE], (unsigned long)n, ok ? "PASS" : "FAIL");
            rc = ok ? 0 : 1;
        }
    }
    wmap_close(&w);
    if (rc == 0)
        printf("pagertest: rec: put %s: done\n", path);
    return rc;
}

/* How the view named is: the counts, and whether the first `acked` are whole and
 * none foreign. */
static bool view_ok(const char *path, const char *how, const unsigned kinds[R_KINDS],
                    uint64_t first_bad, uint64_t n, uint64_t acked)
{
    bool ok = first_bad >= acked && kinds[R_FOREIGN] == 0;
    printf("pagertest: rec: check %s by %s: %u whole, %u zero, %u torn, %u foreign of %lu; "
           "first not whole %lu, %lu acked: %s\n", path, how, kinds[R_WHOLE], kinds[R_ZERO],
           kinds[R_TORN], kinds[R_FOREIGN], (unsigned long)n, (unsigned long)first_bad,
           (unsigned long)acked, ok ? "ok" : "WRONG");
    return ok;
}

static int rec_check(const char *path, uint64_t n, uint64_t salt, uint64_t acked, uint32_t rsz)
{
    static uint8_t exp[REC_MAX], got[REC_MAX];
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK) {
        printf("pagertest: rec: check %s: open: %s\n", path, status_str(st));
        return 2;
    }
    uint64_t size = 0;
    st = file_stat(&f, &size, NULL);
    if (st != OK || size != n * rsz) {
        printf("pagertest: rec: check %s: the file is %lu bytes (%s), %lu wanted\n", path,
               (unsigned long)size, status_str(st), (unsigned long)(n * rsz));
        file_close(&f);
        return 1;
    }
    unsigned kinds[R_KINDS] = { 0 };
    uint64_t first_bad = n;
    for (uint64_t i = 0; i < n; i++) {
        size_t done = 0;
        st = file_read(&f, i * rsz, got, rsz, &done);
        if (st != OK || done != rsz) {
            printf("pagertest: rec: check %s: read of record %lu: %s, %lu bytes\n", path,
                   (unsigned long)i, status_str(st), (unsigned long)done);
            file_close(&f);
            return 1;
        }
        int k = classify(got, exp, rsz, i, salt);
        kinds[k]++;
        if (k != R_WHOLE && i < first_bad)
            first_bad = i;
    }
    file_close(&f);
    bool ok = view_ok(path, "read", kinds, first_bad, n, acked);
    struct mapped_file mf;
    st = mapped_file_open(path, &mf);
    if (st != OK) {
        printf("pagertest: rec: check %s: map: %s\n", path, status_str(st));
        return 2;
    }
    if (mf.size < n * rsz) {
        printf("pagertest: rec: check %s: the mapping is %lu bytes\n", path, (unsigned long)mf.size);
        ok = false;
    } else {
        count_mapped(mf.data, n, salt, rsz, kinds, &first_bad);
        ok = view_ok(path, "a new mapping", kinds, first_bad, n, acked) && ok;
    }
    mapped_file_close(&mf);
    printf("pagertest: rec: check %s: %s\n", path, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---- swarm: eight writers in two processes --------------------------------------------------- */

#define SWARM_WRITERS 8u                 /* in all; half of them in each process */
#define SWARM_THREADS 4u                 /* in each process */
#define SWARM_REC_MAX (16u << 10)        /* the biggest record a swarm stores */
#define SWARM_WAIT    (1800 * NS_PER_S)  /* the forked writers' process, at most */

/* One writer: records id, id + SWARM_WRITERS, ... */
struct swarmer {
    unsigned id;
    uint64_t stored, acked, syncs, slow;   /* slow: the longest msync, ns */
    status_t st;                           /* the msync that failed, or OK */
    uint64_t at;                           /* ... after this record */
};

static struct {
    uint8_t *data;   /* the mapping the stores go through */
    handle_t vmo;    /* a handle of this process's to its VMO, for the msyncs */
    uint64_t n, salt, batch, len;
    uint32_t rsz;
} sw;
static struct swarmer swr[SWARM_THREADS];
static uint8_t sw_stack[SWARM_THREADS][32768] __attribute__((aligned(16)));
static uint8_t sw_rec[SWARM_THREADS][SWARM_REC_MAX];

static void swarm_writer(void *arg)
{
    struct swarmer *s = arg;
    uint8_t *rec = sw_rec[s - swr];
    uint64_t from = s->id;   /* the first record not yet msynced */
    for (uint64_t i = s->id; i < sw.n; i += SWARM_WRITERS) {
        record(rec, sw.rsz, i, sw.salt);
        memcpy(sw.data + i * sw.rsz, rec, sw.rsz);
        s->stored++;
        if (s->stored % sw.batch && i + SWARM_WRITERS < sw.n)
            continue;
        uint64_t lo = (from * sw.rsz) & ~(uint64_t)(PAGE_SIZE - 1), hi = up((i + 1) * sw.rsz);
        if (hi > sw.len)
            hi = sw.len;
        uint64_t t = now();
        status_t st = jam_vmo_sync(sw.vmo, lo, hi - lo, VMO_SYNC_WAIT);
        t = now() - t;
        s->slow = t > s->slow ? t : s->slow;
        s->syncs++;
        if (st != OK) {
            s->st = st;
            s->at = i;
            return;
        }
        s->acked = s->stored;
        from = i + SWARM_WRITERS;
    }
}

/* This process's four writers, from `base` on, run to their end. */
static int swarm_run(unsigned base)
{
    handle_t th[SWARM_THREADS];
    unsigned started = 0;
    for (unsigned i = 0; i < SWARM_THREADS; i++) {
        swr[i] = (struct swarmer){ .id = base + i };
        if (thread_spawn("swarm", swarm_writer, &swr[i], sw_stack[i], sizeof(sw_stack[i]),
                         &th[i]) != OK)
            break;
        started++;
    }
    signals_t seen;
    for (unsigned i = 0; i < started; i++) {
        (void)jam_object_wait_one(th[i], SIG_TERMINATED, DEADLINE_NEVER, &seen);
        jam_handle_close(th[i]);
    }
    int rc = started == SWARM_THREADS ? 0 : 4;
    for (unsigned i = 0; i < started; i++) {
        const struct swarmer *s = &swr[i];
        if (s->st != OK) {
            printf("pagertest: rec: swarm writer %u: msync after record %lu: %s\n", s->id,
                   (unsigned long)s->at, status_str(s->st));
            rc = 4;
        } else {
            printf("pagertest: rec: swarm writer %u: %lu records, %lu acked, %lu msyncs, the "
                   "slowest %lu ms\n", s->id, (unsigned long)s->stored, (unsigned long)s->acked,
                   (unsigned long)s->syncs, (unsigned long)(s->slow / NS_PER_MS));
        }
    }
    return rc;
}

static int rec_swarm(const char *path, uint64_t n, uint64_t salt, uint64_t batch, uint32_t rsz)
{
    struct wmap w;
    status_t st = wmap_open(path, &w);
    if (st != OK) {
        printf("pagertest: rec: swarm %s: map: %s\n", path, status_str(st));
        return 2;
    }
    if (w.size < n * rsz) {
        printf("pagertest: rec: swarm %s: the file is %lu bytes, %lu wanted\n", path,
               (unsigned long)w.size, (unsigned long)(n * rsz));
        wmap_close(&w);
        return 2;
    }
    (void)crc32((const uint8_t *)"", 0);   /* its table made before the threads */
    sw.data = w.data;
    sw.vmo = w.vmo;
    sw.n = n;
    sw.salt = salt;
    sw.batch = batch;
    sw.rsz = rsz;
    sw.len = w.len;
    printf("pagertest: rec: swarm %s: %u writers in 2 processes, %lu records of %u bytes, "
           "msync every %lu\n", path, SWARM_WRITERS, (unsigned long)n, rsz, (unsigned long)batch);
    uint64_t t0 = now();
    int64_t kid = jam_fork(NULL);
    if (kid < 0) {
        printf("pagertest: rec: swarm %s: fork: %s\n", path, status_str((status_t)kid));
        wmap_close(&w);
        return 2;
    }
    if (kid == 0) {
        /* The child: stores through the mapping it inherited; msyncs through
         * a handle of its own (the one it inherited names nothing here). */
        struct wmap own;
        st = wmap_open(path, &own);
        if (st != OK) {
            printf("pagertest: rec: swarm %s: the child's map: %s\n", path, status_str(st));
            return 2;
        }
        sw.vmo = own.vmo;
        int rc = swarm_run(SWARM_THREADS);
        wmap_close(&own);
        return rc;
    }
    int rc = swarm_run(0);
    struct process_info info;
    st = spawn_wait((handle_t)kid, SWARM_WAIT, &info);
    if (st != OK || info.killed || info.exit_code) {
        printf("pagertest: rec: swarm %s: the forked writers: %s, exit %ld%s\n", path,
               status_str(st), (long)info.exit_code, info.killed ? " (killed)" : "");
        rc = rc ? rc : 4;
    }
    jam_handle_close((handle_t)kid);
    if (rc == 0) {
        unsigned kinds[R_KINDS];
        uint64_t bad;
        count_mapped(w.data, n, salt, rsz, kinds, &bad);
        bool ok = kinds[R_WHOLE] == n;
        printf("pagertest: rec: swarm %s: %u whole of %lu in %lu ms: %s\n", path, kinds[R_WHOLE],
               (unsigned long)n, (unsigned long)((now() - t0) / NS_PER_MS), ok ? "PASS" : "FAIL");
        rc = ok ? 0 : 1;
    }
    wmap_close(&w);
    return rc;
}

int pagertest_rec(int argc, char **argv)
{
    uint64_t n = 0, salt = 0, batch = 0, acked = 0, rsz = 0;
    if (argc == 4 && !strcmp(argv[0], "make") && number(argv[2], &n) && number(argv[3], &rsz) &&
        rsz >= 32 && rsz <= REC_MAX)
        return rec_make(argv[1], n, (uint32_t)rsz);
    if ((argc == 7 || argc == 8) && !strcmp(argv[0], "put") && number(argv[2], &n) &&
        number(argv[3], &salt) && (!strcmp(argv[4], "msync") || !strcmp(argv[4], "nosync")) &&
        number(argv[5], &batch) && batch >= 1 && number(argv[6], &rsz) && rsz >= 32 &&
        rsz <= REC_MAX)
        return rec_put(argv[1], n, salt, !strcmp(argv[4], "msync"), batch, (uint32_t)rsz,
                       argc == 8 ? argv[7] : NULL);
    if (argc == 6 && !strcmp(argv[0], "check") && number(argv[2], &n) && number(argv[3], &salt) &&
        number(argv[4], &acked) && acked <= n && number(argv[5], &rsz) && rsz >= 32 &&
        rsz <= REC_MAX)
        return rec_check(argv[1], n, salt, acked, (uint32_t)rsz);
    if (argc == 6 && !strcmp(argv[0], "swarm") && number(argv[2], &n) && number(argv[3], &salt) &&
        number(argv[4], &batch) && batch >= 1 && number(argv[5], &rsz) && rsz >= 32 &&
        rsz <= SWARM_REC_MAX)
        return rec_swarm(argv[1], n, salt, batch, (uint32_t)rsz);
    printf("usage: pagertest rec make <path> <records> <rsz> | "
           "put <path> <records> <salt> msync|nosync <batch> <rsz> [cue] | "
           "check <path> <records> <salt> <acked> <rsz> | "
           "swarm <path> <records> <salt> <batch> <rsz>\n");
    return 64;
}
