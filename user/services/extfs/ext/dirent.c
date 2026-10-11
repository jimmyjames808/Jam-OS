/* ext: entries written into a directory's blocks.
 *
 * A block is a chain of entries, each rec_len bytes long, the last
 * reaching the block's end (less the 12-byte checksum tail with
 * metadata_csum). An entry is added in the first gap that holds it: an
 * unused entry long enough, or the slack after a live entry's name (the
 * live entry is cut to its own size, the new one takes the rest); a
 * directory with no such gap grows by a block. A removed entry becomes
 * part of the one before it in its block, or, first in its block, an
 * unused entry (inode 0) of the same length. So a live entry never moves,
 * and its byte offset (readdir's cookie, and the directory hash's
 * record) stays its own until it is removed.
 *
 * Every block is read through ext_dir_block (checked as it is read) and
 * written back with its checksum set (ext_leaf_seal), so a block this file
 * changes is checked again when it is next read.
 *
 * An indexed directory (Linux's dir_index) is made plain before its first
 * change: the index's root keeps "." and "..", whose record then covers
 * the rest of the block as an ordinary entry; each inner node of the
 * index becomes one unused entry the length of the block; the leaves are
 * left as they are; the inode loses its index flag. Nothing moves, so
 * cookies stay good. */
#include "names_int.h"

status_t ext_name_check(const struct ext_name *n)
{
    if (!n->len)
        return ERR_INVALID_ARGS;
    if (n->len > EXT_NAME_MAX)
        return ERR_NAME_TOO_LONG;
    if ((n->len == 1 && n->s[0] == '.') || (n->len == 2 && n->s[0] == '.' && n->s[1] == '.'))
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < n->len; i++)
        if (n->s[i] == '/' || n->s[i] == '\0')
            return ERR_INVALID_ARGS;
    return OK;
}

uint32_t ext_leaf_limit(const struct ext_fs *fs)
{
    return fs->block_size - (fs->csum ? EXT_DIR_TAIL_SIZE : 0);
}

uint16_t ext_rec_store(const struct ext_fs *fs, uint32_t len)
{
    if (fs->block_size < 65536)
        return (uint16_t)len;
    if (len == 65536)
        return 0xFFFF;
    return (uint16_t)((len & 0xFFFCu) | (len >> 16 & 3));
}

void ext_leaf_seal(struct ext_fs *fs, const struct ext_inode *dir, struct ext_buf *b)
{
    if (fs->csum)
        ext_dirleaf_csum_set(fs, dir->seed, b->data);
    ext_bdirty(fs, b);
}

static uint32_t ent_name_len(const struct ext_fs *fs, const struct ext_dirent *d)
{
    return fs->filetype ? d->name_len : (d->name_len | (uint32_t)d->file_type << 8);
}

static struct ext_dirent *at(uint8_t *data, uint32_t off)
{
    return (struct ext_dirent *)(data + off);
}

/* Write an entry's fields and name at d (its rec_len already set). */
static void put_entry(const struct ext_fs *fs, struct ext_dirent *d, const struct ext_name *n,
                      uint32_t ino, uint8_t type)
{
    d->inode = ino;
    d->name_len = (uint8_t)n->len;
    d->file_type = fs->filetype ? type : 0;
    __builtin_memcpy(d->name, n->s, n->len);
    __builtin_memset(d->name + n->len, 0, ext_dirent_size(n->len) - EXT_DIRENT_HEADER - n->len);
}

/* A gap for `need` bytes in a checked leaf: the entry to cut (*out_off)
 * and where the new one starts (*out_new). */
static bool find_room(const struct ext_fs *fs, uint8_t *data, uint32_t need, uint32_t *out_off,
                      uint32_t *out_new)
{
    uint32_t limit = ext_leaf_limit(fs);
    for (uint32_t off = 0; off < limit;) {
        struct ext_dirent *d = at(data, off);
        uint32_t rl = ext_rec_len(fs, d->rec_len);
        uint32_t used = d->inode ? ext_dirent_size(ent_name_len(fs, d)) : 0;
        if (rl - used >= need) {
            *out_off = off;
            *out_new = off + used;
            return true;
        }
        off += rl;
    }
    return false;
}

/* Put the entry into the gap find_room found. */
static void fill_gap(const struct ext_fs *fs, uint8_t *data, uint32_t off, uint32_t new_off,
                     const struct ext_name *n, uint32_t ino, uint8_t type)
{
    struct ext_dirent *d = at(data, off);
    uint32_t rl = ext_rec_len(fs, d->rec_len);
    if (new_off != off) {
        d->rec_len = ext_rec_store(fs, new_off - off);
        at(data, new_off)->rec_len = ext_rec_store(fs, rl - (new_off - off));
    }
    put_entry(fs, at(data, new_off), n, ino, type);
}

/* Is dir's size whole blocks? (Its count of blocks into *out.) */
static status_t dir_blocks(struct ext_fs *fs, const struct ext_inode *dir, uint32_t *out)
{
    uint64_t size = ext_isize(dir);
    if (size & (fs->block_size - 1) || size >> fs->block_log > UINT32_MAX)
        return ext_damaged(fs, "directory: size", dir->ino, 0);
    *out = (uint32_t)(size >> fs->block_log);
    return OK;
}

/* Add the entry in a new block at the directory's end. */
static status_t add_block(struct ext_fs *fs, struct ext_inode *dir, uint32_t lblk,
                          const struct ext_name *n, uint32_t ino, uint8_t type)
{
    uint64_t pblk;
    status_t st = ext_block_new(fs, dir, lblk, &pblk);
    struct ext_buf *b;
    if (st == OK)
        st = ext_bnew(fs, pblk, &b);
    if (st != OK)
        return st;
    at(b->data, 0)->rec_len = ext_rec_store(fs, ext_leaf_limit(fs));
    put_entry(fs, at(b->data, 0), n, ino, type);
    ext_leaf_seal(fs, dir, b);
    ext_brelse(fs, b);
    uint64_t size = ext_isize(dir) + fs->block_size;
    dir->d.size_lo = (uint32_t)size;
    dir->d.size_high = (uint32_t)(size >> 32);
    ext_names_added(fs, dir, n, (uint64_t)lblk << fs->block_log);
    return OK;
}

status_t ext_dirent_add(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *n,
                        uint32_t ino, uint8_t type)
{
    uint32_t need = ext_dirent_size(n->len), nblocks = 0;
    status_t st = ext_dir_plain(fs, dir);
    if (st == OK)
        st = dir_blocks(fs, dir, &nblocks);
    if (st != OK)
        return st;
    for (uint32_t lblk = ext_names_room_from(fs, dir, need); lblk < nblocks; lblk++) {
        struct ext_buf *b;
        enum ext_dirblk kind;
        st = ext_dir_block(fs, dir, lblk, &b, &kind);
        if (st != OK)
            return st;
        uint32_t off, new_off;
        if (kind != EXT_DIRBLK_LEAF || !find_room(fs, b->data, need, &off, &new_off)) {
            ext_brelse(fs, b);
            continue;
        }
        fill_gap(fs, b->data, off, new_off, n, ino, type);
        ext_leaf_seal(fs, dir, b);
        ext_brelse(fs, b);
        ext_names_room_found(fs, dir, lblk, need);
        ext_names_added(fs, dir, n, ((uint64_t)lblk << fs->block_log) + new_off);
        return OK;
    }
    ext_names_room_found(fs, dir, nblocks, need);
    return add_block(fs, dir, nblocks, n, ino, type);
}

/* The leaf holding byte pos, pinned, and the entry there (*out_prev: the
 * one before it in the block, or UINT32_MAX). ERR_NOT_FOUND unless an
 * entry naming ino starts exactly there. */
static status_t entry_at(struct ext_fs *fs, struct ext_inode *dir, uint64_t pos, uint32_t ino,
                         struct ext_buf **out, uint32_t *out_prev)
{
    struct ext_buf *b;
    enum ext_dirblk kind;
    if (pos >> fs->block_log > UINT32_MAX)
        return ERR_NOT_FOUND;
    status_t st = ext_dir_block(fs, dir, (uint32_t)(pos >> fs->block_log), &b, &kind);
    if (st != OK)
        return st;
    uint32_t want = (uint32_t)(pos & (fs->block_size - 1)), limit = ext_leaf_limit(fs);
    uint32_t prev = UINT32_MAX, off = 0;
    while (kind == EXT_DIRBLK_LEAF && off < want && off < limit) {
        prev = off;
        off += ext_rec_len(fs, at(b->data, off)->rec_len);
    }
    if (kind != EXT_DIRBLK_LEAF || off != want || off >= limit || at(b->data, off)->inode != ino) {
        ext_brelse(fs, b);
        return ERR_NOT_FOUND;
    }
    *out = b;
    *out_prev = prev;
    return OK;
}

status_t ext_dirent_remove(struct ext_fs *fs, struct ext_inode *dir, uint64_t pos, uint32_t ino)
{
    struct ext_buf *b;
    uint32_t prev, off = (uint32_t)(pos & (fs->block_size - 1));
    status_t st = ext_dir_plain(fs, dir);
    if (st == OK)
        st = entry_at(fs, dir, pos, ino, &b, &prev);
    if (st != OK)
        return st;
    struct ext_dirent *d = at(b->data, off);
    char name[EXT_NAME_MAX];
    uint32_t len = ent_name_len(fs, d);
    __builtin_memcpy(name, d->name, len);
    if (prev != UINT32_MAX) {
        struct ext_dirent *p = at(b->data, prev);
        p->rec_len = ext_rec_store(fs, ext_rec_len(fs, p->rec_len) + ext_rec_len(fs, d->rec_len));
    } else {
        d->inode = 0;
    }
    ext_leaf_seal(fs, dir, b);
    ext_brelse(fs, b);
    ext_names_removed(fs, dir, name, len, pos);
    ext_names_room_freed(fs, dir, (uint32_t)(pos >> fs->block_log));
    return OK;
}

status_t ext_dirent_set(struct ext_fs *fs, struct ext_inode *dir, uint64_t pos, uint32_t old,
                        uint32_t ino, uint8_t type)
{
    struct ext_buf *b;
    uint32_t prev;
    status_t st = ext_dir_plain(fs, dir);
    if (st == OK)
        st = entry_at(fs, dir, pos, old, &b, &prev);
    if (st != OK)
        return st;
    struct ext_dirent *d = at(b->data, (uint32_t)(pos & (fs->block_size - 1)));
    d->inode = ino;
    if (fs->filetype)
        d->file_type = type;
    ext_leaf_seal(fs, dir, b);
    ext_brelse(fs, b);
    return OK;
}

/* An index block made an ordinary leaf (see the header). */
static void unindex_block(struct ext_fs *fs, uint8_t *data, enum ext_dirblk kind)
{
    uint32_t limit = ext_leaf_limit(fs);
    if (kind == EXT_DIRBLK_DX_ROOT) {
        at(data, 12)->rec_len = ext_rec_store(fs, limit - 12);
        __builtin_memset(data + 24, 0, fs->block_size - 24);
    } else {
        __builtin_memset(data, 0, fs->block_size);
        at(data, 0)->rec_len = ext_rec_store(fs, limit);
    }
}

status_t ext_dir_plain(struct ext_fs *fs, struct ext_inode *dir)
{
    if (!fs->dir_index || !(dir->d.flags & EXT_INDEX_FL))
        return OK;
    uint32_t nblocks = 0;
    status_t st = dir_blocks(fs, dir, &nblocks);
    if (st != OK)
        return st;
    for (uint32_t lblk = 0; lblk < nblocks; lblk++) {
        struct ext_buf *b;
        enum ext_dirblk kind;
        st = ext_dir_block(fs, dir, lblk, &b, &kind);
        if (st != OK)
            return st;
        if (kind != EXT_DIRBLK_LEAF) {
            unindex_block(fs, b->data, kind);
            ext_leaf_seal(fs, dir, b);
        }
        ext_brelse(fs, b);
    }
    dir->d.flags &= ~EXT_INDEX_FL;
    ext_names_room_freed(fs, dir, 0);
    return ext_iput(fs, dir);
}

status_t ext_dir_empty(struct ext_fs *fs, struct ext_inode *dir, bool *out)
{
    struct ext_dentry d;
    for (uint64_t pos = 0;;) {
        status_t st = ext_dir_next(fs, dir, &pos, &d);
        if (st != OK)
            return st;
        if (!d.ino)
            break;
        if (__builtin_strcmp(d.name, ".") && __builtin_strcmp(d.name, "..")) {
            *out = false;
            return OK;
        }
    }
    *out = true;
    return OK;
}
