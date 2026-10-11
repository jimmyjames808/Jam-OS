/* ext: rename, as one change. Every check comes first (the names, the
 * types, a directory replacing only an empty one, the move below itself,
 * the link counts' room), then the entries change in place:
 *   - a replace points the existing entry of the new name at the moved
 *     inode (so the new name is never missing), then removes the old
 *     entry; the replaced inode loses its name like an unlink's;
 *   - an exchange points each entry at the other inode;
 *   - a plain move adds the new entry, then removes the old one.
 * A directory that changes parent gets its ".." pointed at the new one,
 * and the parents' link counts follow it. All of it lands in one
 * transaction: a cut leaves the old names or the new ones.
 *
 * "Below itself": a directory moved to a new parent must not be that
 * parent or one of its ancestors, found by walking ".." up from the new
 * parent to the root (bounded: a loop of ".." is damage). */
#include "names_int.h"

/* Is directory `dir` the directory `top` or below it? */
static status_t below(struct ext_fs *fs, uint32_t dir, uint32_t top, bool *out)
{
    uint32_t cur = dir;
    for (uint32_t n = 0; n <= fs->inodes; n++) {
        if (cur == top || cur == EXT_ROOT_INO) {
            *out = cur == top;
            return OK;
        }
        struct ext_inode in;
        struct ext_dentry d;
        status_t st = ext_iget(fs, cur, 0, &in);
        if (st == OK)
            st = ext_dir_lookup(fs, &in, "..", 2, &d);
        if (st == ERR_NOT_FOUND || st == ERR_NOT_DIR)
            return ext_damaged(fs, "directory: no way up", cur, 0);
        if (st != OK)
            return st;
        cur = d.ino;
    }
    return ext_damaged(fs, "directory: a loop of ..", dir, 0);
}

/* Point directory in's ".." at parent. */
static status_t set_dotdot(struct ext_fs *fs, struct ext_inode *in, uint32_t parent)
{
    struct ext_dentry d;
    status_t st = ext_dir_lookup(fs, in, "..", 2, &d);
    if (st == ERR_NOT_FOUND)
        return ext_damaged(fs, "directory: no ..", in->ino, 0);
    if (st == OK)
        st = ext_dirent_set(fs, in, d.pos, d.ino, parent, EXT_FT_DIR);
    return st;
}

/* One side of a rename: the entry, and its inode when there is one. */
struct side {
    struct ext_inode *dir;
    struct ext_dentry e;
    struct ext_inode in;
    bool there;
    bool isdir;
};

static status_t find_side(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                          struct side *s)
{
    s->dir = dir;
    s->there = false;
    s->isdir = false;
    status_t st = ext_lookup(fs, dir, name, &s->e);
    if (st == ERR_NOT_FOUND)
        return OK;
    if (st == OK)
        st = ext_iget(fs, s->e.ino, 0, &s->in);
    if (st == ERR_NOT_FOUND)
        return ext_damaged(fs, "directory entry: a free inode", dir->ino, 0);
    s->there = st == OK;
    s->isdir = st == OK && ext_itype(&s->in) == EXT_FT_DIR;
    return st;
}

/* The checks that need both sides (see the header). */
static status_t check_sides(struct ext_fs *fs, uint32_t flags, bool same, struct side *a,
                            struct side *b)
{
    bool swap = flags & EXT_RENAME_EXCHANGE;
    status_t st = OK;
    if (!swap && b->there) {
        if (a->isdir != b->isdir)
            return a->isdir ? ERR_NOT_DIR : ERR_IS_DIR;
        bool empty = true;
        if (b->isdir)
            st = ext_dir_empty(fs, &b->in, &empty);
        if (st == OK && !empty)
            st = ERR_NOT_EMPTY;
    }
    if (st != OK || same)
        return st;
    bool in_self = false;
    if (a->isdir)
        st = below(fs, b->dir->ino, a->in.ino, &in_self);
    if (st == OK && !in_self && swap && b->isdir)
        st = below(fs, a->dir->ino, b->in.ino, &in_self);
    if (st == OK && in_self)
        st = ERR_INVALID_ARGS;
    if (st == OK && a->isdir && !(b->there && b->isdir))
        st = ext_nlink_room(b->dir);
    if (st == OK && swap && b->isdir && !a->isdir)
        st = ext_nlink_room(a->dir);
    return st;
}

/* Directory side x moves from its parent to `to`'s parent. */
static status_t moved_dir(struct ext_fs *fs, struct side *x, struct ext_inode *from,
                          struct ext_inode *to)
{
    ext_nlink_dec(from);
    ext_nlink_inc(to);
    return set_dotdot(fs, &x->in, to->ino);
}

/* A plain move or a replace: a's inode under b's name. */
static status_t move(struct ext_fs *fs, bool same, struct side *a, struct side *b,
                     const struct ext_rename *r, uint32_t *orphan)
{
    uint8_t type = (uint8_t)ext_itype(&a->in);
    status_t st = b->there ? ext_dirent_set(fs, b->dir, b->e.pos, b->in.ino, a->in.ino, type)
                           : ext_dirent_add(fs, b->dir, &r->to, a->in.ino, type);
    if (st == OK)
        st = ext_dirent_remove(fs, a->dir, a->e.pos, a->in.ino);
    if (st == OK && a->isdir && !same)
        st = moved_dir(fs, a, a->dir, b->dir);
    *orphan = 0;
    if (st != OK || !b->there)
        return st;
    if (b->isdir)
        ext_nlink_dec(b->dir);
    return ext_unlinked(fs, &b->in, &r->now, orphan);
}

/* An exchange: each entry at the other's inode. */
static status_t exchange(struct ext_fs *fs, bool same, struct side *a, struct side *b)
{
    status_t st = ext_dirent_set(fs, a->dir, a->e.pos, a->in.ino, b->in.ino,
                                 (uint8_t)ext_itype(&b->in));
    if (st == OK)
        st = ext_dirent_set(fs, b->dir, b->e.pos, b->in.ino, a->in.ino,
                            (uint8_t)ext_itype(&a->in));
    if (st == OK && !same && a->isdir)
        st = moved_dir(fs, a, a->dir, b->dir);
    if (st == OK && !same && b->isdir)
        st = moved_dir(fs, b, b->dir, a->dir);
    return st;
}

/* Write back what changed: the moved inodes (their change time) and the
 * directories (their times); the caller's copies made current. */
static status_t finish(struct ext_fs *fs, const struct ext_rename *r, bool same, struct side *a,
                       struct side *b)
{
    bool swap = r->flags & EXT_RENAME_EXCHANGE;
    status_t st = ext_touch(&a->in, &r->now, false);
    if (st == OK)
        st = ext_iput(fs, &a->in);
    if (st == OK && swap)
        st = ext_touch(&b->in, &r->now, false);
    if (st == OK && swap)
        st = ext_iput(fs, &b->in);
    if (st == OK)
        st = ext_touch(a->dir, &r->now, true);
    if (st == OK)
        st = ext_iput(fs, a->dir);
    if (st == OK && !same)
        st = ext_touch(b->dir, &r->now, true);
    if (st == OK && !same)
        st = ext_iput(fs, b->dir);
    if (st == OK) {
        *r->from_dir = *a->dir;
        *r->to_dir = *b->dir;
    }
    return st;
}

/* Both directories, read again (the caller's copies may be the same
 * inode twice), and both sides looked up and checked. */
static status_t sides(struct ext_fs *fs, const struct ext_rename *r, struct ext_inode dirs[2],
                      struct side *a, struct side *b)
{
    bool same = r->from_dir->ino == r->to_dir->ino;
    status_t st = ext_name_check(&r->from);
    if (st == OK)
        st = ext_name_check(&r->to);
    if (st == OK)
        st = ext_iget(fs, r->from_dir->ino, 0, &dirs[0]);
    if (st == OK && !same)
        st = ext_iget(fs, r->to_dir->ino, 0, &dirs[1]);
    struct ext_inode *to = same ? &dirs[0] : &dirs[1];
    if (st == OK && (ext_itype(&dirs[0]) != EXT_FT_DIR || ext_itype(to) != EXT_FT_DIR))
        st = ERR_NOT_DIR;
    if (st == OK)
        st = find_side(fs, &dirs[0], &r->from, a);
    if (st == OK && !a->there)
        st = ERR_NOT_FOUND;
    if (st == OK)
        st = find_side(fs, to, &r->to, b);
    if (st == OK && (r->flags & EXT_RENAME_EXCHANGE) && !b->there)
        st = ERR_NOT_FOUND;
    if (st == OK && (r->flags & EXT_RENAME_NOREPLACE) && b->there)
        st = ERR_ALREADY_EXISTS;
    return st;
}

status_t ext_rename(struct ext_fs *fs, const struct ext_rename *r, uint32_t *out_orphan)
{
    uint32_t both = EXT_RENAME_NOREPLACE | EXT_RENAME_EXCHANGE;
    if ((r->flags & ~both) || (r->flags & both) == both)
        return ERR_INVALID_ARGS;
    struct ext_inode dirs[2];
    struct side a, b;
    bool same = r->from_dir->ino == r->to_dir->ino;
    status_t st = sides(fs, r, dirs, &a, &b);
    if (st != OK)
        return st;
    uint32_t orphan = 0;
    if (b.there && b.in.ino == a.in.ino) {   /* two names of one inode: nothing to do */
        *out_orphan = 0;
        return OK;
    }
    st = check_sides(fs, r->flags, same, &a, &b);
    if (st == OK && (r->flags & EXT_RENAME_EXCHANGE))
        st = exchange(fs, same, &a, &b);
    else if (st == OK)
        st = move(fs, same, &a, &b, r, &orphan);
    if (st == OK)
        st = finish(fs, r, same, &a, &b);
    if (st == OK)
        *out_orphan = orphan;
    return st;
}
