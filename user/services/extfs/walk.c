/* extfs: paths (docs/M13.6-PLAN.md, "Paths and links" and Q8). A path
 * comes as the client sent it, uncleaned: extfs walks it a name at a time
 * from the view's root, ".." physically (to the directory the one it is
 * in lies in, never above the view's root), and follows each symbolic
 * link whose target is relative, in place of its name, inside the view.
 * An absolute target, or a ".." above the view's root, is not followed:
 * the request is answered ERR_LINK_OUT and changes nothing, and fs.resolve
 * says where the path goes on (the target and the rest of the path, or
 * the rest after that ".."), for libc to walk in the caller's own
 * namespace. A service never follows an absolute link (it doesn't know
 * the caller's namespace), so a link on a shared volume can't make one
 * program's open reach another's mounts.
 *
 * Where a path lies is kept as it is walked: `phys`, the names from the
 * volume's root to it (a view's root's place first, then each name; ".."
 * takes the last one off). The view's flags (<fsview.h>) are judged on
 * that place, not on the string asked for: a link into `etc` is refused
 * as `etc` itself is (the views' shared code, fs_view_dispatch, checks
 * the string first, and stays the first line). A directory the view hides
 * wholly (`etc/ssh` from a view that hides the keys; all of `etc` from one
 * that hides it all) can't even be walked through, so a path that goes in
 * and comes out with ".." learns nothing of what is inside.
 *
 * The permission bits (the plan's Q4, answered B): checked as Unix checks
 * an ordinary user's, for the one user. A file of uid FS_ONE_USER is judged
 * by its owner bits, anything else (a volume Linux made) by its others'
 * bits; the group's are not used. A directory's search bit for every
 * lookup in it, its write bit (and search) to add or remove a name, read
 * to list it; a file's read and write bits at its open. No user bypasses
 * them: any program with a writable view can chmod them back, so they are
 * a guard against mistakes, not authority. */
#include "extfs.h"

#define DEPTH_MAX (FS_PATH_LEN / 2 + 2)   /* directories a path can be below its view's root */

/* The path still to walk, at the end of rest_buf (a link's target is put
 * in front of it), and the directories walked into below the view's root
 * (stack[0] is the root). */
static char rest_buf[2 * FS_PATH_LEN + 2];
static char target[FS_PATH_LEN + 1];
static uint32_t stack[DEPTH_MAX];

bool may(const struct ext_inode *in, unsigned want)
{
    unsigned bits = ext_iuid(in) == FS_ONE_USER ? (in->d.mode >> 6) & 7u : in->d.mode & 7u;
    return (bits & want) == want;
}

static bool is_dir(const struct ext_inode *in)
{
    return (in->d.mode & EXT_S_IFMT) == EXT_S_IFDIR;
}

static bool is_link(const struct ext_inode *in)
{
    return (in->d.mode & EXT_S_IFMT) == EXT_S_IFLNK;
}

/* Append "/name" (or "name" at the volume's root) to w->phys. */
static status_t phys_push(struct xf_walk *w, const char *name, uint32_t len)
{
    uint32_t sep = w->phys_len ? 1 : 0;
    if (w->phys_len + sep + len > FS_PATH_LEN)
        return ERR_NAME_TOO_LONG;
    if (sep)
        w->phys[w->phys_len++] = '/';
    memcpy(w->phys + w->phys_len, name, len);
    w->phys_len += len;
    w->phys[w->phys_len] = '\0';
    return OK;
}

static void phys_pop(struct xf_walk *w)
{
    while (w->phys_len && w->phys[w->phys_len - 1] != '/')
        w->phys_len--;
    if (w->phys_len)
        w->phys_len--;
    w->phys[w->phys_len] = '\0';
}

/* A wholly hidden directory (the hiding flags' "stat" rule: etc itself is
 * hidden only when all of it is). */
static bool hidden_dir(const struct xf_ctx *c, const struct xf_walk *w)
{
    return (c->flags & FS_VIEW_NO_ETC) && fs_view_hides(c->flags, w->phys, true);
}

/* ERR_LINK_OUT: where the path goes on is head (hlen bytes; NULL: none)
 * joined with what is left of it (r, rlen). */
static status_t link_out(struct xf_walk *w, uint8_t kind, uint32_t used, const char *head,
                         uint32_t hlen, const char *r, uint32_t rlen)
{
    while (rlen && *r == '/') {
        r++;
        rlen--;
    }
    uint32_t sep = hlen && rlen ? 1 : 0;
    if ((uint64_t)hlen + sep + rlen >= FS_PATH_LEN)
        return ERR_NAME_TOO_LONG;
    memcpy(w->next, head, hlen);
    if (sep)
        w->next[hlen] = '/';
    memcpy(w->next + hlen + sep, r, rlen);
    w->next_len = hlen + sep + rlen;
    w->next[w->next_len] = '\0';
    w->kind = kind;
    w->used = used;
    return ERR_LINK_OUT;
}

status_t walk(const struct xf_ctx *c, const char *path, uint32_t len, uint32_t flags,
              struct xf_walk *w)
{
    struct ext_fs *fs = &vol.fs;
    w->found = true;
    w->has_name = w->must_dir = false;
    w->kind = 0;
    w->used = 0;
    w->next_len = 0;
    w->name = (struct ext_name){ .s = w->namebuf, .len = 0 };
    if (len > FS_PATH_LEN || c->root_len >= sizeof(w->phys))
        return ERR_NAME_TOO_LONG;
    memcpy(w->phys, c->root, c->root_len);
    w->phys_len = c->root_len;
    w->phys[w->phys_len] = '\0';

    struct ext_inode cur;
    status_t st = ext_iget(fs, c->root_ino, 0, &cur);
    if (st == OK && c->view >= 0 && cur.d.generation != c->root_igen)
        st = ERR_NOT_FOUND;   /* the view's root was removed (and its inode used again) */
    if (st == OK && !is_dir(&cur))
        st = ERR_NOT_DIR;
    if (st != OK)
        return st;
    unsigned depth = 0;
    stack[0] = c->root_ino;
    w->in = cur;

    /* rest: what is left to walk, at the end of rest_buf; orig: how much of
     * it is still the caller's own path (the rest came from links). */
    char *r = rest_buf + sizeof(rest_buf) - len;
    memcpy(r, path, len);
    uint32_t rlen = len, orig = len, links = 0;
    for (;;) {
        while (rlen && *r == '/') {
            r++;
            rlen--;
        }
        if (orig > rlen)
            orig = rlen;
        if (!rlen)
            return OK;
        const char *comp = r;
        uint32_t clen = 0;
        while (clen < rlen && comp[clen] != '/')
            clen++;
        r += clen;
        rlen -= clen;
        if (orig > rlen)
            orig = rlen;
        uint32_t used = len - orig;   /* of the caller's path, up to the end of this name */
        uint32_t k = 0;
        while (k < rlen && r[k] == '/')
            k++;
        bool last = k == rlen, trailing = last && rlen;

        if (clen == 1 && comp[0] == '.') {
            w->has_name = false;
            w->found = true;
            w->in = cur;
            w->must_dir = trailing;
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            if (!depth)
                return link_out(w, FS_RESOLVE_ABOVE, used, NULL, 0, r, rlen);
            depth--;
            phys_pop(w);
            st = ext_iget(fs, stack[depth], 0, &cur);
            if (st != OK)
                return st == ERR_NOT_FOUND ? ERR_IO : st;   /* a directory we came through */
            w->has_name = false;
            w->found = true;
            w->in = cur;
            w->must_dir = trailing;
            continue;
        }
        if (clen > EXT_NAME_MAX)
            return ERR_NAME_TOO_LONG;
        if (!may(&cur, 1))
            return ERR_ACCESS_DENIED;
        memcpy(w->namebuf, comp, clen);
        w->namebuf[clen] = '\0';
        w->name.len = clen;
        w->dir = cur;
        w->has_name = true;
        w->must_dir = trailing;
        struct ext_dentry e;
        st = ext_lookup(fs, &cur, &w->name, &e);
        if (st == ERR_NOT_FOUND) {
            if (!last)
                return ERR_NOT_FOUND;
            w->found = false;
            return phys_push(w, comp, clen);
        }
        if (st != OK)
            return st;
        st = ext_iget(fs, e.ino, 0, &w->in);
        if (st != OK)
            return st == ERR_NOT_FOUND ? ext_damaged(fs, "an entry of a free inode", e.ino, 0)
                                       : st;
        w->found = true;
        if (is_link(&w->in) && (!last || trailing || !(flags & W_NOFOLLOW))) {
            if (++links > XF_LINKS_MAX)
                return ERR_LINK_LOOP;   /* a loop: ELOOP */
            size_t tlen = 0;
            st = ext_readlink(fs, &w->in, target, FS_PATH_LEN, &tlen);
            if (st == ERR_BUFFER_TOO_SMALL)
                return ERR_NAME_TOO_LONG;
            if (st != OK)
                return st;
            if (!tlen)
                return ERR_NOT_FOUND;
            while (rlen && tlen > 1 && target[tlen - 1] == '/')
                tlen--;   /* the rest brings its own '/' */
            if (target[0] == '/')
                return link_out(w, FS_RESOLVE_ABSOLUTE, used, target, (uint32_t)tlen, r, rlen);
            /* In the name's place: the target, then the rest, which
             * starts with its '/' (a '/' after the last name would ask
             * for a directory, so an empty rest adds none). */
            if (tlen + rlen > sizeof(rest_buf))
                return ERR_NAME_TOO_LONG;
            char *nr = rest_buf + sizeof(rest_buf) - (tlen + rlen);
            memmove(nr + tlen, r, rlen);
            memcpy(nr, target, tlen);
            r = nr;
            rlen += (uint32_t)tlen;
            w->has_name = false;
            continue;
        }
        if (is_dir(&w->in)) {
            if (depth + 1 >= DEPTH_MAX)
                return ERR_NAME_TOO_LONG;
            st = phys_push(w, comp, clen);
            if (st != OK)
                return st;
            if (hidden_dir(c, w))
                return ERR_ACCESS_DENIED;
            stack[++depth] = e.ino;
            cur = w->in;
            continue;
        }
        if (!last || trailing)
            return ERR_NOT_DIR;
        return phys_push(w, comp, clen);
    }
}

void walk_inside(const struct xf_ctx *c, const struct xf_walk *w, char *out, uint32_t *out_len)
{
    uint32_t skip = c->root_len;
    if (skip && w->phys_len > skip)
        skip++;   /* the '/' after the root's place */
    uint32_t n = w->phys_len > skip ? w->phys_len - skip : 0;
    memcpy(out, w->phys + w->phys_len - n, n);
    *out_len = n;
}

bool walk_hidden(const struct xf_ctx *c, const struct xf_walk *w, bool stat)
{
    return (c->flags & FS_VIEW_NO_ETC) && fs_view_hides(c->flags, w->phys, stat);
}

bool walk_may_change(const struct xf_ctx *c, const struct xf_walk *w)
{
    if (c->flags & FS_VIEW_READ_ONLY)
        return false;
    return !((c->flags & FS_VIEW_GUARD_ETC) && fs_view_in_etc(w->phys));
}

/* ---- attributes ---------------------------------------------------------------------- */

static int64_t ns_of(const struct ext_time *t)
{
    const int64_t lim = INT64_MAX / 1000000000 - 1;
    if (t->sec > lim)
        return INT64_MAX;
    if (t->sec < -lim)
        return INT64_MIN;
    return t->sec * 1000000000 + t->nsec;
}

void attr_of(const struct ext_inode *in, struct xf_attr *a)
{
    struct ext_time at, mt, ct;
    ext_itime(in, EXT_ATIME, &at);
    ext_itime(in, EXT_MTIME, &mt);
    ext_itime(in, EXT_CTIME, &ct);
    *a = (struct xf_attr){
        .mode = in->d.mode, .nlink = in->d.links_count, .uid = ext_iuid(in),
        .gid = ext_igid(in), .size = ext_isize(in), .blocks = ext_iblocks512(&vol.fs, in),
        .id = in->ino, .atime_ns = ns_of(&at), .mtime_ns = ns_of(&mt), .ctime_ns = ns_of(&ct),
    };
}

static struct ext_time time_of(int64_t ns)
{
    int64_t sec = ns / 1000000000, rem = ns % 1000000000;
    if (rem < 0) {
        rem += 1000000000;
        sec--;
    }
    return (struct ext_time){ .sec = sec, .nsec = (uint32_t)rem };
}

status_t attr_set(struct ext_inode *in, uint32_t mask, uint32_t mode, uint32_t uid,
                  uint32_t gid, int64_t atime_ns, int64_t mtime_ns)
{
    if (mask & ~(uint32_t)FS_ATTR_MASK)
        return ERR_INVALID_ARGS;
    if (((mask & FS_ATTR_UID) && uid != FS_ONE_USER) ||
        ((mask & FS_ATTR_GID) && gid != FS_ONE_USER))
        return ERR_ACCESS_DENIED;   /* no other user, and no root */
    if (!writable())
        return ERR_ACCESS_DENIED;
    if (mask && ext_iuid(in) != FS_ONE_USER)
        return ERR_ACCESS_DENIED;   /* only the owner changes them (POSIX's EPERM) */
    if (mask & FS_ATTR_MODE)
        in->d.mode = (uint16_t)((in->d.mode & EXT_S_IFMT) | (mode & 07777u));
    if (mask & FS_ATTR_UID) {
        in->d.uid = (uint16_t)uid;
        in->d.uid_high = (uint16_t)(uid >> 16);
    }
    if (mask & FS_ATTR_GID) {
        in->d.gid = (uint16_t)gid;
        in->d.gid_high = (uint16_t)(gid >> 16);
    }
    struct ext_time t;
    status_t st = OK;
    if (mask & FS_ATTR_ATIME) {
        t = time_of(atime_ns);
        st = ext_itime_set(in, EXT_ATIME, &t);
    }
    if (st == OK && (mask & FS_ATTR_MTIME)) {
        t = time_of(mtime_ns);
        st = ext_itime_set(in, EXT_MTIME, &t);
    }
    if (st == ERR_OUT_OF_RANGE)
        return ERR_INVALID_ARGS;
    if (st != OK || !mask)
        return st;
    time_now(&t);
    st = ext_itime_set(in, EXT_CTIME, &t);
    return st == OK ? ext_iput(&vol.fs, in) : st;
}
