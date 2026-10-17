/* extfs: the `fs` methods (abi/idl/fs.idl), each run inside its request's
 * transaction (request.c): a method changes blocks in the cache and
 * returns; the transaction is committed if it answers OK and given up
 * otherwise, so a method that fails half way leaves nothing behind.
 *
 * Every path is walked by walk.c (links, "..", the search bits, what the
 * view hides where the path resolves); then each method checks what it
 * changes: the volume writable, the view allowing the change where the
 * path resolves (walk_may_change), the directory's write and search bits
 * for a name added or removed, the file's bits for an open.
 *
 * A method that changed something says so to the watches (watch.c's ev,
 * held until the commit) in the order of docs/INOTIFY-PLAN.md's 2.2,
 * which is Linux's: the directory's event, then the inode's own, except
 * that a new link's IN_ATTRIB and an unlink's come before the name's
 * IN_CREATE or IN_DELETE (fsnotify_link, vfs_unlink). */
#include "extfs.h"

/* The two walks a method may need (rename and link take two paths). */
static struct xf_walk wa, wb;

static bool is_dir(const struct ext_inode *in);

/* An event of what walk w named: to its directory's watches with its name
 * (if the path ended in one), then to in's own (NULL: none). */
static void ev_walk(const struct xf_walk *w, const struct ext_inode *in, uint32_t mask)
{
    if (!watching() || !mask)
        return;
    const struct fsw_key dir = xf_key(&w->dir), self = in ? xf_key(in) : (struct fsw_key){ 0 };
    uint32_t flags = in && is_dir(in) ? FSW_EV_DIR : 0;
    ev(w->has_name ? &dir : NULL, w->name.s, w->name.len, in ? &self : NULL, mask, flags, 0);
}

/* An event of inode in alone (its own watches). */
static void ev_inode(const struct ext_inode *in, uint32_t mask, uint32_t flags)
{
    if (!watching())
        return;
    const struct fsw_key self = xf_key(in);
    ev(NULL, NULL, 0, &self, mask, flags, 0);
}

/* An event in directory dir about its entry `name` alone. */
static void ev_name(const struct ext_inode *dir, const struct ext_name *name, uint32_t mask,
                    bool isdir, uint32_t cookie)
{
    if (!watching())
        return;
    const struct fsw_key d = xf_key(dir);
    ev(&d, name->s, name->len, NULL, mask, isdir ? FSW_EV_DIR : 0, cookie);
}

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
    bool truncated = false;
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
        ev_name(&w->dir, &w->name, FSWATCH_IN_CREATE, false, 0);
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
        truncated = flags & FS_TRUNCATE;   /* an empty file too: Linux truncates it all the same */
    }
    bool nochange = !writable() || !walk_may_change(c, w);
    st = files_open(&in, flags, nochange, w, out_file, out_buffer, out_size);
    if (st != OK)
        return st;
    ev_walk(w, &in, FSWATCH_IN_OPEN);   /* the open, then its truncation (Linux's vfs_open) */
    if (truncated)
        ev_walk(w, &in, FSWATCH_IN_MODIFY);
    return OK;
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
    st = ext_create(&vol.fs, &w->dir, &w->name, &a, &in);
    if (st == OK)
        ev_name(&w->dir, &w->name, FSWATCH_IN_CREATE, true, 0);
    return st;
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
    if (st != OK)
        return st;
    if (orphan)
        defer_drop(orphan);   /* on the orphan list: dropped after the answer, unless open;
                               * its watches' IN_DELETE_SELF then (defer_run) */
    /* A file's link count: IN_ATTRIB first (vfs_unlink); a directory has none. */
    bool dir = is_dir(&w->in);
    if (!dir)
        ev_inode(&w->in, FSWATCH_IN_ATTRIB, 0);
    ev_name(&w->dir, &w->name, FSWATCH_IN_DELETE, dir, 0);
    const struct fsw_key d = xf_key(&w->dir);
    files_unlinked(&d, &w->name);
    return OK;
}

/* A rename done (wa from, wb to): Linux's fsnotify_move, once, or twice
 * for an exchange (each name's move with a cookie of its own): the old
 * directory's IN_MOVED_FROM, the new one's IN_MOVED_TO, a replaced
 * inode's IN_ATTRIB (its link count; its IN_DELETE_SELF when it goes,
 * defer_run), the moved inode's IN_MOVE_SELF. The open files' names move
 * with it. */
static void renamed(bool exchange)
{
    const struct fsw_key from_dir = xf_key(&wa.dir), to_dir = xf_key(&wb.dir);
    files_renamed(&from_dir, &wa.name, &to_dir, &wb.name, exchange);
    if (!watching())
        return;
    bool src_dir = is_dir(&wa.in), dst_dir = wb.found && is_dir(&wb.in);
    uint32_t cookie = ev_cookie();
    ev_name(&wa.dir, &wa.name, FSWATCH_IN_MOVED_FROM, src_dir, cookie);
    ev_name(&wb.dir, &wb.name, FSWATCH_IN_MOVED_TO, src_dir, cookie);
    if (wb.found && !exchange)
        ev_inode(&wb.in, FSWATCH_IN_ATTRIB, dst_dir ? FSW_EV_DIR : 0);
    ev_inode(&wa.in, FSWATCH_IN_MOVE_SELF, 0);   /* never IN_ISDIR (inotify) */
    if (!exchange)
        return;
    cookie = ev_cookie();
    ev_name(&wb.dir, &wb.name, FSWATCH_IN_MOVED_FROM, dst_dir, cookie);
    ev_name(&wa.dir, &wa.name, FSWATCH_IN_MOVED_TO, dst_dir, cookie);
    ev_inode(&wb.in, FSWATCH_IN_MOVE_SELF, 0);
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
    if (st != OK)
        return st;
    if (orphan)
        defer_drop(orphan);
    if (wb.found && wb.in.ino == wa.in.ino)
        return OK;   /* a name onto another name of the same inode: nothing done, no event */
    renamed(flags & FS_RENAME_EXCHANGE);
    return OK;
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    status_t st = wback_all();   /* mapped files' stores, every one made before the call */
    status_t fl = disk_flush();  /* every answered request is committed already; a failed
                                  * one's data too, on a disk with a cache */
    return st != OK ? st : fl;
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
    st = attr_set(&w->in, mask, mode, uid, gid, atime_ns, mtime_ns);
    if (st == OK)
        ev_walk(w, &w->in, ev_attr_mask(mask));
    return st;
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
    st = ext_symlink(&vol.fs, &w->dir, &w->name, &t, &a, &in);
    if (st == OK)
        ev_name(&w->dir, &w->name, FSWATCH_IN_CREATE, false, 0);
    return st;
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
    st = ext_link(&vol.fs, &wb.dir, &wb.name, &wa.in, &now);
    if (st != OK)
        return st;
    ev_inode(&wa.in, FSWATCH_IN_ATTRIB, 0);   /* its link count first (fsnotify_link) */
    ev_name(&wb.dir, &wb.name, FSWATCH_IN_CREATE, false, 0);
    return OK;
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

/* A watcher channel through this view (watch.c): a read-only volume's
 * too, whose watches stay quiet. */
static status_t op_watch(void *ctx, handle_t *out_watcher)
{
    return watch_new(ctx, out_watcher);
}

const struct fs_ops xf_fs_ops = {
    .open = op_open, .stat = op_stat, .readdir = op_readdir, .mkdir = op_mkdir,
    .unlink = op_unlink, .rename = op_rename, .sync = op_sync, .statfs = op_statfs,
    .getattr = op_getattr, .setattr = op_setattr, .symlink = op_symlink,
    .readlink = op_readlink, .link = op_link, .resolve = op_resolve, .subview = op_subview,
    .watch = op_watch,
};

/* ---- fsctl -------------------------------------------------------------------------- */

static bool stopping;

bool ctl_stopping(void)
{
    return stopping;
}

/* fsctl.stop: mapped files' stores written back (for a few seconds at
 * most), every file closed, what was left done, the journal closed (the
 * volume clean on the disk). */
static status_t ctl_stop(void *ctx)
{
    (void)ctx;
    wback_stop();
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
