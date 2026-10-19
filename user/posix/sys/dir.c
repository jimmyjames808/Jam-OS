/* Directories (docs/M13-PLAN.md section 3): the directory kind of
 * descriptor and its listing (getdents64), and the calls that change a
 * directory or the current one: mkdir, unlink, rmdir, rename, chdir,
 * fchdir and getcwd.
 *
 * A directory description is one of two (docs/DIRHANDLE-PLAN.md 4.1):
 *   - a **channel**: on a mount whose fs.statfs says FS_FEAT_DIRS (extfs),
 *     the directory's own `fs` channel (fs.opendir), with its mount's
 *     name, its id and volume and its last known text (a hint: NULL when
 *     too long). Relative paths from it are sent on it as written
 *     (path.c), so it works at any depth and follows a rename. A channel
 *     whose service went (ERR_PEER_CLOSED: the mount went, or a fresh
 *     start of extfs) is replaced by an open of the last known text if
 *     that is still the same directory (its id), else the description is
 *     gone and answers ENOENT;
 *   - a **text** description, its absolute path, as before: on FAT, the
 *     boot image, "/" and /svc, and when the service has no room for
 *     another channel (256 a volume, 64 a process: ERR_NO_RESOURCES, said
 *     once in the log), where renames are not followed and depth stops at
 *     PATH_BYTES.
 * Both hold a position in the listing. The listing starts with "." and
 * "..", which the services leave out, then the service's entries in its
 * batches (fs.readdir: a cookie asks for a batch, and each batch says the
 * cookie of the next). A position is where the next getdents64 starts,
 * and is what d_off, telldir and seekdir carry:
 *     0                       "."
 *     1                       ".."
 *     2 + (cookie << 16 | k)  entry k of the batch asked for with cookie
 * (a batch holds at most FS_READDIR_BYTES / (FS_DIRENT_FIXED + 1) < 2^16
 * entries). The last batch asked for is kept with the description, so a
 * listing read in order asks the service once per batch; one started
 * again (from ".", or after any lseek: rewinddir, seekdir) asks afresh,
 * so it sees the directory as it is now. A channel's listing asks for its
 * first batch before "." (a removed directory is ENOENT from the start,
 * as Linux's). "/" lists the mounts and "svc", "/svc" the services
 * (libjam's fs_readdir, by index: 2 + index).
 *
 * The answers follow Linux's for the same steps where FAT allows them
 * (the errno table is posixtest's files section); rename replaces what
 * is at the new name (fs.rename's FS_RENAME_REPLACE). A rename or link
 * between two directory descriptors is fs.rename_at or fs.link_at
 * (dir_meet). Children: a forked child gets a duplicate of a channel (the
 * fork hook, DL1's); a spawned or exec'd one and a socket's receiver the
 * channel itself (POSIXFDS_DIR_CHANNEL, DL2; a socket's receiver without
 * its text), a text description its path. The current directory is one
 * of these descriptions too (path.c keeps it; chdir, fchdir and getcwd at
 * the end of this file). */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include "files.h"

#define POS_DOTDOT  1
#define POS_ENTRIES 2
#define K_BITS      16
#define K_MASK      ((1u << K_BITS) - 1)
#define DIRENT_HEAD offsetof(struct dirent, d_name)   /* 19: Linux's linux_dirent64 */
#define ID_TOP      1000u   /* "/"'s and /svc's entries: ids of the layer's own, from here */

struct dir {
    char    *path;       /* a text description's path (absolute and clean); a channel's
                          * last known text, a hint (NULL: not known, or too long) */
    bool     chan;       /* a directory channel's description (else a text one) */
    handle_t ch;         /* its channel; HANDLE_INVALID once gone; ch_lock */
    bool     gone;       /* the channel went and wasn't made again: ENOENT; ch_lock */
    lock_t   ch_lock;    /* ch and gone: held for a duplicate and across a replacement (no
                          * other lock is taken under it) */
    uint32_t flags;      /* fs.opendir's: FS_READ, unless it was opened O_PATH */
    char     mount[NS_NAME_MAX];   /* a channel's mount ("/data") */
    uint64_t id;         /* a channel's directory (getattr's id) and its volume (fs.opendir) */
    uint64_t volume;
    uint8_t *batch;      /* the last batch (FS_READDIR_BYTES), made at the first listing */
    uint32_t blen;       /* its bytes */
    uint32_t bcookie;    /* the cookie it was asked with */
    uint32_t bnext;      /* the next batch's cookie; 0: it was the last */
    uint16_t bcount;     /* its entries */
    bool     bvalid;     /* batch holds bcookie's answer */
    uint16_t at_k;       /* the entry at byte at_off of batch (a cursor for reading in order) */
    uint32_t at_off;
};

/* One entry of a listing, as getdents64 writes it. */
struct entry {
    char     name[FS_NAME_MAX];
    uint64_t ino;
    uint8_t  type;     /* DT_* */
    int64_t  next;     /* the position after it */
};

static const struct fd_ops dir_ops;
static bool said_full;   /* the log's line about a service with no room: once a process */

bool dir_is(const struct fd_desc *d)
{
    return d->ops == &dir_ops;
}

/* ---- channels ------------------------------------------------------------------ */

/* fs.opendir of p with flags (FS_READ, FS_NOFOLLOW): *ch, *id, *volume. */
static status_t opendir_raw(const struct path *p, uint32_t flags, handle_t *ch, uint64_t *id,
                            uint64_t *volume)
{
    handle_t fs;
    if (path_fs(p, &fs))
        return ERR_NOT_FOUND;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t st = fs_opendir_within(fs, FILE_TIMEOUT, rel, len, flags, ch, id, volume);
    jam_handle_close(fs);
    return st;
}

/* x's channel went (its service ended): an open of the last known text
 * through the namespace in its place, if that is the same directory (its
 * id on the same volume: an id is an inode number, which another volume
 * mounted at the name has too); else none, and x is gone. x->ch_lock. */
static void replace_locked(struct dir *x)
{
    jam_handle_close(x->ch);
    x->ch = HANDLE_INVALID;
    x->gone = true;
    struct path *p = x->path ? jam_malloc(sizeof(*p)) : NULL;
    if (!p)
        return;
    handle_t ch = HANDLE_INVALID;
    uint64_t id = 0, volume = 0;
    if (!path_set(x->path, p) && p->where == PATH_MOUNT &&
        opendir_raw(p, x->flags, &ch, &id, &volume) == OK) {
        if (id == x->id && volume == x->volume) {
            x->ch = ch;
            x->gone = false;
        } else {
            jam_handle_close(ch);   /* another directory has the name now */
        }
    }
    path_done(p);
    jam_free(p);
}

long dir_channel(struct fd_desc *d, handle_t *out)
{
    struct dir *x = d->priv;
    long r = 0;
    lock_take(&x->ch_lock);
    if (x->ch && jam_object_wait_one(x->ch, SIG_PEER_CLOSED, 0, NULL) == OK)
        replace_locked(x);
    if (!x->ch)
        r = -ENOENT;   /* gone */
    else if (jam_handle_duplicate(x->ch, RIGHT_SAME, out) != OK)
        r = -EIO;
    lock_give(&x->ch_lock);
    return r;
}

const char *dir_mount(const struct fd_desc *d)
{
    const struct dir *x = d->priv;
    return x->mount;
}

/* path_get's base (files.h). (path_done: the caller's) */
long dir_from(struct fd_desc *d, const char *in, size_t n, struct path *out)
{
    const struct dir *x = d->priv;
    if (x->chan) {
        long r = path_in_dir(d, in, n, out);
        return r ? r : 1;
    }
    memcpy(out->abs, x->path, strlen(x->path) + 1);
    return 0;
}

/* The last known text of a directory opened at p: p's own if it is a path
 * from "/" (its trailing slashes dropped), else at's joined with p's path
 * as written, if at has one and it fits. NULL: none (or no memory). */
static char *hint_of(const struct path *p)
{
    uint32_t len;
    const char *rel = path_rel(p, &len);
    const struct dir *at = p->at ? p->at->priv : NULL;
    const char *base = at ? at->path : p->abs;
    if (!base)
        return NULL;
    size_t bl = strlen(base);
    if (!at) {
        len = 0;
        while (bl > p->mount_len + 1 && base[bl - 1] == '/')
            bl--;
    }
    while (len && rel[len - 1] == '/')
        len--;
    size_t n = bl + (len ? 1 + len : 0);
    char *text = n < PATH_BYTES ? jam_malloc(n + 1) : NULL;
    if (!text)
        return NULL;
    memcpy(text, base, bl);
    if (len) {
        text[bl] = '/';
        memcpy(text + bl + 1, rel, len);
    }
    text[n] = '\0';
    return text;
}

/* A channel's description for the directory p's fs.opendir gave. */
static long chan_new(const struct path *p, handle_t ch, uint64_t id, uint64_t volume,
                     uint32_t flags, uint32_t status, struct fd_desc **out)
{
    struct dir *x = jam_calloc(1, sizeof(*x));
    /* read-only, with O_PATH kept: such a description lists nothing (dir_getdents) */
    struct fd_desc *d = x ? fd_new(&dir_ops, (status & ~FD_ACCMODE) | O_RDONLY, x) : NULL;
    if (!d || p->mount_len >= NS_NAME_MAX) {
        if (d)
            fd_put(d);   /* its close frees x */
        else
            jam_free(x);
        jam_handle_close(ch);
        return d ? -EIO : -ENOMEM;
    }
    x->chan = true;
    x->ch = ch;
    x->flags = flags & FS_READ;
    x->id = id;
    x->volume = volume;
    memcpy(x->mount, p->abs, p->mount_len);
    x->mount[p->mount_len] = '\0';
    x->path = hint_of(p);
    *out = d;
    return 0;
}

/* "posix: /data has no room for more directory channels: ..." in the
 * log, once a process (docs/DIRHANDLE-PLAN.md 2.6). */
static void say_full(const struct path *p)
{
    if (__atomic_exchange_n(&said_full, true, __ATOMIC_RELAXED))
        return;
    char line[160];
    int n = snprintf(line, sizeof(line),
                     "posix: %.*s has no room for more directory channels: text descriptions "
                     "from here (renames not followed)\n",
                     (int)(p->mount_len < 32 ? p->mount_len : 32), p->abs);
    if (n > 0)
        (void)jam_debug_write(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/* A text description of the directory at p (placed: its physical text).
 * From a directory channel that is its text joined with the path, if it
 * has one; else -EMFILE (the service has no room, and no text names it). */
static long text_open(struct path *p, uint32_t status, struct fd_desc **out)
{
    if (!p->at) {
        long r = path_place(p);
        return r ? r : dir_new(p, status, out);
    }
    char *text = hint_of(p);
    struct path *q = text ? jam_malloc(sizeof(*q)) : NULL;
    long r = !text ? -EMFILE : !q ? -ENOMEM : path_set(text, q);
    if (!r) {
        q->nofollow = p->nofollow;
        r = path_place(q);
    }
    if (!r)
        r = dir_new(q, status, out);
    if (q)
        path_done(q);
    jam_free(q);
    jam_free(text);
    return r;
}

long dir_open(struct path *p, uint32_t status, struct fd_desc **out)
{
    if (p->where == PATH_MOUNT && (path_features(p) & FS_FEAT_DIRS)) {
        uint32_t ff = ((status & O_PATH) ? 0 : FS_READ) | (p->nofollow ? FS_NOFOLLOW : 0);
        handle_t ch = HANDLE_INVALID;
        uint64_t id = 0, volume = 0;
        status_t st = opendir_raw(p, ff, &ch, &id, &volume);
        if (st == OK)
            return chan_new(p, ch, id, volume, ff, status, out);
        if (st == ERR_INVALID_ARGS && p->nofollow)
            return -ELOOP;   /* FS_NOFOLLOW: a link as the last name */
        if (st != ERR_NO_RESOURCES && st != ERR_NOT_SUPPORTED)
            return path_err_read(p, st);   /* ERR_LINK_OUT: openat follows it */
        if (st == ERR_NO_RESOURCES)
            say_full(p);
    }
    return text_open(p, status, out);
}

struct fd_desc *dir_copy(struct fd_desc *d)
{
    const struct dir *x = d->priv;
    struct dir *y = x->chan ? jam_calloc(1, sizeof(*y)) : NULL;
    char *path = x->path ? jam_strdup(x->path) : NULL;
    handle_t ch = HANDLE_INVALID;
    struct fd_desc *c = NULL;
    if (y && (path || !x->path) && !dir_channel(d, &ch))
        c = fd_new(&dir_ops, O_RDONLY | O_PATH, y);
    if (!c) {
        if (ch)
            jam_handle_close(ch);
        jam_free(path);
        jam_free(y);
        return NULL;
    }
    y->chan = true;
    y->ch = ch;
    y->flags = x->flags;
    memcpy(y->mount, x->mount, sizeof(y->mount));
    y->id = x->id;
    y->volume = x->volume;
    y->path = path;
    return c;
}

/* d's channel's text now, as its service names it (fs.resolve of ""), its
 * mount before it: 0 and out (PATH_BYTES), or -errno. */
static long chan_text(struct fd_desc *d, char *out)
{
    const struct dir *x = d->priv;
    handle_t ch;
    long r = dir_channel(d, &ch);
    if (r)
        return r;
    char *next = jam_malloc(PATH_BYTES);
    uint8_t kind = 0;
    uint16_t used;
    uint32_t nlen = 0;
    status_t st = next ? fs_resolve_within(ch, FILE_TIMEOUT, "", 0, 0, &kind, &used, next, &nlen)
                       : ERR_NO_MEMORY;
    jam_handle_close(ch);
    if (st == OK && (kind != FS_RESOLVE_INSIDE || nlen >= PATH_BYTES))
        st = ERR_IO;   /* "" never leads out: a broken service */
    if (st == OK) {
        next[nlen] = '\0';
        int n = snprintf(out, PATH_BYTES, "%s%s%s", x->mount, nlen ? "/" : "", next);
        r = n < 0 || n >= (int)PATH_BYTES ? -ENAMETOOLONG : 0;
    } else {
        r = st == ERR_NO_MEMORY ? -ENOMEM : lx_err(st);
    }
    jam_free(next);
    return r;
}

long dir_text_now(struct fd_desc *d, char *out)
{
    const struct dir *x = d->priv;
    long r = x->chan ? chan_text(d, out) : -ENOENT;
    if (r && r != -ENAMETOOLONG && x->path && strlen(x->path) < PATH_BYTES) {
        memcpy(out, x->path, strlen(x->path) + 1);   /* removed, gone: its last known */
        r = 0;
    }
    return r;
}

/* ---- the listing ------------------------------------------------------------- */

/* The id of abs + more ("" for ".", "/.." for ".."), or 1 if it can't be
 * asked: a text description's. Its buffers are on the heap: they are
 * 8 KiB, and a listing's first call comes with whatever stack the caller
 * has. */
static uint64_t id_of(const char *abs, const char *more)
{
    struct {
        char             text[PATH_BYTES + 3];
        struct path      p;
        struct path_info i;
    } *w = jam_malloc(sizeof(*w));
    if (!w)
        return 1;
    snprintf(w->text, sizeof(w->text), "%s%s", abs, more);
    uint64_t id = 1;
    if (!path_set(w->text, &w->p) && !path_info_follow(&w->p, &w->i) && w->i.id)
        id = w->i.id;
    path_done(&w->p);
    jam_free(w);
    return id;
}

/* A channel's "..": getattr("..") on it (from the view's root, the
 * directory the mount point is in); 1 if it can't be asked. */
static uint64_t parent_id(struct fd_desc *d)
{
    struct {
        struct path      p;
        struct path_info i;
    } *w = jam_malloc(sizeof(*w));
    if (!w)
        return 1;
    uint64_t id = 1;
    if (!path_in_dir(d, "..", 2, &w->p) && !path_info_follow(&w->p, &w->i) && w->i.id)
        id = w->i.id;
    path_done(&w->p);
    jam_free(w);
    return id;
}

/* Ask the service for cookie's batch of the directory p into x. d->lock. */
static long fetch_path(const struct path *p, struct dir *x, uint32_t cookie)
{
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    if (!x->batch && !(x->batch = jam_malloc(FS_READDIR_BYTES))) {
        jam_handle_close(fs);
        return -ENOMEM;
    }
    uint32_t len, next = 0, blen = 0;
    uint16_t count = 0;
    const char *rel = path_rel(p, &len);
    x->bvalid = false;
    status_t st = fs_readdir_within(fs, FILE_TIMEOUT, rel, len, cookie, &next, &count, x->batch,
                                    &blen);
    jam_handle_close(fs);
    if (st == ERR_LINK_OUT)
        return -ENOENT;   /* the directory's place is a link out now */
    if (st != OK)
        return path_err_read(p, st);
    if (!count && next)
        return -EIO;   /* an empty batch that isn't the last: a broken service */
    x->blen = blen;
    x->bcookie = cookie;
    x->bnext = next;
    x->bcount = count;
    x->bvalid = true;
    x->at_k = 0;
    x->at_off = 0;
    return 0;
}

/* Ask the service for cookie's batch of d's directory. d->lock. */
static long fetch(struct fd_desc *d, uint32_t cookie)
{
    struct path p;
    long r = path_of(d, &p);
    if (!r)
        r = fetch_path(&p, d->priv, cookie);
    path_done(&p);
    return r;
}

/* Entry k of the kept batch into *e (name, ino, type). */
static long batch_entry(struct dir *x, uint16_t k, struct entry *e)
{
    if (k < x->at_k)
        x->at_k = 0, x->at_off = 0;
    struct fs_dirent de;
    for (;;) {
        if (!fs_dirent_get(x->batch, x->blen, &x->at_off, &de))
            return -EIO;
        if (x->at_k++ == k)
            break;
    }
    memcpy(e->name, de.name, de.name_len);
    e->name[de.name_len] = '\0';
    e->ino = de.id ? de.id : 1;
    e->type = de.type ? de.type : de.is_dir ? DT_DIR : DT_REG;   /* FS_DT_* are DT_* */
    return 0;
}

/* The entry at pos of a mount's directory: 1 and *e, 0 at the end, or -errno. */
static long mount_entry(struct fd_desc *d, int64_t pos, struct entry *e)
{
    struct dir *x = d->priv;
    uint64_t v = (uint64_t)(pos - POS_ENTRIES);
    uint32_t cookie = (uint32_t)(v >> K_BITS);
    uint16_t k = (uint16_t)(v & K_MASK);
    for (;;) {
        if (!x->bvalid || x->bcookie != cookie) {
            long r = fetch(d, cookie);
            if (r)
                return r;
        }
        if (k < x->bcount)
            break;
        if (!x->bnext)
            return 0;
        cookie = x->bnext;
        k = 0;
    }
    long r = batch_entry(x, k, e);
    if (r)
        return r;
    bool last = k + 1 == x->bcount && x->bnext;
    uint64_t at = last ? (uint64_t)x->bnext << K_BITS : ((uint64_t)cookie << K_BITS) | (k + 1u);
    e->next = POS_ENTRIES + (int64_t)at;
    return 1;
}

/* The entry at pos of "/" or /svc (libjam's own listing of them). */
static long top_entry(const struct dir *x, int64_t pos, struct entry *e)
{
    struct fs_entry fe;
    uint32_t index = (uint32_t)(pos - POS_ENTRIES);
    status_t st = fs_readdir(x->path, index, &fe);
    if (st == ERR_NOT_FOUND)
        return 0;
    if (st != OK)
        return lx_err(st);
    memcpy(e->name, fe.name, strlen(fe.name) + 1);
    e->ino = ID_TOP + index;
    e->type = fe.is_dir ? DT_DIR : DT_SOCK;
    e->next = pos + 1;
    return 1;
}

/* Is the directory at abs (a description's path) inside a mount (else "/"
 * or /svc)? A function of its own so that its path is gone before the
 * listing resolves the directory again: one path on the stack, not two. */
static long on_mount(const char *abs, bool *mount)
{
    struct path p;
    long r = path_set(abs, &p);
    *mount = !r && p.where == PATH_MOUNT;
    path_done(&p);
    return r;
}

static long entry_at(struct fd_desc *d, int64_t pos, struct entry *e)
{
    const struct dir *x = d->priv;
    if (pos < POS_ENTRIES) {
        memcpy(e->name, pos ? ".." : ".", pos ? 3 : 2);
        e->ino = x->chan ? (pos ? parent_id(d) : x->id) : id_of(x->path, pos ? "/.." : "");
        e->type = DT_DIR;
        e->next = pos + 1;
        return 1;
    }
    if (x->chan)
        return mount_entry(d, pos, e);
    bool mount;
    long r = on_mount(x->path, &mount);
    if (r)
        return r;
    return mount ? mount_entry(d, pos, e) : top_entry(x, pos, e);
}

long dir_put(void *buf, size_t room, const char *name, uint64_t ino, uint8_t type,
             int64_t next)
{
    size_t nlen = strlen(name);
    size_t reclen = (DIRENT_HEAD + nlen + 1 + 7) & ~(size_t)7;
    union {
        struct dirent de;
        uint8_t       bytes[(DIRENT_HEAD + FS_NAME_MAX + 1 + 7) & ~(size_t)7];
    } rec;
    if (reclen > sizeof(rec))
        return -ENAMETOOLONG;   /* longer than any name of ours */
    if (reclen > room)
        return 0;
    memset(&rec, 0, reclen);
    rec.de.d_ino = ino;
    rec.de.d_off = next;
    rec.de.d_reclen = (unsigned short)reclen;
    rec.de.d_type = type;
    memcpy(rec.de.d_name, name, nlen);
    return uc_put(buf, &rec, reclen) ? -EFAULT : (long)reclen;
}

static long dir_getdents(struct fd_desc *d, void *buf, size_t len)
{
    if (fd_status(d) & O_PATH)
        return -EBADF;   /* an O_PATH descriptor names the directory and reads nothing */
    struct entry *e = jam_malloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    lock_take(&d->lock);
    struct dir *x = d->priv;
    long r = 0;
    if (d->offset < POS_ENTRIES) {   /* a listing from the start: asked afresh */
        x->bvalid = false;
        if (x->chan)
            r = fetch(d, 0);   /* a removed directory: ENOENT before ".", as Linux's */
    }
    size_t used = 0;
    while (!r && (r = entry_at(d, d->offset, e)) > 0) {
        long k = dir_put((uint8_t *)buf + used, len - used, e->name, e->ino, e->type, e->next);
        if (k <= 0) {
            r = used ? 0 : k ? k : -EINVAL;   /* the buffer can't hold the next one */
            break;
        }
        used += (size_t)k;
        d->offset = e->next;
        r = 0;
    }
    lock_give(&d->lock);
    jam_free(e);
    return r < 0 && !used ? r : (long)used;
}

/* ---- the kind -------------------------------------------------------------------- */

static long dir_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return -EISDIR;
}

static long dir_seek(struct fd_desc *d, int64_t off, int whence)
{
    lock_take(&d->lock);
    int64_t to = -1;
    if (whence == SEEK_SET)
        to = off;
    else if (whence == SEEK_CUR && __builtin_add_overflow(d->offset, off, &to))
        to = -1;
    if (to >= 0) {
        struct dir *x = d->priv;
        d->offset = to;
        x->bvalid = false;   /* asked afresh (rewinddir after a change sees it) */
    }
    lock_give(&d->lock);
    return to >= 0 ? to : -EINVAL;
}

static long dir_stat(struct fd_desc *d, struct stat *st)
{
    struct path p;
    struct path_info i;
    long r = path_of(d, &p);
    if (!r)
        r = path_info_follow(&p, &i);
    if (!r)
        path_fill_stat(&p, &i, st);
    path_done(&p);
    return r;
}

/* fs.sync of p's mount (on a channel's path, on the channel). */
static long sync_mount(const struct path *p)
{
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    status_t st = fs_sync_within(fs, FILE_TIMEOUT);
    jam_handle_close(fs);
    return st == OK ? 0 : path_err(p, st);
}

static long dir_sync(struct fd_desc *d)
{
    struct path p;
    long r = path_of(d, &p);
    if (!r && p.where == PATH_MOUNT)
        r = sync_mount(&p);
    path_done(&p);
    return r;
}

static const char *dir_path(struct fd_desc *d)
{
    const struct dir *x = d->priv;
    return x->path;
}

/* The directory as a path: a channel's is itself (""), a text one its
 * text. (path_done: the caller's) */
static long dir_place(struct fd_desc *d, struct path *out)
{
    const struct dir *x = d->priv;
    out->at = NULL;
    return x->chan ? path_in_dir(d, "", 0, out) : path_set(x->path, out);
}

/* x's channel as POSIXFDS_DIR_CHANNEL's data (<posixfds.h>): its ids,
 * its mount and its last known text (none if it doesn't fit), in a new
 * buffer of *len bytes; NULL: no memory. */
static uint8_t *chan_data(const struct dir *x, uint32_t *len)
{
    const size_t head = sizeof(struct posixfds_dirch), ml = strlen(x->mount) + 1;
    size_t pl = x->path ? strlen(x->path) + 1 : 1;
    if (head + ml + pl > POSIXFDS_DATA_MAX)
        pl = 1;   /* a hint too long to send: none */
    uint8_t *b = jam_malloc(head + ml + pl);
    if (!b)
        return NULL;
    const struct posixfds_dirch ids = { x->id, x->volume };
    memcpy(b, &ids, head);
    memcpy(b + head, x->mount, ml);
    if (pl > 1)
        memcpy(b + head + ml, x->path, pl);
    else
        b[head + ml] = '\0';
    *len = (uint32_t)(head + ml + pl);
    return b;
}

/* What a child or a socket's receiver gets of d: a channel's
 * description its channel (a duplicate) as POSIXFDS_DIR_CHANNEL, a text
 * one its text as POSIXFDS_DIR. A channel's that is gone is left out
 * (-ENOTSUP): no channel is the directory now, and its text may name
 * another. */
static long dir_give(struct fd_desc *d, struct fd_given *out)
{
    const struct dir *x = d->priv;
    handle_t ch = HANDLE_INVALID;
    uint32_t len = 0;
    uint8_t *data = NULL;
    if (x->chan) {
        long r = dir_channel(d, &ch);
        if (r)
            return r == -ENOENT ? -ENOTSUP : r;
        data = chan_data(x, &len);
        if (!data) {
            jam_handle_close(ch);
            return -ENOMEM;
        }
    } else {
        data = (uint8_t *)jam_strdup(x->path);
        if (!data)
            return -ENOMEM;
        len = (uint32_t)strlen(x->path) + 1;
    }
    lock_take(&d->lock);
    int64_t off = d->offset;
    lock_give(&d->lock);
    *out = (struct fd_given){
        .kind = x->chan ? POSIXFDS_DIR_CHANNEL : POSIXFDS_DIR,
        .status = fd_status(d) & POSIXFDS_STATUS, .offset = (uint64_t)off,
        .handles = { ch }, .nhandles = x->chan ? 1 : 0, .data = data, .data_len = len,
    };
    return 0;
}

/* fork: a channel's description gives the child a duplicate of its
 * channel; one whose channel doesn't come is the child's as text if it
 * has its text, else left out. A text description, or a gone one, has
 * nothing to pass. */
static bool dir_fork(struct fd_desc *d, struct fd_fork *f)
{
    struct dir *x = d->priv;
    if (!x->chan)
        return true;
    bool child = fd_fork_child(f);
    if (child)
        x->ch_lock = (lock_t){ 0 };
    if (!x->ch)
        return true;   /* gone */
    if (!child) {
        fd_fork_dup(f, 0, x->ch);
        return true;
    }
    x->ch = fd_fork_take(f, 0);
    if (x->ch)
        return true;
    x->chan = false;   /* the text, as the description was before channels */
    return x->path != NULL;
}

static void dir_close(struct fd_desc *d)
{
    struct dir *x = d->priv;
    if (x->ch)
        jam_handle_close(x->ch);
    jam_free(x->batch);
    jam_free(x->path);
    jam_free(x);
}

static const struct fd_ops dir_ops = {
    .name = "directory", .read = dir_read, .seek = dir_seek, .stat = dir_stat,
    .sync = dir_sync, .getdents = dir_getdents, .path = dir_path, .place = dir_place,
    .give = dir_give, .fork = dir_fork, .close = dir_close,
};

long dir_new(const struct path *p, uint32_t status, struct fd_desc **out)
{
    struct dir *x = jam_calloc(1, sizeof(*x));
    char *path = jam_strdup(p->abs);
    /* read-only, with O_PATH kept: such a description lists nothing (dir_getdents) */
    struct fd_desc *d = x && path ? fd_new(&dir_ops, (status & ~FD_ACCMODE) | O_RDONLY, x) : NULL;
    if (!d) {
        jam_free(path);
        jam_free(x);
        return -ENOMEM;
    }
    x->path = path;
    *out = d;
    return 0;
}

long dir_adopt_fd(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                  struct fd_desc **out)
{
    if (rec->offset > INT64_MAX)
        return -EINVAL;
    struct path p;
    /* data: a string (the reader's check) */
    long r = path_set(data, &p) ? -EINVAL : dir_new(&p, rec->status, out);
    path_done(&p);
    if (!r)
        (*out)->offset = (int64_t)rec->offset;
    return r;
}

/* hint (a starter's word: an absolute path) as a last known text on the
 * mount m: cleaned (fs_path_clean: no ".", ".." or "//"), and only if it
 * is on m; else NULL (no hint). */
static char *hint_given(const char *m, const char *hint)
{
    size_t ml = strlen(m), hl = strlen(hint);
    char *clean = hl > 1 && hl < PATH_BYTES ? jam_malloc(PATH_BYTES) : NULL;
    if (!clean)
        return NULL;
    clean[0] = '/';   /* fs_path_clean gives the names without it */
    if (fs_path_clean(hint, (uint32_t)hl, clean + 1) != OK || strncmp(clean, m, ml) ||
        (clean[ml] && clean[ml] != '/')) {
        jam_free(clean);
        return NULL;
    }
    return clean;
}

/* A description of the directory channel ch (consumed) that data
 * (len bytes, a POSIXFDS_DIR_CHANNEL's) describes, with open's status
 * flags (O_PATH: the channel was opened without FS_READ, so its
 * replacement is too). The channel is believed; its ids are the
 * starter's word, and its text a hint (hint_given). */
static long chan_adopt(handle_t ch, const void *data, uint32_t len, uint32_t status,
                       struct fd_desc **out)
{
    struct posixfds_dirch ids;
    const char *m, *hint;
    if (!posixfds_dirch_parse(data, len, &ids, &m, &hint)) {
        jam_handle_close(ch);
        return -EINVAL;
    }
    struct dir *x = jam_calloc(1, sizeof(*x));
    struct fd_desc *d = x ? fd_new(&dir_ops, (status & ~FD_ACCMODE) | O_RDONLY, x) : NULL;
    if (!d) {
        jam_free(x);
        jam_handle_close(ch);
        return -ENOMEM;
    }
    x->chan = true;
    x->ch = ch;
    x->flags = (status & O_PATH) ? 0 : FS_READ;
    x->id = ids.id;
    x->volume = ids.volume;
    memcpy(x->mount, m, strlen(m) + 1);   /* shorter than NS_NAME_MAX: parsed */
    x->path = hint_given(m, hint);
    *out = d;
    return 0;
}

long dir_adopt_ch(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                  struct fd_desc **out)
{
    if (rec->offset > INT64_MAX) {
        jam_handle_close(hs[0]);
        return -EINVAL;
    }
    long r = chan_adopt(hs[0], data, rec->data_len, rec->status, out);
    if (!r)
        (*out)->offset = (int64_t)rec->offset;
    return r;
}

long dir_adopt_cwd(handle_t ch, const void *data, uint32_t len, struct fd_desc **out)
{
    return chan_adopt(ch, data, len, O_RDONLY | O_PATH, out);
}

long dir_new_text(const char *abs, struct fd_desc **out)
{
    size_t n = strlen(abs);
    if (abs[0] != '/' || n >= PATH_BYTES)
        return -EINVAL;
    char *clean = jam_malloc(PATH_BYTES);
    struct dir *x = jam_calloc(1, sizeof(*x));
    struct fd_desc *d = clean && x ? fd_new(&dir_ops, O_RDONLY | O_PATH, x) : NULL;
    if (!d) {
        jam_free(x);
        jam_free(clean);
        return -ENOMEM;
    }
    clean[0] = '/';   /* fs_path_clean gives the names without it */
    if (fs_path_clean(abs, (uint32_t)n, clean + 1) != OK)
        memcpy(clean, "/", 2);
    x->path = clean;
    *out = d;
    return 0;
}

/* ---- calls that change directories ------------------------------------------- */

/* p is "/", /svc, a service, or a mount's root: nothing there can be made
 * or removed. (A path from a directory channel never is: the service
 * judges what it names.) */
static bool top_level(const struct path *p)
{
    return p->where != PATH_MOUNT || (!p->at && !p->abs[p->mount_len]);
}

/* Does something exist at p (a link itself, if p->nofollow)? */
static bool exists(const struct path *p)
{
    struct path_info i;
    struct stat st;
    struct path q = *p;
    q.must_dir = false;
    enum path_libc which = path_libc(&q);
    return which != LIBC_NONE ? !path_libc_stat(&q, which, &st) : !path_info(&q, &i);
}

/* mkdir of p with mode (the umask applied). */
static long mkdir_path(struct path *p, uint32_t mode)
{
    enum path_libc which = path_libc(p);
    if (which != LIBC_NONE || top_level(p)) {
        if (exists(p))
            return -EEXIST;
        return which != LIBC_NONE ? path_libc_refuse(which) : -EROFS;
    }
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t st = fs_mkdir_within(fs, FILE_TIMEOUT, rel, len, mode);
    jam_handle_close(fs);
    if (st == ERR_ACCESS_DENIED && exists(p))
        return -EEXIST;   /* Linux looks for it before it asks for write access */
    return st == OK ? 0 : path_err(p, st);
}

long lx_mkdirat(long dirfd, long pathname, long mode, long d, long e, long f)
{
    struct path p;
    long r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    p.nofollow = true;   /* a link there, dangling or not, is EEXIST */
    uint32_t bits = ((uint32_t)mode & 07777u) & ~lx_umask_now();
    if (!r)
        do
            r = mkdir_path(&p, bits);
        while (path_again(&p, &r));
    path_done(&p);
    return r;
}

long lx_mkdir(long pathname, long mode, long c, long d, long e, long f)
{
    return lx_mkdirat(AT_FDCWD, pathname, mode, 0, 0, 0);
}

/* unlink (flags 0) or rmdir (FS_UNLINK_DIR) of p (a link itself). */
static long remove_path(struct path *p, uint32_t flags)
{
    bool dir = flags & FS_UNLINK_DIR;
    if (dir && p->last_dot)
        return -EINVAL;   /* rmdir("x/."): rmdir(2) */
    if (dir && p->last_dotdot)
        return -ENOTEMPTY;
    enum path_libc which = path_libc(p);
    if (which == LIBC_DEV && !dir)
        return dev_unlink(p->abs);
    if (which != LIBC_NONE)
        return path_libc_refuse(which);
    struct path_info i;
    if (top_level(p)) {
        long r = path_info(p, &i);
        if (r)
            return r;
        return !i.is_dir ? (dir ? -ENOTDIR : -EROFS) : dir ? -EBUSY : -EISDIR;
    }
    if (p->must_dir && !dir) {   /* unlink("x/"): a file is ENOTDIR, a directory EISDIR */
        long r = path_info(p, &i);
        return r ? r : -EISDIR;
    }
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t st = fs_unlink_within(fs, FILE_TIMEOUT, rel, len, flags);
    jam_handle_close(fs);
    return st == OK ? 0 : path_err(p, st);
}

long lx_unlinkat(long dirfd, long pathname, long flags, long d, long e, long f)
{
    if (flags & ~(long)AT_REMOVEDIR)
        return -EINVAL;
    struct path p;
    long r = path_get(dirfd, (const char *)(uintptr_t)pathname, &p);
    p.nofollow = true;
    if (!r)
        do
            r = remove_path(&p, flags & AT_REMOVEDIR ? FS_UNLINK_DIR : 0);
        while (path_again(&p, &r));
    path_done(&p);
    return r;
}

long lx_unlink(long pathname, long b, long c, long d, long e, long f)
{
    return lx_unlinkat(AT_FDCWD, pathname, 0, 0, 0, 0);
}

long lx_rmdir(long pathname, long b, long c, long d, long e, long f)
{
    return lx_unlinkat(AT_FDCWD, pathname, AT_REMOVEDIR, 0, 0, 0);
}

/* rename's checks that the service can't make: -errno, or 1 to go on. */
static long rename_check(const struct path *a, const struct path *b)
{
    bool same_mount = a->mount_len == b->mount_len && !memcmp(a->abs, b->abs, a->mount_len);
    if (!same_mount || a->where != PATH_MOUNT)
        return same_mount ? -EBUSY : -EXDEV;
    if (a->last_dot || a->last_dotdot || b->last_dot || b->last_dotdot || top_level(a) ||
        top_level(b))
        return -EBUSY;
    bool same = a->at == b->at && !strcmp(a->abs, b->abs);   /* one name, from one place */
    if (same || a->must_dir || b->must_dir) {
        struct path_info i;
        struct path q = *a;
        q.must_dir = false;
        long r = path_info(&q, &i);
        if (r)
            return r;
        if (!i.is_dir && (a->must_dir || b->must_dir))
            return -ENOTDIR;
        if (same)
            return 0;   /* the same name: nothing to do (rename(2)) */
    }
    return 1;
}

/* rename of a to b (both physical: path_place) with fs.rename's flags. */
static long rename_paths(const struct path *a, const struct path *b, uint32_t flags)
{
    enum path_libc wa = path_libc(a), wb = path_libc(b);
    if (wa != LIBC_NONE || wb != LIBC_NONE)
        return path_libc_refuse(wa != LIBC_NONE ? wa : wb);
    if ((flags & FS_RENAME_EXCHANGE) && !a->links)
        return -EINVAL;   /* FAT and the boot image have no exchange */
    long r = rename_check(a, b);
    if (r <= 0)
        return r;
    if (a->at != b->at)
        return dir_meet(a, b, flags, false);   /* two directories: fs.rename_at */
    handle_t fs;
    if ((r = path_fs(a, &fs)) != 0)
        return r;
    uint32_t alen, blen;
    const char *arel = path_rel(a, &alen), *brel = path_rel(b, &blen);
    status_t st = fs_rename_within(fs, FILE_TIMEOUT, arel, alen, brel, blen, flags);
    jam_handle_close(fs);
    if (st == ERR_LINK_OUT)
        return -EIO;   /* both were placed just now: a link made meanwhile */
    return st == OK ? 0 : path_err(a, st);
}

long lx_renameat2(long olddirfd, long oldpath, long newdirfd, long newpath, long flags, long f)
{
    if ((flags & ~(long)(RENAME_NOREPLACE | RENAME_EXCHANGE)) ||
        (flags & (RENAME_NOREPLACE | RENAME_EXCHANGE)) == (RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;   /* RENAME_WHITEOUT: none here */
    struct path *two = jam_malloc(2 * sizeof(*two));
    if (!two)
        return -ENOMEM;
    long r = path_get(olddirfd, (const char *)(uintptr_t)oldpath, &two[0]);
    two[1].at = NULL;   /* if the second path_get is not made, path_done still can be */
    if (!r)
        r = path_get(newdirfd, (const char *)(uintptr_t)newpath, &two[1]);
    two[0].nofollow = two[1].nofollow = true;   /* rename moves a link, never its target */
    if (!r)
        r = path_place(&two[0]);
    if (!r)
        r = path_place(&two[1]);
    uint32_t ff = flags & RENAME_EXCHANGE ? FS_RENAME_EXCHANGE
                  : flags & RENAME_NOREPLACE ? 0 : FS_RENAME_REPLACE;
    if (!r)
        r = rename_paths(&two[0], &two[1], ff);
    path_done(&two[0]);
    path_done(&two[1]);
    jam_free(two);
    return r;
}

long lx_renameat(long olddirfd, long oldpath, long newdirfd, long newpath, long e, long f)
{
    return lx_renameat2(olddirfd, oldpath, newdirfd, newpath, 0, 0);
}

long lx_rename(long oldpath, long newpath, long c, long d, long e, long f)
{
    return lx_renameat2(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0, 0);
}

/* ---- two directories in one request (docs/DIRHANDLE-PLAN.md 2.4) ------------- */

/* One side of dir_meet: the channel its name is in (a duplicate, or a
 * whole path's directory opened through its mount), that directory's id
 * and volume, and the name (in p's abs). */
struct side {
    handle_t    ch;
    uint64_t    id, volume;
    const char *rel;
    uint32_t    len;
};

/* A whole path's directory as a channel of its own, for s: the path cut
 * at its last name. */
static long side_of_text(const struct path *p, struct side *s)
{
    size_t end = strlen(p->abs), slash;
    while (end > p->mount_len + 1 && p->abs[end - 1] == '/')
        end--;   /* "a/b/": b */
    for (slash = end; slash > p->mount_len && p->abs[slash - 1] != '/'; slash--)
        ;
    if (slash <= p->mount_len || slash == end)
        return -EBUSY;   /* the mount's root (rename_check's top_level) */
    struct {
        char        text[PATH_BYTES];
        struct path q;
    } *w = jam_malloc(sizeof(*w));
    if (!w)
        return -ENOMEM;
    memcpy(w->text, p->abs, slash - 1);
    w->text[slash - 1] = '\0';   /* "/data" for "/data/x": the root, "/" in the mount */
    long r = path_set(w->text, &w->q);
    if (!r) {
        status_t st = opendir_raw(&w->q, 0, &s->ch, &s->id, &s->volume);
        r = st == OK ? 0 : path_err_read(&w->q, st);
        if (r == PATH_LINK_OUT)
            r = -EIO;   /* placed just now */
    }
    path_done(&w->q);
    jam_free(w);
    s->rel = p->abs + slash;
    s->len = (uint32_t)(end - slash);
    return r;
}

static long side_of(const struct path *p, struct side *s)
{
    if (!p->at)
        return side_of_text(p, s);
    const struct dir *x = p->at->priv;
    s->id = x->id;
    s->volume = x->volume;
    s->rel = path_rel(p, &s->len);
    return dir_channel(p->at, &s->ch);
}

/* Is rel (len bytes; "" for the directory itself) there on ch? */
static bool there(handle_t ch, const char *rel, uint32_t len)
{
    uint32_t mode, nlink, uid, gid;
    uint64_t size, blocks, id;
    int64_t at, mt, ct;
    return fs_getattr_within(ch, FILE_TIMEOUT, rel, len, FS_NOFOLLOW, &mode, &nlink, &uid, &gid,
                             &size, &blocks, &id, &at, &mt, &ct) == OK;
}

/* rename_at's or link_at's ERR_NOT_FOUND: the name a moves isn't there,
 * or b's directory or the directory its name goes in isn't (ENOENT); else
 * the service can't place b's directory from a's view: EXDEV, as Linux's
 * answer across mounts (a descriptor from a process whose view is
 * another). */
static long met_not_found(const struct side *a, const struct side *b)
{
    uint32_t dl = b->len;
    while (dl && b->rel[dl - 1] != '/')
        dl--;
    if (!there(a->ch, a->rel, a->len) || !there(b->ch, "", 0) || (dl && !there(b->ch, b->rel, dl)))
        return -ENOENT;
    return -EXDEV;
}

long dir_meet(const struct path *a, const struct path *b, uint32_t flags, bool link)
{
    struct side sa = { .ch = HANDLE_INVALID }, sb = { .ch = HANDLE_INVALID };
    long r = side_of(a, &sa);
    if (!r)
        r = side_of(b, &sb);
    if (!r && sa.volume != sb.volume)
        r = -EXDEV;   /* two volumes: an id names nothing on the other */
    if (!r) {
        status_t st = link ? fs_link_at_within(sa.ch, FILE_TIMEOUT, sa.rel, sa.len, sb.id, sb.rel,
                                               sb.len)
                           : fs_rename_at_within(sa.ch, FILE_TIMEOUT, sa.rel, sa.len, sb.id,
                                                 sb.rel, sb.len, flags);
        if (st == ERR_NOT_FOUND)
            r = met_not_found(&sa, &sb);
        else if (st == ERR_LINK_OUT)
            r = -EIO;   /* both were placed just now: a link made meanwhile */
        else if (st == ERR_IS_DIR && link)
            r = -EPERM;   /* link(2): no hard link to a directory */
        else
            r = st == OK ? 0 : path_err(link ? b : a, st);
    }
    if (sa.ch)
        jam_handle_close(sa.ch);
    if (sb.ch)
        jam_handle_close(sb.ch);
    return r;
}

/* ---- the current directory ------------------------------------------------------ */

/* The current directory is a directory description (path.c keeps it,
 * docs/DIRHANDLE-PLAN.md 4.4): chdir opens one, fchdir shares the
 * descriptor's (Linux's fchdir takes the directory, not the listing), and
 * getcwd asks it where it is now. On a mount with directory channels it
 * is the directory's own channel, so it works at any depth, follows a
 * rename and, removed, still answers ".." (chdir("..") gets out). */

/* chdir's check of p (a directory), and its physical form. */
static long chdir_path(struct path *p)
{
    struct path_info i;
    struct stat st;
    long r;
    enum path_libc which = path_libc(p);
    if (which != LIBC_NONE) {
        r = path_libc_stat(p, which, &st);
        i.is_dir = !r && S_ISDIR(st.st_mode);
    } else {
        r = path_info(p, &i);
    }
    if (!r && !i.is_dir)
        r = -ENOTDIR;
    return r ? r : path_place(p);   /* where a link led: getcwd says that */
}

/* The directory at p as the current directory: its channel's description
 * (fs.opendir without FS_READ: the walk's search bits alone, one call) on
 * a mount with directory channels, else (and when the service has no
 * room) a text one once p is found to be a directory. -errno, or
 * PATH_LINK_OUT. */
static long cwd_open(struct path *p, struct fd_desc **out)
{
    const uint32_t status = O_RDONLY | O_PATH | O_DIRECTORY;
    if (p->where == PATH_MOUNT && path_libc(p) == LIBC_NONE &&
        (path_features(p) & FS_FEAT_DIRS)) {
        long r = dir_open(p, status, out);
        if (r || ((const struct dir *)(*out)->priv)->chan)
            return r;
        fd_put(*out);   /* a text one: found to be a directory below first */
        *out = NULL;
    }
    long r = chdir_path(p);
    return r ? r : text_open(p, status, out);
}

long dir_cwd_open(struct fd_desc *base, const char *in, struct fd_desc **out)
{
    struct path *p = jam_malloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    long r = path_get_at(base, in, p);
    if (!r)
        do
            r = cwd_open(p, out);
        while (path_again(p, &r));
    path_done(p);
    jam_free(p);
    return r;
}

long lx_chdir(long pathname, long b, long c, long d, long e, long f)
{
    struct fd_desc *base = path_cwd(), *dir = NULL;
    long r = dir_cwd_open(base, (const char *)(uintptr_t)pathname, &dir);
    if (base)
        fd_put(base);
    if (!r)
        path_cwd_set(dir);
    return r;
}

long lx_fchdir(long fd, long b, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    if (!dir_is(desc)) {
        fd_put(desc);
        return -ENOTDIR;
    }
    path_cwd_set(desc);   /* our reference is the current directory's now */
    return 0;
}

/* Is the text description x's directory removed (the service says it is
 * not there)? Only a mount of ours can say: a program started without
 * the mount its directory is on (the shell's cd, then a program that has
 * no /data) keeps the name. A service that's slow or refuses doesn't
 * count. */
static bool text_removed(const struct dir *x)
{
    struct path *p = jam_malloc(sizeof(*p));
    if (!p)
        return false;
    bool gone = false;
    char m[NS_NAME_MAX];
    if (!path_set(x->path, p) && p->where == PATH_MOUNT && path_libc(p) == LIBC_NONE &&
        p->mount_len < NS_NAME_MAX) {
        memcpy(m, p->abs, p->mount_len);
        m[p->mount_len] = '\0';
        struct path_info i;
        long r = path_is_mount(m) ? path_info(p, &i) : 0;
        gone = r == -ENOENT || r == -ENOTDIR;
    }
    path_done(p);
    jam_free(p);
    return gone;
}

/* A removed directory's last known text with " (deleted)" into out, as
 * Linux's /proc links say it; -ENOENT if it has none. */
static long deleted_text(const struct dir *x, char *out)
{
    if (!x->path)
        return -ENOENT;
    int n = snprintf(out, PATH_BYTES, "%s (deleted)", x->path);
    return n < 0 || n >= (int)PATH_BYTES ? -ENAMETOOLONG : 0;
}

/* Is the channel's directory removed (no links), not just cut off from
 * its view? */
static bool chan_removed(struct fd_desc *d)
{
    struct path p;
    struct path_info i;
    bool gone = !dir_place(d, &p) && !path_info(&p, &i) && i.has_attr && !i.nlink;
    path_done(&p);
    return gone;
}

long dir_where(struct fd_desc *d, char *out, bool deleted)
{
    const struct dir *x = d->priv;
    if (!x->chan) {
        if (text_removed(x))
            return deleted ? deleted_text(x, out) : -ENOENT;
        memcpy(out, x->path, strlen(x->path) + 1);
        return 0;
    }
    long r = chan_text(d, out);   /* fs.resolve of "": removed or cut off, ERR_NOT_FOUND */
    if (r == -ENOENT && deleted && chan_removed(d))
        r = deleted_text(x, out);
    return r;
}

long lx_getcwd(long buf, long size, long c, long d, long e, long f)
{
    char *text = jam_malloc(PATH_BYTES);
    if (!text)
        return -ENOMEM;
    struct fd_desc *cwd = path_cwd();
    long r = 0;
    if (cwd)
        r = dir_where(cwd, text, false);   /* removed under us: ENOENT, as Linux's */
    else
        memcpy(text, "/", 2);
    if (cwd)
        fd_put(cwd);
    size_t n = r ? 0 : strlen(text) + 1;
    if (!r)
        r = (size_t)size < n ? -ERANGE : uc_put((void *)(uintptr_t)buf, text, n) ? -EFAULT : (long)n;
    jam_free(text);
    return r;
}
