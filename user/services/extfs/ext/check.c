/* ext: the read-only checker (ext_check.h has the model and the list of
 * what is checked). Four passes over a mounted volume, each reporting what
 * is wrong and going on where it can:
 *   1. the orphan list;
 *   2. the tree from the root: every name counted, every directory once;
 *   3. each group's inodes: bitmap, counts, link counts, and the blocks
 *      each inode owns (marked in a bitmap of the volume's blocks);
 *   4. each group's blocks: the owned blocks against the block bitmap, and
 *      the counts, the superblock's last.
 * The memory is the caller's (struct ext_check_io): per inode a link count,
 * a subdirectory count, a parent, an orphan byte and a place on the walk's
 * stack; per block a bit; per entry of the directory being read, a hash of
 * its name (for the names that appear twice). */
#include "ext_check.h"
#include "ext_names.h"

#define MAX_XATTR 64

struct xattr_use {
    uint64_t blk;
    uint32_t users, refcount;
};

struct chk {
    struct ext_fs *fs;
    const struct ext_check_io *io;
    struct ext_check_result *out;
    uint16_t *refs;         /* names naming each inode (saturating) */
    uint16_t *subdirs;      /* subdirectories of each directory (saturating) */
    uint32_t *parent;       /* each directory's parent (0: not reached) */
    uint8_t  *orphan;       /* on the orphan list */
    uint8_t  *owned;        /* a bit per block */
    uint32_t *stack;        /* the walk's directories to read */
    uint32_t  stack_cap;
    uint64_t *hashes;       /* the names of the directory being read */
    size_t    hash_cap;
    struct xattr_use xattr[MAX_XATTR];
    unsigned nxattr;
    bool exact;             /* every owned block is known */
    bool stop;              /* the problem limit or a cancel */
    bool cancelled;
};

static bool bit(const uint8_t *map, uint64_t i)
{
    return map[i / 8] >> (i % 8) & 1;
}

static void set(uint8_t *map, uint64_t i)
{
    map[i / 8] |= (uint8_t)(1u << (i % 8));
}

/* A problem found. After the limit the check stops (ext_check's passes
 * look at c->stop between steps). */
static void bad(struct chk *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bad(struct chk *c, const char *fmt, ...)
{
    if (c->stop)
        return;
    va_list ap;
    va_start(ap, fmt);
    c->io->report(c->io->ctx, fmt, ap);
    va_end(ap);
    c->out->problems++;
    if (c->out->problems >= EXT_CHECK_MAX_PROBLEMS) {
        c->stop = true;
        c->out->stopped = true;
    }
}

/* The library says why the last call failed. */
static const char *why(const struct chk *c)
{
    return c->fs->why ? c->fs->why : "no reason given";
}

static bool cancelled(struct chk *c)
{
    if (c->io->cancel && c->io->cancel(c->io->ctx)) {
        c->stop = true;
        c->cancelled = true;
    }
    return c->stop;
}

static void own(struct chk *c, uint64_t blk, uint64_t n, uint32_t ino)
{
    if (blk > c->fs->blocks || n > c->fs->blocks - blk) {
        bad(c, "inode %u: blocks %llu+%llu outside the volume", ino, (unsigned long long)blk,
            (unsigned long long)n);
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        if (bit(c->owned, blk + i)) {
            bad(c, "block %llu owned twice (inode %u)", (unsigned long long)(blk + i), ino);
            continue;
        }
        set(c->owned, blk + i);
    }
}

/* ---- the tree ---------------------------------------------------------------------------- */

static void bump(uint16_t *n)
{
    if (*n != UINT16_MAX)
        ++*n;
}

static uint64_t name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a */
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

/* Heap sort: the names' hashes in order (no recursion, no memory). */
static void sift(uint64_t *a, size_t root, size_t n)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && a[child] < a[child + 1])
            child++;
        if (a[root] >= a[child])
            return;
        uint64_t t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

static void sort_hashes(uint64_t *a, size_t n)
{
    for (size_t i = n / 2; i-- > 0;)
        sift(a, i, n);
    for (size_t end = n; end-- > 1;) {
        uint64_t t = a[0];
        a[0] = a[end];
        a[end] = t;
        sift(a, 0, end);
    }
}

/* Room for a directory's names: the most entries its bytes can hold. */
static bool hash_room(struct chk *c, const struct ext_inode *dir)
{
    size_t want = (size_t)(ext_isize(dir) / 12) + 1;
    if (want <= c->hash_cap)
        return true;
    if (want > (size_t)1 << 28)
        want = (size_t)1 << 28;
    uint64_t *p = c->io->alloc(c->io->ctx, want * sizeof(*p));
    if (!p)
        return false;
    if (c->hashes)
        c->io->release(c->io->ctx, c->hashes);
    c->hashes = p;
    c->hash_cap = want;
    return true;
}

/* An entry other than "." and "..": counted, and a directory pushed. */
static void entry(struct chk *c, struct ext_inode *dir, const struct ext_dentry *d)
{
    struct ext_fs *fs = c->fs;
    struct ext_inode in;
    if (d->ino > fs->inodes) {
        bad(c, "dir %u: %s names inode %u, past the volume's %u", dir->ino, d->name, d->ino,
            fs->inodes);
        return;
    }
    if (ext_iget(fs, d->ino, 0, &in) != OK) {
        bad(c, "dir %u: %s names inode %u, not in use (%s)", dir->ino, d->name, d->ino, why(c));
        return;
    }
    if (fs->filetype && d->type != ext_itype(&in))
        bad(c, "dir %u: %s's type %u, its inode's %u", dir->ino, d->name, d->type,
            ext_itype(&in));
    bump(&c->refs[d->ino]);
    if (ext_itype(&in) != EXT_FT_DIR)
        return;
    if (c->parent[d->ino]) {
        bad(c, "directory %u has two names (%s in directory %u)", d->ino, d->name, dir->ino);
        return;
    }
    c->parent[d->ino] = dir->ino;
    bump(&c->subdirs[dir->ino]);
    if (c->stack_cap == 0 || c->stack[0] >= c->stack_cap) {
        bad(c, "more directories than the volume has inodes in use");
        c->stop = true;
        return;
    }
    c->stack[++c->stack[0]] = d->ino;
}

static void walk_dir(struct chk *c, struct ext_inode *dir)
{
    struct ext_fs *fs = c->fs;
    if (!hash_room(c, dir)) {
        bad(c, "directory %u: out of memory for its names", dir->ino);
        c->stop = true;
        return;
    }
    size_t n = 0;
    struct ext_dentry d;
    bool any = false;
    uint64_t last = 0;
    for (uint64_t pos = 0;;) {
        if (ext_dir_next(fs, dir, &pos, &d) != OK) {
            bad(c, "dir %u: readdir (%s)", dir->ino, why(c));
            break;
        }
        if (!d.ino)
            break;
        if (any && d.pos <= last) {
            bad(c, "dir %u: readdir went back", dir->ino);
            break;
        }
        any = true;
        last = d.pos;
        if (!__builtin_strcmp(d.name, ".") || !__builtin_strcmp(d.name, "..")) {
            uint32_t want = d.name[1] ? c->parent[dir->ino] : dir->ino;
            if (d.ino != want)
                bad(c, "dir %u: %s is %u, not %u", dir->ino, d.name, d.ino, want);
            continue;
        }
        entry(c, dir, &d);
        if (n < c->hash_cap)
            c->hashes[n++] = name_hash(d.name);
        if (c->stop)
            return;
    }
    sort_hashes(c->hashes, n);
    for (size_t i = 1; i < n; i++)
        if (c->hashes[i - 1] == c->hashes[i])
            bad(c, "dir %u: a name twice (hash %016llx)", dir->ino,
                (unsigned long long)c->hashes[i]);
    c->out->names += (uint32_t)n;
}

static void walk_tree(struct chk *c)
{
    struct ext_fs *fs = c->fs;
    c->parent[EXT_ROOT_INO] = EXT_ROOT_INO;
    c->stack[0] = 1;                 /* the stack's top is its slot 0 */
    c->stack[1] = EXT_ROOT_INO;
    while (c->stack[0] && !cancelled(c)) {
        struct ext_inode dir;
        uint32_t ino = c->stack[c->stack[0]--];
        if (ext_iget(fs, ino, 0, &dir) != OK) {
            bad(c, "directory %u: %s", ino, why(c));
            continue;
        }
        if (ext_isize(&dir) % fs->block_size) {
            bad(c, "directory %u: size %llu is not whole blocks", ino,
                (unsigned long long)ext_isize(&dir));
            continue;
        }
        walk_dir(c, &dir);
        c->out->dirs++;
    }
}

/* ---- an inode's blocks ------------------------------------------------------------------- */

/* Own every block of an extent tree; their count. */
static uint64_t own_tree(struct chk *c, struct ext_inode *in)
{
    struct ext_fs *fs = c->fs;
    uint64_t node[EXT_EXTENT_MAX_DEPTH + 1] = { 0 }, n = 0, lblk = 0;
    while (!c->stop) {
        struct ext_path p;
        if (ext_extent_find(fs, in, (uint32_t)lblk, &p) != OK) {
            ext_path_release(fs, &p);
            bad(c, "inode %u: extent tree (%s)", in->ino, why(c));
            return n;
        }
        struct ext_extent_header *h = p.lv[p.depth].h;
        for (uint32_t i = 0; i < h->entries; i++) {
            own(c, ext_extent_start(&ext_extents(h)[i]), ext_extent_len(&ext_extents(h)[i]),
                in->ino);
            n += ext_extent_len(&ext_extents(h)[i]);
        }
        for (uint32_t lv = 1; lv <= p.depth; lv++)
            if (node[lv] != p.lv[lv].buf->blk) {
                node[lv] = p.lv[lv].buf->blk;
                own(c, node[lv], 1, in->ino);
                n++;
            }
        uint64_t next = p.bound;
        ext_path_release(fs, &p);
        if (!next)
            return n;
        if (next <= lblk) {
            bad(c, "inode %u: extent tree goes back", in->ino);
            return n;
        }
        lblk = next;
    }
    return n;
}

/* A block-mapped file's data (its map blocks can't be seen: not exact). */
static void own_mapped(struct chk *c, struct ext_inode *in)
{
    struct ext_fs *fs = c->fs;
    uint64_t end = (ext_isize(in) + fs->block_size - 1) >> fs->block_log;
    for (uint64_t lblk = 0; lblk < end && lblk <= UINT32_MAX && !c->stop;) {
        struct ext_run r;
        if (ext_map(fs, in, (uint32_t)lblk, &r) != OK || !r.len) {
            bad(c, "inode %u: block map (%s)", in->ino, why(c));
            break;
        }
        if (r.kind == EXT_RUN_DATA)
            own(c, r.pblk, r.len, in->ino);
        lblk += r.len;
    }
    c->exact = false;
}

static void use_xattr(struct chk *c, struct ext_inode *in)
{
    struct ext_buf *b;
    status_t st = ext_xattr_block(c->fs, in, &b);
    if (st == ERR_NOT_FOUND)
        return;
    if (st != OK) {
        bad(c, "inode %u: attribute block (%s)", in->ino, why(c));
        return;
    }
    uint64_t blk = b->blk;
    uint32_t refcount = ((const struct ext_xattr_header *)b->data)->refcount;
    ext_brelse(c->fs, b);
    for (unsigned i = 0; i < c->nxattr; i++)
        if (c->xattr[i].blk == blk) {
            c->xattr[i].users++;
            return;
        }
    if (c->nxattr == MAX_XATTR) {
        bad(c, "more than %d attribute blocks: the rest are not counted", MAX_XATTR);
        c->exact = false;
        return;
    }
    c->xattr[c->nxattr++] = (struct xattr_use){ .blk = blk, .users = 1, .refcount = refcount };
    own(c, blk, 1, in->ino);
}

/* Own in's blocks; check its i_blocks when they are all known. */
static void own_inode(struct chk *c, struct ext_inode *in)
{
    struct ext_fs *fs = c->fs;
    enum ext_ftype t = ext_itype(in);
    bool fast = t == EXT_FT_SYMLINK && !(in->d.flags & EXT_EXTENTS_FL) &&
                ext_isize(in) < sizeof(in->d.block);
    uint64_t n = 0;
    use_xattr(c, in);
    bool xattr = in->d.file_acl_lo || (fs->is64 && in->d.file_acl_high);
    if (in->d.flags & EXT_EXTENTS_FL)
        n = own_tree(c, in);
    else if (!fast && (t == EXT_FT_REG || t == EXT_FT_DIR || t == EXT_FT_SYMLINK) &&
             ext_iblocks512(fs, in) > (xattr ? fs->block_size / 512 : 0))
        return own_mapped(c, in);
    if ((n + xattr) * (fs->block_size / 512) != ext_iblocks512(fs, in))
        bad(c, "inode %u: i_blocks %llu, owns %llu blocks", in->ino,
            (unsigned long long)ext_iblocks512(fs, in), (unsigned long long)(n + xattr));
}

/* ---- inodes ------------------------------------------------------------------------------ */

/* The links inode `in` (in use, read) must have. */
static void check_links(struct chk *c, struct ext_inode *in)
{
    uint32_t ino = in->ino, want = c->refs[ino];
    if (ext_itype(in) == EXT_FT_DIR && c->refs[ino])
        want = 2 + c->subdirs[ino];
    if (c->orphan[ino] && !in->d.links_count && !c->refs[ino])
        return;
    if (!c->refs[ino]) {
        bad(c, "inode %u in use, with no name and not an orphan", ino);
        return;
    }
    bool many = ext_itype(in) == EXT_FT_DIR && in->d.links_count == 1 && want > EXT_LINK_MAX;
    if (in->d.links_count != want && !many)
        bad(c, "inode %u: %u links, %u counted", ino, in->d.links_count, want);
}

/* Group g's inodes: bits, counts, the used part. */
static void check_inode_group(struct chk *c, uint32_t g)
{
    struct ext_fs *fs = c->fs;
    struct ext_group gr;
    struct ext_buf *b = NULL;
    if (ext_group_get(fs, g, &gr) != OK) {
        bad(c, "group %u: descriptor (%s)", g, why(c));
        return;
    }
    status_t st = ext_bitmap_read(fs, g, EXT_BITMAP_INODE, &b);
    if (st != OK && st != ERR_BAD_STATE) {
        bad(c, "group %u: inode bitmap (%s)", g, why(c));
        return;
    }
    uint32_t used = 0, dirs = 0;
    for (uint32_t i = 0; i < fs->inodes_per_group && !c->stop; i++) {
        uint32_t ino = g * fs->inodes_per_group + i + 1;
        bool on = b && bit(b->data, i), wanted = c->refs[ino] || c->orphan[ino];
        if (ino < fs->first_ino && !on && b)
            bad(c, "reserved inode %u: its bit is clear", ino);
        if (ino >= fs->first_ino && on != wanted)
            bad(c, "inode %u: bit %d, but %u names and %s the orphan list", ino, on, c->refs[ino],
                c->orphan[ino] ? "on" : "not on");
        if (!on)
            continue;
        used++;
        struct ext_inode in;
        if (i >= fs->inodes_per_group - gr.itable_unused)
            bad(c, "inode %u: in use past the group's used part", ino);
        if (ext_iget(fs, ino, EXT_IGET_UNLINKED, &in) != OK) {
            if (ino >= fs->first_ino)
                bad(c, "inode %u: bit set, not readable (%s)", ino, why(c));
            continue;
        }
        dirs += ext_itype(&in) == EXT_FT_DIR;
        if (ino >= fs->first_ino)
            check_links(c, &in);
        if (ino >= fs->first_ino || ino == fs->sb.journal_inum || ino == EXT_ROOT_INO)
            own_inode(c, &in);
        c->out->inodes++;
    }
    for (uint32_t i = fs->inodes_per_group; b && i < fs->block_size * 8; i++)
        if (!bit(b->data, i)) {
            bad(c, "group %u: inode bitmap padding clear at %u", g, i);
            break;
        }
    if (b)
        ext_brelse(fs, b);
    if (gr.free_inodes != fs->inodes_per_group - used || gr.used_dirs != dirs)
        bad(c, "group %u: %u free inodes, %u directories; counted %u, %u", g, gr.free_inodes,
            gr.used_dirs, fs->inodes_per_group - used, dirs);
    c->out->free_inodes += gr.free_inodes;
}

/* ---- blocks ------------------------------------------------------------------------------ */

/* The groups' own metadata: superblock copies, descriptors, the resize
 * reserve, bitmaps, inode tables. */
static void own_metadata(struct chk *c)
{
    struct ext_fs *fs = c->fs;
    bool resize = fs->sb.feature_compat & EXT_COMPAT_RESIZE_INODE;
    for (uint32_t g = 0; g < fs->groups && !c->stop; g++) {
        struct ext_group gr;
        if (ext_group_get(fs, g, &gr) != OK) {
            bad(c, "group %u: descriptor (%s)", g, why(c));
            c->exact = false;
            continue;
        }
        if (ext_group_has_super(fs, g))
            own(c, ext_group_first(fs, g),
                1 + fs->gdt_blocks + (resize ? fs->sb.reserved_gdt_blocks : 0), 0);
        own(c, gr.block_bitmap, 1, 0);
        own(c, gr.inode_bitmap, 1, 0);
        own(c, gr.inode_table, fs->itable_blocks, 0);
    }
    if (fs->block_size == 1024)
        set(c->owned, 0);
    if (resize)
        c->exact = false;   /* the reserve's map blocks */
}

static void check_block_group(struct chk *c, uint32_t g)
{
    struct ext_fs *fs = c->fs;
    struct ext_group gr;
    struct ext_buf *b;
    if (ext_group_get(fs, g, &gr) != OK) {
        bad(c, "group %u: descriptor (%s)", g, why(c));
        return;
    }
    uint64_t first = ext_group_first(fs, g);
    uint32_t n = ext_group_blocks(fs, g), owned = 0, on = 0;
    for (uint32_t i = 0; i < n; i++)
        owned += bit(c->owned, first + i);
    status_t st = ext_bitmap_read(fs, g, EXT_BITMAP_BLOCK, &b);
    if (st == ERR_BAD_STATE) {   /* never written: only metadata may be in it */
        if (gr.free_blocks != n - owned)
            bad(c, "group %u (never written): %u free, %u owned of %u", g, gr.free_blocks, owned,
                n);
        c->out->free_blocks += gr.free_blocks;
        return;
    }
    if (st != OK) {
        bad(c, "group %u: block bitmap (%s)", g, why(c));
        return;
    }
    uint32_t reported = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool set_ = bit(b->data, i), mine = bit(c->owned, first + i);
        on += set_;
        if (reported < 3 && mine && !set_) {
            bad(c, "block %llu is used but free in its bitmap", (unsigned long long)(first + i));
            reported++;
        }
        if (reported < 3 && set_ && !mine && c->exact) {
            bad(c, "block %llu is set in its bitmap, used by nothing",
                (unsigned long long)(first + i));
            reported++;
        }
    }
    for (uint32_t i = n; i < fs->block_size * 8; i++)
        if (!bit(b->data, i)) {
            bad(c, "group %u: block bitmap padding clear at %u", g, i);
            break;
        }
    ext_brelse(fs, b);
    if (gr.free_blocks != n - on)
        bad(c, "group %u: %u free blocks, its bitmap %u", g, gr.free_blocks, n - on);
    c->out->free_blocks += gr.free_blocks;
}

/* ---- the whole volume -------------------------------------------------------------------- */

static void check_orphan_list(struct chk *c)
{
    struct ext_fs *fs = c->fs;
    uint32_t cap = 1024, n = 0;
    uint32_t *list = c->io->alloc(c->io->ctx, cap * sizeof(*list));
    if (!list) {
        bad(c, "out of memory for the orphan list");
        c->stop = true;
        return;
    }
    status_t st = ext_orphans(fs, list, cap, &n);
    if (st != OK) {
        bad(c, "orphan list (%s)", why(c));
        n = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (list[i] > fs->inodes) {
            bad(c, "orphan list: inode %u is past the volume's %u", list[i], fs->inodes);
            continue;
        }
        if (c->orphan[list[i]])
            bad(c, "inode %u twice on the orphan list", list[i]);
        c->orphan[list[i]] = 1;
    }
    c->out->orphans = n;
    c->io->release(c->io->ctx, list);
}

static void release_all(struct chk *c)
{
    void *const mem[] = { c->refs, c->subdirs, c->parent, c->orphan, c->owned, c->stack,
                          c->hashes };
    for (unsigned i = 0; i < sizeof(mem) / sizeof(mem[0]); i++)
        if (mem[i])
            c->io->release(c->io->ctx, mem[i]);
}

status_t ext_check(struct ext_fs *fs, const struct ext_check_io *io, struct ext_check_result *out)
{
    struct chk c = { .fs = fs, .io = io, .out = out, .exact = true };
    *out = (struct ext_check_result){ 0 };
    size_t ninodes = (size_t)fs->inodes + 1;
    c.refs = io->alloc(io->ctx, ninodes * sizeof(uint16_t));
    c.subdirs = io->alloc(io->ctx, ninodes * sizeof(uint16_t));
    c.parent = io->alloc(io->ctx, ninodes * sizeof(uint32_t));
    c.orphan = io->alloc(io->ctx, ninodes);
    c.owned = io->alloc(io->ctx, (size_t)(fs->blocks / 8 + 1));
    /* In use: at most all of them (the groups' free counts are checked
     * later; this is the walk's own limit). Slot 0 is the stack's top. */
    c.stack_cap = fs->inodes < UINT32_MAX - 2 ? fs->inodes + 1 : fs->inodes;
    c.stack = io->alloc(io->ctx, ((size_t)c.stack_cap + 1) * sizeof(uint32_t));
    if (!c.refs || !c.subdirs || !c.parent || !c.orphan || !c.owned || !c.stack) {
        bad(&c, "out of memory (the check needs about %llu MiB)",
            (unsigned long long)((ninodes * 13 + fs->blocks / 8) >> 20) + 1);
        release_all(&c);
        return ERR_NO_MEMORY;
    }
    check_orphan_list(&c);
    if (!c.stop)
        walk_tree(&c);
    if (!c.stop)
        own_metadata(&c);
    for (uint32_t g = 0; g < fs->groups && !cancelled(&c); g++)
        check_inode_group(&c, g);
    for (unsigned i = 0; i < c.nxattr && !c.stop; i++)
        if (c.xattr[i].users != c.xattr[i].refcount)
            bad(&c, "attribute block %llu: refcount %u, %u users",
                (unsigned long long)c.xattr[i].blk, c.xattr[i].refcount, c.xattr[i].users);
    for (uint32_t g = 0; g < fs->groups && !cancelled(&c); g++)
        check_block_group(&c, g);
    if (!c.stop) {
        uint64_t sb_free = fs->sb.free_blocks_count_lo |
                           (fs->is64 ? (uint64_t)fs->sb.free_blocks_count_hi << 32 : 0);
        if (sb_free != out->free_blocks || fs->sb.free_inodes_count != out->free_inodes)
            bad(&c, "superblock: %llu free blocks, %u free inodes; the groups %llu, %u",
                (unsigned long long)sb_free, fs->sb.free_inodes_count,
                (unsigned long long)out->free_blocks, out->free_inodes);
    }
    out->exact = c.exact;
    release_all(&c);
    return c.cancelled ? ERR_CANCELED : OK;
}
