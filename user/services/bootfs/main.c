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

static status_t op_open(void *ctx, const uint8_t path[256], uint32_t flags, handle_t *out_file,
                        handle_t *out_buffer, uint64_t *out_size)
{
    char rel[FS_PATH_MAX];
    const struct bootfs_entry *e;
    struct fsserver_file *f;
    if ((flags & ~FS_FLAGS) || !(flags & (FS_READ | FS_WRITE)) || fs_path_clean(path, rel) != OK)
        return ERR_INVALID_ARGS;
    if ((flags & (FS_CREATE | FS_TRUNCATE | FS_APPEND)) && !(flags & FS_WRITE))
        return ERR_INVALID_ARGS;
    if (flags & FS_WRITE)
        return ERR_ACCESS_DENIED;
    status_t st = look(rel, &e);
    if (st != OK)
        return st;
    if (!e)
        return ERR_WRONG_TYPE;   /* a directory */
    /* The entry is the file's state: nothing to release when it closes. */
    st = fsserver_open(ctx, (void *)(uintptr_t)e, false, out_file, out_buffer, &f);
    if (st == OK)
        *out_size = e->size;
    return st;
}

static status_t op_stat(void *ctx, const uint8_t path[256], uint64_t *out_size,
                        uint8_t *out_is_dir, uint64_t *out_mtime)
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    const struct bootfs_entry *e;
    status_t st = fs_path_clean(path, rel);
    if (st == OK)
        st = look(rel, &e);
    if (st != OK)
        return st;
    *out_size = e ? e->size : 0;
    *out_is_dir = !e;
    *out_mtime = 0;   /* the image keeps no times */
    return OK;
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

static status_t op_readdir(void *ctx, const uint8_t path[256], uint32_t index,
                           uint8_t out_name[256], uint8_t *out_is_dir, uint64_t *out_size)
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    const struct bootfs_entry *e;
    status_t st = fs_path_clean(path, rel);
    if (st == OK)
        st = look(rel, &e);
    if (st != OK)
        return st;
    if (e)
        return ERR_WRONG_TYPE;   /* a file */
    uint32_t seen = 0;
    for (uint32_t i = 0; i < image->count; i++) {
        const char *name = name_of(i), *rest = name ? inside(name, rel) : NULL;
        if (!rest)
            continue;
        const char *slash = strchr(rest, '/');
        size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (!first_child(i, rel, rest, len) || seen++ != index)
            continue;
        memcpy(out_name, rest, len);   /* out_name is zeroed: shorter than BOOTFS_NAME_MAX */
        *out_is_dir = slash != NULL;
        *out_size = slash ? 0 : entries[i].size;
        return OK;
    }
    return ERR_NOT_FOUND;
}

/* mkdir and unlink: nothing here can change. */
static status_t op_read_only(void *ctx, const uint8_t path[256])
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    return fs_path_clean(path, rel) == OK ? ERR_ACCESS_DENIED : ERR_INVALID_ARGS;
}

static status_t op_rename(void *ctx, const uint8_t from[256], const uint8_t to[256])
{
    status_t st = op_read_only(ctx, from);
    return st == ERR_ACCESS_DENIED ? op_read_only(ctx, to) : st;
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    return OK;   /* nothing is ever written */
}

static status_t op_statfs(void *ctx, uint64_t *out_total, uint64_t *out_free,
                          uint8_t *out_read_only, uint8_t out_label[16])
{
    (void)ctx;
    *out_total = image->size;
    *out_free = 0;
    *out_read_only = 1;
    memcpy(out_label, "BOOTFS", 7);
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

static status_t file_op_stat(void *ctx, uint64_t *out_size, uint64_t *out_mtime)
{
    struct fsserver_file *f = ctx;
    *out_size = ((const struct bootfs_entry *)f->ctx)->size;
    *out_mtime = 0;
    return OK;
}

static const struct fs_ops fs_ops = {
    .open = op_open, .stat = op_stat, .readdir = op_readdir, .mkdir = op_read_only,
    .unlink = op_read_only, .rename = op_rename, .sync = op_sync, .statfs = op_statfs,
};

static const struct file_ops file_ops = {
    .read = file_op_read, .write = file_op_write, .truncate = file_op_truncate,
    .stat = file_op_stat, .sync = op_sync,
};

static struct fsserver server = { .fs_ops = &fs_ops, .file_ops = &file_ops };

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t ch = startup_handle(SR_USER + 0);
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
