/* extinv: a volume's invariants, checked the way a read-only fsck would,
 * for extcheck's writing tests (tools/extnames.c) on the Mac, where there
 * is no e2fsck. Each fails the run at the first broken one:
 *   - the tree: every directory reached once from the root, its "."
 *     itself and its ".." its parent, its names unique, each entry's type
 *     its inode's, readdir's offsets rising;
 *   - link counts: a file's is its names, a directory's 2 and one per
 *     subdirectory; an inode with no names is on the orphan list with no
 *     links (one on the list with links is a truncation: its count must
 *     still be right);
 *   - inodes: a bit set in the inode bitmap for exactly the reserved
 *     inodes, the named ones and the orphans; each in use inside its
 *     group's used part (itable_unused); each group's free inodes and
 *     directories counted, the superblock's free inodes their sum; the
 *     bitmap's padding set;
 *   - blocks: each owned once (data and extent tree nodes; an attribute
 *     block by exactly its refcount of inodes), each extent inode's
 *     i_blocks what it owns, every owned block set in its group's bitmap
 *     and, when every block's owner is known (no block-mapped file with
 *     blocks, no resize inode), nothing else set but the groups'
 *     metadata; each group's free blocks counted, the superblock's their
 *     sum; the padding set.
 * The library's own checks (every checksum) run as each block is read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ext_names.h>
#include "extcheck.h"

#define MAX_XATTR 64

struct xattr_use {
    uint64_t blk;
    uint32_t users, refcount;
};

struct inv_state {
    struct ext_fs *fs;
    uint32_t *refs;         /* names naming each inode */
    uint32_t *subdirs;      /* subdirectories of each directory */
    uint32_t *parent;       /* each directory's parent (0: not reached) */
    uint8_t  *orphan;       /* on the orphan list */
    uint8_t  *owned;        /* a bit per block */
    struct xattr_use xattr[MAX_XATTR];
    unsigned nxattr;
    bool exact;             /* every owned block is known */
    struct inv *out;
};

static bool bit(const uint8_t *map, uint64_t i)
{
    return map[i / 8] >> (i % 8) & 1;
}

static void set(uint8_t *map, uint64_t i)
{
    map[i / 8] |= (uint8_t)(1u << (i % 8));
}

static void own(struct inv_state *s, uint64_t blk, uint64_t n, uint32_t ino)
{
    for (uint64_t i = 0; i < n; i++) {
        if (blk + i >= s->fs->blocks)
            fail("inode %u: block %llu outside the volume", ino, (unsigned long long)(blk + i));
        if (bit(s->owned, blk + i))
            fail("block %llu owned twice (inode %u)", (unsigned long long)(blk + i), ino);
        set(s->owned, blk + i);
    }
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---- the tree ---------------------------------------------------------------------------- */

/* An entry other than "." and "..": counted, and a directory pushed. */
static void entry(struct inv_state *s, struct ext_inode *dir, const struct ext_dentry *d,
                  uint32_t *stack, uint32_t *top)
{
    struct ext_fs *fs = s->fs;
    struct ext_inode in;
    if (ext_iget(fs, d->ino, 0, &in) != OK)
        fail("dir %u: %s names inode %u, not in use", dir->ino, d->name, d->ino);
    if (fs->filetype && d->type != ext_itype(&in))
        fail("dir %u: %s's type %u, its inode's %u", dir->ino, d->name, d->type, ext_itype(&in));
    s->refs[d->ino]++;
    if (ext_itype(&in) != EXT_FT_DIR)
        return;
    if (s->parent[d->ino])
        fail("directory %u has two names", d->ino);
    s->parent[d->ino] = dir->ino;
    s->subdirs[dir->ino]++;
    stack[(*top)++] = d->ino;
}

static void walk_dir(struct inv_state *s, struct ext_inode *dir, uint32_t *stack, uint32_t *top)
{
    struct ext_fs *fs = s->fs;
    size_t cap = 64, n = 0;
    char **names = malloc(cap * sizeof(*names));
    struct ext_dentry d;
    bool any = false;
    uint64_t last = 0;
    for (uint64_t pos = 0;;) {
        if (ext_dir_next(fs, dir, &pos, &d) != OK)
            fail("dir %u: readdir (%s)", dir->ino, fs->why ? fs->why : "");
        if (!d.ino)
            break;
        if (any && d.pos <= last)
            fail("dir %u: readdir went back", dir->ino);
        any = true;
        last = d.pos;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) {
            uint32_t want = d.name[1] ? s->parent[dir->ino] : dir->ino;
            if (d.ino != want)
                fail("dir %u: %s is %u, not %u", dir->ino, d.name, d.ino, want);
            continue;
        }
        entry(s, dir, &d, stack, top);
        if (n == cap)
            names = realloc(names, (cap *= 2) * sizeof(*names));
        names[n++] = strdup(d.name);
    }
    qsort(names, n, sizeof(*names), cmp_names);
    for (size_t i = 1; i < n; i++)
        if (!strcmp(names[i - 1], names[i]))
            fail("dir %u: the name %s twice", dir->ino, names[i]);
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    s->out->names += (uint32_t)n;
}

static void walk_tree(struct inv_state *s)
{
    struct ext_fs *fs = s->fs;
    uint32_t *stack = malloc((fs->inodes + 1) * sizeof(*stack)), top = 0;
    s->parent[EXT_ROOT_INO] = EXT_ROOT_INO;
    stack[top++] = EXT_ROOT_INO;
    while (top) {
        struct ext_inode dir;
        uint32_t ino = stack[--top];
        if (ext_iget(fs, ino, 0, &dir) != OK)
            fail("directory %u: %s", ino, fs->why ? fs->why : "not in use");
        if (ext_isize(&dir) % fs->block_size)
            fail("directory %u: size %llu", ino, (unsigned long long)ext_isize(&dir));
        walk_dir(s, &dir, stack, &top);
        s->out->dirs++;
        check_pins(fs);
    }
    free(stack);
}

/* ---- an inode's blocks ------------------------------------------------------------------- */

/* Own every block of an extent tree; their count. */
static uint64_t own_tree(struct inv_state *s, struct ext_inode *in)
{
    struct ext_fs *fs = s->fs;
    uint64_t node[EXT_EXTENT_MAX_DEPTH + 1] = { 0 }, n = 0, lblk = 0;
    for (;;) {
        struct ext_path p;
        if (ext_extent_find(fs, in, (uint32_t)lblk, &p) != OK)
            fail("inode %u: extent tree (%s)", in->ino, fs->why ? fs->why : "");
        struct ext_extent_header *h = p.lv[p.depth].h;
        for (uint32_t i = 0; i < h->entries; i++) {
            own(s, ext_extent_start(&ext_extents(h)[i]), ext_extent_len(&ext_extents(h)[i]),
                in->ino);
            n += ext_extent_len(&ext_extents(h)[i]);
        }
        for (uint32_t lv = 1; lv <= p.depth; lv++)
            if (node[lv] != p.lv[lv].buf->blk) {
                node[lv] = p.lv[lv].buf->blk;
                own(s, node[lv], 1, in->ino);
                n++;
            }
        uint64_t next = p.bound;
        ext_path_release(fs, &p);
        if (!next)
            return n;
        if (next <= lblk)
            fail("inode %u: extent tree goes back", in->ino);
        lblk = next;
    }
}

/* A block-mapped file's data (its map blocks can't be seen: not exact). */
static void own_mapped(struct inv_state *s, struct ext_inode *in)
{
    struct ext_fs *fs = s->fs;
    uint64_t end = (ext_isize(in) + fs->block_size - 1) >> fs->block_log;
    for (uint64_t lblk = 0; lblk < end && lblk <= UINT32_MAX;) {
        struct ext_run r;
        if (ext_map(fs, in, (uint32_t)lblk, &r) != OK || !r.len)
            fail("inode %u: map", in->ino);
        if (r.kind == EXT_RUN_DATA)
            own(s, r.pblk, r.len, in->ino);
        lblk += r.len;
    }
    s->exact = false;
}

static void use_xattr(struct inv_state *s, struct ext_inode *in)
{
    struct ext_buf *b;
    status_t st = ext_xattr_block(s->fs, in, &b);
    if (st == ERR_NOT_FOUND)
        return;
    if (st != OK)
        fail("inode %u: attribute block", in->ino);
    uint64_t blk = b->blk;
    uint32_t refcount = ((const struct ext_xattr_header *)b->data)->refcount;
    ext_brelse(s->fs, b);
    for (unsigned i = 0; i < s->nxattr; i++)
        if (s->xattr[i].blk == blk) {
            s->xattr[i].users++;
            return;
        }
    if (s->nxattr == MAX_XATTR)
        fail("too many attribute blocks");
    s->xattr[s->nxattr++] = (struct xattr_use){ .blk = blk, .users = 1, .refcount = refcount };
    own(s, blk, 1, in->ino);
}

/* Own in's blocks; check its i_blocks when they are all known. */
static void own_inode(struct inv_state *s, struct ext_inode *in)
{
    struct ext_fs *fs = s->fs;
    enum ext_ftype t = ext_itype(in);
    bool fast = t == EXT_FT_SYMLINK && !(in->d.flags & EXT_EXTENTS_FL) &&
                ext_isize(in) < sizeof(in->d.block);
    uint64_t n = 0;
    use_xattr(s, in);
    bool xattr = in->d.file_acl_lo || (fs->is64 && in->d.file_acl_high);
    if (in->d.flags & EXT_EXTENTS_FL)
        n = own_tree(s, in);
    else if (!fast && (t == EXT_FT_REG || t == EXT_FT_DIR || t == EXT_FT_SYMLINK) &&
             ext_iblocks512(fs, in) > (xattr ? fs->block_size / 512 : 0))
        return own_mapped(s, in);
    if ((n + xattr) * (fs->block_size / 512) != ext_iblocks512(fs, in))
        fail("inode %u: i_blocks %llu, owns %llu blocks", in->ino,
             (unsigned long long)ext_iblocks512(fs, in), (unsigned long long)(n + xattr));
}

/* ---- inodes ------------------------------------------------------------------------------ */

/* The links inode `ino` (in use, read) must have. */
static void check_links(struct inv_state *s, struct ext_inode *in)
{
    uint32_t ino = in->ino, want = s->refs[ino];
    if (ext_itype(in) == EXT_FT_DIR && s->refs[ino])
        want = 2 + s->subdirs[ino];
    if (s->orphan[ino] && !in->d.links_count && !s->refs[ino])
        return;
    if (!s->refs[ino])
        fail("inode %u in use, with no name and not an orphan", ino);
    bool many = ext_itype(in) == EXT_FT_DIR && in->d.links_count == 1 && want > EXT_LINK_MAX;
    if (in->d.links_count != want && !many)
        fail("inode %u: %u links, %u counted", ino, in->d.links_count, want);
}

/* Group g's inodes: bits, counts, the used part. */
static void check_inode_group(struct inv_state *s, uint32_t g)
{
    struct ext_fs *fs = s->fs;
    struct ext_group gr;
    struct ext_buf *b = NULL;
    if (ext_group_get(fs, g, &gr) != OK)
        fail("group %u: descriptor", g);
    status_t st = ext_bitmap_read(fs, g, EXT_BITMAP_INODE, &b);
    if (st != OK && st != ERR_BAD_STATE)
        fail("group %u: inode bitmap (%s)", g, fs->why ? fs->why : "");
    uint32_t used = 0, dirs = 0;
    for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
        uint32_t ino = g * fs->inodes_per_group + i + 1;
        bool on = b && bit(b->data, i), wanted = s->refs[ino] || s->orphan[ino];
        if (ino < fs->first_ino && !on && b)
            fail("reserved inode %u: its bit is clear", ino);
        if (ino >= fs->first_ino && on != wanted)
            fail("inode %u: bit %d, but %u names and %s the orphan list", ino, on,
                 s->refs[ino], s->orphan[ino] ? "on" : "not on");
        if (!on)
            continue;
        used++;
        struct ext_inode in;
        if (i >= fs->inodes_per_group - gr.itable_unused)
            fail("inode %u: in use past the group's used part", ino);
        if (ext_iget(fs, ino, EXT_IGET_UNLINKED, &in) != OK) {
            if (ino < fs->first_ino)
                continue;
            fail("inode %u: bit set, not readable (%s)", ino, fs->why ? fs->why : "");
        }
        dirs += ext_itype(&in) == EXT_FT_DIR;
        if (ino >= fs->first_ino)
            check_links(s, &in);
        if (ino >= fs->first_ino || ino == fs->sb.journal_inum || ino == EXT_ROOT_INO)
            own_inode(s, &in);
        s->out->inodes++;
    }
    for (uint32_t i = fs->inodes_per_group; b && i < fs->block_size * 8; i++)
        if (!bit(b->data, i))
            fail("group %u: inode bitmap padding clear at %u", g, i);
    if (b)
        ext_brelse(fs, b);
    if (gr.free_inodes != fs->inodes_per_group - used || gr.used_dirs != dirs)
        fail("group %u: %u free inodes, %u directories; counted %u, %u", g, gr.free_inodes,
             gr.used_dirs, fs->inodes_per_group - used, dirs);
    s->out->free_inodes += gr.free_inodes;
}

/* ---- blocks ------------------------------------------------------------------------------ */

/* The groups' own metadata: superblock copies, descriptors, the resize
 * reserve, bitmaps, inode tables. */
static void own_metadata(struct inv_state *s)
{
    struct ext_fs *fs = s->fs;
    bool resize = fs->sb.feature_compat & EXT_COMPAT_RESIZE_INODE;
    for (uint32_t g = 0; g < fs->groups; g++) {
        struct ext_group gr;
        if (ext_group_get(fs, g, &gr) != OK)
            fail("group %u: descriptor", g);
        if (ext_group_has_super(fs, g))
            own(s, ext_group_first(fs, g), 1 + fs->gdt_blocks +
                (resize ? fs->sb.reserved_gdt_blocks : 0), 0);
        own(s, gr.block_bitmap, 1, 0);
        own(s, gr.inode_bitmap, 1, 0);
        own(s, gr.inode_table, fs->itable_blocks, 0);
    }
    if (fs->block_size == 1024)
        set(s->owned, 0);
    if (resize)
        s->exact = false;   /* the reserve's map blocks */
}

static void check_block_group(struct inv_state *s, uint32_t g)
{
    struct ext_fs *fs = s->fs;
    struct ext_group gr;
    struct ext_buf *b;
    if (ext_group_get(fs, g, &gr) != OK)
        fail("group %u: descriptor", g);
    uint64_t first = ext_group_first(fs, g);
    uint32_t n = ext_group_blocks(fs, g), owned = 0, on = 0;
    for (uint32_t i = 0; i < n; i++)
        owned += bit(s->owned, first + i);
    status_t st = ext_bitmap_read(fs, g, EXT_BITMAP_BLOCK, &b);
    if (st == ERR_BAD_STATE) {   /* never written: only metadata may be in it */
        if (gr.free_blocks != n - owned)
            fail("group %u (never written): %u free, %u owned of %u", g, gr.free_blocks, owned, n);
        s->out->free_blocks += gr.free_blocks;
        return;
    }
    if (st != OK)
        fail("group %u: block bitmap (%s)", g, fs->why ? fs->why : "");
    for (uint32_t i = 0; i < n; i++) {
        bool set_ = bit(b->data, i), mine = bit(s->owned, first + i);
        on += set_;
        if (mine && !set_)
            fail("block %llu is used but free in its bitmap", (unsigned long long)(first + i));
        if (set_ && !mine && s->exact)
            fail("block %llu is set in its bitmap, used by nothing",
                 (unsigned long long)(first + i));
    }
    for (uint32_t i = n; i < fs->block_size * 8; i++)
        if (!bit(b->data, i))
            fail("group %u: block bitmap padding clear at %u", g, i);
    ext_brelse(fs, b);
    if (gr.free_blocks != n - on)
        fail("group %u: %u free blocks, its bitmap %u", g, gr.free_blocks, n - on);
    s->out->free_blocks += gr.free_blocks;
}

/* ---- the whole volume -------------------------------------------------------------------- */

static void check_orphan_list(struct inv_state *s)
{
    struct ext_fs *fs = s->fs;
    uint32_t list[4096], n;
    if (ext_orphans(fs, list, 4096, &n) != OK)
        fail("orphan list (%s)", fs->why ? fs->why : "");
    for (uint32_t i = 0; i < n; i++) {
        if (s->orphan[list[i]])
            fail("inode %u twice on the orphan list", list[i]);
        s->orphan[list[i]] = 1;
    }
    s->out->orphans = n;
}

void check_invariants(struct ext_fs *fs, struct inv *out)
{
    struct inv_state s = { .fs = fs, .exact = true, .out = out };
    memset(out, 0, sizeof(*out));
    s.refs = calloc(fs->inodes + 1, sizeof(uint32_t));
    s.subdirs = calloc(fs->inodes + 1, sizeof(uint32_t));
    s.parent = calloc(fs->inodes + 1, sizeof(uint32_t));
    s.orphan = calloc(fs->inodes + 1, 1);
    s.owned = calloc(fs->blocks / 8 + 1, 1);
    if (!s.refs || !s.subdirs || !s.parent || !s.orphan || !s.owned)
        fail("out of memory");
    check_orphan_list(&s);
    walk_tree(&s);
    own_metadata(&s);
    for (uint32_t g = 0; g < fs->groups; g++)
        check_inode_group(&s, g);
    for (unsigned i = 0; i < s.nxattr; i++)
        if (s.xattr[i].users != s.xattr[i].refcount)
            fail("attribute block %llu: refcount %u, %u users",
                 (unsigned long long)s.xattr[i].blk, s.xattr[i].refcount, s.xattr[i].users);
    for (uint32_t g = 0; g < fs->groups; g++)
        check_block_group(&s, g);
    uint64_t sb_free = fs->sb.free_blocks_count_lo |
                       (fs->is64 ? (uint64_t)fs->sb.free_blocks_count_hi << 32 : 0);
    if (sb_free != out->free_blocks || fs->sb.free_inodes_count != out->free_inodes)
        fail("superblock: %llu free blocks, %u free inodes; the groups %llu, %u",
             (unsigned long long)sb_free, fs->sb.free_inodes_count,
             (unsigned long long)out->free_blocks, out->free_inodes);
    out->exact = s.exact;
    check_pins(fs);
    free(s.refs);
    free(s.subdirs);
    free(s.parent);
    free(s.orphan);
    free(s.owned);
}
