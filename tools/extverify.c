/* extcheck's checker of a whole volume after writes (tools/extwrite.c
 * calls it after every step): what Linux's e2fsck -fn would call damage
 * in the parts U2a writes, checked on the Mac, from the volume's own
 * structures and nothing the writers remember.
 *
 *   - Every block in use has exactly one owner: a group's copy of the
 *     superblock and descriptors (and resize_inode's reserve), a group's
 *     bitmaps and inode table, an inode's extent nodes or data, a block
 *     map's indirect blocks, an attribute block (owned by as many inodes
 *     as its refcount says).
 *   - Each group's block bitmap says exactly that, its padding bits set; a
 *     group still BLOCK_UNINIT owns nothing but its metadata. The free
 *     counts agree: bitmap, descriptor, superblock.
 *   - Each inode in an inode bitmap is readable and below itable_unused's
 *     line; each inode outside it is not in use (links 0); the free inode
 *     and directory counts agree, padding bits set.
 *   - Each extent tree: nodes' magic, max (the block's room, or the test
 *     room), depth, entries in order inside their parent's range, every
 *     index entry's key its child's first block, no empty node but an
 *     empty root, no overlap; i_blocks the count of nodes, data and
 *     attribute block; no written block at or past the size's last block
 *     in a regular file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ext_alloc.h>
#include "extcheck.h"

#define RESIZE_INO 7   /* resize_inode's: it owns the reserved descriptor blocks */

struct vctx {
    struct ext_fs *fs;
    uint8_t *own;                 /* a byte per block: 0 free, 1 metadata, 2 a file's */
    uint64_t xattr_blk[64];       /* attribute blocks seen, and their owners */
    uint32_t xattr_refs[64];
    unsigned nxattr;
    uint32_t *dirs;               /* per group: directories in use */
    uint32_t ino;                 /* the inode being walked */
    uint32_t orphans[64];         /* the orphan list: a truncation may be part done */
    uint32_t norphans;
};

static bool orphan(const struct vctx *v, uint32_t ino)
{
    for (uint32_t i = 0; i < v->norphans; i++)
        if (v->orphans[i] == ino)
            return true;
    return false;
}

static void mark(struct vctx *v, uint64_t blk, uint64_t n, uint8_t who, bool shared_ok)
{
    if (blk > v->fs->blocks || n > v->fs->blocks - blk)
        fail("verify: inode %u: blocks %llu+%llu outside the volume", v->ino,
             (unsigned long long)blk, (unsigned long long)n);
    for (uint64_t b = blk; b < blk + n; b++) {
        if (v->own[b] && !shared_ok)
            fail("verify: block %llu owned twice (inode %u)", (unsigned long long)b, v->ino);
        if (!v->own[b])
            v->own[b] = who;
    }
}

/* A node's max: the room of the inode or of a block, or the tests' room
 * (ext_extent_test_room) for a node made while it was set. */
static bool max_ok(const struct ext_fs *fs, bool root, uint32_t max)
{
    if (root)
        return max == 4;
    return max == (fs->block_size - 12) / 12 ||
           (ext_extent_test_room && max == ext_extent_test_room);
}

/* One extent node, its keys inside [lo, hi): blocks counted into *count,
 * the end of its last written extent into *wend. */
static void walk_node(struct vctx *v, struct ext_inode *in, struct ext_extent_header *h,
                      uint32_t depth, uint64_t lo, uint64_t hi, bool root, uint64_t *count,
                      uint64_t *wend)
{
    struct ext_fs *fs = v->fs;
    if (h->magic != EXT_EXTENT_MAGIC || h->depth != depth || !max_ok(fs, root, h->max) ||
        h->entries > h->max || (!root && !h->entries) || (root && !h->entries && depth))
        fail("verify: inode %u: extent node (depth %u, %u of %u, magic %#x)", in->ino, h->depth,
             h->entries, h->max, h->magic);
    struct ext_extent *e = ext_extents(h);
    if (!root && e[0].block != lo)
        fail("verify: inode %u: a node's first block %u, its key %llu", in->ino, e[0].block,
             (unsigned long long)lo);
    for (uint32_t i = 0; i < h->entries; i++) {
        uint64_t next = i + 1 < h->entries ? e[i + 1].block : hi;
        if (e[i].block < lo || (i + 1 < h->entries && e[i].block >= next) || e[i].block >= hi)
            fail("verify: inode %u: entry %u out of order or range", in->ino, i);
        if (!depth) {
            uint64_t end = (uint64_t)e[i].block + ext_extent_len(&e[i]);
            if (!ext_extent_len(&e[i]) || end > next)
                fail("verify: inode %u: extent %u overlaps", in->ino, e[i].block);
            mark(v, ext_extent_start(&e[i]), ext_extent_len(&e[i]), 2, false);
            *count += ext_extent_len(&e[i]);
            if (!ext_extent_unwritten(&e[i]) && end > *wend)
                *wend = end;
            continue;
        }
        struct ext_extent_idx *ix = &ext_idxs(h)[i];
        uint64_t child = ext_idx_leaf(ix);
        struct ext_buf *b;
        if (ext_bread(fs, child, &b) != OK)
            fail("verify: inode %u: node %llu unreadable", in->ino, (unsigned long long)child);
        if (fs->csum && !ext_extent_csum_ok(fs, in->seed, b->data))
            fail("verify: inode %u: node %llu checksum", in->ino, (unsigned long long)child);
        mark(v, child, 1, 2, false);
        (*count)++;
        walk_node(v, in, (struct ext_extent_header *)b->data, depth - 1, ix->block, next, false,
                  count, wend);
        ext_brelse(fs, b);
    }
}

/* A block map's tree from ptr (levels of indirection), its blocks counted. */
static void walk_map(struct vctx *v, uint32_t ptr, uint32_t levels, uint64_t *count)
{
    if (!ptr)
        return;
    mark(v, ptr, 1, 2, v->ino == RESIZE_INO);
    if (v->ino != RESIZE_INO || !levels)
        (*count)++;
    if (!levels)
        return;
    struct ext_buf *b;
    if (ext_bread(v->fs, ptr, &b) != OK)
        fail("verify: inode %u: map block %u unreadable", v->ino, ptr);
    uint32_t words[1024 * 16];
    memcpy(words, b->data, v->fs->block_size);
    ext_brelse(v->fs, b);
    for (uint32_t i = 0; i < v->fs->addr_per_block; i++)
        walk_map(v, words[i], levels - 1, count);
}

static void note_xattr(struct vctx *v, struct ext_inode *in, uint64_t *count)
{
    struct ext_buf *b;
    status_t st = ext_xattr_block(v->fs, in, &b);
    if (st == ERR_NOT_FOUND)
        return;
    if (st != OK)
        fail("verify: inode %u: attribute block %d", in->ino, st);
    uint64_t blk = b->blk;
    uint32_t refs = ((struct ext_xattr_header *)b->data)->refcount;
    ext_brelse(v->fs, b);
    (*count)++;
    for (unsigned i = 0; i < v->nxattr; i++)
        if (v->xattr_blk[i] == blk) {
            v->xattr_refs[i]--;
            return;
        }
    if (v->nxattr == 64)
        fail("verify: too many attribute blocks");
    mark(v, blk, 1, 2, false);
    v->xattr_blk[v->nxattr] = blk;
    v->xattr_refs[v->nxattr++] = refs - 1;
}

static void walk_inode(struct vctx *v, struct ext_inode *in)
{
    struct ext_fs *fs = v->fs;
    uint64_t count = 0, wend = 0;
    v->ino = in->ino;
    if (in->d.flags & EXT_EXTENTS_FL) {
        struct ext_extent_header *root = (struct ext_extent_header *)in->d.block;
        walk_node(v, in, root, root->depth, 0, (uint64_t)1 << 32, true, &count, &wend);
    } else if (!(ext_itype(in) == EXT_FT_SYMLINK && ext_isize(in) < 60) &&
               (ext_itype(in) == EXT_FT_REG || ext_itype(in) == EXT_FT_DIR ||
                ext_itype(in) == EXT_FT_SYMLINK)) {
        for (uint32_t i = 0; i < 12; i++)
            walk_map(v, in->d.block[i], 0, &count);
        for (uint32_t l = 1; l <= 3; l++)
            walk_map(v, in->d.block[11 + l], l, &count);
    }
    note_xattr(v, in, &count);
    if (in->ino != RESIZE_INO && ext_iblocks512(fs, in) != count << (fs->block_log - 9))
        fail("verify: inode %u: i_blocks %llu, owns %llu blocks", in->ino,
             (unsigned long long)ext_iblocks512(fs, in), (unsigned long long)count);
    uint64_t last = (ext_isize(in) + fs->block_size - 1) >> fs->block_log;
    if (ext_itype(in) == EXT_FT_REG && wend > last && !orphan(v, in->ino))
        fail("verify: inode %u: written blocks to %llu past its size %llu", in->ino,
             (unsigned long long)wend, (unsigned long long)ext_isize(in));
}

/* Every inode of group g: in use as its bitmap says, walked. */
static void check_inodes(struct vctx *v, uint32_t g, uint32_t *free_out)
{
    struct ext_fs *fs = v->fs;
    struct ext_group gr;
    if (ext_group_get(fs, g, &gr) != OK)
        fail("verify: group %u descriptor", g);
    uint32_t ipg = fs->inodes_per_group, free_n = 0, dirs = 0;
    uint8_t map[65536 / 8 * 8];
    memset(map, 0, sizeof(map));
    if (!(gr.flags & EXT_BG_INODE_UNINIT)) {
        struct ext_buf *b;
        if (ext_bitmap_read(fs, g, EXT_BITMAP_INODE, &b) != OK)
            fail("verify: group %u inode bitmap: %s", g, fs->why ? fs->why : "");
        memcpy(map, b->data, fs->block_size);
        ext_brelse(fs, b);
        for (uint32_t i = ipg; i < fs->block_size * 8; i++)
            if (!(map[i >> 3] >> (i & 7) & 1))
                fail("verify: group %u inode bitmap padding", g);
    }
    for (uint32_t i = 0; i < ipg; i++) {
        bool used = map[i >> 3] >> (i & 7) & 1;
        uint32_t ino = g * ipg + i + 1;
        struct ext_inode in;
        status_t st = ext_iget(fs, ino, EXT_IGET_UNLINKED, &in);
        free_n += !used;
        if (used && i >= ipg - gr.itable_unused)
            fail("verify: inode %u in use past itable_unused", ino);
        if (!used && st == OK && in.d.links_count)
            fail("verify: inode %u has links but its bit is clear", ino);
        if (!used || (st == ERR_NOT_FOUND && ino < fs->first_ino))
            continue;
        if (st != OK)
            fail("verify: inode %u in use but unreadable: %d (%s)", ino, st,
                 fs->why ? fs->why : "");
        dirs += ext_itype(&in) == EXT_FT_DIR;
        walk_inode(v, &in);
    }
    if (free_n != gr.free_inodes || dirs != gr.used_dirs)
        fail("verify: group %u: %u inodes free, %u dirs; descriptor says %u, %u", g, free_n, dirs,
             gr.free_inodes, gr.used_dirs);
    *free_out = free_n;
}

/* Group metadata into own[]. */
static void mark_meta(struct vctx *v)
{
    struct ext_fs *fs = v->fs;
    for (uint32_t g = 0; g < fs->groups; g++) {
        struct ext_group gr;
        if (ext_group_get(fs, g, &gr) != OK)
            fail("verify: group %u descriptor", g);
        v->ino = 0;
        if (ext_group_has_super(fs, g)) {
            uint32_t r = fs->sb.feature_compat & EXT_COMPAT_RESIZE_INODE
                             ? fs->sb.reserved_gdt_blocks : 0;
            mark(v, ext_group_first(fs, g), 1 + fs->gdt_blocks + r, 1, false);
        }
        mark(v, gr.block_bitmap, 1, 1, false);
        mark(v, gr.inode_bitmap, 1, 1, false);
        mark(v, gr.inode_table, fs->itable_blocks, 1, false);
    }
}

/* Group g's block bitmap against own[]. */
static uint32_t check_blocks(struct vctx *v, uint32_t g)
{
    struct ext_fs *fs = v->fs;
    struct ext_group gr;
    if (ext_group_get(fs, g, &gr) != OK)
        fail("verify: group %u descriptor", g);
    uint64_t first = ext_group_first(fs, g);
    uint32_t n = ext_group_blocks(fs, g), free_n = 0;
    struct ext_buf *b = NULL;
    if (!(gr.flags & EXT_BG_BLOCK_UNINIT) && ext_bitmap_read(fs, g, EXT_BITMAP_BLOCK, &b) != OK)
        fail("verify: group %u block bitmap: %s", g, fs->why ? fs->why : "");
    for (uint32_t i = 0; i < n; i++) {
        uint8_t who = v->own[first + i];
        bool set = b ? b->data[i >> 3] >> (i & 7) & 1 : who == 1;
        if (set != (who != 0))
            fail("verify: block %llu: bitmap %d, owner %u", (unsigned long long)(first + i), set,
                 who);
        free_n += !set;
    }
    for (uint32_t i = n; b && i < fs->block_size * 8; i++)
        if (!(b->data[i >> 3] >> (i & 7) & 1))
            fail("verify: group %u block bitmap padding", g);
    if (b)
        ext_brelse(fs, b);
    if (free_n != gr.free_blocks)
        fail("verify: group %u: %u blocks free, its descriptor says %u", g, free_n,
             gr.free_blocks);
    return free_n;
}

void verify_volume(struct ext_fs *fs)
{
    struct vctx v = { .fs = fs };
    v.own = calloc(fs->blocks, 1);
    if (!v.own)
        fail("out of memory");
    if (ext_orphans(fs, v.orphans, 64, &v.norphans) != OK)
        fail("verify: the orphan list");
    mark_meta(&v);
    uint64_t free_i = 0, free_b = 0;
    for (uint32_t g = 0; g < fs->groups; g++) {
        uint32_t n;
        check_inodes(&v, g, &n);
        free_i += n;
    }
    for (unsigned i = 0; i < v.nxattr; i++)
        if (v.xattr_refs[i])
            fail("verify: attribute block %llu: refcount off by %d",
                 (unsigned long long)v.xattr_blk[i], (int)v.xattr_refs[i]);
    for (uint32_t g = 0; g < fs->groups; g++)
        free_b += check_blocks(&v, g);
    if (free_b != ext_free_blocks(fs) || free_i != fs->sb.free_inodes_count)
        fail("verify: %llu blocks and %llu inodes free; the superblock says %llu, %u",
             (unsigned long long)free_b, (unsigned long long)free_i,
             (unsigned long long)ext_free_blocks(fs), fs->sb.free_inodes_count);
    free(v.own);
    check_pins(fs);
}
