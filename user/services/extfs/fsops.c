/* extfs: the `fs` methods (abi/idl/fs.idl), each run inside its request's
 * transaction (request.c): a method changes blocks in the cache and
 * returns; the transaction is committed if it answers OK and given up
 * otherwise, so a method that fails half way leaves nothing behind.
 *
 * Every path is walked by walk.c (links, "..", the search bits, what the
 * view hides where the path resolves); then each method checks what it
 * changes: the volume writable, the view allowing the change where the
 * path resolves (walk_may_change), the directory's write and search bits
 * for a name added or removed, the file's bits for an open. */
#include "extfs.h"

/* The two walks a method may need (rename and link take two paths). */
static struct xf_walk wa, wb;

static void attrs_new(struct ext_attrs *a, uint16_t mode)
{
    *a = (struct ext_attrs){
        .mode = mode, .uid = FS_ONE_USER, .gid = FS_ONE_USER, .generation = new_generation(),
    };
    time_now(&a->now);
}

static bool is_dir(const struct ext_inode *in)
{
    return (in->d.mode & EXT_S_IFMT) == EXT_S_IFDIR;
}

/* The checks every method that adds or removes the name w names makes. */
static status_t may_name(const struct xf_ctx *c, const struct xf_walk *w)
{
    if (!writable() || !walk_may_change(c, w))
        return ERR_ACCESS_DENIED;
    return may(&w->dir, 3) ? OK : ERR_ACCESS_DENIED;
}

/* A walk of path whose answer must be found, and not hidden. */
static status_t walk_found(const struct xf_ctx *c, const char *path, uint32_t len,
                           uint32_t flags, struct xf_walk *w)
{
    test_crash(path, len);
    status_t st = walk(c, path, len, flags, w);
    if (st == OK && walk_hidden(c, w, flags & W_STAT))
        st = ERR_ACCESS_DENIED;
    if (st == OK && !w->found)
        st = ERR_NOT_FOUND;
    if (st == OK && w->must_dir && !is_dir(&w->in))
        st = ERR_NOT_DIR;
    return st;
}

static status_t op_open(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                        uint32_t mode, handle_t *out_file, handle_t *out_buffer,
                        uint64_t *out_size)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    if (flags & ~(uint32_t)FS_FLAGS)
        return ERR_INVALID_ARGS;
    bool excl = (flags & (FS_CREATE | FS_EXCL)) == (FS_CREATE | FS_EXCL);
    uint32_t wf = (flags & FS_NOFOLLOW) || excl ? W_NOFOLLOW : 0;
    status_t st = walk(c, path, path_len, wf, w);
    if (st != OK)
        return st;
    if (walk_hidden(c, w, false))
        return ERR_ACCESS_DENIED;
    struct ext_inode in;
    if (!w->found) {
        if (!(flags & FS_CREATE))
            return ERR_NOT_FOUND;
        if (w->must_dir || (flags & FS_DIRECTORY))
            return ERR_IS_DIR;
        st = may_name(c, w);
        if (st != OK)
            return st;
        struct ext_attrs a;
        attrs_new(&a, (uint16_t)(EXT_S_IFREG | (mode & 07777u)));
        st = ext_create(&vol.fs, &w->dir, &w->name, &a, &in);
        if (st != OK)
            return st;
    } else {
        in = w->in;
        uint32_t kind = in.d.mode & EXT_S_IFMT;
        if (excl)
            return ERR_ALREADY_EXISTS;   /* a dangling link too: never made at its target */
        if (kind == EXT_S_IFLNK)
            return ERR_INVALID_ARGS;   /* FS_NOFOLLOW, and the last name is a link */
        if (kind == EXT_S_IFDIR)
            return ERR_IS_DIR;
        if ((flags & FS_DIRECTORY) || w->must_dir)
            return ERR_NOT_DIR;
        if (kind != EXT_S_IFREG)
            return ERR_NOT_SUPPORTED;   /* devices and FIFOs: not in this milestone */
        if (((flags & FS_READ) && !may(&in, 4)) ||
            ((flags & (FS_WRITE | FS_APPEND | FS_TRUNCATE)) && !may(&in, 2)))
            return ERR_ACCESS_DENIED;
        if ((flags & (FS_WRITE | FS_APPEND | FS_TRUNCATE)) &&
            (!writable() || !walk_may_change(c, w)))
            return ERR_ACCESS_DENIED;
        if ((flags & (FS_WRITE | FS_APPEND | FS_TRUNCATE)) && maps_exec(in.ino))
            return ERR_BUSY;   /* a program runs from it (pager.c) */
        if ((flags & FS_TRUNCATE) && ext_isize(&in)) {
            st = files_truncate(&in, 0);
            if (st != OK)
                return st;
        }
    }
    bool nochange = !writable() || !walk_may_change(c, w);
    return files_open(&in, flags, nochange, out_file, out_buffer, out_size);
}

static status_t op_stat(void *ctx, const char *path, uint32_t path_len, uint64_t *out_size,
                        uint8_t *out_is_dir, uint64_t *out_mtime, uint64_t *out_id)
{
    struct xf_walk *w = &wa;
    status_t st = walk_found(ctx, path, path_len, W_STAT, w);
    if (st != OK)
        return st;
    struct ext_time t;
    ext_itime(&w->in, EXT_MTIME, &t);
    bool dir = is_dir(&w->in);
    *out_size = dir ? 0 : ext_isize(&w->in);
    *out_is_dir = dir;
    *out_mtime = t.sec > 0 ? (uint64_t)t.sec : 0;
    *out_id = w->in.ino;
    return OK;
}

static uint8_t dt_of(uint8_t ftype)
{
    switch (ftype) {
    case EXT_FT_DIR:
        return FS_DT_DIR;
    case EXT_FT_REG:
        return FS_DT_REG;
    case EXT_FT_SYMLINK:
        return FS_DT_LNK;
    default:
        return FS_DT_UNKNOWN;
    }
}

/* The cookie is an entry's byte offset in the directory (ext's readdir
 * cookie): it stays good while the directory changes, since entries never
 * move, so no entry is skipped or repeated. */
static status_t op_readdir(void *ctx, const char *path, uint32_t path_len, uint32_t cookie,
                           uint32_t *out_next, uint16_t *out_count, uint8_t *out_entries,
                           uint32_t *out_entries_len)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    struct ext_fs *fs = &vol.fs;
    status_t st = walk_found(c, path, path_len, 0, w);
    if (st != OK)
        return st;
    if (!is_dir(&w->in))
        return ERR_NOT_DIR;
    if (!may(&w->in, 4))
        return ERR_ACCESS_DENIED;
    uint64_t pos = cookie;
    uint32_t len = 0, base = w->phys_len;
    uint16_t count = 0;
    *out_next = 0;
    for (;;) {
        struct ext_dentry e;
        uint64_t at = pos;
        st = ext_dir_next(fs, &w->in, &pos, &e);
        if (st != OK)
            return st;
        if (!e.ino)
            break;
        if ((e.name_len == 1 && e.name[0] == '.') ||
            (e.name_len == 2 && e.name[0] == '.' && e.name[1] == '.'))
            continue;
        /* What the view hides wholly is left out where it lies (the root's
         * `etc`, however the listing was reached). */
        if (c->flags & FS_VIEW_NO_ETC) {
            w->phys_len = base;
            w->phys[base] = '\0';
            if (base + 1 + e.name_len <= FS_PATH_LEN) {
                if (base)
                    w->phys[w->phys_len++] = '/';
                memcpy(w->phys + w->phys_len, e.name, e.name_len + 1u);
                w->phys_len += e.name_len;
            }
            bool hide = fs_view_hides(c->flags, w->phys, true);
            w->phys_len = base;
            w->phys[base] = '\0';
            if (hide)
                continue;
        }
        struct ext_inode in;
        st = ext_iget(fs, e.ino, 0, &in);
        if (st != OK)
            return st == ERR_NOT_FOUND ? ext_damaged(fs, "an entry of a free inode", e.ino, 0)
                                       : st;
        struct ext_time t;
        ext_itime(&in, EXT_MTIME, &t);
        bool dir = is_dir(&in);
        uint8_t type = fs->filetype ? dt_of(e.type) : FS_DT_UNKNOWN;
        if (!fs_dt_ok(type, dir))
            type = FS_DT_UNKNOWN;
        const struct fs_dirent d = {
            .id = e.ino, .size = dir ? 0 : ext_isize(&in), .mtime = t.sec > 0 ? (uint64_t)t.sec : 0,
            .is_dir = dir, .type = type, .name_len = e.name_len, .name = e.name,
        };
        if (!fs_dirent_put(out_entries, FS_READDIR_BYTES, &len, &d)) {
            if (at > UINT32_MAX)
                return ERR_OUT_OF_RANGE;
            *out_next = (uint32_t)at;
            break;
        }
        count++;
    }
    *out_count = count;
    *out_entries_len = len;
    return OK;
}

static status_t op_mkdir(void *ctx, const char *path, uint32_t path_len, uint32_t mode)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    status_t st = walk(c, path, path_len, W_NOFOLLOW, w);
    if (st != OK)
        return st;
    if (walk_hidden(c, w, false))
        return ERR_ACCESS_DENIED;
    if (w->found || !w->has_name)
        return ERR_ALREADY_EXISTS;
    st = may_name(c, w);
    if (st != OK)
        return st;
    struct ext_attrs a;
    struct ext_inode in;
    attrs_new(&a, (uint16_t)(EXT_S_IFDIR | (mode & 07777u)));
    return ext_create(&vol.fs, &w->dir, &w->name, &a, &in);
}

static status_t op_unlink(void *ctx, const char *path, uint32_t path_len, uint32_t flags)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    if (flags & ~(uint32_t)FS_UNLINK_DIR)
        return ERR_INVALID_ARGS;
    status_t st = walk_found(c, path, path_len, W_NOFOLLOW, w);
    if (st != OK)
        return st;
    if (!w->has_name)   /* the root, ".", "..": nothing names it in a directory here */
        return w->in.ino == c->root_ino ? ERR_ACCESS_DENIED : ERR_INVALID_ARGS;
    st = may_name(c, w);
    if (st != OK)
        return st;
    if (maps_exec(w->in.ino))
        return ERR_BUSY;   /* a program runs from it (pager.c) */
    struct ext_time now;
    time_now(&now);
    uint32_t orphan = 0;
    st = ext_unlink(&vol.fs, &w->dir, &w->name, (flags & FS_UNLINK_DIR) ? EXT_UNLINK_DIR : 0,
                    &now, &orphan);
    if (st == OK && orphan)
        defer_drop(orphan);   /* on the orphan list: dropped after the answer, unless open */
    return st;
}

static status_t op_rename(void *ctx, const char *from, uint32_t from_len, const char *to,
                          uint32_t to_len, uint32_t flags)
{
    const struct xf_ctx *c = ctx;
    if ((flags & ~(uint32_t)(FS_RENAME_REPLACE | FS_RENAME_EXCHANGE)) ||
        (flags & FS_RENAME_REPLACE && flags & FS_RENAME_EXCHANGE))
        return ERR_INVALID_ARGS;
    status_t st = walk_found(c, from, from_len, W_NOFOLLOW, &wa);
    if (st != OK)
        return st;
    st = walk(c, to, to_len, W_NOFOLLOW, &wb);
    if (st != OK)
        return st;
    if (walk_hidden(c, &wb, false))
        return ERR_ACCESS_DENIED;
    if (!wa.has_name || !wb.has_name)
        return ERR_INVALID_ARGS;   /* the root, or "." / ".." */
    if ((flags & FS_RENAME_EXCHANGE) && !wb.found)
        return ERR_NOT_FOUND;
    st = may_name(c, &wa);
    if (st == OK)
        st = may_name(c, &wb);
    if (st != OK)
        return st;
    if (wb.found && !(flags & FS_RENAME_EXCHANGE) && wb.in.ino != wa.in.ino &&
        maps_exec(wb.in.ino))
        return ERR_BUSY;   /* renamed over, a program's file would lose its name (pager.c) */
    /* One directory: one copy of it, which both halves keep current. */
    bool same = wa.dir.ino == wb.dir.ino;
    struct ext_rename r = {
        .from_dir = &wa.dir, .from = wa.name, .to_dir = same ? &wa.dir : &wb.dir,
        .to = wb.name,
        .flags = (flags & FS_RENAME_EXCHANGE) ? EXT_RENAME_EXCHANGE
                 : (flags & FS_RENAME_REPLACE) ? 0 : EXT_RENAME_NOREPLACE,
    };
    time_now(&r.now);
    uint32_t orphan = 0;
    st = ext_rename(&vol.fs, &r, &orphan);
    if (st == OK && orphan)
        defer_drop(orphan);
    return st;
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    return OK;   /* every answered request is committed already */
}

static status_t op_statfs(void *ctx, uint64_t *out_total, uint64_t *out_free,
                          uint8_t *out_read_only, uint8_t out_label[16], uint32_t *out_magic,
                          uint32_t *out_name_max, uint32_t *out_features)
{
    (void)ctx;
    struct ext_fs *fs = &vol.fs;
    *out_total = fs->blocks << fs->block_log;
    *out_free = ext_free_blocks(fs) << fs->block_log;
    *out_read_only = !writable();
    *out_magic = XF_MAGIC;
    *out_name_max = XF_NAME_MAX;
    *out_features = XF_FEATURES;
    /* The label as shown: printable ASCII only (anything can be in it). */
    for (unsigned i = 0; i < 15 && fs->sb.volume_name[i]; i++) {
        char ch = fs->sb.volume_name[i];
        out_label[i] = (uint8_t)(ch >= 0x20 && ch < 0x7f ? ch : '?');
    }
    return OK;
}

static status_t op_getattr(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                           uint32_t *out_mode, uint32_t *out_nlink, uint32_t *out_uid,
                           uint32_t *out_gid, uint64_t *out_size, uint64_t *out_blocks,
                           uint64_t *out_id, int64_t *out_atime_ns, int64_t *out_mtime_ns,
                           int64_t *out_ctime_ns)
{
    if (flags & ~(uint32_t)FS_NOFOLLOW)
        return ERR_INVALID_ARGS;
    struct xf_walk *w = &wa;
    status_t st = walk_found(ctx, path, path_len, (flags & FS_NOFOLLOW ? W_NOFOLLOW : 0) | W_STAT,
                             w);
    if (st != OK)
        return st;
    struct xf_attr a;
    attr_of(&w->in, &a);
    *out_mode = a.mode;
    *out_nlink = a.nlink;
    *out_uid = a.uid;
    *out_gid = a.gid;
    *out_size = a.size;
    *out_blocks = a.blocks;
    *out_id = a.id;
    *out_atime_ns = a.atime_ns;
    *out_mtime_ns = a.mtime_ns;
    *out_ctime_ns = a.ctime_ns;
    return OK;
}

static status_t op_setattr(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                           uint32_t mask, uint32_t mode, uint32_t uid, uint32_t gid,
                           int64_t atime_ns, int64_t mtime_ns)
{
    if (flags & ~(uint32_t)FS_NOFOLLOW)
        return ERR_INVALID_ARGS;
    struct xf_walk *w = &wa;
    status_t st = walk_found(ctx, path, path_len, flags & FS_NOFOLLOW ? W_NOFOLLOW : 0, w);
    if (st != OK)
        return st;
    if (!walk_may_change(ctx, w))
        return ERR_ACCESS_DENIED;
    return attr_set(&w->in, mask, mode, uid, gid, atime_ns, mtime_ns);
}

static status_t op_symlink(void *ctx, const char *target, uint32_t target_len, const char *path,
                           uint32_t path_len)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    if (!target_len)
        return ERR_INVALID_ARGS;
    status_t st = walk(c, path, path_len, W_NOFOLLOW, w);
    if (st != OK)
        return st;
    if (walk_hidden(c, w, false))
        return ERR_ACCESS_DENIED;
    if (w->found || !w->has_name)
        return ERR_ALREADY_EXISTS;
    st = may_name(c, w);
    if (st != OK)
        return st;
    if (target_len >= vol.fs.block_size)
        return ERR_NAME_TOO_LONG;
    struct ext_attrs a;
    struct ext_inode in;
    attrs_new(&a, (uint16_t)(EXT_S_IFLNK | 0777u));
    const struct ext_name t = { .s = target, .len = target_len };
    return ext_symlink(&vol.fs, &w->dir, &w->name, &t, &a, &in);
}

static status_t op_readlink(void *ctx, const char *path, uint32_t path_len, char *out_target,
                            uint32_t *out_target_len)
{
    struct xf_walk *w = &wa;
    status_t st = walk_found(ctx, path, path_len, W_NOFOLLOW, w);
    if (st != OK)
        return st;
    if ((w->in.d.mode & EXT_S_IFMT) != EXT_S_IFLNK)
        return ERR_INVALID_ARGS;
    size_t n = 0;
    st = ext_readlink(&vol.fs, &w->in, out_target, FS_PATH_LEN, &n);
    if (st == ERR_BUFFER_TOO_SMALL)
        return ERR_NAME_TOO_LONG;
    *out_target_len = (uint32_t)n;
    return st;
}

static status_t op_link(void *ctx, const char *from, uint32_t from_len, const char *to,
                        uint32_t to_len)
{
    const struct xf_ctx *c = ctx;
    status_t st = walk_found(c, from, from_len, W_NOFOLLOW, &wa);
    if (st != OK)
        return st;
    if (is_dir(&wa.in))
        return ERR_IS_DIR;
    st = walk(c, to, to_len, W_NOFOLLOW, &wb);
    if (st != OK)
        return st;
    if (walk_hidden(c, &wb, false))
        return ERR_ACCESS_DENIED;
    if (wb.found || !wb.has_name)
        return ERR_ALREADY_EXISTS;
    /* A link to a file the view may not change would let it be changed
     * from outside where the view guards it. */
    if (!walk_may_change(c, &wa))
        return ERR_ACCESS_DENIED;
    st = may_name(c, &wb);
    if (st != OK)
        return st;
    struct ext_time now;
    time_now(&now);
    return ext_link(&vol.fs, &wb.dir, &wb.name, &wa.in, &now);
}

static status_t op_resolve(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                           uint8_t *out_kind, uint16_t *out_used, char *out_next,
                           uint32_t *out_next_len)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    if (flags & ~(uint32_t)FS_NOFOLLOW)
        return ERR_INVALID_ARGS;
    status_t st = walk(c, path, path_len, flags & FS_NOFOLLOW ? W_NOFOLLOW : 0, w);
    if (st == ERR_LINK_OUT) {
        *out_kind = w->kind;
        *out_used = (uint16_t)w->used;
        memcpy(out_next, w->next, w->next_len);
        *out_next_len = w->next_len;
        return OK;
    }
    if (st != OK)
        return st;
    if (walk_hidden(c, w, true))
        return ERR_ACCESS_DENIED;
    *out_kind = FS_RESOLVE_INSIDE;
    *out_used = (uint16_t)path_len;
    walk_inside(c, w, out_next, out_next_len);
    return OK;
}

static status_t op_subview(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                           handle_t *out_fs)
{
    const struct xf_ctx *c = ctx;
    struct xf_walk *w = &wa;
    if (flags & ~(uint32_t)FS_VIEW_FLAGS)
        return ERR_INVALID_ARGS;
    status_t st = walk_found(c, path, path_len, 0, w);
    if (st != OK)
        return st;
    if (!is_dir(&w->in))
        return ERR_NOT_DIR;
    return views_sub(c, w, flags, out_fs);
}

const struct fs_ops xf_fs_ops = {
    .open = op_open, .stat = op_stat, .readdir = op_readdir, .mkdir = op_mkdir,
    .unlink = op_unlink, .rename = op_rename, .sync = op_sync, .statfs = op_statfs,
    .getattr = op_getattr, .setattr = op_setattr, .symlink = op_symlink,
    .readlink = op_readlink, .link = op_link, .resolve = op_resolve, .subview = op_subview,
};

/* ---- fsctl -------------------------------------------------------------------------- */

static bool stopping;

bool ctl_stopping(void)
{
    return stopping;
}

/* fsctl.stop: every file closed, what was left done, the journal closed
 * (the volume clean on the disk). */
static status_t ctl_stop(void *ctx)
{
    (void)ctx;
    files_close_all();
    defer_run();
    stopping = true;
    if (vol.disk_gone)
        return ERR_PEER_CLOSED;
    if (vol.fs.jnl && !vol.fs.ro)
        return ext_jnl_close(&vol.fs);
    return OK;
}

static status_t ctl_stats(void *ctx, uint64_t *out_entries_read, uint64_t *out_cache_hits,
                          uint64_t *out_cache_fills, uint64_t *out_cache_bypassed,
                          uint64_t *out_cache_updated)
{
    (void)ctx;
    const struct ext_fs *fs = &vol.fs;
    *out_entries_read = fs->names ? fs->names->builds : 0;
    *out_cache_hits = fs->names ? fs->names->hits : 0;
    *out_cache_fills = fs->cache.reads;
    *out_cache_bypassed = 0;
    *out_cache_updated = fs->cache.writes;
    return OK;
}

const struct fsctl_ops xf_ctl_ops = { .stop = ctl_stop, .stats = ctl_stats };
