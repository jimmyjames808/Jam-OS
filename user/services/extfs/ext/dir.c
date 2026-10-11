/* ext: reading directories. A directory's blocks each hold a chain of
 * entries (inode, rec_len to the next, the name), the last reaching the
 * block's end, or the 12-byte checksum tail before it with metadata_csum.
 * An entry's byte offset in the directory is its place for readdir: an
 * entry never moves (a removed one becomes part of the one before it), so
 * a reader that resumes at an offset finds the first entry at or after
 * it, walking from its block's start.
 *
 * An indexed directory (dir_index, and the inode's EXT_INDEX_FL) is read
 * as a plain one: its block 0 holds "." and an entry for ".." that covers
 * the rest of the block, where the index's root lies; the index's inner
 * nodes are blocks holding one empty entry as long as the block; its
 * leaves are ordinary entry blocks. What tells them apart: block 0 is the
 * root, a block whose first entry is empty and the block's length is a
 * node (a leaf with metadata_csum always stops 12 bytes short), anything
 * else a leaf. The root and nodes carry the index's checksum (dx_tail)
 * instead of a leaf's, and are checked by it; the index itself (its
 * hashes) is never followed.
 *
 * Every block is checked when it is read (and remembered as checked on its
 * buffer, kind EXT_KIND_DIR with what it is): its checksum, then each
 * entry's rec_len (a multiple of 4, room for its name, inside the block,
 * the chain ending exactly at the block's end), its inode inside the
 * volume, its name 1 to 255 bytes with no '/' and no NUL (a name that
 * could be taken for a path, or cut short, is damage, not a name). A
 * walk over a checked block then trusts it. A hole in a directory is
 * damage too. */
#include "ext_int.h"

uint32_t ext_rec_len(const struct ext_fs *fs, uint16_t stored)
{
    if (fs->block_size < 65536)
        return stored;
    if (stored == 0xFFFF || stored == 0)   /* 64 KiB blocks: the whole block */
        return 65536;
    return (stored & 0xFFFCu) | ((uint32_t)(stored & 3) << 16);
}

/* An entry's name length: 8 bits with filetype, 16 without. */
static uint32_t name_len(const struct ext_fs *fs, const struct ext_dirent *d)
{
    if (fs->filetype)
        return d->name_len;
    return d->name_len | (uint32_t)d->file_type << 8;
}

static bool name_ok(const char *name, uint32_t len)
{
    if (!len || len > EXT_NAME_MAX)
        return false;
    for (uint32_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] == '\0')
            return false;
    return true;
}

/* Check the chain of entries in data[0, limit). */
static status_t check_entries(struct ext_fs *fs, struct ext_inode *dir, const uint8_t *data,
                              uint32_t limit, uint64_t blk)
{
    for (uint32_t off = 0; off < limit;) {
        if (limit - off < EXT_DIRENT_HEADER)
            return ext_damaged(fs, "directory entry: past the block", dir->ino, blk);
        const struct ext_dirent *d = (const struct ext_dirent *)(data + off);
        uint32_t rl = ext_rec_len(fs, d->rec_len), nl = name_len(fs, d);
        if (rl % 4 || rl < ext_dirent_size(nl) || rl > limit - off)
            return ext_damaged(fs, "directory entry: record length", dir->ino, blk);
        if (d->inode > fs->inodes)
            return ext_damaged(fs, "directory entry: inode", dir->ino, blk);
        if (d->inode && !name_ok(d->name, nl))
            return ext_damaged(fs, "directory entry: name", dir->ino, blk);
        off += rl;
    }
    return OK;
}

/* Is dir read through an index? */
static bool indexed(const struct ext_fs *fs, const struct ext_inode *dir)
{
    return fs->dir_index && (dir->d.flags & EXT_INDEX_FL);
}

static enum ext_dirblk classify(const struct ext_fs *fs, const struct ext_inode *dir,
                                const uint8_t *data, uint32_t lblk)
{
    const struct ext_dirent *d = (const struct ext_dirent *)data;
    if (!indexed(fs, dir))
        return EXT_DIRBLK_LEAF;
    if (lblk == 0)
        return EXT_DIRBLK_DX_ROOT;
    if (!d->inode && ext_rec_len(fs, d->rec_len) == fs->block_size)
        return EXT_DIRBLK_DX_NODE;
    return EXT_DIRBLK_LEAF;
}

/* An index block's count and limit at count_off: within the block, room
 * for the checksum tail with metadata_csum, and its checksum. */
static status_t check_dx(struct ext_fs *fs, struct ext_inode *dir, const uint8_t *data,
                         uint32_t count_off, uint64_t blk)
{
    const struct ext_dx_countlimit *cl = (const void *)(data + count_off);
    uint32_t tail = fs->csum ? sizeof(struct ext_dx_tail) : 0;
    if (cl->count > cl->limit ||
        count_off + (uint32_t)cl->limit * sizeof(struct ext_dx_entry) + tail > fs->block_size)
        return ext_damaged(fs, "directory index: count", dir->ino, blk);
    if (fs->csum && !ext_dx_csum_ok(fs, dir->seed, data, count_off))
        return ext_damaged(fs, "directory index checksum", dir->ino, blk);
    return OK;
}

/* The root: "." (12 bytes), ".." to the block's end, the root's info. */
static status_t check_root(struct ext_fs *fs, struct ext_inode *dir, const uint8_t *data,
                           uint64_t blk)
{
    const struct ext_dirent *dot = (const void *)data;
    const struct ext_dirent *dotdot = (const void *)(data + 12);
    const struct ext_dx_root_info *info = (const void *)(data + 24);
    if (dot->inode != dir->ino || ext_rec_len(fs, dot->rec_len) != 12 ||
        name_len(fs, dot) != 1 || dot->name[0] != '.' ||
        ext_rec_len(fs, dotdot->rec_len) != fs->block_size - 12 || name_len(fs, dotdot) != 2 ||
        info->info_length != sizeof(*info) || info->indirect_levels > 1)
        return ext_damaged(fs, "directory index: root", dir->ino, blk);
    status_t st = check_dx(fs, dir, data, EXT_DX_ROOT_COUNT, blk);
    if (st == OK)
        st = check_entries(fs, dir, data, fs->block_size, blk);
    return st;
}

/* Check block blk (dir's logical block lblk) as `kind`. */
static status_t check_block(struct ext_fs *fs, struct ext_inode *dir, const uint8_t *data,
                            enum ext_dirblk kind, uint64_t blk)
{
    if (kind == EXT_DIRBLK_DX_ROOT)
        return check_root(fs, dir, data, blk);
    if (kind == EXT_DIRBLK_DX_NODE)
        return check_dx(fs, dir, data, EXT_DX_NODE_COUNT, blk);
    if (fs->csum && !ext_dirleaf_csum_ok(fs, dir->seed, data))
        return ext_damaged(fs, "directory block checksum", dir->ino, blk);
    return check_entries(fs, dir, data, fs->block_size - (fs->csum ? EXT_DIR_TAIL_SIZE : 0), blk);
}

status_t ext_dir_block(struct ext_fs *fs, struct ext_inode *dir, uint32_t lblk,
                       struct ext_buf **out, enum ext_dirblk *out_kind)
{
    if (ext_itype(dir) != EXT_FT_DIR)
        return ERR_NOT_DIR;
    if ((uint64_t)lblk << fs->block_log >= ext_isize(dir))
        return ERR_OUT_OF_RANGE;
    struct ext_run r;
    status_t st = ext_map(fs, dir, lblk, &r);
    if (st != OK)
        return st;
    if (r.kind != EXT_RUN_DATA)
        return ext_damaged(fs, "directory: hole", dir->ino, 0);
    struct ext_buf *b;
    st = ext_bread(fs, r.pblk, &b);
    if (st != OK)
        return st;
    enum ext_dirblk kind = classify(fs, dir, b->data, lblk);
    if (!ext_bchecked(b, EXT_KIND_DIR, (uint8_t)kind, dir->seed)) {
        st = check_block(fs, dir, b->data, kind, r.pblk);
        if (st != OK) {
            ext_brelse(fs, b);
            return st;
        }
        ext_bmark(b, EXT_KIND_DIR, (uint8_t)kind, dir->seed);
    }
    *out = b;
    *out_kind = kind;
    return OK;
}

static void fill(const struct ext_fs *fs, const struct ext_dirent *d, uint64_t pos,
                 struct ext_dentry *out)
{
    uint32_t nl = name_len(fs, d);
    out->ino = d->inode;
    out->type = fs->filetype && d->file_type < EXT_FT_MAX ? d->file_type : EXT_FT_UNKNOWN;
    out->name_len = (uint8_t)nl;
    out->pos = pos;
    __builtin_memcpy(out->name, d->name, nl);
    out->name[nl] = '\0';
}

/* The first entry in use at or after byte `from` of a checked block;
 * NULL if none. *at: its offset. */
static const struct ext_dirent *first_from(const struct ext_fs *fs, const uint8_t *data,
                                           enum ext_dirblk kind, uint32_t from, uint32_t *at)
{
    uint32_t limit = fs->block_size;
    if (kind == EXT_DIRBLK_DX_NODE)
        return NULL;
    if (kind == EXT_DIRBLK_LEAF && fs->csum)
        limit -= EXT_DIR_TAIL_SIZE;
    for (uint32_t off = 0; off < limit;) {
        const struct ext_dirent *d = (const struct ext_dirent *)(data + off);
        if (off >= from && d->inode) {
            *at = off;
            return d;
        }
        off += ext_rec_len(fs, d->rec_len);
    }
    return NULL;
}

status_t ext_dir_next(struct ext_fs *fs, struct ext_inode *dir, uint64_t *pos,
                      struct ext_dentry *out)
{
    if (ext_itype(dir) != EXT_FT_DIR)
        return ERR_NOT_DIR;
    uint64_t size = ext_isize(dir);
    while (*pos < size && *pos >> fs->block_log <= UINT32_MAX) {
        uint32_t lblk = (uint32_t)(*pos >> fs->block_log);
        uint64_t start = (uint64_t)lblk << fs->block_log;
        struct ext_buf *b;
        enum ext_dirblk kind;
        status_t st = ext_dir_block(fs, dir, lblk, &b, &kind);
        if (st != OK)
            return st;
        uint32_t at;
        const struct ext_dirent *d = first_from(fs, b->data, kind, (uint32_t)(*pos - start), &at);
        if (d) {
            fill(fs, d, start + at, out);
            *pos = start + at + ext_rec_len(fs, d->rec_len);
            ext_brelse(fs, b);
            return OK;
        }
        ext_brelse(fs, b);
        *pos = start + fs->block_size;
    }
    out->ino = 0;
    return OK;
}

status_t ext_dir_lookup(struct ext_fs *fs, struct ext_inode *dir, const char *name, size_t len,
                        struct ext_dentry *out)
{
    if (!len || len > EXT_NAME_MAX)
        return ERR_NOT_FOUND;
    uint64_t pos = 0;
    for (;;) {
        status_t st = ext_dir_next(fs, dir, &pos, out);
        if (st != OK)
            return st;
        if (!out->ino)
            return ERR_NOT_FOUND;
        if (out->name_len == len && !__builtin_memcmp(out->name, name, len))
            return OK;
    }
}
