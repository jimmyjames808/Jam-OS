/* jamext: the volume as a file (or part of one) under the ext library:
 * its device, opening and closing, making a volume, and the transactions
 * a change runs in. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "jamext.h"

#define CACHE_BYTES (32u << 20)         /* the block cache (any block size) */
#define NAMES       65536u              /* names the directory hash holds */
#define REVOKES     1024u               /* revoke records the journal's region holds */
#define JNL_MEM_MAX (64u << 20)
#define MKFS_MEM    (1u << 20)
#define RECOVER_STEP 16u

/* ---- messages ---------------------------------------------------------------------------- */

const char *jx_why(status_t st)
{
    switch (st) {
    case OK:                 return "ok";
    case ERR_NOT_FOUND:      return "no such file or directory";
    case ERR_NOT_DIR:        return "not a directory";
    case ERR_IS_DIR:         return "is a directory";
    case ERR_ALREADY_EXISTS: return "it exists already";
    case ERR_NOT_EMPTY:      return "the directory has entries";
    case ERR_NO_SPACE:       return "no space left on the volume";
    case ERR_NAME_TOO_LONG:  return "the name is too long";
    case ERR_INVALID_ARGS:   return "not a valid name or argument";
    case ERR_ACCESS_DENIED:  return "the volume is read-only";
    case ERR_NO_RESOURCES:   return "too many links, or the cache is full";
    case ERR_LINK_LOOP:      return "too many levels of symbolic links";
    case ERR_NOT_SUPPORTED:  return "not supported (a feature this library doesn't write)";
    case ERR_IO:             return "the volume is damaged, or the disk failed";
    case ERR_OUT_OF_RANGE:   return "out of range";
    case ERR_NO_MEMORY:      return "out of memory";
    case ERR_BAD_STATE:      return "bad state (a transaction was left open)";
    case ERR_CANCELED:       return "interrupted";
    default:                 return "failed";
    }
}

void jx_fail(const char *what, status_t st)
{
    fprintf(stderr, "jamext: %s: %s (%d)\n", what, jx_why(st), (int)st);
}

/* ---- the device -------------------------------------------------------------------------- */

static status_t dev_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    const struct jx *x = ctx;
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(x->fd, p, len, (off_t)(x->base + off));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return ERR_IO;
        p += n;
        off += (uint64_t)n;
        len -= (uint32_t)n;
    }
    return OK;
}

static status_t dev_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    const struct jx *x = ctx;
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(x->fd, p, len, (off_t)(x->base + off));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return ERR_IO;
        p += n;
        off += (uint64_t)n;
        len -= (uint32_t)n;
    }
    return OK;
}

static struct ext_dev device(struct jx *x, bool write)
{
    return (struct ext_dev){ .ctx = x, .bytes = x->bytes, .read = dev_read,
                             .write = write ? dev_write : NULL };
}

/* ---- where the volume is ----------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static bool pread_all(int fd, void *buf, size_t n, uint64_t off)
{
    return pread(fd, buf, n, (off_t)off) == (ssize_t)n;
}

/* Partition n (1-based) of the file's GPT, or its MBR; 512-byte sectors. */
static bool find_part(int fd, int n, uint64_t *off, uint64_t *size)
{
    uint8_t mbr[512], hdr[512];
    if (!pread_all(fd, mbr, sizeof(mbr), 0)) {
        fprintf(stderr, "jamext: can't read the partition table\n");
        return false;
    }
    if (pread_all(fd, hdr, sizeof(hdr), 512) && !memcmp(hdr, "EFI PART", 8)) {
        uint64_t lba = le64(hdr + 72);
        uint32_t count = le32(hdr + 80), esize = le32(hdr + 84);
        uint8_t e[128];
        if (n < 1 || (uint32_t)n > count || esize < 48 || esize > sizeof(e) ||
            !pread_all(fd, e, esize, lba * 512 + (uint64_t)(n - 1) * esize)) {
            fprintf(stderr, "jamext: no partition %d in the GPT\n", n);
            return false;
        }
        bool used = false;
        for (int i = 0; i < 16; i++)
            used |= e[i] != 0;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (!used || last < first) {
            fprintf(stderr, "jamext: GPT partition %d is not used\n", n);
            return false;
        }
        *off = first * 512;
        *size = (last - first + 1) * 512;
        return true;
    }
    if (mbr[510] == 0x55 && mbr[511] == 0xAA && n >= 1 && n <= 4) {
        const uint8_t *e = mbr + 446 + 16 * (n - 1);
        if (e[4] && le32(e + 12)) {
            *off = (uint64_t)le32(e + 8) * 512;
            *size = (uint64_t)le32(e + 12) * 512;
            return true;
        }
    }
    fprintf(stderr, "jamext: no partition %d (no GPT, and no such MBR entry)\n", n);
    return false;
}

/* The superblock's say of the volume's size (a raw device has no file size). */
static bool super_size(int fd, uint64_t base, uint64_t *size)
{
    uint8_t sb[1024];
    if (!pread_all(fd, sb, sizeof(sb), base + 1024) || sb[0x38] != 0x53 || sb[0x39] != 0xEF)
        return false;
    uint32_t log = le32(sb + 0x18);
    if (log > 6)
        return false;
    *size = (uint64_t)le32(sb + 4) << (10 + log);
    return true;
}

static bool place(struct jx *x, const struct jx_opts *o)
{
    uint64_t off = o->offset, size = o->size;
    if (o->part) {
        uint64_t psize;
        if (!find_part(x->fd, o->part, &off, &psize))
            return false;
        off += o->offset;
        if (!size || size > psize)
            size = psize;
    }
    if (!size) {
        struct stat st;
        if (fstat(x->fd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size > off)
            size = (uint64_t)st.st_size - off;
        else if (!super_size(x->fd, off, &size)) {
            fprintf(stderr, "jamext: %s: can't tell the volume's size (--size)\n", o->image);
            return false;
        }
    }
    x->base = off;
    x->bytes = size & ~(uint64_t)1023;
    return true;
}

/* ---- random numbers ---------------------------------------------------------------------- */

static uint64_t next_random(uint64_t *s)
{
    uint64_t v = *s ? *s : 0x9E3779B97F4A7C15ull;
    v ^= v >> 12;
    v ^= v << 25;
    v ^= v >> 27;
    *s = v;
    return v * 0x2545F4914F6CDD1Dull;
}

static void seed_rng(uint64_t *rng, uint64_t seed)
{
    *rng = seed;
    if (seed)
        return;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rng, sizeof(*rng)) != (ssize_t)sizeof(*rng))
        *rng = (uint64_t)time(NULL) * 6364136223846793005ull + (uint64_t)getpid();
    if (fd >= 0)
        close(fd);
}

uint32_t jx_random(struct jx *x)
{
    return (uint32_t)(next_random(&x->rng) >> 32);
}

/* ---- transactions ------------------------------------------------------------------------ */

bool jx_begin(struct jx *x)
{
    status_t st = ext_jnl_begin(&x->fs, NULL);
    if (st == ERR_BAD_STATE) {
        ext_jnl_abort(&x->fs);
        st = ext_jnl_begin(&x->fs, NULL);
    }
    if (st != OK)
        jx_fail("begin", st);
    return st == OK;
}

bool jx_commit(struct jx *x)
{
    status_t st = ext_jnl_commit(&x->fs);
    if (st != OK) {
        ext_jnl_abort(&x->fs);
        jx_fail("commit", st);
    }
    return st == OK;
}

void jx_abort(struct jx *x)
{
    ext_jnl_abort(&x->fs);
}

/* ---- opening and closing ----------------------------------------------------------------- */

static const char *ro_reason(uint32_t ro)
{
    if (ro & EXT_RO_ERRORS)
        return "errors are recorded on it (e2fsck clears them)";
    if (ro & EXT_RO_DAMAGED)
        return "damage found while mounting";
    if (ro & EXT_RO_RECOVER)
        return "its journal needs replaying";
    if (ro & EXT_RO_FEATURES)
        return "it has features this library reads but doesn't write";
    if (ro & EXT_RO_LACKS)
        return "it lacks a feature Jam OS writes with";
    return "?";
}

static void report_orphans(struct jx *x)
{
    uint32_t done = RECOVER_STEP;
    while (done == RECOVER_STEP && x->fs.sb.last_orphan) {
        struct ext_time now = { .sec = x->o.now };
        if (!jx_begin(x))
            return;
        status_t st = ext_orphans_recover(&x->fs, NULL, NULL, &now, RECOVER_STEP, &done);
        if (st != OK) {
            jx_abort(x);
            jx_fail("orphan list", st);
            return;
        }
        if (!jx_commit(x))
            return;
    }
}

bool jx_open(struct jx *x, const struct jx_opts *o, bool write)
{
    memset(x, 0, sizeof(*x));
    x->o = *o;
    x->fd = open(o->image, write ? O_RDWR : O_RDONLY);
    if (x->fd < 0) {
        fprintf(stderr, "jamext: %s: %s\n", o->image, strerror(errno));
        return false;
    }
    seed_rng(&x->rng, o->seed);
    if (!place(x, o))
        return false;
    x->cache_size = CACHE_BYTES;
    x->cache = malloc(x->cache_size);
    size_t names_bytes = ext_names_mem(NAMES);
    x->names = malloc(names_bytes);
    if (!x->cache || !x->names) {
        fprintf(stderr, "jamext: out of memory\n");
        return false;
    }
    struct ext_dev ro_dev = device(x, false);
    status_t st = ext_mount(&x->fs, &ro_dev, x->cache, x->cache_size, EXT_MOUNT_READ_ONLY);
    if (st != OK) {
        fprintf(stderr, "jamext: %s: not a volume this library mounts: %s (%s)\n", o->image,
                jx_why(st), x->fs.why ? x->fs.why : "?");
        return false;
    }
    if (!write) {
        if (x->fs.needs_recovery && !o->quiet)
            fprintf(stderr, "jamext: note: %s was not unmounted cleanly; its journal is not "
                    "replayed, so what follows is the volume as it lies (--replay replays it)\n",
                    o->image);
        return true;
    }
    uint64_t jblocks = 1024;
    struct ext_inode j;
    if (x->fs.has_journal && ext_iget(&x->fs, x->fs.sb.journal_inum, 0, &j) == OK)
        jblocks = ext_isize(&j) >> x->fs.block_log;
    x->jsize = ext_jnl_mem_size(x->fs.block_size,
                                jblocks > UINT32_MAX ? UINT32_MAX : (uint32_t)jblocks, REVOKES);
    if (x->jsize > JNL_MEM_MAX) {
        fprintf(stderr, "jamext: a journal of %llu blocks is too big to open\n",
                (unsigned long long)jblocks);
        return false;
    }
    x->jmem = malloc(x->jsize);
    if (!x->jmem) {
        fprintf(stderr, "jamext: out of memory\n");
        return false;
    }
    struct ext_dev rw_dev = device(x, true);
    st = ext_jnl_init(&x->jnl, x->jmem, x->jsize, 0);
    if (st == OK)
        st = ext_jnl_mount(&x->fs, &x->jnl, &rw_dev, x->cache, x->cache_size, 0);
    if (st != OK) {
        fprintf(stderr, "jamext: %s: can't open it for writing: %s (%s)\n", o->image, jx_why(st),
                x->fs.why ? x->fs.why : "?");
        return false;
    }
    if (x->fs.ro) {
        fprintf(stderr, "jamext: %s: read-only: %s\n", o->image, ro_reason(x->fs.ro));
        return false;
    }
    if (x->jnl.replayed && !o->quiet)
        fprintf(stderr, "jamext: the journal: %u transaction(s), %u block(s) replayed\n",
                (unsigned)x->jnl.replayed, (unsigned)x->jnl.replayed_blocks);
    (void)ext_names_init(&x->fs, x->names, names_bytes);
    x->write = true;
    if (!jx_begin(x))
        return false;
    if (ext_count_free(&x->fs) != OK) {
        jx_abort(x);
        fprintf(stderr, "jamext: the free counts can't be set\n");
        return false;
    }
    if (!jx_commit(x))
        return false;
    report_orphans(x);
    return true;
}

bool jx_close(struct jx *x)
{
    bool ok = true;
    if (x->write && x->fs.jnl) {
        status_t st = ext_jnl_close(&x->fs);
        if (st != OK) {
            jx_fail("closing the journal", st);
            ok = false;
        }
    }
    if (x->fd >= 0) {
        if (x->write && fsync(x->fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
            fprintf(stderr, "jamext: fsync: %s\n", strerror(errno));
            ok = false;
        }
        close(x->fd);
    }
    free(x->cache);
    free(x->names);
    free(x->jmem);
    memset(x, 0, sizeof(*x));
    x->fd = -1;
    return ok;
}

/* ---- making a volume --------------------------------------------------------------------- */

bool jx_mkfs(const struct jx_opts *o, uint64_t size, struct ext_mkfs *p)
{
    struct jx x;
    memset(&x, 0, sizeof(x));
    x.o = *o;
    x.fd = open(o->image, O_RDWR | O_CREAT, 0666);
    if (x.fd < 0) {
        fprintf(stderr, "jamext: %s: %s\n", o->image, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(x.fd, &st) != 0) {
        fprintf(stderr, "jamext: %s: %s\n", o->image, strerror(errno));
        close(x.fd);
        return false;
    }
    uint64_t off = o->offset;
    if (o->part) {
        uint64_t psize;
        if (!find_part(x.fd, o->part, &off, &psize)) {
            close(x.fd);
            return false;
        }
        off += o->offset;
        if (!size || size > psize)
            size = psize;
    }
    size &= ~(uint64_t)1023;
    bool zeroed = false;
    if (S_ISREG(st.st_mode)) {
        if (!o->part && off == 0) {   /* a whole image: started over, so it reads zeros */
            zeroed = ftruncate(x.fd, 0) == 0;
            if (!zeroed || ftruncate(x.fd, (off_t)size) != 0) {
                fprintf(stderr, "jamext: %s: %s\n", o->image, strerror(errno));
                close(x.fd);
                return false;
            }
        } else if ((uint64_t)st.st_size < off + size) {
            zeroed = (uint64_t)st.st_size <= off;
            if (ftruncate(x.fd, (off_t)(off + size)) != 0) {
                fprintf(stderr, "jamext: %s: %s\n", o->image, strerror(errno));
                close(x.fd);
                return false;
            }
        }
    }
    x.base = off;
    x.bytes = size;
    seed_rng(&x.rng, o->seed);
    bool any = false;
    for (unsigned i = 0; i < sizeof(p->uuid); i++)
        any |= p->uuid[i] != 0;
    if (!any)
        for (unsigned i = 0; i < sizeof(p->uuid); i++)
            p->uuid[i] = (uint8_t)(next_random(&x.rng) >> 40);
    if (!p->uid && !p->gid)
        p->uid = p->gid = JX_USER;
    p->now = o->now;
    p->flags = zeroed ? EXT_MKFS_ZEROED : 0;
    void *mem = malloc(MKFS_MEM);
    struct ext_dev dev = device(&x, true);
    status_t rc = mem ? ext_mkfs(&dev, p, mem, MKFS_MEM) : ERR_NO_MEMORY;
    free(mem);
    if (rc == OK && fsync(x.fd) != 0 && errno != EINVAL && errno != ENOTSUP)
        rc = ERR_IO;
    close(x.fd);
    if (rc != OK) {
        jx_fail("mkfs", rc);
        return false;
    }
    return true;
}
