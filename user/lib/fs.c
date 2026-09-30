/* The file calls of <os.h>: each finds its path's mount in the namespace
 * (ns.c) and calls that mount's `fs` service, or an open file's `file`
 * channel (abi/idl/fs.idl, file.idl).
 *
 * File data moves through the buffer VMO fs.open hands out with the file's
 * channel, mapped here for as long as the file is open: a read has the
 * service fill it and copies out, a write copies in and has the service
 * take it. Transfers larger than the buffer are split. Every call has a
 * deadline (FS_CALL_TIMEOUT), and a service that died closes its channels,
 * so none of these can hang on a broken mount. */
#include <fs_idl.h>
#include "ns.h"

#define BUF_MAX (1u << 20)   /* a bigger transfer buffer than this is a broken service */

static uint64_t deadline(void)
{
    return now() + FS_CALL_TIMEOUT;
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
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    if (flags & ~FS_FLAGS)
        return ERR_INVALID_ARGS;
    status_t st = ns_resolve(path, rel, &fs, &mount);
    if (st != OK)
        return st;
    if (!fs)
        return ERR_WRONG_TYPE;   /* "/" is a directory */
    struct jfile f = { 0 };
    st = fs_open_until(fs, deadline(), rel, flags, &f.ch, &f.buf_vmo, &f.size);
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
        status_t st = file_read_until(f->ch, deadline(), offset + got, want, &actual);
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
        status_t st = file_write_until(f->ch, deadline(), offset + put, want, &actual);
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
    return file_sync_until(f->ch, deadline());
}

status_t file_stat(struct jfile *f, uint64_t *size, uint64_t *mtime)
{
    return file_stat_until(f->ch, deadline(), size, mtime);
}

status_t file_truncate(struct jfile *f, uint64_t size)
{
    return file_truncate_until(f->ch, deadline(), size);
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
        st = jam_vmo_create(pages ? pages : PAGE_SIZE, 0, HANDLE_INVALID, &v);
    }
    while (st == OK && off < total) {
        uint32_t want = total - off < f.buf_size ? (uint32_t)(total - off) : f.buf_size;
        uint32_t actual = 0;
        st = file_read_until(f.ch, deadline(), off, want, &actual);
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
    if (st == OK && fs) {   /* else "/": a directory */
        st = fs_stat_until(fs, deadline(), rel, &sz, &dir, &mt);
        jam_handle_close(fs);
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

status_t fs_readdir(const char *path, uint32_t index, struct fs_entry *out)
{
    uint8_t rel[FS_PATH_MAX], dir = 0;
    uint64_t size = 0;
    handle_t fs;
    unsigned mount;
    status_t st = ns_resolve(path, rel, &fs, &mount);
    if (st != OK)
        return st;
    memset(out, 0, sizeof(*out));
    if (!fs) {   /* "/": the mount points */
        char point[NS_NAME_MAX];
        if (!ns_mount_at(index, point))
            return ERR_NOT_FOUND;
        memcpy(out->name, point + 1, strlen(point + 1) + 1);
        out->is_dir = true;
        return OK;
    }
    st = fs_readdir_until(fs, deadline(), rel, index, (uint8_t *)out->name, &dir, &size);
    jam_handle_close(fs);
    if (st == OK && out->name[FS_PATH_MAX - 1])
        st = ERR_INTERNAL;   /* a name without its NUL */
    out->is_dir = dir != 0;
    out->size = size;
    return st;
}

status_t fs_mkdir(const char *path)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_ALREADY_EXISTS);
    if (st != OK)
        return st;
    st = fs_mkdir_until(fs, deadline(), rel);
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
    st = fs_unlink_until(fs, deadline(), rel);
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
        st = mount == mount_to ? fs_rename_until(fs, deadline(), rel_from, rel_to)
                               : ERR_NOT_SUPPORTED;
    }
    jam_handle_close(fs);
    return st;
}

status_t fs_sync_by(const char *path, uint64_t deadline_ns)
{
    uint8_t rel[FS_PATH_MAX];
    handle_t fs;
    unsigned mount;
    status_t st = on_mount(path, rel, &fs, &mount, ERR_INVALID_ARGS);
    if (st != OK)
        return st;
    st = fs_sync_until(fs, deadline_ns);
    jam_handle_close(fs);
    return st;
}

status_t fs_sync(const char *path)
{
    return fs_sync_by(path, deadline());
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
    st = fs_statfs_until(fs, deadline(), &tot, &fr, &ro, name);
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
