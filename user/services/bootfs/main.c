/* bootfs: the boot image served as a mount (init mounts it at /boot).
 *
 * The image (SR_BOOTFS, <jam/bootfs.h>) is a flat table of names like
 * "bin/shell" with their bytes; this program serves it through the `fs`
 * and `file` protocols (abi/idl/fs.idl, file.idl), so /boot is a mount
 * like any other. Directories are implied by the '/' in the names: "bin"
 * is a directory because some file is called "bin/...".
 *
 * Read-only: everything that would change it is ERR_ACCESS_DENIED, and the
 * transfer buffers it hands out can't be written by their clients.
 *
 * Startup handles: SR_BOOTFS (the image), SR_USER + 0 (the server end of
 * the mount's `fs` channel). Exits 0 when its last client is gone. The
 * loop is libos's (<fsserver.h>). */
#include <fsserver.h>
#include <jam/bootfs.h>

static const struct bootfs_view *image;      /* the mapped image */
static const struct bootfs_entry *entries;   /* its table: image->count entries */

/* Entry i's name if it is usable: NUL-terminated, its bytes inside the
 * image. NULL otherwise (the kernel checked the image at boot; this only
 * keeps a lookup from pointing outside the mapping). */
static const char *name_of(uint32_t i)
{
    const struct bootfs_entry *e = &entries[i];
    if (strnlen(e->name, BOOTFS_NAME_MAX) == BOOTFS_NAME_MAX || e->offset > image->size ||
        e->size > image->size - e->offset)
        return NULL;
    return e->name;
}

/* name is inside directory dir ("" is the root): the rest of it after the
 * directory, else NULL. */
static const char *inside(const char *name, const char *dir)
{
    size_t l = strlen(dir);
    if (!l)
        return name;
    return !strncmp(name, dir, l) && name[l] == '/' ? name + l + 1 : NULL;
}

/* What the cleaned path rel names: OK and *file (NULL: a directory), or
 * ERR_NOT_FOUND. */
static status_t look(const char *rel, const struct bootfs_entry **file)
{
    *file = NULL;
    if (!rel[0])
        return OK;
    status_t st = ERR_NOT_FOUND;
    for (uint32_t i = 0; i < image->count; i++) {
        const char *name = name_of(i);
        if (!name)
            continue;
        if (!strcmp(name, rel)) {
            *file = &entries[i];
            return OK;
        }
        if (inside(name, rel))
            st = OK;
    }
    return st;
}

/* ---- fs ---------------------------------------------------------------------------- */

/* A path as the protocol carries it, cleaned into rel (FS_PATH_LEN + 1
 * bytes). */
static status_t clean_path(const char *path, uint32_t len, char *rel)
{
    return fs_path_clean(path, len, rel) == OK ? OK : ERR_INVALID_ARGS;
}

/* Ids (fs.stat): a file is its entry's index + 1; a directory, implied by
 * the names, is known by the first entry inside it; the root has its own. */
#define ID_DIR  (1ull << 32)
#define ID_ROOT (2ull << 32)

/* The directory rel's id: the first entry inside it (it has one, or look
 * would not have found it). */
static uint64_t dir_id(const char *rel)
{
    if (!rel[0])
        return ID_ROOT;
    for (uint32_t i = 0; i < image->count; i++) {
        const char *name = name_of(i);
        if (name && inside(name, rel))
            return ID_DIR + i;
    }
    return ID_ROOT;
}

#define STATFS_MAGIC    0x858458f6u   /* statfs's magic: Linux's RAMFS_MAGIC */
#define STATFS_NAME_MAX 255u          /* what statfs says a name holds (as libc did) */

/* The mode made up for an entry (fs.idl's getattr): a directory 0555, a file
 * 0444, 0555 for a program (a file directly in a directory named bin or
 * sbin): libc's old rule, with the write bits off as the image is read-only. */
static uint32_t made_mode(const struct bootfs_entry *e)
{
    if (!e)
        return FS_MODE_DIR | 0555u;
    const char *slash = strrchr(e->name, '/'), *dir = slash;
    while (dir && dir > e->name && dir[-1] != '/')
        dir--;
    size_t l = slash ? (size_t)(slash - dir) : 0;
    bool program = (l == 3 && !memcmp(dir, "bin", 3)) || (l == 4 && !memcmp(dir, "sbin", 4));
    return FS_MODE_REG | (program ? 0555u : 0444u);
}

/* `mode` and FS_NOFOLLOW mean nothing to a read-only image without links. */
static status_t op_open(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                        uint32_t mode, handle_t *out_file, handle_t *out_buffer,
                        uint64_t *out_size)
{
    (void)mode;
    char rel[FS_PATH_LEN + 1];
    const struct bootfs_entry *e;
    struct fsserver_file *f;
    if ((flags & ~FS_FLAGS) || !(flags & (FS_READ | FS_WRITE)) ||
        clean_path(path, path_len, rel) != OK)
        return ERR_INVALID_ARGS;
    if ((flags & (FS_CREATE | FS_TRUNCATE | FS_APPEND | FS_GATHER | FS_EXCL)) &&
        !(flags & FS_WRITE))
        return ERR_INVALID_ARGS;
    if (flags & FS_WRITE)
        return ERR_ACCESS_DENIED;
    status_t st = look(rel, &e);
    if (st != OK)
        return st;
    if (!e)
        return ERR_IS_DIR;
    if (flags & FS_DIRECTORY)
        return ERR_NOT_DIR;
    /* The entry is the file's state: nothing to release when it closes. */
    st = fsserver_open(ctx, (void *)(uintptr_t)e, false, out_file, out_buffer, &f);
    if (st == OK)
        *out_size = e->size;
    return st;
}

static status_t op_stat(void *ctx, const char *path, uint32_t path_len, uint64_t *out_size,
                        uint8_t *out_is_dir, uint64_t *out_mtime, uint64_t *out_id)
{
    (void)ctx;
    char rel[FS_PATH_LEN + 1];
    const struct bootfs_entry *e;
    status_t st = clean_path(path, path_len, rel);
    if (st == OK)
        st = look(rel, &e);
    if (st != OK)
        return st;
    *out_size = e ? e->size : 0;
    *out_is_dir = !e;
    *out_mtime = 0;   /* the image keeps no times */
    *out_id = e ? (uint64_t)(e - entries) + 1 : dir_id(rel);
    return OK;
}

static status_t op_getattr(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                           uint32_t *out_mode, uint32_t *out_nlink, uint32_t *out_uid,
                           uint32_t *out_gid, uint64_t *out_size, uint64_t *out_blocks,
                           uint64_t *out_id, int64_t *out_atime_ns, int64_t *out_mtime_ns,
                           int64_t *out_ctime_ns)
{
    (void)ctx;
    char rel[FS_PATH_LEN + 1];
    const struct bootfs_entry *e;
    if (flags & ~FS_NOFOLLOW)
        return ERR_INVALID_ARGS;
    status_t st = clean_path(path, path_len, rel);
    if (st == OK)
        st = look(rel, &e);
    if (st != OK)
        return st;
    *out_mode = made_mode(e);
    *out_nlink = 1;
    *out_uid = FS_ONE_USER;
    *out_gid = FS_ONE_USER;
    *out_size = e ? e->size : 0;
    *out_blocks = e ? (e->size + 511) / 512 : 0;
    *out_id = e ? (uint64_t)(e - entries) + 1 : dir_id(rel);
    *out_atime_ns = *out_mtime_ns = *out_ctime_ns = 0;   /* the image keeps no times */
    return OK;
}

/* Read-only: nothing to set, for a path that is there. */
static status_t op_setattr(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                           uint32_t mask, uint32_t mode, uint32_t uid, uint32_t gid,
                           int64_t atime_ns, int64_t mtime_ns)
{
    (void)ctx;
    (void)mode;
    (void)uid;
    (void)gid;
    (void)atime_ns;
    (void)mtime_ns;
    char rel[FS_PATH_LEN + 1];
    const struct bootfs_entry *e;
    if ((flags & ~FS_NOFOLLOW) || (mask & ~FS_ATTR_MASK))
        return ERR_INVALID_ARGS;
    status_t st = clean_path(path, path_len, rel);
    if (st == OK)
        st = look(rel, &e);
    return st != OK ? st : ERR_ACCESS_DENIED;
}

/* Entry i is the first with this name of len bytes directly in dir. */
static bool first_child(uint32_t i, const char *dir, const char *child, size_t len)
{
    for (uint32_t k = 0; k < i; k++) {
        const char *name = name_of(k), *rest = name ? inside(name, dir) : NULL;
        if (rest && !strncmp(rest, child, len) && (rest[len] == '/' || !rest[len]))
            return false;
    }
    return true;
}

/* The cookie is the number of the directory's entries before the next
 * one, counted as a walk of the table finds them. */
static status_t op_readdir(void *ctx, const char *path, uint32_t path_len, uint32_t cookie,
                           uint32_t *out_next, uint16_t *out_count, uint8_t *out_entries,
                           uint32_t *out_entries_len)
{
    (void)ctx;
    char rel[FS_PATH_LEN + 1];
    const struct bootfs_entry *e;
    status_t st = clean_path(path, path_len, rel);
    if (st == OK)
        st = look(rel, &e);
    if (st != OK)
        return st;
    if (e)
        return ERR_NOT_DIR;
    uint32_t seen = 0, len = 0;
    uint16_t count = 0;
    *out_next = 0;
    for (uint32_t i = 0; i < image->count; i++) {
        const char *name = name_of(i), *rest = name ? inside(name, rel) : NULL;
        if (!rest)
            continue;
        const char *slash = strchr(rest, '/');
        size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
        if (!first_child(i, rel, rest, n) || seen++ < cookie)
            continue;
        char sub[BOOTFS_NAME_MAX];   /* the subdirectory's path: shorter than name */
        memcpy(sub, name, (size_t)(rest - name) + n);
        sub[(rest - name) + n] = '\0';
        const struct fs_dirent d = {
            .id = slash ? dir_id(sub) : (uint64_t)i + 1, .size = slash ? 0 : entries[i].size,
            .is_dir = slash != NULL, .type = slash ? FS_DT_DIR : FS_DT_REG,
            .name_len = (uint32_t)n, .name = rest,
        };
        if (!fs_dirent_put(out_entries, FS_READDIR_BYTES, &len, &d)) {
            *out_next = seen - 1;   /* it goes first in the next batch */
            break;
        }
        count++;
    }
    *out_count = count;
    *out_entries_len = len;
    return OK;
}

/* mkdir, unlink and rename: nothing here can change. */
static status_t refuse(const char *path, uint32_t path_len)
{
    char rel[FS_PATH_LEN + 1];
    return clean_path(path, path_len, rel) == OK ? ERR_ACCESS_DENIED : ERR_INVALID_ARGS;
}

static status_t op_mkdir(void *ctx, const char *path, uint32_t path_len, uint32_t mode)
{
    (void)ctx;
    (void)mode;
    return refuse(path, path_len);
}

static status_t op_unlink(void *ctx, const char *path, uint32_t path_len, uint32_t flags)
{
    (void)ctx;
    return flags & ~FS_UNLINK_DIR ? ERR_INVALID_ARGS : refuse(path, path_len);
}

static status_t op_rename(void *ctx, const char *from, uint32_t from_len, const char *to,
                          uint32_t to_len, uint32_t flags)
{
    (void)ctx;
    if (flags & ~(FS_RENAME_REPLACE | FS_RENAME_EXCHANGE))
        return ERR_INVALID_ARGS;
    if (flags & FS_RENAME_EXCHANGE)   /* as fat's: not a thing this filesystem does */
        return (flags & FS_RENAME_REPLACE) ? ERR_INVALID_ARGS : ERR_NOT_SUPPORTED;
    status_t st = refuse(from, from_len);
    return st == ERR_ACCESS_DENIED ? refuse(to, to_len) : st;
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    return OK;   /* nothing is ever written */
}

static status_t op_statfs(void *ctx, uint64_t *out_total, uint64_t *out_free,
                          uint8_t *out_read_only, uint8_t out_label[16], uint32_t *out_magic,
                          uint32_t *out_name_max, uint32_t *out_features)
{
    (void)ctx;
    *out_total = image->size;
    *out_free = 0;
    *out_read_only = 1;
    memcpy(out_label, "BOOTFS", 7);
    *out_magic = STATFS_MAGIC;
    *out_name_max = STATFS_NAME_MAX;
    *out_features = 0;
    return OK;
}

/* ---- file -------------------------------------------------------------------------- */

static status_t file_op_read(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct fsserver_file *f = ctx;
    const struct bootfs_entry *e = f->ctx;
    if (length > FSSERVER_BUF_SIZE)
        return ERR_INVALID_ARGS;
    uint64_t left = offset < e->size ? e->size - offset : 0;
    uint32_t n = left < length ? (uint32_t)left : length;
    status_t st = n ? jam_vmo_write(f->buf, 0, image->base + e->offset + offset, n) : OK;
    if (st == OK)
        *out_actual = n;
    return st;
}

static status_t file_op_write(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    (void)ctx;
    (void)offset;
    (void)length;
    (void)out_actual;
    return ERR_ACCESS_DENIED;
}

static status_t file_op_truncate(void *ctx, uint64_t size)
{
    (void)ctx;
    (void)size;
    return ERR_ACCESS_DENIED;
}

static status_t file_op_stat(void *ctx, uint64_t *out_size, uint8_t *out_is_dir,
                             uint64_t *out_mtime, uint64_t *out_id)
{
    struct fsserver_file *f = ctx;
    const struct bootfs_entry *e = f->ctx;
    *out_size = e->size;
    *out_is_dir = 0;
    *out_mtime = 0;
    *out_id = (uint64_t)(e - entries) + 1;
    return OK;
}

/* The file's attributes: those of its path (a file's entry is its state). */
static status_t file_op_getattr(void *ctx, uint32_t *out_mode, uint32_t *out_nlink,
                                uint32_t *out_uid, uint32_t *out_gid, uint64_t *out_size,
                                uint64_t *out_blocks, uint64_t *out_id, int64_t *out_atime_ns,
                                int64_t *out_mtime_ns, int64_t *out_ctime_ns)
{
    struct fsserver_file *f = ctx;
    const struct bootfs_entry *e = f->ctx;
    *out_mode = made_mode(e);
    *out_nlink = 1;
    *out_uid = FS_ONE_USER;
    *out_gid = FS_ONE_USER;
    *out_size = e->size;
    *out_blocks = (e->size + 511) / 512;
    *out_id = (uint64_t)(e - entries) + 1;
    *out_atime_ns = *out_mtime_ns = *out_ctime_ns = 0;
    return OK;
}

static status_t file_op_setattr(void *ctx, uint32_t mask, uint32_t mode, uint32_t uid,
                                uint32_t gid, int64_t atime_ns, int64_t mtime_ns)
{
    (void)ctx;
    (void)mode;
    (void)uid;
    (void)gid;
    (void)atime_ns;
    (void)mtime_ns;
    return mask & ~FS_ATTR_MASK ? ERR_INVALID_ARGS : ERR_ACCESS_DENIED;
}

/* symlink, readlink, link, resolve and subview are left out: the generated
 * dispatch answers them ERR_NOT_SUPPORTED (there are no links here, and a
 * directory of the image is not served as a root of its own). */
static const struct fs_ops fs_ops = {
    .open = op_open, .stat = op_stat, .readdir = op_readdir, .mkdir = op_mkdir,
    .unlink = op_unlink, .rename = op_rename, .sync = op_sync, .statfs = op_statfs,
    .getattr = op_getattr, .setattr = op_setattr,
};

static const struct file_ops file_ops = {
    .read = file_op_read, .write = file_op_write, .truncate = file_op_truncate,
    .stat = file_op_stat, .sync = op_sync, .getattr = file_op_getattr,
    .setattr = file_op_setattr,
};

static struct fsserver server = { .fs_ops = &fs_ops, .file_ops = &file_ops };

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t ch = startup_handle(BOOTFS_SR_FS);
    status_t st = ch ? bootfs_default(&image) : ERR_BAD_HANDLE;
    if (st == OK)
        st = fsserver_init(&server);
    if (st == OK)
        st = fsserver_add_fs(&server, ch);
    if (st != OK) {
        printf("bootfs: can't serve the boot image (%s)\n", status_str(st));
        return 1;
    }
    entries = (const struct bootfs_entry *)(image->base + sizeof(struct bootfs_header));
    printf("bootfs: serving the boot image: %u files, %lu KiB\n", image->count,
           (unsigned long)(image->size >> 10));
    st = fsserver_run(&server);
    if (st != OK)
        printf("bootfs: %s\n", status_str(st));
    return st == OK ? 0 : 1;
}
