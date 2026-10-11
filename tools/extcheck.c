/* extcheck: the ext library (user/services/extfs/ext/, the same files the
 * extfs service is built from) on the Mac, built with ASan and UBSan.
 * `make check` runs it on the fixture images Linux's mke2fs made
 * (tools/extfix/: how, in make.sh; the images gzipped, unpacked by make
 * into build/extfix).
 *
 *     extcheck <fixtures dir> <images dir> [--fuzz N] [--seed S] [--save DIR]
 *
 * Six parts, each failing the run (exit 1, a line saying what) at the
 * first mistake:
 *   - the CRCs against their standard check values;
 *   - each fixture against its listing (<name>.list, made by
 *     tools/extfix/listing.py with Linux's debugfs when the image was
 *     made): every name reachable from the root, with its inode number,
 *     mode, owner, links, size, blocks, three times, and its bytes (a
 *     hash), link target or device numbers, exactly as debugfs read them,
 *     with a big cache and with the smallest; the orphan list; every
 *     bitmap's checksum; the shapes each fixture is there for (an extent
 *     tree one and two levels deep, an indexed directory, triple indirect
 *     blocks); and what may be done with it (fs->ro);
 *   - every structure's checksum broken alone (the superblock, a group
 *     descriptor, an inode, an extent node, a directory leaf, an index
 *     root, both bitmaps): refused with its reason; set again with the
 *     library's own _set function: taken. And every write path given back
 *     what it read (each inode, each descriptor, the superblock, the times)
 *     must change no byte, through ext_sync to the image;
 *   - every feature bit set alone on Jam OS's own fixture: refused,
 *     read-only or written, as docs/M13.6-PLAN.md's table says (the table
 *     here is a copy written from the plan, not the library's);
 *   - the writers (U2a's: tools/extwrite.c, extfile.c, extverify.c):
 *     blocks, inodes, extent trees, file writes and truncation on copies
 *     of the writable fixtures, the whole volume checked as they go;
 *     --save DIR keeps the images Linux's e2fsck -fn should pass;
 *   - the fuzz pass (extmutate.c): N runs (default 3000) of mutated
 *     images, from --seed. A run neither crashes (ASan, UBSan), reads
 *     outside the image, reads without end, nor returns with a buffer
 *     pinned. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jam/sha256.h>
#include "extcheck.h"
#include "extwrite.h"

#define FUZZ_DEFAULT 3000
#define MAX_LINES    4096
#define PATH_MAX_    4096

uint64_t rng = 0x9e3779b97f4a7c15ull;
const char *where = "";

void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "extcheck: FAIL %s: ", where);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

uint64_t below(uint64_t n)
{
    return n ? rnd() % n : 0;
}

/* ---- the image as a device -------------------------------------------------------------- */

void poke(struct img *m, uint64_t off, const void *p, size_t n)
{
    if (off > m->size || n > m->size - off)
        fail("poke past the image (%llu + %zu)", (unsigned long long)off, n);
    for (size_t i = 0; i < n; i++) {
        if (m->nundo == m->capundo) {
            m->capundo = m->capundo ? 2 * m->capundo : 4096;
            m->undo_off = realloc(m->undo_off, m->capundo * sizeof(*m->undo_off));
            m->undo_old = realloc(m->undo_old, m->capundo);
            if (!m->undo_off || !m->undo_old)
                fail("out of memory");
        }
        m->undo_off[m->nundo] = off + i;
        m->undo_old[m->nundo++] = m->bytes[off + i];
        m->bytes[off + i] = ((const uint8_t *)p)[i];
    }
}

void undo(struct img *m)
{
    while (m->nundo) {
        m->nundo--;
        m->bytes[m->undo_off[m->nundo]] = m->undo_old[m->nundo];
    }
    m->dev_bytes = m->size;
}

/* The library's promise: whole kibibytes, inside the device. */
static void check_io(struct img *m, uint64_t off, uint32_t len, const char *what)
{
    if (!len || off % 1024 || len % 1024)
        fail("%s of %u bytes at %llu: not whole kibibytes", what, len, (unsigned long long)off);
    if (off > m->dev_bytes || len > m->dev_bytes - off)
        fail("%s of %u bytes at %llu: outside the device of %llu bytes", what, len,
             (unsigned long long)off, (unsigned long long)m->dev_bytes);
}

static status_t dev_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    struct img *m = ctx;
    check_io(m, off, len, "read");
    if (++m->reads > m->read_limit)
        fail("more than %llu reads: a walk without end?", (unsigned long long)m->read_limit);
    if (m->reads == m->fail_at)
        return ERR_IO;
    memcpy(buf, m->bytes + off, len);
    return OK;
}

static status_t dev_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    struct img *m = ctx;
    check_io(m, off, len, "write");
    if (m->same_writes && memcmp(m->bytes + off, buf, len))
        fail("a write changed bytes at %llu that were written back unchanged",
             (unsigned long long)off);
    poke(m, off, buf, len);
    return OK;
}

status_t mount_img(struct img *m, struct ext_fs *fs, size_t bytes, uint32_t flags)
{
    struct ext_dev dev = { .ctx = m, .bytes = m->dev_bytes, .read = dev_read, .write = dev_write };
    m->reads = 0;
    return ext_mount(fs, &dev, m->cache, bytes, flags);
}

void check_pins(const struct ext_fs *fs)
{
    uint32_t n = ext_cache_pins(fs);
    if (n)
        fail("%u buffers left pinned", n);
}

static void load(struct img *m, const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.img", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END))
        fail("%s: %s", path, strerror(errno));
    long n = ftell(f);
    rewind(f);
    m->name = name;
    m->size = m->dev_bytes = (uint64_t)n;
    m->bytes = malloc((size_t)n);
    m->cache = malloc(CACHE_BYTES);
    if (!m->bytes || !m->cache || fread(m->bytes, 1, (size_t)n, f) != (size_t)n)
        fail("%s: can't read it", path);
    fclose(f);
    m->read_limit = 1000000;
}

/* ---- reading a file as listing.py does ------------------------------------------------- */

#define CHUNK 4096u

/* Is [off, off + n) of the file all hole? If so, *skip: where the hole ends. */
static bool all_hole(struct ext_fs *fs, struct ext_inode *in, uint64_t off, uint64_t n,
                     uint64_t *skip)
{
    uint64_t pos = off;
    while (pos < off + n) {
        uint64_t lblk = pos >> fs->block_log;
        if (lblk > UINT32_MAX) {
            *skip = UINT64_MAX;
            return true;
        }
        struct ext_run r;
        status_t st = ext_map(fs, in, (uint32_t)lblk, &r);
        if (st != OK)
            fail("inode %u: ext_map %llu: %d (%s)", in->ino, (unsigned long long)lblk, st,
                 fs->why ? fs->why : "");
        if (r.kind == EXT_RUN_DATA)
            return false;
        pos = (lblk + r.len) << fs->block_log;
    }
    *skip = pos;
    return true;
}

/* Read [off, off + n) in one to three pieces of random sizes. */
static void read_pieces(struct ext_fs *fs, struct ext_inode *in, uint64_t off, uint8_t *dst,
                        size_t n)
{
    while (n) {
        size_t take = below(3) ? n : 1 + below(n);
        size_t got;
        status_t st = ext_read(fs, in, off, dst, take, &got);
        if (st != OK || got != take)
            fail("inode %u: read %zu at %llu: %d, %zu (%s)", in->ino, take,
                 (unsigned long long)off, st, got, fs->why ? fs->why : "");
        check_pins(fs);
        off += take;
        dst += take;
        n -= take;
    }
}

void sparse_hash(struct ext_fs *fs, struct ext_inode *in, char out[65])
{
    static const uint8_t zero[CHUNK];
    uint8_t chunk[CHUNK];
    uint64_t size = ext_isize(in);
    struct sha256 s;
    sha256_init(&s);
    for (uint64_t i = 0; i < (size + CHUNK - 1) / CHUNK;) {
        uint64_t off = i * CHUNK, n = size - off < CHUNK ? size - off : CHUNK, skip;
        if (all_hole(fs, in, off, n, &skip)) {
            i = skip / CHUNK > i ? skip / CHUNK : i + 1;
            continue;
        }
        read_pieces(fs, in, off, chunk, (size_t)n);
        if (memcmp(chunk, zero, (size_t)n)) {
            uint8_t le[8];
            for (int k = 0; k < 8; k++)
                le[k] = (uint8_t)(i >> 8 * k);
            sha256_add(&s, le, 8);
            sha256_add(&s, chunk, (size_t)n);
        }
        i++;
    }
    uint8_t d[SHA256_BYTES];
    sha256_done(&s, d);
    sha256_hex(d, out);
}

/* Every run ext_map gives must agree with the map of each block in it
 * (both ends of a long hole): the same kind, the data's blocks counting
 * up; and the runs must tile the file with nothing between them. */
static void check_runs(struct ext_fs *fs, struct ext_inode *in)
{
    uint64_t end = (ext_isize(in) + fs->block_size - 1) >> fs->block_log;
    for (uint64_t lblk = 0; lblk < end && lblk <= UINT32_MAX;) {
        struct ext_run r, one;
        if (ext_map(fs, in, (uint32_t)lblk, &r) != OK || !r.len)
            fail("inode %u: no run at %llu", in->ino, (unsigned long long)lblk);
        uint64_t last = lblk + r.len - 1;
        for (uint64_t b = lblk; b <= last && b <= UINT32_MAX;
             b = r.kind == EXT_RUN_DATA || b == last || last - b < 64 ? b + 1 : last) {
            if (ext_map(fs, in, (uint32_t)b, &one) != OK || one.kind != r.kind ||
                (r.kind == EXT_RUN_DATA && (one.pblk != r.pblk + (b - lblk) ||
                                            one.len != r.len - (b - lblk))))
                fail("inode %u: block %llu disagrees with its run from %llu", in->ino,
                     (unsigned long long)b, (unsigned long long)lblk);
        }
        lblk = last + 1;
    }
    check_pins(fs);
}

/* ---- the listing ----------------------------------------------------------------------- */

struct lines {
    char *v[MAX_LINES];
    unsigned n;
    uint32_t orphans[16];
    unsigned norphans;
    unsigned xattr_blocks;        /* attribute blocks read (listing) */
};

static void add_line(struct lines *l, const char *s)
{
    if (l->n == MAX_LINES)
        fail("more than %d names", MAX_LINES);
    l->v[l->n++] = strdup(s);
}

static int cmp_line(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* A name or target as listing.py writes it. */
static void text(const char *s, size_t n, char *out, size_t cap)
{
    bool plain = true;
    for (size_t i = 0; i < n; i++)
        plain &= (uint8_t)s[i] > 0x20 && (uint8_t)s[i] < 0x7f;
    if (plain) {
        snprintf(out, cap, "%.*s", (int)n, s);
        return;
    }
    size_t at = (size_t)snprintf(out, cap, "hex:");
    for (size_t i = 0; i < n && at + 3 < cap; i++)
        at += (size_t)snprintf(out + at, cap - at, "%02x", (uint8_t)s[i]);
}

static const char kinds[EXT_FT_MAX] = { '?', 'f', 'd', 'c', 'b', 'p', 's', 'l' };

static void describe(struct ext_fs *fs, struct ext_inode *in, char *what, size_t cap)
{
    enum ext_ftype t = ext_itype(in);
    if (t == EXT_FT_REG || t == EXT_FT_DIR)
        check_runs(fs, in);
    if (t == EXT_FT_REG) {
        sparse_hash(fs, in, what);
    } else if (t == EXT_FT_SYMLINK) {
        char target[EXT_NAME_MAX * 16 + 1];
        size_t n;
        status_t st = ext_readlink(fs, in, target, sizeof(target), &n);
        if (st != OK)
            fail("readlink of %u: %d", in->ino, st);
        text(target, n, what, cap);
    } else if (t == EXT_FT_CHR || t == EXT_FT_BLK) {
        uint32_t major, minor;
        ext_irdev(in, &major, &minor);
        snprintf(what, cap, "%u,%u", major, minor);
    } else {
        snprintf(what, cap, "-");
    }
}

static void line_of(struct ext_fs *fs, const char *path, struct ext_inode *in, struct lines *l)
{
    char what[PATH_MAX_ * 2 + 8], line[PATH_MAX_ * 4], tm[3][40];
    static const enum ext_time_which order[3] = { EXT_ATIME, EXT_CTIME, EXT_MTIME };
    for (int i = 0; i < 3; i++) {
        struct ext_time t;
        ext_itime(in, order[i], &t);
        snprintf(tm[i], sizeof(tm[i]), "%lld.%u", (long long)t.sec, t.nsec);
    }
    describe(fs, in, what, sizeof(what));
    snprintf(line, sizeof(line), "%s\t%c\t%u\t%o\t%u\t%u\t%u\t%llu\t%llu\t%s\t%s\t%s\t%s", path,
             kinds[ext_itype(in)], in->ino, in->d.mode, ext_iuid(in), ext_igid(in),
             in->d.links_count, (unsigned long long)ext_isize(in),
             (unsigned long long)ext_iblocks512(fs, in), tm[0], tm[1], tm[2], what);
    add_line(l, line);
}

/* Every name under the root, as listing.py lists them. */
static void list_volume(struct ext_fs *fs, struct lines *l)
{
    static char paths[MAX_LINES][PATH_MAX_];
    uint32_t inos[MAX_LINES], seen[MAX_LINES];
    uint8_t types[MAX_LINES];
    unsigned top = 0, nseen = 0;
    snprintf(paths[0], PATH_MAX_, "/");
    types[top] = EXT_FT_DIR;
    inos[top++] = EXT_ROOT_INO;
    while (top) {
        top--;
        struct ext_inode in;
        status_t st = ext_iget(fs, inos[top], 0, &in);
        if (st != OK)
            fail("%s: inode %u: %d (%s)", paths[top], inos[top], st, fs->why ? fs->why : "");
        char path[PATH_MAX_];
        snprintf(path, sizeof(path), "%s", paths[top]);
        if (fs->filetype && types[top] != ext_itype(&in))
            fail("%s: its entry says type %u, its inode %u", path, types[top], ext_itype(&in));
        line_of(fs, path, &in, l);
        struct ext_buf *b;
        st = ext_xattr_block(fs, &in, &b);
        if (st == OK) {
            ext_brelse(fs, b);
            l->xattr_blocks++;
        } else if (st != ERR_NOT_FOUND) {
            fail("%s: its attribute block: %d (%s)", path, st, fs->why ? fs->why : "");
        }
        check_pins(fs);
        if (ext_itype(&in) != EXT_FT_DIR)
            continue;
        for (unsigned i = 0; i < nseen; i++)
            if (seen[i] == in.ino)
                fail("%s: a directory loop", path);
        seen[nseen++] = in.ino;
        struct ext_dentry d;
        for (uint64_t pos = 0;;) {
            st = ext_dir_next(fs, &in, &pos, &d);
            if (st != OK)
                fail("%s: readdir: %d (%s)", path, st, fs->why ? fs->why : "");
            check_pins(fs);
            if (!d.ino)
                break;
            if (!strcmp(d.name, ".") || !strcmp(d.name, ".."))
                continue;
            if (top == MAX_LINES)
                fail("too many names");
            char name[EXT_NAME_MAX * 2 + 8];
            text(d.name, d.name_len, name, sizeof(name));
            snprintf(paths[top], PATH_MAX_, "%s/%s", strcmp(path, "/") ? path : "", name);
            types[top] = d.type;
            inos[top++] = d.ino;
        }
    }
    qsort(l->v, l->n, sizeof(l->v[0]), cmp_line);
}

static void read_list(const char *dir, const char *name, struct lines *l)
{
    char path[1024], buf[PATH_MAX_ * 4];
    snprintf(path, sizeof(path), "%s/%s.list", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        fail("%s: %s", path, strerror(errno));
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = '\0';
        if (!strncmp(buf, "#orphans", 8)) {
            char *p = buf + 8;
            for (char *e; l->norphans < 16; p = e) {
                unsigned long v = strtoul(p, &e, 10);
                if (e == p)
                    break;
                l->orphans[l->norphans++] = (uint32_t)v;
            }
        } else if (buf[0] != '#' && buf[0]) {
            add_line(l, buf);
        }
    }
    fclose(f);
    qsort(l->v, l->n, sizeof(l->v[0]), cmp_line);
}

static void compare(const struct lines *want, const struct lines *got)
{
    unsigned n = want->n < got->n ? want->n : got->n;
    for (unsigned i = 0; i < n; i++)
        if (strcmp(want->v[i], got->v[i]))
            fail("listing differs:\n  debugfs: %s\n  ext:     %s", want->v[i], got->v[i]);
    if (want->n != got->n)
        fail("debugfs listed %u names, ext %u", want->n, got->n);
}

static void free_lines(struct lines *l)
{
    for (unsigned i = 0; i < l->n; i++)
        free(l->v[i]);
    l->n = 0;
}

/* ---- the fixtures ---------------------------------------------------------------------- */

struct fixture {
    const char *name;
    uint32_t block_size;
    uint32_t ro;                  /* what fs->ro must be */
    const char *tree;             /* a file whose extent tree is `depth` deep (NULL: none) */
    uint32_t depth;
    const char *indexed;          /* a directory read through an index (NULL: none) */
    const char *far;              /* a file whose last block is mapped through triple indirect */
    unsigned xattr_blocks;        /* names whose inode has an attribute block */
};

static const struct fixture fixtures[] = {
    { "jam4k", 4096, 0, "holes", 1, NULL, NULL, 0 },
    { "linux1k", 1024, 0, "deep", 2, "many", NULL, 1 },
    { "old2k", 2048, EXT_RO_FEATURES | EXT_RO_LACKS, NULL, 0, "many", "sparse5g", 0 },
};

void lookup(struct ext_fs *fs, const char *name, struct ext_inode *out)
{
    struct ext_inode root;
    struct ext_dentry d;
    status_t st = ext_iget(fs, EXT_ROOT_INO, 0, &root);
    if (st == OK)
        st = ext_dir_lookup(fs, &root, name, strlen(name), &d);
    if (st == OK)
        st = ext_iget(fs, d.ino, 0, out);
    if (st != OK)
        fail("/%s: %d (%s)", name, st, fs->why ? fs->why : "");
    check_pins(fs);
}

/* The shapes the fixture is there for. */
static void check_shapes(struct ext_fs *fs, const struct fixture *fx)
{
    struct ext_inode in;
    if (fx->tree) {
        lookup(fs, fx->tree, &in);
        struct ext_path p;
        status_t st = ext_extent_find(fs, &in, 0, &p);
        uint32_t depth = p.depth;
        ext_path_release(fs, &p);
        if (st != OK || depth != fx->depth)
            fail("/%s: an extent tree %u deep, not %u (%d)", fx->tree, depth, fx->depth, st);
    }
    if (fx->indexed) {
        lookup(fs, fx->indexed, &in);
        struct ext_buf *b;
        enum ext_dirblk kind;
        status_t st = ext_dir_block(fs, &in, 0, &b, &kind);
        if (st != OK || kind != EXT_DIRBLK_DX_ROOT)
            fail("/%s: not indexed (%d, kind %d)", fx->indexed, st, st == OK ? (int)kind : -1);
        ext_brelse(fs, b);
    }
    if (fx->far) {
        lookup(fs, fx->far, &in);
        uint64_t last = (ext_isize(&in) - 1) >> fs->block_log;
        uint64_t a = fs->addr_per_block;
        struct ext_run r;
        if (in.d.flags & EXT_EXTENTS_FL || last < 12 + a + a * a ||
            ext_map(fs, &in, (uint32_t)last, &r) != OK || r.kind != EXT_RUN_DATA)
            fail("/%s: its last block isn't triple indirect data", fx->far);
    }
    check_pins(fs);
}

static void check_bitmaps(struct ext_fs *fs)
{
    for (uint32_t g = 0; g < fs->groups; g++)
        for (int w = 0; w < 2; w++) {
            struct ext_buf *b;
            status_t st = ext_bitmap_read(fs, g, (enum ext_bitmap)w, &b);
            if (st == ERR_BAD_STATE)
                continue;
            if (st != OK)
                fail("group %u's %s bitmap: %d (%s)", g, w ? "inode" : "block", st,
                     fs->why ? fs->why : "");
            ext_brelse(fs, b);
        }
}

static void check_orphans(struct ext_fs *fs, const struct lines *want)
{
    uint32_t list[16], n;
    status_t st = ext_orphans(fs, list, 16, &n);
    if (st != OK)
        fail("orphans: %d (%s)", st, fs->why ? fs->why : "");
    if (n != want->norphans || memcmp(list, want->orphans, n * sizeof(list[0])))
        fail("orphans: %u of them, not the %u listed", n, want->norphans);
}

static void check_fixture(struct img *m, const struct fixture *fx, const char *listdir)
{
    struct lines want = { .n = 0 }, got = { .n = 0 };
    read_list(listdir, fx->name, &want);
    const size_t sizes[2] = { CACHE_BYTES, ext_mem_size(EXT_CACHE_MIN, fx->block_size) };
    for (int i = 0; i < 2; i++) {
        struct ext_fs fs;
        status_t st = mount_img(m, &fs, sizes[i], 0);
        if (st != OK)
            fail("mount: %d (%s)", st, fs.why ? fs.why : "");
        if (fs.block_size != fx->block_size || fs.ro != fx->ro)
            fail("block size %u, ro %#x: not %u, %#x", fs.block_size, fs.ro, fx->block_size,
                 fx->ro);
        got.xattr_blocks = 0;
        list_volume(&fs, &got);
        compare(&want, &got);
        if (got.xattr_blocks != fx->xattr_blocks)
            fail("%u attribute blocks, not %u", got.xattr_blocks, fx->xattr_blocks);
        free_lines(&got);
        check_orphans(&fs, &want);
        check_bitmaps(&fs);
        check_shapes(&fs, fx);
        check_pins(&fs);
    }
    free_lines(&want);
}

/* ---- the program ----------------------------------------------------------------------- */

static void check_crcs(void)
{
    where = "the CRCs";
    if (~ext_crc32c(~0u, "123456789", 9) != 0xE3069283u)
        fail("CRC32C of 123456789");
    if (ext_crc16(0, "123456789", 9) != 0xBB3Du)
        fail("CRC16 of 123456789");
    uint8_t odd[37];
    for (unsigned i = 0; i < sizeof(odd); i++)
        odd[i] = (uint8_t)(i * 7 + 1);
    uint32_t whole = ext_crc32c(~0u, odd, sizeof(odd));
    if (ext_crc32c(ext_crc32c(~0u, odd, 5), odd + 5, 32) != whole)
        fail("CRC32C in pieces");
}

int main(int argc, char **argv)
{
    unsigned nfuzz = FUZZ_DEFAULT;
    if (argc < 3)
        fail("usage: extcheck <fixtures dir> <images dir> [--fuzz N] [--seed S]");
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fuzz"))
            nfuzz = (unsigned)strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--seed"))
            rng = strtoull(argv[i + 1], NULL, 0) | 1;
        else if (!strcmp(argv[i], "--save"))
            save_dir = argv[i + 1];
        else
            fail("unknown option %s", argv[i]);
    }
    check_crcs();
    enum { N = sizeof(fixtures) / sizeof(fixtures[0]) };
    static struct img imgs[N];
    for (unsigned i = 0; i < N; i++) {
        load(&imgs[i], argv[2], fixtures[i].name);
        where = fixtures[i].name;
        check_fixture(&imgs[i], &fixtures[i], argv[1]);
        check_breaks(&imgs[i]);
        check_roundtrip(&imgs[i]);
    }
    check_features(&imgs[0]);
    for (unsigned i = 0; i < N; i++)
        check_writes(&imgs[i], fixtures[i].ro == 0);
    for (unsigned i = 0; i < N; i++)
        fuzz(&imgs[i], nfuzz / N + (i < nfuzz % N));
    printf("extcheck: %u fixtures, broken checksums, the feature table, the writers, "
           "%u fuzzed images: ok\n",
           (unsigned)N, nfuzz);
    return 0;
}
