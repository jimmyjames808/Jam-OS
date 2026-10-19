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
 * A directory channel's path (fs.opendir) starts at its directory unless
 * it starts with '/' (docs/DIRHANDLE-PLAN.md 3.1): a ".." there is the
 * directory's own ".." entry, which ext keeps right across renames, up to
 * the view's root and ERR_LINK_OUT above it as from the root itself. One
 * removed while its channel holds it has no place and no name: "" and "."
 * are it, ".." the parent it was removed from, nothing else (walk_gone).
 *
 * Where a path lies is kept as it is walked: its region (<fsview.h>: the
 * depth below the volume's root and its first two names' class; the
 * start's first, then each name pushed; ".." pops one), and `rel`, the
 * names below where it started as text, which only fs.resolve's answer
 * reads (3.2): nothing is ever text from the volume's root, so no place is
 * too deep to walk from. Where a view's root or a directory channel's
 * directory lies is found by climbing its ".." entries to the volume's
 * root and kept (struct xf_place) until a rename moves a directory (the
 * epoch): a subview whose root was moved into `etc` is judged there, and a
 * directory channel whose directory was moved out of its view's reach, or
 * where the view hides, is cut off (ERR_NOT_FOUND, Q2) while it lies
 * there. The view's flags are judged on the region, where the path
 * resolves, not on the string asked for: a link into `etc` is refused as
 * `etc` itself is (the views' shared code, fs_view_dispatch, checks the
 * string first, and stays the first line).
 * A directory the view hides wholly (`etc/ssh` from a view that hides the
 * keys; all of `etc` from one that hides it all) can't even be walked
 * through, so a path that goes in and comes out with ".." learns nothing
 * of what is inside.
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

#define DEPTH_MAX (FS_PATH_LEN / 2 + 2)   /* directories a path can be below its start */
#define CLIMB_MAX 65536u                  /* levels a climb goes up at most */

/* The path still to walk, at the end of rest_buf (a link's target is put
 * in front of it), and the directories walked into below where it
 * started (stack[0]: the view's root or a directory channel's directory). */
static char rest_buf[2 * FS_PATH_LEN + 2];
static char target[FS_PATH_LEN + 1];
static uint32_t stack[DEPTH_MAX];
static char up_buf[FS_PATH_LEN + 1];   /* walk_inside's climb, named from its end */

/* Places found at an older epoch are found again (walk_moved). A
 * successor starts at 1, so every place is climbed once after a restart. */
static uint64_t epoch = 1;

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

/* w goes on to the name (len bytes) in the place it has reached: its
 * region, and "/name" (or "name" first) on w->rel while it fits. */
static void place_push(struct xf_walk *w, const char *name, uint32_t len)
{
    uint32_t sep = w->rel_len ? 1 : 0;
    w->region = fs_region_child(w->region, name, len);
    if (w->rel_long || w->rel_len + sep + len > FS_PATH_LEN) {
        w->rel_long = true;   /* only fs.resolve's answer needed it */
        return;
    }
    if (sep)
        w->rel[w->rel_len++] = '/';
    memcpy(w->rel + w->rel_len, name, len);
    w->rel_len += len;
    w->rel[w->rel_len] = '\0';
}

/* w goes back to the directory its place lies in (".."). */
static void place_pop(struct xf_walk *w)
{
    while (w->rel_len && w->rel[w->rel_len - 1] != '/')
        w->rel_len--;
    if (w->rel_len)
        w->rel_len--;
    w->rel[w->rel_len] = '\0';
    w->region = fs_region_parent(w->region);
}

/* ---- where a directory lies -------------------------------------------------------- */

void walk_moved(void)
{
    epoch++;
}

struct xf_place walk_place_of(const struct xf_walk *w)
{
    return (struct xf_place){ .epoch = epoch, .st = OK, .region = w->region };
}

/* The directory in's ".." entry: its parent (0: none, damaged). "." and
 * ".." are a directory's first two entries. */
static uint32_t parent_of(struct ext_inode *in)
{
    uint64_t pos = 0;
    struct ext_dentry e;
    for (unsigned i = 0; i < 2 && ext_dir_next(&vol.fs, in, &pos, &e) == OK && e.ino; i++)
        if (e.name_len == 2 && e.name[0] == '.' && e.name[1] == '.')
            return e.ino;
    return 0;
}

/* The name directory child has in directory parent (a scan of its
 * entries), into name (EXT_NAME_MAX + 1 bytes). ERR_IO if it has none: its
 * ".." names a parent that doesn't hold it. */
static status_t name_of(uint32_t parent, uint32_t child, char *name, uint32_t *len)
{
    struct ext_inode p;
    struct ext_dentry e;
    uint64_t pos = 0;
    status_t st = ext_iget(&vol.fs, parent, 0, &p);
    while (st == OK && (st = ext_dir_next(&vol.fs, &p, &pos, &e)) == OK && e.ino) {
        bool dot = e.name[0] == '.' && (e.name_len == 1 || (e.name_len == 2 && e.name[1] == '.'));
        if (e.ino != child || dot)
            continue;
        memcpy(name, e.name, e.name_len);
        *len = e.name_len;
        return OK;
    }
    return st == OK || st == ERR_NOT_FOUND ? ERR_IO : st;
}

status_t walk_place(struct xf_place *p, uint32_t ino, uint32_t gen, uint32_t stop,
                    uint32_t stop_gen)
{
    if (p->epoch == epoch)
        return p->st;
    struct ext_fs *fs = &vol.fs;
    struct ext_inode in;
    /* below[0]: the last directory met before the root (the root's child
     * on the way), below[1] the one before it. */
    uint32_t depth = 0, at = ino, below[2] = { 0, 0 };
    bool reach = !stop;
    status_t st;
    for (;;) {
        st = ext_iget(fs, at, 0, &in);
        if (st == OK && ((gen && in.d.generation != gen) || !is_dir(&in)))
            st = ERR_NOT_FOUND;
        if (st == ERR_OUT_OF_RANGE)
            st = ERR_NOT_FOUND;
        if (st != OK) {
            if (at != ino && st == ERR_NOT_FOUND)
                st = ERR_IO;   /* a ".." naming no directory */
            break;
        }
        if (at == stop && (!stop_gen || in.d.generation == stop_gen))
            reach = true;
        if (at == EXT_ROOT_INO)
            break;
        if (++depth > CLIMB_MAX || !(at = parent_of(&in))) {
            st = depth > CLIMB_MAX ? ERR_NAME_TOO_LONG : ERR_IO;
            break;
        }
        below[1] = below[0];
        below[0] = in.ino;
        gen = 0;
    }
    /* Its region: the class of the first two names (the second only in
     * etc), named by a scan of the root and of etc. */
    struct fs_region r = { .depth = 0, .cls = FS_REGION_ROOT };
    char name[EXT_NAME_MAX + 1];
    uint32_t len = 0;
    if (st == OK && depth && (st = name_of(EXT_ROOT_INO, below[0], name, &len)) == OK)
        r = fs_region_child(r, name, len);
    if (st == OK && depth >= 2 && r.cls == FS_REGION_ETC &&
        (st = name_of(below[0], below[1], name, &len)) == OK)
        r = fs_region_child(r, name, len);
    r.depth = depth;
    if (st == OK && !reach)
        st = ERR_NOT_FOUND;
    *p = (struct xf_place){ .epoch = st == ERR_IO ? 0 : epoch, .st = st, .region = r };
    return st;
}

/* Where a walk starts (*out): the view's root, or (from_dir) a directory
 * channel's directory, cut off while it lies outside the view or where the
 * view hides (ERR_NOT_FOUND). */
static status_t start_of(const struct xf_ctx *c, bool from_dir, struct fs_region *out)
{
    struct xf_place here = { 0 };
    if (from_dir) {
        struct xf_place *p = c->dir_at ? c->dir_at : &here;
        status_t st = walk_place(p, c->dir_ino, c->dir_igen, c->root_ino,
                                 c->view >= 0 ? c->root_igen : 0);
        if (st == OK && fs_region_hides(c->flags, p->region, false))
            st = ERR_NOT_FOUND;
        *out = p->region;
        return st;
    }
    *out = (struct fs_region){ .depth = 0, .cls = FS_REGION_ROOT };
    if (c->root_ino == EXT_ROOT_INO)
        return OK;
    struct xf_place *p = c->root_at ? c->root_at : &here;
    status_t st = walk_place(p, c->root_ino, c->view >= 0 ? c->root_igen : 0, 0, 0);
    *out = p->region;
    return st;
}

static status_t link_out(struct xf_walk *w, uint8_t kind, uint32_t used, const char *head,
                         uint32_t hlen, const char *r, uint32_t rlen);

/* Removed, and held: directory ino of generation gen with no name left,
 * which a directory channel keeps on the orphan list (dirs.c); into *in. */
static bool gone_dir(uint32_t ino, uint32_t gen, struct ext_inode *in)
{
    return ext_iget(&vol.fs, ino, EXT_IGET_UNLINKED, in) == OK && in->d.generation == gen &&
           is_dir(in) && !in->d.links_count;
}

/* A path from removed directory *in (docs/DIRHANDLE-PLAN.md 3.3): "" and
 * "." name it (the answer with W_GONE: w->gone), ".." the parent it had
 * (dirs.c remembers it: ERR_NOT_FOUND if that inode is free or used
 * again), removed too (and held) or not; every other name ERR_NOT_FOUND.
 * At a parent
 * that wasn't removed: *in is it, w->region where it lies (in the view's
 * reach, not where it hides, else ERR_NOT_FOUND) and *skip the bytes of
 * path walked; the rest is walked from it. */
static status_t walk_gone(const struct xf_ctx *c, const char *path, uint32_t len, uint32_t flags,
                          struct ext_inode *in, uint32_t *skip, struct xf_walk *w)
{
    uint32_t at = 0;
    for (;;) {
        while (at < len && path[at] == '/')
            at++;
        uint32_t clen = 0;
        while (at + clen < len && path[at + clen] != '/')
            clen++;
        if (!clen) {
            if (!(flags & W_GONE))
                return ERR_NOT_FOUND;   /* readdir, a change, resolve: it has no name */
            w->gone = w->found = true;
            w->in = *in;
            w->region = (struct fs_region){ .depth = 0, .cls = FS_REGION_OTHER };   /* none */
            return OK;
        }
        const char *comp = path + at;
        at += clen;
        if (clen == 1 && comp[0] == '.')
            continue;
        if (clen != 2 || comp[0] != '.' || comp[1] != '.')
            return ERR_NOT_FOUND;
        if (in->ino == c->root_ino && (c->view < 0 || in->d.generation == c->root_igen))
            return link_out(w, FS_RESOLVE_ABOVE, at, NULL, 0, path + at, len - at);
        uint32_t up = 0, up_gen = 0;
        struct ext_inode p;
        if (!dirs_up(in->ino, in->d.generation, &up, &up_gen))
            return ERR_NOT_FOUND;
        status_t st = ext_iget(&vol.fs, up, EXT_IGET_UNLINKED, &p);
        if (st == ERR_OUT_OF_RANGE || (st == OK && (p.d.generation != up_gen || !is_dir(&p))))
            st = ERR_NOT_FOUND;
        if (st != OK)
            return st;
        *in = p;
        if (!p.d.links_count && !dirs_has(p.ino))
            return ERR_NOT_FOUND;   /* freed (ext_iget reads a free inode as it was left) */
        if (!p.d.links_count)
            continue;               /* removed too, and held */
        struct xf_place pl = { 0 };
        st = walk_place(&pl, p.ino, p.d.generation, c->root_ino, c->view >= 0 ? c->root_igen : 0);
        if (st == OK && fs_region_hides(c->flags, pl.region, false))
            st = ERR_NOT_FOUND;
        if (st != OK)
            return st;
        w->region = pl.region;
        *skip = at;
        return OK;
    }
}

/* A wholly hidden directory (the hiding flags' "stat" rule: etc itself is
 * hidden only when all of it is). */
static bool hidden_dir(const struct xf_ctx *c, const struct xf_walk *w)
{
    return fs_region_hides(c->flags, w->region, true);
}

/* Does the view hide the name comp (clen bytes) in the directory w has
 * reached? For what a walk goes through or follows: a view that hides
 * part of etc (the SSH server's) lets the walk into etc, and a hidden name
 * there must give nothing away, not whether it is there, nor what it is
 * (ERR_NOT_DIR), nor a link's target (fs.resolve's answer). */
static bool name_hidden(const struct xf_ctx *c, const struct xf_walk *w, const char *comp,
                        uint32_t clen)
{
    return fs_region_hides(c->flags, fs_region_child(w->region, comp, clen), false);
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
    w->gone = false;
    w->has_name = w->must_dir = false;
    w->kind = 0;
    w->used = 0;
    w->next_len = 0;
    w->name = (struct ext_name){ .s = w->namebuf, .len = 0 };
    w->rel_len = 0;
    w->rel[0] = '\0';
    w->rel_long = false;
    if (len > FS_PATH_LEN)
        return ERR_NAME_TOO_LONG;
    bool from_dir = c->dir_ino && !(len && path[0] == '/');
    uint32_t start = from_dir ? c->dir_ino : c->root_ino, skip = 0;
    struct ext_inode cur;
    status_t st = OK;
    if (c->dir_ino && gone_dir(c->dir_ino, c->dir_igen, &cur)) {
        /* Removed, held by this channel: itself, or its ".." to walk on. */
        st = from_dir ? walk_gone(c, path, len, flags, &cur, &skip, w) : ERR_NOT_FOUND;
        if (st != OK || w->gone)
            return st;
        start = cur.ino;
    } else if (c->dir_ino) {
        /* A directory channel's directory, whatever the path: there (not
         * removed, its inode not used again), and where its view reaches. */
        st = start_of(c, true, &w->region);
        if (st == OK)
            st = ext_iget(fs, c->dir_ino, 0, &cur);
        if (st == OK && (cur.d.generation != c->dir_igen || !is_dir(&cur)))
            st = ERR_NOT_FOUND;
        if (st != OK)
            return st == ERR_OUT_OF_RANGE ? ERR_NOT_FOUND : st;
    }
    if (!from_dir) {
        st = start_of(c, false, &w->region);
        if (st == OK)
            st = ext_iget(fs, c->root_ino, 0, &cur);
        if (st == OK && c->view >= 0 && cur.d.generation != c->root_igen)
            st = ERR_NOT_FOUND;   /* the view's root was removed (and its inode used again) */
        if (st == OK && !is_dir(&cur))
            st = ERR_NOT_DIR;
        if (st != OK)
            return st;
    }
    unsigned depth = 0;
    stack[0] = w->base = start;
    w->in = cur;

    /* rest: what is left to walk, at the end of rest_buf; orig: how much of
     * it is still the caller's own path (the rest came from links). */
    char *r = rest_buf + sizeof(rest_buf) - (len - skip);
    memcpy(r, path + skip, len - skip);
    uint32_t rlen = len - skip, orig = len - skip, links = 0;
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
            if (!depth && stack[0] == c->root_ino &&
                (c->view < 0 || cur.d.generation == c->root_igen))
                return link_out(w, FS_RESOLVE_ABOVE, used, NULL, 0, r, rlen);
            if (!depth) {
                /* A directory channel's start (or a directory its ".."
                 * reached): its own "..". One that reaches the volume's
                 * root before the view's has left the view. */
                uint32_t up = stack[0] == EXT_ROOT_INO ? 0 : parent_of(&cur);
                if (!up)
                    return ERR_NOT_FOUND;
                st = ext_iget(fs, up, 0, &cur);
                if (st == OK && !is_dir(&cur))
                    st = ERR_IO;
                if (st != OK)
                    return st == ERR_NOT_FOUND ? ERR_IO : st;
                stack[0] = w->base = up;
                w->region = fs_region_parent(w->region);
                w->has_name = false;
                w->found = true;
                w->in = cur;
                w->must_dir = trailing;
                continue;
            }
            depth--;
            place_pop(w);
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
                return name_hidden(c, w, comp, clen) ? ERR_ACCESS_DENIED : ERR_NOT_FOUND;
            w->found = false;
            place_push(w, comp, clen);
            return OK;
        }
        if (st != OK)
            return st;
        st = ext_iget(fs, e.ino, 0, &w->in);
        if (st != OK)
            return st == ERR_NOT_FOUND ? ext_damaged(fs, "an entry of a free inode", e.ino, 0)
                                       : st;
        w->found = true;
        if (is_link(&w->in) && (!last || trailing || !(flags & W_NOFOLLOW))) {
            if (name_hidden(c, w, comp, clen))
                return ERR_ACCESS_DENIED;   /* its target is the view's to hide too */
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
            place_push(w, comp, clen);
            if (hidden_dir(c, w))
                return ERR_ACCESS_DENIED;
            stack[++depth] = e.ino;
            cur = w->in;
            continue;
        }
        if (!last || trailing)
            return name_hidden(c, w, comp, clen) ? ERR_ACCESS_DENIED : ERR_NOT_DIR;
        place_push(w, comp, clen);
        return OK;
    }
}

status_t walk_inside(const struct xf_ctx *c, const struct xf_walk *w, char *out, uint32_t *out_len)
{
    if (w->rel_long)
        return ERR_NAME_TOO_LONG;
    /* Where the walk's names start, below the view's root: nothing on any
     * channel but a directory channel's, whose directory (or one its ".."
     * reached) is named by climbing to the root, each name found in its
     * parent, into up_buf from its end. */
    uint32_t at = w->base, top = sizeof(up_buf);
    while (at != c->root_ino) {
        struct ext_inode in;
        char name[EXT_NAME_MAX + 1];
        uint32_t len = 0, up = 0;
        status_t st = at == EXT_ROOT_INO ? ERR_NOT_FOUND : ext_iget(&vol.fs, at, 0, &in);
        if (st == OK && !(up = parent_of(&in)))
            st = ERR_IO;
        if (st == OK)
            st = name_of(up, at, name, &len);
        if (st == OK && len + (top < sizeof(up_buf) ? 1 : 0) > top)
            st = ERR_NAME_TOO_LONG;
        if (st != OK)
            return st;
        if (top < sizeof(up_buf))
            up_buf[--top] = '/';
        top -= len;
        memcpy(up_buf + top, name, len);
        at = up;
    }
    uint32_t n = (uint32_t)sizeof(up_buf) - top, sep = n && w->rel_len ? 1 : 0;
    if (n + sep + w->rel_len > FS_PATH_LEN)
        return ERR_NAME_TOO_LONG;
    memcpy(out, up_buf + top, n);
    if (sep)
        out[n] = '/';
    memcpy(out + n + sep, w->rel, w->rel_len);
    *out_len = n + sep + w->rel_len;
    return OK;
}

bool walk_hidden(const struct xf_ctx *c, const struct xf_walk *w, bool stat)
{
    return !w->gone && fs_region_hides(c->flags, w->region, stat);
}

bool walk_may_change(const struct xf_ctx *c, const struct xf_walk *w)
{
    if (c->flags & FS_VIEW_READ_ONLY)
        return false;
    return !((c->flags & FS_VIEW_GUARD_ETC) && fs_region_in_etc(w->region)) &&
           !((c->flags & FS_VIEW_GUARD_PKG) && fs_region_in_pkg(w->region));
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
