/* ext: from a file's logical block to where it lies. An inode with
 * EXT_EXTENTS_FL maps through its extent tree (extent.c); any other
 * (ext2's and ext3's files, which an ext4 volume may still hold) through
 * its block map: i_block's first 12 words name the first 12 blocks, the
 * 13th a block of block numbers (single indirect), the 14th a block of
 * such blocks (double), the 15th one more level (triple). 0 is a hole
 * at any level. Block-map blocks carry no checksum; every number in them
 * is checked against the volume before it is used.
 *
 * A mapping answers a run, not one block: how many blocks from the one
 * asked for are the same kind (data, contiguous on the disk, or a hole,
 * or allocated and unwritten), so a reader moves through a file an extent
 * at a time, and a hole of gigabytes costs one call. */
#include "ext_int.h"

#define NDIR 12   /* direct blocks in i_block */

static uint32_t cap32(uint64_t n)
{
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/* The run starting at entry i of `n` block numbers: a hole as long as the
 * zeros, or data as long as the numbers count up by one. */
static status_t run_in(struct ext_fs *fs, struct ext_inode *in, const uint32_t *e, uint32_t i,
                       uint32_t n, struct ext_run *out)
{
    uint32_t k = i + 1;
    if (!e[i]) {
        while (k < n && !e[k])
            k++;
        *out = (struct ext_run){ .pblk = 0, .len = k - i, .kind = EXT_RUN_HOLE };
        return OK;
    }
    while (k < n && e[k] == e[k - 1] + 1)
        k++;
    if (!ext_run_ok(fs, e[i], k - i))
        return ext_damaged(fs, "block map: block outside the volume", in->ino, e[i]);
    *out = (struct ext_run){ .pblk = e[i], .len = k - i, .kind = EXT_RUN_DATA };
    return OK;
}

/* Walk a tree of `levels` levels of block-number blocks from `ptr`, which
 * covers `cover` blocks, to block `rel` of them. */
static status_t walk(struct ext_fs *fs, struct ext_inode *in, uint32_t ptr, uint32_t levels,
                     uint64_t cover, uint64_t rel, struct ext_run *out)
{
    uint32_t a = fs->addr_per_block;
    for (; levels; levels--) {
        if (!ptr) {
            *out = (struct ext_run){ .pblk = 0, .len = cap32(cover - rel), .kind = EXT_RUN_HOLE };
            return OK;
        }
        if (!ext_blk_ok(fs, ptr))
            return ext_damaged(fs, "block map: block outside the volume", in->ino, ptr);
        struct ext_buf *b;
        status_t st = ext_bread(fs, ptr, &b);
        if (st != OK)
            return st;
        const uint32_t *e = (const uint32_t *)b->data;
        uint64_t each = cover / a;   /* blocks each entry covers */
        uint32_t i = (uint32_t)(rel / each);
        if (levels == 1) {
            st = run_in(fs, in, e, i, a, out);
            ext_brelse(fs, b);
            return st;
        }
        ptr = e[i];
        ext_brelse(fs, b);
        cover = each;
        rel %= each;
    }
    return ERR_INTERNAL;   /* not reached: levels >= 1 */
}

status_t ext_blockmap(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, struct ext_run *out)
{
    if (lblk < NDIR)
        return run_in(fs, in, in->d.block, lblk, NDIR, out);
    uint64_t a = fs->addr_per_block, rel = lblk - NDIR, cover = a;
    for (uint32_t levels = 1; levels <= 3; levels++, cover *= a) {
        if (rel < cover)
            return walk(fs, in, in->d.block[NDIR + levels - 1], levels, cover, rel, out);
        rel -= cover;
    }
    *out = (struct ext_run){ .pblk = 0, .len = cap32((1ull << 32) - lblk), .kind = EXT_RUN_HOLE };
    return OK;
}

/* lblk in an extent tree. */
static status_t extent_map(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk,
                           struct ext_run *out)
{
    struct ext_path p;
    status_t st = ext_extent_find(fs, in, lblk, &p);
    if (st != OK) {
        ext_path_release(fs, &p);
        return st;
    }
    struct ext_extent_header *h = p.lv[p.depth].h;
    struct ext_extent *e = ext_extents(h);
    int32_t at = p.lv[p.depth].at;
    uint64_t next = p.bound ? p.bound : 1ull << 32;   /* where the hole after lblk ends */
    if (at >= 0 && lblk - e[at].block < ext_extent_len(&e[at])) {
        uint32_t into = lblk - e[at].block;
        out->pblk = ext_extent_start(&e[at]) + into;
        out->len = ext_extent_len(&e[at]) - into;
        out->kind = ext_extent_unwritten(&e[at]) ? EXT_RUN_UNWRITTEN : EXT_RUN_DATA;
    } else {
        if ((uint32_t)(at + 1) < h->entries && e[at + 1].block < next)
            next = e[at + 1].block;
        *out = (struct ext_run){ .pblk = 0, .len = cap32(next - lblk), .kind = EXT_RUN_HOLE };
    }
    ext_path_release(fs, &p);
    return OK;
}

status_t ext_map(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, struct ext_run *out)
{
    enum ext_ftype t = ext_itype(in);
    if (t != EXT_FT_REG && t != EXT_FT_DIR && t != EXT_FT_SYMLINK)
        return ERR_INVALID_ARGS;
    if (t == EXT_FT_SYMLINK && ext_isize(in) < sizeof(in->d.block))
        return ERR_INVALID_ARGS;   /* a fast link: i_block holds its target, not blocks */
    if (in->d.flags & (EXT_INLINE_DATA_FL | EXT_ENCRYPT_FL))
        return ext_damaged(fs, "inode: a flag of a feature the volume lacks", in->ino, 0);
    if (in->d.flags & EXT_EXTENTS_FL)
        return extent_map(fs, in, lblk, out);
    return ext_blockmap(fs, in, lblk, out);
}
