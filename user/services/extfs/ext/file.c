/* ext: reading a file's bytes, and a symbolic link's target.
 *
 * A read moves through the file a run at a time (ext_map): a hole or an
 * unwritten extent is zeros; whole blocks of data go from the device
 * straight into the caller's buffer, around the cache (a big read would
 * only push the metadata out); a part of a block is copied from the
 * cache. Nothing past the inode's size is read, and the logical blocks
 * past 2^32 that a hostile size reaches are holes.
 *
 * A symbolic link's target is in i_block itself when it is shorter than
 * its 60 bytes (a "fast" link: the rule both Linux and e2fsprogs use),
 * else in the link's one block. */
#include "ext_int.h"

#define DIRECT_MAX (1u << 30)   /* bytes one device read takes at most */

/* Copy part of block pblk, from byte `within`, n bytes, through the cache. */
static status_t copy_part(struct ext_fs *fs, uint64_t pblk, uint32_t within, uint8_t *dst,
                          uint32_t n)
{
    struct ext_buf *b;
    status_t st = ext_bread(fs, pblk, &b);
    if (st != OK)
        return st;
    __builtin_memcpy(dst, b->data + within, n);
    ext_brelse(fs, b);
    return OK;
}

/* Read [off, off + n) of the file, all of it inside its size. */
static status_t read_span(struct ext_fs *fs, struct ext_inode *in, uint64_t off, uint8_t *dst,
                          size_t n)
{
    uint32_t bs = fs->block_size;
    while (n) {
        uint64_t lblk = off >> fs->block_log;
        uint32_t within = (uint32_t)(off & (bs - 1));
        struct ext_run r = { .pblk = 0, .len = UINT32_MAX, .kind = EXT_RUN_HOLE };
        if (lblk <= UINT32_MAX) {
            status_t st = ext_map(fs, in, (uint32_t)lblk, &r);
            if (st != OK)
                return st;
        }
        uint64_t span = ((uint64_t)r.len << fs->block_log) - within;
        size_t take = n < span ? n : (size_t)span;
        if (r.kind != EXT_RUN_DATA) {
            __builtin_memset(dst, 0, take);
        } else if (within || take < bs) {
            take = take < bs - within ? take : bs - within;
            status_t st = copy_part(fs, r.pblk, within, dst, (uint32_t)take);
            if (st != OK)
                return st;
        } else {
            take = take > DIRECT_MAX ? DIRECT_MAX : take & ~(size_t)(bs - 1);
            fs->cache.reads++;
            if (fs->dev.read(fs->dev.ctx, r.pblk << fs->block_log, (uint32_t)take, dst) != OK)
                return ERR_IO;
        }
        off += take;
        dst += take;
        n -= take;
    }
    return OK;
}

status_t ext_read(struct ext_fs *fs, struct ext_inode *in, uint64_t off, void *buf, size_t len,
                  size_t *out_n)
{
    uint64_t size = ext_isize(in);
    size_t n = 0;
    if (off < size)
        n = len < size - off ? len : (size_t)(size - off);
    if (n && ext_itype(in) == EXT_FT_SYMLINK && size < sizeof(in->d.block)) {
        __builtin_memcpy(buf, (const uint8_t *)in->d.block + off, n);   /* a fast link */
        *out_n = n;
        return OK;
    }
    status_t st = read_span(fs, in, off, buf, n);
    if (st == OK)
        *out_n = n;
    return st;
}

status_t ext_readlink(struct ext_fs *fs, struct ext_inode *in, char *buf, size_t cap,
                      size_t *out_len)
{
    if (ext_itype(in) != EXT_FT_SYMLINK)
        return ERR_INVALID_ARGS;
    uint64_t size = ext_isize(in);
    if (!size || size > fs->block_size)
        return ext_damaged(fs, "symbolic link: size", in->ino, 0);
    if (size > cap)
        return ERR_BUFFER_TOO_SMALL;
    if (size < sizeof(in->d.block)) {
        __builtin_memcpy(buf, in->d.block, (size_t)size);
    } else {
        size_t got;
        status_t st = ext_read(fs, in, 0, buf, (size_t)size, &got);
        if (st != OK)
            return st;
    }
    *out_len = (size_t)size;
    return OK;
}
