/* Paths (files.h has the model): resolving a program's path against the
 * current directory or a directory descriptor, the current directory
 * itself, and what the layer asks a mount about a path: stat, read-only
 * or not, and the errno of its answers.
 *
 * The current directory is a directory description (dir.c's kind) under
 * cwd_lock, made from the hand-over's at the start (else "/": NULL), and
 * a relative path with AT_FDCWD starts from it as from a descriptor: on
 * its channel, or from its text. Whether a mount is read-only,
 * and what it is (its FS_FEAT_*: links or not), is asked with fs.statfs
 * and remembered per mount (ro_cache, under ro_lock: the read-only flag
 * for a second, the features, which change only with the mount, for a
 * minute), since every path needs the features and stat's mode bits the
 * read-only flag,
 * and FAT's statfs may count free clusters; a view changed under a
 * program shows in the mode bits within that second, and the service
 * refuses its writes at once either way.
 *
 * On a mount with links (files.h) the path after the mount is sent as
 * written; a link out of it comes back as PATH_LINK_OUT and path_follow
 * asks fs.resolve where it goes on, which is resolved again from our own
 * "/": the link's target, or "/" and the rest for a ".." above the
 * mount's root. Each such step counts as a link, 40 at most (ELOOP); a
 * loop inside one walk is the service's ERR_LINK_LOOP.
 *
 * A relative path from a directory channel's description (dir.c) is not
 * joined to anything: the path keeps the description (`at`) and the text
 * as written, which path_rel gives and path_fs's channel (the
 * directory's) takes; the service walks it from the directory. A link out
 * of the mount, or ".." above the view's root, comes back as
 * ERR_LINK_OUT as from the mount's channel, and the path goes on from our
 * "/" as text (go_on drops `at`). */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <posixfork.h>
#include <string.h>
#include "files.h"

#define ID_ROOT    1u          /* "/": the layer's own ids, below any service's... */
#define ID_SVC     2u          /* "/svc" */
#define ID_SVC0    3u          /* ... and the services from here, in their order */
#define RO_KEEP_NS NS_PER_S    /* how long a mount's read-only answer is remembered */
#define FEAT_KEEP_NS (60 * NS_PER_S)   /* ... and its features (they change only with the mount) */
#define BLOCK      512u        /* st_blocks' unit */
#define BLKSIZE    65536u      /* st_blksize: the transfer buffer */
#define ONE_USER   1000u       /* the one user's uid and gid (M13 Q7) */
#define LINKS_MAX  40u         /* links a path may follow out of mounts (Linux's ELOOP) */

static struct fd_desc *cwd;   /* cwd_lock: the current directory, a reference; NULL: "/" */
static lock_t          cwd_lock;

static struct {
    char     mount[NS_NAME_MAX];   /* "" for a free entry */
    bool     ro;
    uint32_t features;             /* FS_FEAT_* */
    uint64_t at;                   /* when it was asked (now()) */
} ro_cache[NS_MAX_MOUNTS];         /* ro_lock */
static unsigned ro_next;           /* ro_lock: the entry the next new mount takes */
static lock_t   ro_lock;

static bool mount_name(const struct path *p, char m[NS_NAME_MAX]);
static bool mount_ask(const char *m, uint64_t keep, bool *ro, uint32_t *features);

/* ---- resolving ------------------------------------------------------------------- */

/* The next name of s at or after *at (skipping slashes): its start and
 * length (0: no more), *at moved past it. */
static size_t next_name(const char *s, size_t *at, const char **name)
{
    while (s[*at] == '/')
        (*at)++;
    size_t l = 0;
    while (s[*at + l] && s[*at + l] != '/')
        l++;
    *name = s + *at;
    *at += l;
    return l;
}

/* Does the mount p has just found have links? Its features are asked
 * only of a mount of our namespace ("/svc", "/dev" and the like aren't). */
static bool mount_links(const struct path *p)
{
    char m[NS_NAME_MAX];
    bool ro;
    uint32_t features;
    if ((p->mount_len == 4 && !memcmp(p->abs, "/svc", 4)) || !mount_name(p, m) ||
        !path_is_mount(m))
        return false;
    return mount_ask(m, FEAT_KEEP_NS, &ro, &features) && (features & FS_FEAT_LINKS);
}

/* The rest of a path from name on, as written, after a '/' (a mount with
 * links: the service walks it). */
static long as_written(const char *name, struct path *out, size_t *len)
{
    size_t l = strlen(name);
    if (*len + 1 + l >= PATH_BYTES)
        return -ENAMETOOLONG;
    out->abs[(*len)++] = '/';
    memcpy(out->abs + *len, name, l);
    *len += l;
    return 0;
}

/* Walk the names of s onto out (files.h's cleaning rules). */
static long walk(const char *s, struct path *out, size_t *len)
{
    const char *name;
    size_t at = 0, l;
    while ((l = next_name(s, &at, &name)) != 0) {
        if (out->links)
            return as_written(name, out, len);
        if (l == 1 && name[0] == '.')
            continue;
        if (l == 2 && name[0] == '.' && name[1] == '.') {
            while (out->mount_len && *len > out->mount_len && out->abs[*len - 1] != '/')
                (*len)--;
            if (out->mount_len && *len > out->mount_len)
                (*len)--;   /* the slash before the name it took away */
            continue;
        }
        size_t sep = *len > 1;   /* "/" + name at the root, else "/name" after the rest */
        if (*len + sep + l >= PATH_BYTES)
            return -ENAMETOOLONG;
        if (sep)
            out->abs[(*len)++] = '/';
        memcpy(out->abs + *len, name, l);
        *len += l;
        if (!out->mount_len) {
            out->mount_len = (uint32_t)*len;
            out->abs[*len] = '\0';
            out->links = mount_links(out);
        }
    }
    return 0;
}

/* How in ends: with a slash, ".", or "..". */
static void ending(const char *in, size_t n, struct path *out)
{
    size_t start = n;
    while (start && in[start - 1] != '/')
        start--;
    size_t l = n - start;
    out->last_dot = l == 1 && in[start] == '.';
    out->last_dotdot = l == 2 && in[start] == '.' && in[start + 1] == '.';
    out->must_dir = in[n - 1] == '/' || out->last_dot || out->last_dotdot;
}

/* out->abs holds the directory a path starts from, "/" or the text of a
 * current directory or a directory description (absolute and clean, which
 * is what they keep; on a mount with links as written, as walk would copy
 * it): its length, with out's mount noted. The base is taken as it stands
 * and the path walked on from its end, so no copy of it is needed. */
static size_t start_at(struct path *out)
{
    size_t len = strlen(out->abs);
    out->mount_len = 0;
    out->links = false;
    if (len > 1) {
        const char *end = memchr(out->abs + 1, '/', len - 1);
        out->mount_len = (uint32_t)(end ? (size_t)(end - out->abs) : len);
        out->links = mount_links(out);
    }
    return len;
}

/* in (n bytes) onto the directory out->abs holds (start_at); out's
 * nofollow and hops are the caller's. */
static long resolve(const char *in, size_t n, struct path *out)
{
    size_t len = start_at(out);
    long r = walk(in, out, &len);
    if (r)
        return r;
    out->abs[len] = '\0';
    ending(in, n, out);
    if (!out->mount_len)
        out->where = PATH_ROOT;
    else if (out->mount_len == 4 && !memcmp(out->abs, "/svc", 4))
        out->where = PATH_SVC;
    else
        out->where = PATH_MOUNT;
    return 0;
}

/* in (an absolute path, n bytes) from "/" into out. */
static long resolve_abs(const char *in, size_t n, struct path *out)
{
    out->abs[0] = '/';
    out->abs[1] = '\0';
    return resolve(in, n, out);
}

/* The directory relative path in (n bytes) starts from, d (a directory
 * description, or a /proc directory's; NULL: "/"): 0 with its text in
 * out->abs, or 1 with out made from a directory channel (dir_from). */
static long base_in(struct fd_desc *d, const char *in, size_t n, struct path *out)
{
    if (!d) {
        memcpy(out->abs, "/", 2);
        return 0;
    }
    if (dir_is(d))
        return dir_from(d, in, n, out);
    return proc_dir_base(d, out->abs) ? 0 : -ENOTDIR;
}

/* ... and from dirfd (AT_FDCWD: the current directory). */
static long base_of(long dirfd, const char *in, size_t n, struct path *out)
{
    struct fd_desc *d = dirfd == AT_FDCWD ? path_cwd() : fd_get(dirfd);
    if (!d && dirfd != AT_FDCWD)
        return -EBADF;
    long r = base_in(d, in, n, out);
    if (d)
        fd_put(d);
    return r;
}

/* path_get's checks of in: 0 and its length in *n, or -errno. */
static long given(const char *in, struct path *out, size_t *n)
{
    out->at = NULL;
    long len = in ? uc_strnlen(in, PATH_BYTES) : -EFAULT;   /* the caller's string */
    if (len < 0)
        return len;
    *n = (size_t)len;
    if (!*n)
        return -ENOENT;
    if (*n == PATH_BYTES)
        return -ENAMETOOLONG;
    out->nofollow = false;
    out->hops = 0;
    return 0;
}

long path_get(long dirfd, const char *in, struct path *out)
{
    size_t n;
    long r = given(in, out, &n);
    if (r)
        return r;
    if (in[0] == '/')
        return resolve_abs(in, n, out);
    r = base_of(dirfd, in, n, out);
    return r < 0 ? r : r ? 0 : resolve(in, n, out);
}

long path_get_at(struct fd_desc *base, const char *in, struct path *out)
{
    size_t n;
    long r = given(in, out, &n);
    if (r)
        return r;
    if (in[0] == '/')
        return resolve_abs(in, n, out);
    r = base_in(base, in, n, out);
    return r < 0 ? r : r ? 0 : resolve(in, n, out);
}

long path_in_dir(struct fd_desc *dir, const char *rel, size_t n, struct path *out)
{
    const char *m = dir_mount(dir);
    size_t ml = strlen(m);
    out->at = NULL;
    out->nofollow = false;
    out->hops = 0;
    if (n >= PATH_BYTES || ml + 1 + n >= sizeof(out->abs))
        return -ENAMETOOLONG;
    memcpy(out->abs, m, ml);
    out->abs[ml] = '/';
    memcpy(out->abs + ml + 1, rel, n);   /* the program's string for path_get: checked there */
    out->abs[ml + 1 + n] = '\0';
    out->mount_len = (uint32_t)ml;
    out->where = PATH_MOUNT;
    out->links = mount_links(out);
    if (n) {
        ending(rel, n, out);
    } else {   /* the directory itself */
        out->must_dir = true;
        out->last_dot = out->last_dotdot = false;
    }
    fd_ref(dir);
    out->at = dir;
    return 0;
}

long path_of(struct fd_desc *d, struct path *out)
{
    out->at = NULL;
    if (d->ops->place)
        return d->ops->place(d, out);
    const char *abs = d->ops->path ? d->ops->path(d) : NULL;
    if (abs)
        return path_set(abs, out);
    long r = path_set("/", out);   /* a pipe, a device: no mount */
    return r ? r : 1;
}

long path_set(const char *abs, struct path *out)
{
    out->at = NULL;
    size_t n = strnlen(abs, PATH_BYTES);
    if (!n || n == PATH_BYTES)
        return -ENAMETOOLONG;
    out->nofollow = false;
    out->hops = 0;
    return resolve_abs(abs, n, out);
}

long path_whole(struct path *p)
{
    if (!p->at)
        return 0;
    char *text = jam_malloc(2 * PATH_BYTES);
    if (!text)
        return -ENOMEM;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    long r = dir_where(p->at, text, false);   /* asked now: removed or cut off, ENOENT */
    if (!r && snprintf(text + PATH_BYTES, PATH_BYTES, "%s/%s", text, rel) >= (int)PATH_BYTES)
        r = -ENAMETOOLONG;
    if (!r) {
        bool nofollow = p->nofollow;
        path_done(p);
        r = path_set(text + PATH_BYTES, p);
        p->nofollow = nofollow;
    }
    jam_free(text);
    return r;
}

void path_done(struct path *p)
{
    if (p->at) {
        fd_put(p->at);
        p->at = NULL;
    }
}

/* ---- links that lead out of a mount ------------------------------------------- */

/* fs.resolve of p (p->nofollow: the last link not followed): its kind and
 * next (PATH_BYTES, NUL-terminated). */
static long ask_resolve(const struct path *p, uint8_t *kind, char *next)
{
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len, nlen = 0;
    uint16_t used;
    const char *rel = path_rel(p, &len);
    status_t st = fs_resolve_within(fs, FILE_TIMEOUT, rel, len, p->nofollow ? FS_NOFOLLOW : 0,
                                    kind, &used, next, &nlen);
    jam_handle_close(fs);
    if (st != OK)
        return st == ERR_LINK_OUT ? -EIO : path_err_read(p, st);   /* resolve never leads out */
    next[nlen < PATH_BYTES ? nlen : PATH_BYTES - 1] = '\0';
    return 0;
}

/* p again from where fs.resolve said it goes on (buf: PATH_BYTES of room):
 * inside the mount, its physical form; else from our "/". What p asked
 * of its end (a directory, "." or "..") still holds: the service's next
 * drops a trailing '/'. */
static long go_on(struct path *p, uint8_t kind, const char *next, char *buf)
{
    if (kind == FS_RESOLVE_INSIDE) {
        int n = snprintf(buf, PATH_BYTES, "%.*s%s%s", (int)p->mount_len, p->abs,
                         next[0] ? "/" : "", next);
        if (n < 0 || n >= (int)PATH_BYTES)
            return -ENAMETOOLONG;
    } else {
        if (kind != FS_RESOLVE_ABSOLUTE && kind != FS_RESOLVE_ABOVE)
            return -EIO;   /* a broken service */
        if (++p->hops > LINKS_MAX)
            return -ELOOP;
        int n = snprintf(buf, PATH_BYTES, "%s%s", kind == FS_RESOLVE_ABOVE ? "/" : "", next);
        if (n < 0 || n >= (int)PATH_BYTES)
            return -ENAMETOOLONG;
        if (buf[0] != '/')
            return -EIO;
    }
    bool must_dir = p->must_dir, dot = p->last_dot, dotdot = p->last_dotdot;
    path_done(p);   /* p becomes a whole path from "/": nothing it starts from now */
    long r = resolve_abs(buf, strlen(buf), p);
    p->must_dir |= must_dir;
    p->last_dot |= dot;
    p->last_dotdot |= dotdot;
    return r;
}

long path_follow(struct path *p)
{
    if (!p->links || p->where != PATH_MOUNT)
        return -EIO;   /* only a mount with links leads out */
    char *buf = jam_malloc(2 * PATH_BYTES);
    if (!buf)
        return -ENOMEM;
    uint8_t kind = 0;
    long r = ask_resolve(p, &kind, buf);
    if (!r)
        r = go_on(p, kind, buf, buf + PATH_BYTES);
    jam_free(buf);
    return r;
}

long path_place(struct path *p)
{
    if (!p->links || p->where != PATH_MOUNT)
        return 0;
    char *buf = jam_malloc(2 * PATH_BYTES);
    if (!buf)
        return -ENOMEM;
    long r = 0;
    while (!r && p->links && p->where == PATH_MOUNT) {
        uint8_t kind = 0;
        r = ask_resolve(p, &kind, buf);
        if (p->at && ((!r && kind == FS_RESOLVE_INSIDE) || r == -ENAMETOOLONG)) {
            r = 0;   /* inside: walked from the directory as it is (its text may not fit) */
            break;
        }
        if (!r)
            r = go_on(p, kind, buf, buf + PATH_BYTES);
        if (!r && kind == FS_RESOLVE_INSIDE)
            break;
    }
    jam_free(buf);
    return r;
}

const char *path_rel(const struct path *p, uint32_t *len)
{
    const char *rel = p->abs + p->mount_len;
    if (p->at) {   /* as written, after the mount's '/': "" for the directory itself */
        *len = (uint32_t)strlen(rel + 1);
        return rel + 1;
    }
    *len = (uint32_t)strlen(rel);
    if (*len)
        return rel;
    *len = 1;
    return "/";
}

/* p's mount point ("/data") into m: false if it can't be one. */
static bool mount_name(const struct path *p, char m[NS_NAME_MAX])
{
    if (p->mount_len >= NS_NAME_MAX)
        return false;
    memcpy(m, p->abs, p->mount_len);
    m[p->mount_len] = '\0';
    return true;
}

long path_fs(const struct path *p, handle_t *fs)
{
    char m[NS_NAME_MAX];
    if (p->at)
        return dir_channel(p->at, fs);
    if (!mount_name(p, m) || ns_channel(m, fs) != OK)
        return -ENOENT;
    return 0;
}

/* ---- what a path is ----------------------------------------------------------- */

/* "/svc" or one service in it. */
static long svc_info(const struct path *p, struct path_info *out)
{
    const char *rel = p->abs + p->mount_len;
    char name[SVC_NAME_MAX + 1];
    if (!*rel) {
        if (!ns_svc_at(0, name))
            return -ENOENT;
        out->is_dir = true;
        out->id = ID_SVC;
        return 0;
    }
    for (unsigned i = 0; ns_svc_at(i, name); i++)
        if (!strcmp(rel + 1, name)) {
            out->is_svc = true;
            out->id = ID_SVC0 + i;
            return 0;
        }
    return -ENOENT;
}

/* path_info on a mount that keeps modes: fs.getattr's answer. */
static long attr_info(const struct path *p, struct path_info *out)
{
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t st = fs_getattr_within(fs, FILE_TIMEOUT, rel, len, p->nofollow ? FS_NOFOLLOW : 0,
                                    &out->mode, &out->nlink, &out->uid, &out->gid, &out->size,
                                    &out->blocks, &out->id, &out->atime_ns, &out->mtime_ns,
                                    &out->ctime_ns);
    jam_handle_close(fs);
    if (st != OK)
        return path_err_read(p, st);
    out->has_attr = true;
    out->is_dir = (out->mode & FS_MODE_FMT) == FS_MODE_DIR;
    out->is_link = (out->mode & FS_MODE_FMT) == FS_MODE_LNK;
    out->mtime = out->mtime_ns > 0 ? (uint64_t)out->mtime_ns / NS_PER_S : 0;
    return 0;
}

long path_info(const struct path *p, struct path_info *out)
{
    memset(out, 0, sizeof(*out));
    long r = 0;
    if (p->where == PATH_ROOT) {
        out->is_dir = true;
        out->id = ID_ROOT;
    } else if (p->where == PATH_SVC) {
        r = svc_info(p, out);
    } else if (path_features(p) & (FS_FEAT_LINKS | FS_FEAT_MODES)) {
        r = attr_info(p, out);
    } else {
        handle_t fs;
        r = path_fs(p, &fs);
        uint32_t len;
        const char *rel = path_rel(p, &len);
        uint8_t dir = 0;
        status_t st = r ? OK : fs_stat_within(fs, FILE_TIMEOUT, rel, len, &out->size, &dir,
                                              &out->mtime, &out->id);
        if (!r)
            jam_handle_close(fs);
        if (!r && st != OK)
            r = path_err_read(p, st);
        out->is_dir = dir != 0;
    }
    if (!r && p->must_dir && !out->is_dir)
        r = -ENOTDIR;
    return r;
}

long path_info_follow(struct path *p, struct path_info *out)
{
    long r;
    do
        r = path_info(p, out);
    while (path_again(p, &r));
    return r;
}

/* The mount's statfs answer from the cache, if it is younger than keep:
 * true and *ro, *features. */
static bool ro_cached(const char *m, uint64_t t, uint64_t keep, bool *ro, uint32_t *features)
{
    bool found = false;
    lock_take(&ro_lock);
    for (unsigned i = 0; i < NS_MAX_MOUNTS; i++)
        if (!strcmp(ro_cache[i].mount, m) && t - ro_cache[i].at < keep) {
            *ro = ro_cache[i].ro;
            *features = ro_cache[i].features;
            found = true;
        }
    lock_give(&ro_lock);
    return found;
}

static void ro_remember(const char *m, bool ro, uint32_t features, uint64_t t)
{
    lock_take(&ro_lock);
    unsigned slot = ro_next;
    for (unsigned i = 0; i < NS_MAX_MOUNTS; i++)
        if (!strcmp(ro_cache[i].mount, m))
            slot = i;
    if (slot == ro_next)
        ro_next = (ro_next + 1) % NS_MAX_MOUNTS;
    memcpy(ro_cache[slot].mount, m, strlen(m) + 1);
    ro_cache[slot].ro = ro;
    ro_cache[slot].features = features;
    ro_cache[slot].at = t;
    lock_give(&ro_lock);
}

/* Mount m's read-only flag and features (fs.statfs, or the cache's if
 * younger than keep): false if it couldn't be asked (not a mount, or no
 * answer). */
static bool mount_ask(const char *m, uint64_t keep, bool *ro, uint32_t *features)
{
    uint64_t t = now();
    if (ro_cached(m, t, keep, ro, features))
        return true;
    handle_t fs;
    if (ns_channel(m, &fs) != OK)
        return false;
    uint64_t total, free_bytes;
    uint8_t r = 0, label[16];
    uint32_t magic = 0, name_max = 0, f = 0;
    status_t st = fs_statfs_within(fs, FILE_TIMEOUT, &total, &free_bytes, &r, label, &magic,
                                   &name_max, &f);
    jam_handle_close(fs);
    if (st != OK)
        return false;
    *ro = r != 0;
    *features = f & FS_FEAT_MASK;
    ro_remember(m, *ro, *features, t);
    return true;
}

bool path_read_only(const struct path *p)
{
    char m[NS_NAME_MAX];
    bool ro = false;
    uint32_t features;
    if (p->where != PATH_MOUNT)
        return true;
    return mount_name(p, m) && mount_ask(m, RO_KEEP_NS, &ro, &features) && ro;
}

uint32_t path_features(const struct path *p)
{
    char m[NS_NAME_MAX];
    bool ro;
    uint32_t features = 0;
    if (p->where != PATH_MOUNT || !mount_name(p, m) || !mount_ask(m, FEAT_KEEP_NS, &ro, &features))
        return 0;
    return features;
}

long path_err(const struct path *p, status_t st)
{
    switch (st) {
    case ERR_ACCESS_DENIED:  return path_read_only(p) ? -EROFS : -EACCES;
    case ERR_NOT_FOUND:      return -ENOENT;
    case ERR_NO_RESOURCES:   return -ENFILE;   /* the service's own limit, not ours */
    case ERR_WRONG_TYPE:     return -ENXIO;    /* a service: no file to open */
    case ERR_PEER_CLOSED:    return -EIO;      /* the mount went */
    case ERR_TIMED_OUT:      return -EIO;
    case ERR_OUT_OF_RANGE:   return -EFBIG;
    case ERR_INVALID_ARGS:   return -EINVAL;   /* a name FAT can't hold, as Linux's vfat */
    case ERR_LINK_OUT:       return p ? PATH_LINK_OUT : -EIO;
    default:                 return lx_err(st);   /* ERR_LINK_LOOP: ELOOP */
    }
}

/* path_err for a request that changes nothing (stat, readdir, statfs,
 * an open to read): a refusal there is the view hiding the path, not
 * the mount being read-only, so EACCES whatever the mount. */
long path_err_read(const struct path *p, status_t st)
{
    return st == ERR_ACCESS_DENIED ? -EACCES : path_err(p, st);
}

/* A program under /boot: in a directory named bin or sbin there. */
static bool boot_program(const struct path *p)
{
    if (p->mount_len != 5 || memcmp(p->abs, "/boot", 5))
        return false;
    const char *slash = strrchr(p->abs, '/');
    const char *dir = slash;
    while (dir > p->abs && dir[-1] != '/')
        dir--;
    size_t l = (size_t)(slash - dir);
    return (l == 3 && !memcmp(dir, "bin", 3)) || (l == 4 && !memcmp(dir, "sbin", 4));
}

/* st_dev: the mount's place in the namespace (1 on), 0 for "/" and /svc. */
static dev_t mount_dev(const struct path *p)
{
    char want[NS_NAME_MAX], m[NS_NAME_MAX];
    if (p->where != PATH_MOUNT || !mount_name(p, want))
        return 0;
    for (unsigned i = 0; ns_mount_at(i, m); i++)
        if (!strcmp(m, want))
            return i + 1;
    return 0;
}

unsigned path_perm(uint32_t mode, uint32_t uid)
{
    return uid == ONE_USER ? (mode >> 6) & 7u : mode & 7u;
}

long path_program(const char *abs, char *out)
{
    struct path *p = jam_malloc(sizeof(*p));
    struct path_info i;
    if (!p)
        return -ENOMEM;
    long r = path_set(abs, p);
    if (!r)
        r = path_place(p);   /* where the links lead: approvals are by that path */
    if (!r && (path_features(p) & FS_FEAT_MODES)) {
        r = path_info(p, &i);
        if (!r && (i.is_dir || !(path_perm(i.mode, i.uid) & 1)))
            r = -EACCES;   /* no execute bit: refused before anything else */
    }
    if (!r)
        memcpy(out, p->abs, strlen(p->abs) + 1);
    path_done(p);
    jam_free(p);
    return r;
}

/* ns since 1970 as a timespec. */
static struct timespec ts_of(int64_t ns)
{
    int64_t s = ns / (int64_t)NS_PER_S, rem = ns % (int64_t)NS_PER_S;
    if (rem < 0) {
        rem += NS_PER_S;
        s--;
    }
    return (struct timespec){ .tv_sec = (time_t)s, .tv_nsec = (long)rem };
}

void path_fill_stat(const struct path *p, const struct path_info *i, struct stat *st)
{
    if (i->has_attr) {   /* the inode's own */
        st->st_mode = i->mode;
        st->st_dev = mount_dev(p);
        st->st_ino = i->id;
        st->st_nlink = i->nlink;
        st->st_uid = i->uid;
        st->st_gid = i->gid;
        st->st_size = (off_t)i->size;
        st->st_blksize = BLKSIZE;
        st->st_blocks = (blkcnt_t)i->blocks;
        st->st_atim = ts_of(i->atime_ns);
        st->st_mtim = ts_of(i->mtime_ns);
        st->st_ctim = ts_of(i->ctime_ns);
        return;
    }
    mode_t mode;
    if (i->is_svc)
        mode = S_IFSOCK | 0666;
    else if (i->is_dir)
        mode = S_IFDIR | 0755;
    else
        mode = S_IFREG | (boot_program(p) ? 0755 : 0644);
    if (!i->is_svc && path_read_only(p))
        mode &= ~(mode_t)0222;
    st->st_mode = mode;
    st->st_dev = mount_dev(p);
    st->st_ino = i->id;
    st->st_nlink = 1;
    st->st_uid = ONE_USER;
    st->st_gid = ONE_USER;
    st->st_size = (off_t)i->size;
    st->st_blksize = BLKSIZE;
    st->st_blocks = (blkcnt_t)((i->size + BLOCK - 1) / BLOCK);
    st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = (time_t)i->mtime;
}

/* ---- the current directory ------------------------------------------------------ */

/* At the start, before the program's constructors: the hand-over's
 * directory channel, else its path (cleaned: dir_new_text), else "/".
 * Nothing is asked of a service here, so a program that never uses its
 * directory pays nothing for it. */
__attribute__((constructor(101))) static void cwd_start(void)
{
    const void *data;
    uint32_t len;
    const char *text = NULL;
    struct fd_desc *d = NULL;
    handle_t ch = lx_handover_dir(&data, &len);
    if (ch) {
        if (dir_adopt_cwd(ch, data, len, &d))
            d = NULL;   /* no memory: "/" */
    } else if (lx_handover(&text) && text && text[0] == '/' && strcmp(text, "/")) {
        if (dir_new_text(text, &d))
            d = NULL;
    }
    lock_take(&cwd_lock);
    cwd = d;
    lock_give(&cwd_lock);
}

struct fd_desc *path_cwd(void)
{
    lock_take(&cwd_lock);
    struct fd_desc *d = cwd;
    if (d)
        fd_ref(d);
    lock_give(&cwd_lock);
    return d;
}

void path_cwd_set(struct fd_desc *d)
{
    lock_take(&cwd_lock);
    struct fd_desc *old = cwd;
    cwd = d;
    lock_give(&cwd_lock);
    if (old)
        fd_put(old);   /* outside the lock: its close may ask its service */
}

void path_fork(int step)
{
    if (step == LX_FORK_TAKE) {
        lock_take(&cwd_lock);
    } else if (step == LX_FORK_GIVE) {
        lock_give(&cwd_lock);
    } else {   /* the child: the directory is ours (path_fork_cwd); the cache asks again */
        cwd_lock = (lock_t){ 0 };
        ro_lock = (lock_t){ 0 };
        memset(ro_cache, 0, sizeof(ro_cache));
        ro_next = 0;
    }
}

void path_fork_cwd(struct lx_fork_io *io)
{
    struct fd_desc *d = cwd;   /* the parent's: cwd_lock held since before the clone */
    if (!d)
        return;
    bool keep = fd_fork_met(io, d, POSIXFORK_CWD);
    if (lx_fork_child(io) && keep)
        d->refs++;   /* the current directory's own reference */
    else if (lx_fork_child(io))
        cwd = NULL;   /* its channel didn't come and it has no text: "/" */
}

/* ---- libc's own trees -------------------------------------------------------- */

enum path_libc path_libc(const struct path *p)
{
    if (p->at)
        return LIBC_NONE;   /* a directory channel's mount: a service's */
    if (dev_path(p->abs))
        return LIBC_DEV;
    if (proc_path(p->abs))
        return LIBC_PROC;
    return etc_path(p->abs) ? LIBC_ETC : LIBC_NONE;
}

long path_libc_stat(const struct path *p, enum path_libc which, struct stat *st)
{
    if (which == LIBC_PROC)
        return proc_stat(p, true, st);   /* its own trailing-slash rule, after the link */
    long r = which == LIBC_DEV ? dev_stat(p->abs, st) : etc_stat(p->abs, st);
    if (!r && p->must_dir && !S_ISDIR(st->st_mode))
        r = -ENOTDIR;
    return r;
}

long path_libc_refuse(enum path_libc which)
{
    return which == LIBC_ETC ? -EROFS : -EACCES;
}

bool path_is_mount(const char *m)
{
    char name[NS_NAME_MAX];
    for (unsigned i = 0; ns_mount_at(i, name); i++)
        if (!strcmp(name, m))
            return true;
    return false;
}
