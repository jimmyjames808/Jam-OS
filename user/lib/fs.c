/* The file calls of <os.h>: each finds its path's mount in the namespace
 * (ns.c) and calls that mount's `fs` service, or an open file's `file`
 * channel (abi/idl/fs.idl, file.idl).
 *
 * File data moves through the buffer VMO fs.open hands out with the file's
 * channel, mapped here for as long as the file is open: a read has the
 * service fill it and copies out, a write copies in and has the service
 * take it. Transfers larger than the buffer are split. Every call has a
 * time limit (FS_CALL_TIMEOUT, given to the kernel as a timeout: no clock
 * read first), and a service that died closes its channels, so none of
 * these can hang on a broken mount. A mapped file (fs_map_file, mapped_file_*)
 * is the file's pages as a service supplies them (file.map): no copy
 * through the buffer, and a page the service can't supply is a fault. */
#include <fs_idl.h>
#include "ns.h"

#define BUF_MAX (1u << 20)   /* a bigger transfer buffer than this is a broken service */

/* ns_resolve's path inside the mount, as the protocol's str. */
static uint32_t rel_len(const uint8_t rel[FS_PATH_MAX])
{
    return (uint32_t)strnlen((const char *)rel, FS_PATH_MAX);
}

/* ---- open files --------------------------------------------------------------- */

/* Map f's buffer VMO: whole pages, at most BUF_MAX. */
static status_t map_buffer(struct jfile *f, bool writable)
{
    uint64_t size = 0, addr = 0;
    status_t st = jam_vmo_get_size(f->buf_vmo, &size);
    if (st == OK && (!size || size > BUF_MAX || (size & (PAGE_SIZE - 1))))
        st = ERR_INTERNAL;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), f->buf_vmo, 0, size,
                          VMAR_READ | (writable ? VMAR_WRITE : 0), &addr);
    if (st != OK)
        return st;
    f->buf = (uint8_t *)(uintptr_t)addr;
    f->buf_size = (uint32_t)size;
    return OK;
}

status_t file_open(const char *path, uint32_t flags, struct jfile *out)
{
    return file_open_mode(path, flags, FS_MODE_FILE_DEFAULT, out);
}

status_t file_open_mode(const char *path, uint32_t flags, uint32_t mode, struct jfile *out)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    if (flags & ~FS_FLAGS)
        return ERR_INVALID_ARGS;
    status_t st = ns_resolve(path, rel, &fs, &mount);
    if (st != OK)
        return st;
    if (!fs)   /* "/" and "/svc" are directories; a service is neither */
        return mount == NS_AT_SVC && rel[1] ? ERR_WRONG_TYPE : ERR_IS_DIR;
    struct jfile f = { 0 };
    st = fs_open_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), flags, mode, &f.ch,
                        &f.buf_vmo, &f.size);
    jam_handle_close(fs);
    if (st != OK)
        return st;
    f.flags = flags;
    st = map_buffer(&f, flags & FS_WRITE);
    if (st != OK) {
        jam_handle_close(f.ch);
        jam_handle_close(f.buf_vmo);
        return st;
    }
    *out = f;
    return OK;
}

void file_give(struct jfile *f, handle_t *ch, handle_t *buf)
{
    if (f->buf)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)f->buf, f->buf_size);
    *ch = f->ch;
    *buf = f->buf_vmo;
    memset(f, 0, sizeof(*f));
}

status_t file_adopt(handle_t ch, handle_t buf, uint32_t flags, struct jfile *out)
{
    struct jfile f = { .ch = ch, .buf_vmo = buf, .flags = flags };
    status_t st = flags & ~FS_FLAGS ? ERR_INVALID_ARGS : map_buffer(&f, flags & FS_WRITE);
    if (st == OK)
        st = file_stat(&f, &f.size, NULL);
    if (st != OK) {
        file_close(&f);   /* closes both handles, unmaps what was mapped */
        return st;
    }
    *out = f;
    return OK;
}

/* The next piece of an n-byte transfer of which `done` bytes are through. */
static uint32_t piece(const struct jfile *f, size_t n, size_t done)
{
    return n - done < f->buf_size ? (uint32_t)(n - done) : f->buf_size;
}

/* f is open with `flag`, and n bytes at offset is a range a file can have. */
static status_t may(const struct jfile *f, uint32_t flag, uint64_t offset, size_t n)
{
    if (!f->buf_size)
        return ERR_BAD_STATE;
    if (!(f->flags & flag))
        return ERR_ACCESS_DENIED;
    return n > UINT64_MAX - offset ? ERR_OUT_OF_RANGE : OK;
}

status_t file_read(struct jfile *f, uint64_t offset, void *dst, size_t n, size_t *done)
{
    status_t ok = may(f, FS_READ, offset, n);
    if (ok != OK)
        return ok;
    size_t got = 0;
    while (got < n) {
        uint32_t want = piece(f, n, got), actual = 0;
        status_t st = file_read_within(f->ch, FS_CALL_TIMEOUT, offset + got, want, &actual);
        if (st == OK && actual > want)
            st = ERR_INTERNAL;
        if (st != OK)
            return st;
        memcpy((uint8_t *)dst + got, f->buf, actual);
        got += actual;
        if (actual < want)
            break;   /* the end of the file */
    }
    *done = got;
    return OK;
}

status_t file_write(struct jfile *f, uint64_t offset, const void *src, size_t n, size_t *done)
{
    /* Without FS_WRITE the buffer is mapped read-only: never copy into it. */
    status_t ok = may(f, FS_WRITE, offset, n);
    if (ok != OK)
        return ok;
    size_t put = 0;
    while (put < n) {
        uint32_t want = piece(f, n, put), actual = 0;
        memcpy(f->buf, (const uint8_t *)src + put, want);
        status_t st = file_write_within(f->ch, FS_CALL_TIMEOUT, offset + put, want, &actual);
        if (st == OK && actual > want)
            st = ERR_INTERNAL;
        if (st != OK)
            return st;
        put += actual;
        if (actual < want)
            break;   /* the service took less: the caller sees it in *done */
    }
    *done = put;
    return OK;
}

status_t file_sync(struct jfile *f)
{
    return file_sync_within(f->ch, FS_CALL_TIMEOUT);
}

status_t file_stat(struct jfile *f, uint64_t *size, uint64_t *mtime)
{
    return file_stat_within(f->ch, FS_CALL_TIMEOUT, size, NULL, mtime, NULL);
}

status_t file_truncate(struct jfile *f, uint64_t size)
{
    return file_truncate_within(f->ch, FS_CALL_TIMEOUT, size);
}

status_t file_getattr(struct jfile *f, struct fs_attr *out)
{
    struct fs_attr a;
    status_t st = file_getattr_within(f->ch, FS_CALL_TIMEOUT, &a.mode, &a.nlink, &a.uid, &a.gid,
                                      &a.size, &a.blocks, &a.id, &a.atime_ns, &a.mtime_ns,
                                      &a.ctime_ns);
    if (st == OK)
        *out = a;
    return st;
}

status_t file_setattr(struct jfile *f, const struct fs_attr_set *set)
{
    return file_setattr_within(f->ch, FS_CALL_TIMEOUT, set->mask, set->mode, set->uid, set->gid,
                               set->atime_ns, set->mtime_ns);
}

void file_close(struct jfile *f)
{
    if (f->buf)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)f->buf, f->buf_size);
    if (f->ch)
        jam_handle_close(f->ch);
    if (f->buf_vmo)
        jam_handle_close(f->buf_vmo);
    memset(f, 0, sizeof(*f));
}

status_t file_read_vmo(const char *path, uint64_t max_size, handle_t *vmo, uint64_t *size)
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    uint64_t total = 0, off = 0;
    handle_t v = HANDLE_INVALID;
    st = file_stat(&f, &total, NULL);
    if (st == OK && total > max_size)
        st = ERR_OUT_OF_RANGE;
    if (st == OK) {
        uint64_t pages = (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        st = jam_vmo_create(pages ? pages : PAGE_SIZE, 0, &v);
    }
    while (st == OK && off < total) {
        uint32_t want = total - off < f.buf_size ? (uint32_t)(total - off) : f.buf_size;
        uint32_t actual = 0;
        st = file_read_within(f.ch, FS_CALL_TIMEOUT, off, want, &actual);
        if (st == OK && (!actual || actual > want))
            st = ERR_IO;   /* the file shrank under us, or a broken service */
        if (st == OK)
            st = jam_vmo_write(v, off, f.buf, actual);
        off += actual;
    }
    file_close(&f);
    if (st != OK) {
        if (v)
            jam_handle_close(v);
        return st;
    }
    *vmo = v;
    *size = total;
    return OK;
}

/* ---- mapped files ------------------------------------------------------------- */

status_t fs_map_file(struct jfile *f, uint32_t flags, handle_t *vmo, uint64_t *size)
{
    if (flags != FILE_MAP_READ && flags != FILE_MAP_EXEC)
        return ERR_INVALID_ARGS;
    if (!f->ch)
        return ERR_BAD_STATE;
    if (!(f->flags & FS_READ))
        return ERR_ACCESS_DENIED;   /* the service would say so too, on a view */
    handle_t v = HANDLE_INVALID;
    uint64_t sz = 0;
    status_t st = file_map_within(f->ch, FS_CALL_TIMEOUT, flags, &v, &sz);
    if (st != OK)
        return st;
    *vmo = v;
    *size = sz;
    return OK;
}

status_t file_map_path(const char *path, uint32_t flags, handle_t *vmo, uint64_t *size)
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    st = fs_map_file(&f, flags, vmo, size);
    file_close(&f);   /* the mapping outlives the file, as an mmap outlives its close */
    return st;
}

status_t mapped_file_from_vmo(handle_t vmo, uint64_t size, struct mapped_file *out)
{
    uint64_t have = 0, addr = 0;
    status_t st = jam_vmo_get_size(vmo, &have);
    uint64_t len = 0;
    if (st == OK && size > UINT64_MAX - (PAGE_SIZE - 1))
        st = ERR_OUT_OF_RANGE;
    if (st == OK) {
        len = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (len > have)
            st = ERR_INTERNAL;   /* a broken service: the VMO is smaller than the file */
    }
    if (st == OK && len)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, len, VMAR_READ, &addr);
    if (st != OK) {
        jam_handle_close(vmo);
        return st;
    }
    *out = (struct mapped_file){ .vmo = vmo, .data = (const uint8_t *)(uintptr_t)addr,
                                 .size = size, .map_len = len };
    return OK;
}

status_t mapped_file_open(const char *path, struct mapped_file *out)
{
    handle_t vmo = HANDLE_INVALID;
    uint64_t size = 0;
    status_t st = file_map_path(path, FILE_MAP_READ, &vmo, &size);
    return st == OK ? mapped_file_from_vmo(vmo, size, out) : st;
}

void mapped_file_close(struct mapped_file *mf)
{
    if (mf->data)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)mf->data, mf->map_len);
    if (mf->vmo)
        jam_handle_close(mf->vmo);
    memset(mf, 0, sizeof(*mf));
}

/* ---- paths -------------------------------------------------------------------- */

/* path's mount for a call that can't be made on "/" itself (no_root: what
 * "/" gets instead). The caller closes *fs. */
static status_t on_mount(const char *path, uint8_t rel[FS_PATH_MAX], handle_t *fs,
                         unsigned *mount, status_t no_root)
{
    status_t st = ns_resolve(path, rel, fs, mount);
    if (st == OK && !*fs)
        st = no_root;
    return st;
}

status_t fs_stat(const char *path, uint64_t *size, bool *is_dir, uint64_t *mtime)
{
    uint8_t rel[FS_PATH_MAX], dir = 1;
    uint64_t sz = 0, mt = 0;
    handle_t fs;
    unsigned mount;
    status_t st = ns_resolve(path, rel, &fs, &mount);
    if (st == OK && fs) {   /* else "/" or "/svc": directories; a service is neither */
        st = fs_stat_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), &sz, &dir, &mt,
                            NULL);
        jam_handle_close(fs);
    } else if (st == OK && mount == NS_AT_SVC && rel[1]) {
        dir = 0;
    }
    if (st != OK)
        return st;
    if (size)
        *size = sz;
    if (is_dir)
        *is_dir = dir != 0;
    if (mtime)
        *mtime = mt;
    return OK;
}

/* Entry `index` of "/": the mount points, then "svc" if we have services. */
static status_t root_entry(uint32_t index, struct fs_entry *out)
{
    char point[NS_NAME_MAX], name[SVC_NAME_MAX + 1];
    uint32_t mounts = 0;
    while (ns_mount_at(mounts, point)) {
        if (mounts++ == index) {
            memcpy(out->name, point + 1, strlen(point + 1) + 1);
            out->is_dir = true;
            out->type = FS_DT_DIR;
            return OK;
        }
    }
    if (index != mounts || !ns_svc_at(0, name))
        return ERR_NOT_FOUND;
    memcpy(out->name, "svc", 4);
    out->is_dir = true;
    out->type = FS_DT_DIR;
    return OK;
}

/* Entry `index` of "/svc": a service's name (neither file nor directory). */
static status_t svc_entry(uint32_t index, struct fs_entry *out)
{
    char name[SVC_NAME_MAX + 1];
    if (!ns_svc_at(index, name))
        return ERR_NOT_FOUND;
    memcpy(out->name, name, strlen(name) + 1);
    return OK;
}

/* One batch of fs.readdir's entries: of directory `path` (as asked), entry
 * `first` on, `count` of them; `next` the cookie after them (0: the last). */
struct batch {
    char     path[FS_PATH_MAX];
    uint32_t first;
    uint32_t next;
    uint32_t count;
    uint32_t len;
    uint8_t  entries[FS_READDIR_BYTES];
};

/* The last batch a fs_readdir was given (NULL: none); the thread using it
 * takes it out under the lock, so two threads never share one. */
static struct batch *cached;
static lock_t cached_lock;

/* Every record of b in order: OK, or ERR_INTERNAL for a malformed batch
 * (the service is broken). Entry k into *out if k < b->count. */
static status_t batch_entry(const struct batch *b, uint32_t k, struct fs_entry *out)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        struct fs_dirent e;
        if (!fs_dirent_get(b->entries, b->len, &at, &e))
            return ERR_INTERNAL;
        if (i == k && out) {
            memcpy(out->name, e.name, e.name_len);
            out->name[e.name_len] = '\0';
            out->is_dir = e.is_dir;
            out->type = e.type;
            out->size = e.size;
            out->mtime = e.mtime;
            out->id = e.id;
        }
    }
    return at == b->len ? OK : ERR_INTERNAL;
}

/* The batch after b (or the first, if b holds none) from the service. */
static status_t next_batch(handle_t fs, const uint8_t rel[FS_PATH_MAX], struct batch *b,
                           bool first)
{
    uint32_t cookie = first ? 0 : b->next, start = first ? 0 : b->first + b->count;
    uint16_t count = 0;
    b->count = 0;
    status_t st = fs_readdir_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), cookie,
                                    &b->next, &count, b->entries, &b->len);
    b->first = start;
    b->count = st == OK ? count : 0;
    if (st == OK)
        st = batch_entry(b, b->count, NULL);
    if (st == OK && !b->count && b->next)
        st = ERR_INTERNAL;   /* an empty batch that isn't the last: it would never end */
    return st;
}

status_t fs_readdir(const char *path, uint32_t index, struct fs_entry *out)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = ns_resolve(path, rel, &fs, &mount);
    if (st != OK)
        return st;
    memset(out, 0, sizeof(*out));
    if (!fs && mount == NS_AT_SVC)
        return rel[1] ? ERR_NOT_DIR : svc_entry(index, out);   /* a service is no directory */
    if (!fs)
        return root_entry(index, out);
    lock_take(&cached_lock);
    struct batch *b = cached;
    cached = NULL;
    lock_give(&cached_lock);
    /* Going on from the last batch: the same directory, further on; index
     * 0 starts a listing afresh. */
    bool going_on = b && index && index >= b->first && !strcmp(b->path, path);
    if (!b)
        b = malloc(sizeof(*b));
    if (!b)
        st = ERR_NO_MEMORY;
    if (st == OK && !going_on) {
        memcpy(b->path, path, strlen(path) + 1);   /* ns_resolve: shorter than FS_PATH_MAX */
        st = next_batch(fs, rel, b, true);
    }
    while (st == OK && index >= b->first + b->count) {
        if (!b->next) {
            st = ERR_NOT_FOUND;
            break;
        }
        st = next_batch(fs, rel, b, false);
    }
    jam_handle_close(fs);
    if (st == OK)
        st = batch_entry(b, index - b->first, out);
    if (b && st != OK && st != ERR_NOT_FOUND)
        b->path[0] = '\0';   /* nothing to go on from */
    lock_take(&cached_lock);
    if (!cached) {
        cached = b;
        b = NULL;
    }
    lock_give(&cached_lock);
    free(b);
    return st;
}

status_t fs_mkdir(const char *path)
{
    return fs_mkdir_mode(path, FS_MODE_DIR_DEFAULT);
}

status_t fs_mkdir_mode(const char *path, uint32_t mode)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_ALREADY_EXISTS);
    if (st != OK)
        return st;
    st = fs_mkdir_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), mode);
    jam_handle_close(fs);
    return st;
}

status_t fs_unlink(const char *path)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_ACCESS_DENIED);
    if (st != OK)
        return st;
    st = fs_unlink_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), 0);
    if (st == ERR_IS_DIR)   /* a directory: remove it as one */
        st = fs_unlink_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), FS_UNLINK_DIR);
    jam_handle_close(fs);
    return st;
}

status_t fs_rename(const char *from, const char *to)
{
    uint8_t rel_from[FS_PATH_MAX], rel_to[FS_PATH_MAX];
    handle_t fs, fs_to;
    unsigned mount, mount_to;
    status_t st = on_mount(from, rel_from, &fs, &mount, ERR_ACCESS_DENIED);
    if (st != OK)
        return st;
    st = on_mount(to, rel_to, &fs_to, &mount_to, ERR_ACCESS_DENIED);
    if (st == OK) {
        jam_handle_close(fs_to);
        st = mount == mount_to
                 ? fs_rename_within(fs, FS_CALL_TIMEOUT, (const char *)rel_from,
                                    rel_len(rel_from), (const char *)rel_to, rel_len(rel_to), 0)
                 : ERR_NOT_SUPPORTED;
    }
    jam_handle_close(fs);
    return st;
}

/* fs.sync on path's mount: t is a deadline, or with `within` a timeout. */
static status_t sync_mount(const char *path, bool within, uint64_t t)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_INVALID_ARGS);
    if (st != OK)
        return st;
    st = within ? fs_sync_within(fs, t) : fs_sync_until(fs, t);
    jam_handle_close(fs);
    return st;
}

status_t fs_sync_by(const char *path, uint64_t deadline_ns)
{
    return sync_mount(path, false, deadline_ns);
}

status_t fs_sync(const char *path)
{
    return sync_mount(path, true, FS_CALL_TIMEOUT);
}

status_t fs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes, bool *read_only,
                   char label[17])
{
    uint8_t rel[FS_PATH_MAX], ro = 0, name[16];
    uint64_t tot = 0, fr = 0;
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_INVALID_ARGS);
    if (st != OK)
        return st;
    uint32_t magic, name_max, features;
    st = fs_statfs_within(fs, FS_CALL_TIMEOUT, &tot, &fr, &ro, name, &magic, &name_max,
                          &features);
    jam_handle_close(fs);
    if (st != OK)
        return st;
    if (total)
        *total = tot;
    if (free_bytes)
        *free_bytes = fr;
    if (read_only)
        *read_only = ro != 0;
    if (label) {
        memcpy(label, name, 16);
        label[16] = '\0';
    }
    return OK;
}

status_t fs_statfs_kind(const char *path, uint32_t *magic, uint32_t *name_max,
                        uint32_t *features)
{
    uint8_t rel[FS_PATH_MAX], ro = 0, name[16];
    uint64_t tot = 0, fr = 0;
    uint32_t m = 0, n = 0, f = 0;
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_INVALID_ARGS);
    if (st != OK)
        return st;
    st = fs_statfs_within(fs, FS_CALL_TIMEOUT, &tot, &fr, &ro, name, &m, &n, &f);
    jam_handle_close(fs);
    if (st != OK)
        return st;
    if (magic)
        *magic = m;
    if (name_max)
        *name_max = n;
    if (features)
        *features = f;
    return OK;
}

/* ---- links, modes and views ----------------------------------------------------- */

status_t fs_getattr(const char *path, uint32_t flags, struct fs_attr *out)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = ns_resolve(path, rel, &fs, &mount);
    if (st != OK)
        return st;
    struct fs_attr a = { 0 };
    if (fs) {
        st = fs_getattr_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), flags,
                               &a.mode, &a.nlink, &a.uid, &a.gid, &a.size, &a.blocks, &a.id,
                               &a.atime_ns, &a.mtime_ns, &a.ctime_ns);
        jam_handle_close(fs);
    } else if (mount == NS_AT_SVC && rel[1]) {
        st = ERR_WRONG_TYPE;   /* a service is neither file nor directory */
    } else {   /* "/" and "/svc": directories the namespace makes up */
        a = (struct fs_attr){ .mode = FS_MODE_DIR | 0555, .nlink = 1, .uid = FS_ONE_USER,
                              .gid = FS_ONE_USER };
    }
    if (st == OK)
        *out = a;
    return st;
}

status_t fs_setattr(const char *path, uint32_t flags, const struct fs_attr_set *set)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_ACCESS_DENIED);
    if (st != OK)
        return st;
    st = fs_setattr_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), flags,
                           set->mask, set->mode, set->uid, set->gid, set->atime_ns,
                           set->mtime_ns);
    jam_handle_close(fs);
    return st;
}

status_t fs_symlink(const char *target, const char *path)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    size_t n = strlen(target);
    status_t st = on_mount(path, rel, &fs, &mount, ERR_ALREADY_EXISTS);
    if (st != OK)
        return st;
    st = n > FS_PATH_LEN ? ERR_NAME_TOO_LONG
                         : fs_symlink_within(fs, FS_CALL_TIMEOUT, target, (uint32_t)n,
                                             (const char *)rel, rel_len(rel));
    jam_handle_close(fs);
    return st;
}

status_t fs_readlink(const char *path, char *buf, size_t cap)
{
    uint8_t rel[FS_PATH_MAX];
    char target[FS_PATH_LEN + 1];
    uint32_t len = 0;
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_INVALID_ARGS);   /* "/" is no link */
    if (st != OK)
        return st;
    st = fs_readlink_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), target, &len);
    jam_handle_close(fs);
    if (st == OK && len >= cap)
        st = ERR_BUFFER_TOO_SMALL;
    if (st == OK) {
        memcpy(buf, target, len);
        buf[len] = '\0';
    }
    return st;
}

status_t fs_link(const char *from, const char *to)
{
    uint8_t rel_from[FS_PATH_MAX], rel_to[FS_PATH_MAX];
    handle_t fs, fs_to;
    unsigned mount, mount_to;
    status_t st = on_mount(from, rel_from, &fs, &mount, ERR_ACCESS_DENIED);
    if (st != OK)
        return st;
    st = on_mount(to, rel_to, &fs_to, &mount_to, ERR_ALREADY_EXISTS);
    if (st == OK) {
        jam_handle_close(fs_to);
        st = mount == mount_to
                 ? fs_link_within(fs, FS_CALL_TIMEOUT, (const char *)rel_from, rel_len(rel_from),
                                  (const char *)rel_to, rel_len(rel_to))
                 : ERR_NOT_SUPPORTED;
    }
    jam_handle_close(fs);
    return st;
}

status_t fs_resolve(const char *path, uint32_t flags, uint8_t *kind, uint16_t *used,
                    char next[4096])
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    uint32_t len = 0;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_NOT_SUPPORTED);
    if (st != OK)
        return st;
    st = fs_resolve_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), flags, kind,
                           used, next, &len);
    jam_handle_close(fs);
    return st;
}

status_t fs_subview(const char *path, uint32_t flags, handle_t *out)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_NOT_SUPPORTED);
    if (st != OK)
        return st;
    st = fs_subview_within(fs, FS_CALL_TIMEOUT, (const char *)rel, rel_len(rel), flags, out);
    jam_handle_close(fs);
    return st;
}
