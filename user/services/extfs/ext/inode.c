/* ext: inodes. Inode n (from 1) is entry (n - 1) % inodes_per_group of
 * group (n - 1) / inodes_per_group's inode table, inode_size bytes each.
 *
 * ext_iget checks an inode before handing it out: an inode in the part
 * of a table the group says was never used (with group checksums), or all
 * zeros, is not there; its i_extra_isize must fit the inode; its checksum
 * (metadata_csum) must match, over all inode_size bytes; its type must be
 * one of the seven. It copies out only the fields i_extra_isize covers,
 * since the bytes after them are extended attributes, and ext_iput writes
 * back exactly those bytes.
 *
 * Times: each of atime, ctime and mtime is 32 bits of seconds in the
 * inode's first part and, in a large inode, an _extra word whose low two
 * bits extend the seconds (sec = (int32)lo + (extra & 3) << 32, to the
 * year 2446) and whose other 30 bits are nanoseconds.
 *
 * The orphan list: inodes being deleted while open, or being truncated,
 * chained from the superblock's last_orphan through each inode's dtime,
 * for a mount after a cut to finish. */
#include "ext_int.h"

enum ext_ftype ext_mode_type(uint16_t mode)
{
    switch (mode & EXT_S_IFMT) {
    case EXT_S_IFREG:  return EXT_FT_REG;
    case EXT_S_IFDIR:  return EXT_FT_DIR;
    case EXT_S_IFLNK:  return EXT_FT_SYMLINK;
    case EXT_S_IFCHR:  return EXT_FT_CHR;
    case EXT_S_IFBLK:  return EXT_FT_BLK;
    case EXT_S_IFIFO:  return EXT_FT_FIFO;
    case EXT_S_IFSOCK: return EXT_FT_SOCK;
    default:           return EXT_FT_UNKNOWN;
    }
}

enum ext_ftype ext_itype(const struct ext_inode *in)
{
    return ext_mode_type(in->d.mode);
}

/* Where inode ino lies, and whether its group says it was never used. */
static status_t locate(struct ext_fs *fs, uint32_t ino, uint64_t *blk, uint32_t *off, bool *unused)
{
    if (ino == 0 || ino > fs->inodes)
        return ERR_OUT_OF_RANGE;
    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t i = (ino - 1) % fs->inodes_per_group;
    struct ext_group gr;
    status_t st = ext_group_get(fs, g, &gr);
    if (st != OK)
        return st;
    *unused = (gr.flags & EXT_BG_INODE_UNINIT) || i >= fs->inodes_per_group - gr.itable_unused;
    *blk = gr.inode_table + i / fs->inodes_per_block;
    *off = i % fs->inodes_per_block * fs->inode_size;
    return OK;
}

status_t ext_iloc(struct ext_fs *fs, uint32_t ino, uint64_t *out_blk, uint32_t *out_off)
{
    bool unused;
    return locate(fs, ino, out_blk, out_off, &unused);
}

/* The bytes of an inode ext_iget copies and ext_iput writes. */
static uint32_t copied(const struct ext_fs *fs, uint16_t extra_isize)
{
    if (fs->inode_size == EXT_GOOD_OLD_INODE)
        return EXT_GOOD_OLD_INODE;
    uint32_t n = EXT_GOOD_OLD_INODE + extra_isize;
    return n < sizeof(struct ext_disk_inode) ? n : sizeof(struct ext_disk_inode);
}

static bool all_zero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* Check the raw inode (inode_size bytes); its seed into *seed. */
static status_t check_raw(struct ext_fs *fs, uint32_t ino, uint32_t flags, const uint8_t *raw,
                          uint64_t blk, uint32_t *seed)
{
    const struct ext_disk_inode *d = (const struct ext_disk_inode *)raw;
    if (all_zero(raw, fs->inode_size))
        return ERR_NOT_FOUND;
    if (fs->inode_size > EXT_GOOD_OLD_INODE &&
        ((d->extra_isize & 3) || d->extra_isize > fs->inode_size - EXT_GOOD_OLD_INODE))
        return ext_damaged(fs, "inode: extra size", ino, blk);
    *seed = ext_inode_seed(fs, ino, d->generation);
    if (fs->csum && !ext_inode_csum_ok(fs, *seed, raw))
        return ext_damaged(fs, "inode checksum", ino, blk);
    if (d->links_count == 0 && !(flags & EXT_IGET_UNLINKED))
        return ERR_NOT_FOUND;
    if (ext_mode_type(d->mode) == EXT_FT_UNKNOWN)
        return d->mode ? ext_damaged(fs, "inode: type", ino, blk) : ERR_NOT_FOUND;
    return OK;
}

status_t ext_iget(struct ext_fs *fs, uint32_t ino, uint32_t flags, struct ext_inode *out)
{
    if (flags & ~(uint32_t)EXT_IGET_UNLINKED)
        return ERR_INVALID_ARGS;
    uint64_t blk;
    uint32_t off;
    bool unused;
    status_t st = locate(fs, ino, &blk, &off, &unused);
    if (st != OK)
        return st;
    if (unused)
        return ERR_NOT_FOUND;
    struct ext_buf *b;
    st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    const uint8_t *raw = b->data + off;
    uint32_t seed = 0;
    st = check_raw(fs, ino, flags, raw, blk, &seed);
    if (st == OK) {
        __builtin_memset(out, 0, sizeof(*out));
        out->ino = ino;
        out->seed = seed;
        uint16_t extra = ((const struct ext_disk_inode *)raw)->extra_isize;
        __builtin_memcpy(&out->d, raw, copied(fs, extra));
    }
    ext_brelse(fs, b);
    return st;
}

status_t ext_iput(struct ext_fs *fs, const struct ext_inode *in)
{
    if (fs->inode_size > EXT_GOOD_OLD_INODE &&
        ((in->d.extra_isize & 3) || in->d.extra_isize > fs->inode_size - EXT_GOOD_OLD_INODE))
        return ERR_INVALID_ARGS;
    uint64_t blk;
    uint32_t off;
    status_t st = ext_iloc(fs, in->ino, &blk, &off);
    if (st != OK)
        return st;
    struct ext_buf *b;
    st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    uint8_t *raw = b->data + off;
    __builtin_memcpy(raw, &in->d, copied(fs, in->d.extra_isize));
    if (fs->csum)
        ext_inode_csum_set(fs, ext_inode_seed(fs, in->ino, in->d.generation), raw);
    ext_bdirty(fs, b);
    ext_brelse(fs, b);
    return OK;
}

uint64_t ext_iblocks512(const struct ext_fs *fs, const struct ext_inode *in)
{
    uint64_t n = in->d.blocks_lo;
    if (!fs->huge_file)
        return n;
    n |= (uint64_t)in->d.blocks_high << 32;
    if (in->d.flags & EXT_HUGE_FILE_FL)
        n <<= fs->block_log - 9;
    return n;
}

/* Where each time's two words lie in the inode, and the end of the
 * _extra word (the large part must reach it). */
#define AT(f) __builtin_offsetof(struct ext_disk_inode, f)
static const struct {
    uint16_t lo, extra, end;
} times[] = {
    [EXT_ATIME]  = { AT(atime), AT(atime_extra), AT(atime_extra) + 4 },
    [EXT_CTIME]  = { AT(ctime), AT(ctime_extra), AT(ctime_extra) + 4 },
    [EXT_MTIME]  = { AT(mtime), AT(mtime_extra), AT(mtime_extra) + 4 },
    [EXT_CRTIME] = { AT(crtime), AT(crtime_extra), AT(crtime_extra) + 4 },
};

static uint32_t word(const struct ext_inode *in, uint32_t at)
{
    uint32_t v;
    __builtin_memcpy(&v, (const uint8_t *)&in->d + at, 4);
    return v;
}

static void set_word(struct ext_inode *in, uint32_t at, uint32_t v)
{
    __builtin_memcpy((uint8_t *)&in->d + at, &v, 4);
}

void ext_itime(const struct ext_inode *in, enum ext_time_which w, struct ext_time *out)
{
    out->sec = 0;
    out->nsec = 0;
    if (w == EXT_CRTIME && !ext_ifits(in, times[w].lo + 4))
        return;
    out->sec = (int32_t)word(in, times[w].lo);
    if (!ext_ifits(in, times[w].end))
        return;
    uint32_t extra = word(in, times[w].extra);
    out->sec += (int64_t)(extra & 3) << 32;
    out->nsec = extra >> 2;
    if (out->nsec > 999999999)
        out->nsec = 999999999;
}

status_t ext_itime_set(struct ext_inode *in, enum ext_time_which w, const struct ext_time *t)
{
    bool has_extra = ext_ifits(in, times[w].end);
    if (t->nsec > 999999999 || (w == EXT_CRTIME && !has_extra) || t->sec < INT32_MIN ||
        t->sec > ((int64_t)3 << 32) + INT32_MAX)
        return ERR_OUT_OF_RANGE;
    int64_t low = (int32_t)(uint32_t)t->sec;
    int64_t epoch = (t->sec - low) >> 32;   /* exact: sec - low is a multiple of 2^32 */
    if (epoch < 0 || epoch > 3 || (!has_extra && epoch))
        return ERR_OUT_OF_RANGE;
    set_word(in, times[w].lo, (uint32_t)t->sec);
    if (has_extra)
        set_word(in, times[w].extra, (uint32_t)epoch | t->nsec << 2);
    return OK;
}

void ext_irdev(const struct ext_inode *in, uint32_t *out_major, uint32_t *out_minor)
{
    *out_major = 0;
    *out_minor = 0;
    enum ext_ftype t = ext_itype(in);
    if (t != EXT_FT_CHR && t != EXT_FT_BLK)
        return;
    uint32_t old = in->d.block[0], neu = in->d.block[1];
    if (old) {   /* 8-bit major and minor */
        *out_major = old >> 8 & 0xff;
        *out_minor = old & 0xff;
    } else {     /* 12-bit major, 20-bit minor */
        *out_major = neu >> 8 & 0xfff;
        *out_minor = (neu & 0xff) | (neu >> 12 & 0xfff00);
    }
}

status_t ext_xattr_block(struct ext_fs *fs, const struct ext_inode *in, struct ext_buf **out)
{
    uint64_t blk = in->d.file_acl_lo;
    if (fs->is64)   /* the high half is something else without 64bit */
        blk |= (uint64_t)in->d.file_acl_high << 32;
    if (!blk)
        return ERR_NOT_FOUND;
    if (!ext_blk_ok(fs, blk))
        return ext_damaged(fs, "inode: attribute block outside the volume", in->ino, blk);
    struct ext_buf *b;
    status_t st = ext_bread(fs, blk, &b);
    if (st != OK)
        return st;
    if (!ext_bchecked(b, EXT_KIND_XATTR, 0, 0)) {
        const struct ext_xattr_header *h = (const struct ext_xattr_header *)b->data;
        if (h->magic != EXT_XATTR_MAGIC || h->blocks != 1)
            st = ext_damaged(fs, "attribute block: header", in->ino, blk);
        else if (fs->csum && ext_xattr_csum(fs, blk, b->data) != h->checksum)
            st = ext_damaged(fs, "attribute block checksum", in->ino, blk);
        if (st != OK) {
            ext_brelse(fs, b);
            return st;
        }
        ext_bmark(b, EXT_KIND_XATTR, 0, 0);
    }
    *out = b;
    return OK;
}

status_t ext_orphans(struct ext_fs *fs, uint32_t *list, uint32_t cap, uint32_t *out_n)
{
    uint32_t n = 0;
    for (uint32_t ino = fs->sb.last_orphan; ino;) {
        if (n == cap)
            return ERR_NO_RESOURCES;
        struct ext_inode in;
        status_t st = ext_iget(fs, ino, EXT_IGET_UNLINKED, &in);
        if (st == ERR_OUT_OF_RANGE || st == ERR_NOT_FOUND)
            return ext_damaged(fs, "orphan list: not an inode in use", ino, 0);
        if (st != OK)
            return st;
        list[n++] = ino;
        ino = in.d.dtime;
    }
    *out_n = n;
    return OK;
}
