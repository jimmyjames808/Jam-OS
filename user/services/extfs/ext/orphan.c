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
 * Freeing an orphan: its blocks freed a truncation step at a time while it
 * is still on the list (so a cut between the steps is finished at the
 * next mount); a file too big for one transaction's steps is left on the
 * list for the next (ERR_SHOULD_WAIT). Then off the list, and the inode
 * freed (ext_inode_free: its attribute block counted down, its deletion
 * time set, never 0: e2fsck reads a deleted inode with no deletion time
 * as damage). Its mode and the rest stay, as Linux leaves them. A file
 * mapped by blocks (ext2's, ext3's) can't be freed yet: ERR_NOT_SUPPORTED,
 * and it stays on the list. */
#include "names_int.h"

#define DROP_STEPS 16   /* truncation steps one drop takes: a few dozen dirty blocks each */

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

/* Cut in (on the list) to `size`, at most DROP_STEPS steps: ERR_SHOULD_WAIT
 * if more are left (commit, then call again: the list keeps it). A file
 * without an extent tree has nothing to cut here. */
static status_t cut(struct ext_fs *fs, struct ext_inode *in, uint64_t size)
{
    if (!(in->d.flags & EXT_EXTENTS_FL))
        return OK;
    for (uint32_t n = 0; n < DROP_STEPS; n++) {
        bool done = false;
        status_t st = ext_truncate(fs, in, size, &done);
        if (st != OK || done)
            return st;
    }
    return ERR_SHOULD_WAIT;
}

/* Free orphan `in` (read; no links). */
static status_t free_orphan(struct ext_fs *fs, struct ext_inode *in, const struct ext_time *now)
{
    bool isdir = ext_itype(in) == EXT_FT_DIR;
    status_t st = cut(fs, in, 0);
    if (st == OK)
        st = unchain(fs, in);
    if (st != OK)
        return st;
    if (isdir)
        ext_names_forget(fs, in->ino);
    uint32_t dtime = now->sec > 0 && now->sec <= UINT32_MAX ? (uint32_t)now->sec : 1;
    return ext_inode_free(fs, in, dtime);
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
    status_t st = cut(fs, in, ext_isize(in));
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
        if (st == ERR_SHOULD_WAIT) {   /* a big one: the rest after a commit */
            *out_done = max;
            return OK;
        }
        if (st != OK)
            return st;
        done += !kept;
        cur = next;
    }
    *out_done = done;
    return OK;
}
