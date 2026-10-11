/* ext: the orphan list, written. Inodes with no name left (still open
 * somewhere, or about to be freed in the same transaction) and inodes
 * whose truncation is under way are chained from the superblock's
 * last_orphan through each inode's dtime, so a mount after a cut finishes
 * what was started: an orphan without links is freed, one with links is
 * truncated to its size, and either leaves the list.
 *
 * The list is singly linked, so taking an inode off it walks from the
 * head to the one before it (bounded by the volume's inode count: a loop
 * is damage). It is short in practice: the files open after their
 * unlink, and the truncations of the current transaction.
 *
 * Freeing an orphan: off the list, its blocks freed (ext_truncate), its
 * attribute block counted down (freed by its last user: blocks are
 * shared), its deletion time set (never 0: e2fsck reads a deleted inode
 * with no deletion time as damage), the inode given back. Its mode and
 * the rest stay, as Linux leaves them. */
#include "ext_alloc.h"
#include "names_int.h"

/* Is inode ino on the list already (a truncation's orphan losing its last
 * name)? */
static status_t listed(struct ext_fs *fs, uint32_t ino, bool *out)
{
    uint32_t cur = fs->sb.last_orphan;
    for (uint32_t n = 0; cur && n < fs->inodes; n++) {
        struct ext_inode p;
        if (cur == ino) {
            *out = true;
            return OK;
        }
        status_t st = ext_iget(fs, cur, EXT_IGET_UNLINKED, &p);
        if (st == ERR_NOT_FOUND || st == ERR_OUT_OF_RANGE)
            return ext_damaged(fs, "orphan list: not an inode in use", cur, 0);
        if (st != OK)
            return st;
        cur = p.d.dtime;
    }
    if (cur)
        return ext_damaged(fs, "orphan list: a loop", cur, 0);
    *out = false;
    return OK;
}

status_t ext_orphan_add(struct ext_fs *fs, struct ext_inode *in)
{
    bool on = false;
    status_t st = listed(fs, in->ino, &on);
    if (st != OK || on)
        return st == OK ? ext_iput(fs, in) : st;
    in->d.dtime = fs->sb.last_orphan;
    st = ext_iput(fs, in);
    if (st != OK)
        return st;
    fs->sb.last_orphan = in->ino;
    return ext_super_write(fs);
}

/* Take `in` (read, its dtime the next orphan) off the list; in's dtime
 * becomes 0 in memory only. */
static status_t unchain(struct ext_fs *fs, struct ext_inode *in)
{
    uint32_t next = in->d.dtime;
    in->d.dtime = 0;
    if (fs->sb.last_orphan == in->ino) {
        fs->sb.last_orphan = next;
        return ext_super_write(fs);
    }
    uint32_t cur = fs->sb.last_orphan;
    for (uint32_t n = 0; cur && n < fs->inodes; n++) {
        struct ext_inode p;
        status_t st = ext_iget(fs, cur, EXT_IGET_UNLINKED, &p);
        if (st == ERR_NOT_FOUND || st == ERR_OUT_OF_RANGE)
            return ext_damaged(fs, "orphan list: not an inode in use", cur, 0);
        if (st != OK)
            return st;
        if (p.d.dtime == in->ino) {
            p.d.dtime = next;
            return ext_iput(fs, &p);
        }
        cur = p.d.dtime;
    }
    return cur ? ext_damaged(fs, "orphan list: a loop", cur, 0) : ERR_NOT_FOUND;
}

status_t ext_orphan_remove(struct ext_fs *fs, uint32_t ino)
{
    struct ext_inode in;
    status_t st = ext_iget(fs, ino, EXT_IGET_UNLINKED, &in);
    if (st == OK)
        st = unchain(fs, &in);
    if (st == OK)
        st = ext_iput(fs, &in);
    return st;
}

/* in's attribute block counted down, freed by its last user. */
static status_t drop_xattr(struct ext_fs *fs, struct ext_inode *in)
{
    struct ext_buf *b;
    status_t st = ext_xattr_block(fs, in, &b);
    if (st == ERR_NOT_FOUND)
        return OK;
    if (st != OK)
        return st;
    uint64_t blk = b->blk;
    struct ext_xattr_header *h = (struct ext_xattr_header *)b->data;
    bool last = h->refcount <= 1;
    if (!last) {
        h->refcount--;
        if (fs->csum)
            h->checksum = ext_xattr_csum(fs, blk, b->data);
        ext_bdirty(fs, b);
    }
    ext_brelse(fs, b);
    if (last)
        st = ext_bfree(fs, blk, 1);
    in->d.file_acl_lo = 0;
    in->d.file_acl_high = 0;
    uint64_t sectors = ext_iblocks512(fs, in), one = fs->block_size / 512;
    sectors = sectors > one ? sectors - one : 0;
    in->d.blocks_lo = (uint32_t)sectors;
    in->d.blocks_high = fs->huge_file ? (uint16_t)(sectors >> 32) : 0;
    return st;
}

/* Free orphan `in` (read; no links). */
static status_t free_orphan(struct ext_fs *fs, struct ext_inode *in, const struct ext_time *now)
{
    bool isdir = ext_itype(in) == EXT_FT_DIR;
    status_t st = unchain(fs, in);
    if (st == OK)
        st = ext_truncate(fs, in, 0);
    if (st == OK)
        st = drop_xattr(fs, in);
    if (st != OK)
        return st;
    in->d.dtime = now->sec > 0 && now->sec <= UINT32_MAX ? (uint32_t)now->sec : 1;
    if (isdir)
        ext_names_forget(fs, in->ino);
    st = ext_iput(fs, in);
    if (st == OK)
        st = ext_ifree(fs, in->ino, isdir);
    return st;
}

status_t ext_orphan_drop(struct ext_fs *fs, uint32_t ino, const struct ext_time *now)
{
    struct ext_inode in;
    status_t st = ext_iget(fs, ino, EXT_IGET_UNLINKED, &in);
    if (st != OK)
        return st;
    if (in.d.links_count)
        return ERR_BAD_STATE;
    return free_orphan(fs, &in, now);
}

/* A truncation a cut stopped: finish it, and take the inode off the list. */
static status_t finish_truncate(struct ext_fs *fs, struct ext_inode *in)
{
    status_t st = ext_truncate(fs, in, ext_isize(in));
    if (st == OK)
        st = unchain(fs, in);
    if (st == OK)
        st = ext_iput(fs, in);
    return st;
}

status_t ext_orphans_recover(struct ext_fs *fs, ext_keep_fn keep, void *ctx,
                             const struct ext_time *now, uint32_t max, uint32_t *out_done)
{
    uint32_t done = 0, cur = fs->sb.last_orphan;
    for (uint32_t n = 0; cur && done < max; n++) {
        struct ext_inode in;
        if (n >= fs->inodes)
            return ext_damaged(fs, "orphan list: a loop", cur, 0);
        status_t st = ext_iget(fs, cur, EXT_IGET_UNLINKED, &in);
        if (st == ERR_NOT_FOUND || st == ERR_OUT_OF_RANGE)
            return ext_damaged(fs, "orphan list: not an inode in use", cur, 0);
        if (st != OK)
            return st;
        uint32_t next = in.d.dtime;
        bool kept = !in.d.links_count && keep && keep(ctx, cur);
        if (in.d.links_count)
            st = finish_truncate(fs, &in);
        else if (!kept)
            st = free_orphan(fs, &in, now);
        if (st != OK)
            return st;
        done += !kept;
        cur = next;
    }
    *out_done = done;
    return OK;
}
