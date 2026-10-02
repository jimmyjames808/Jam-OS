/* fat: the `fs` protocol (abi/idl/fs.idl) on the volume. Every method
 * resolves its paths first (path.c), so FatFs only sees canonical paths
 * inside the volume, and every method that would change the volume is
 * refused with ERR_ACCESS_DENIED on a read-only one before FatFs is called.
 *
 * FatFs flushes its own state at the end of mkdir, unlink and rename; the
 * volume is then settled (marked clean, disk.c) unless an open file still
 * has unwritten changes. */
#include "fat.h"

/* A changing method on a read-only volume. */
static status_t writable(void)
{
    return vol.read_only ? ERR_ACCESS_DENIED : OK;
}

/* After a method that changed the volume and let FatFs flush it. */
static status_t settled(status_t st)
{
    if (st != OK || files_unsynced())
        return st;
    return disk_settle(false);
}

static bool is_dir(const char *path)
{
    FILINFO fi;
    return path_is_root(path) || (f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR));
}

static status_t op_open(void *ctx, const uint8_t path[256], uint32_t flags, handle_t *out_file,
                        handle_t *out_buffer, uint64_t *out_size)
{
    (void)ctx;
    const uint32_t known = FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_APPEND | FS_GATHER;
    if ((flags & ~known) || !(flags & (FS_READ | FS_WRITE)))
        return ERR_INVALID_ARGS;
    if ((flags & (FS_CREATE | FS_TRUNCATE | FS_APPEND | FS_GATHER)) && !(flags & FS_WRITE))
        return ERR_INVALID_ARGS;
    if ((flags & FS_WRITE) && vol.read_only)
        return ERR_ACCESS_DENIED;
    char p[FS_PATH_MAX];
    status_t st = path_resolve(path, p);
    if (st != OK)
        return st;
    if (path_is_root(p))
        return ERR_WRONG_TYPE;
    return files_open(p, flags, out_file, out_buffer, out_size);
}

static status_t op_stat(void *ctx, const uint8_t path[256], uint64_t *out_size,
                        uint8_t *out_is_dir, uint64_t *out_mtime)
{
    (void)ctx;
    char p[FS_PATH_MAX];
    FILINFO fi;
    status_t st = path_resolve(path, p);
    if (st != OK)
        return st;
    if (path_is_root(p)) {   /* FatFs has no entry for the root */
        *out_size = 0;
        *out_is_dir = 1;
        *out_mtime = 0;
        return OK;
    }
    st = fr_status(f_stat(p, &fi));
    if (st != OK)
        return st;
    *out_is_dir = (fi.fattrib & AM_DIR) ? 1 : 0;
    *out_size = *out_is_dir ? 0 : fi.fsize;
    *out_mtime = fat_unix_time(fi.fdate, fi.ftime);
    return OK;
}

/* From a cursor (dirs.c): a listing costs one entry read per entry. */
static status_t op_readdir(void *ctx, const uint8_t path[256], uint32_t index,
                           uint8_t out_name[256], uint8_t *out_is_dir, uint64_t *out_size)
{
    (void)ctx;
    char p[FS_PATH_MAX];
    FILINFO fi;
    status_t st = path_resolve(path, p);
    if (st == OK)
        st = dirs_read(p, index, &fi);
    if (st != OK)
        return st;
    name_shown(fi.fname);
    size_t n = strnlen(fi.fname, FS_PATH_MAX - 1);
    memcpy(out_name, fi.fname, n);   /* out_name came zeroed */
    *out_is_dir = (fi.fattrib & AM_DIR) ? 1 : 0;
    *out_size = *out_is_dir ? 0 : fi.fsize;
    return OK;
}

static status_t op_mkdir(void *ctx, const uint8_t path[256])
{
    (void)ctx;
    char p[FS_PATH_MAX];
    status_t st = writable();
    if (st == OK)
        st = path_resolve(path, p);
    if (st != OK)
        return st;
    if (path_is_root(p))
        return ERR_ALREADY_EXISTS;
    dirs_forget();
    FRESULT fr = f_mkdir(p);
    /* FR_DENIED: no free cluster, or no room in a fixed root directory. */
    return settled(fr == FR_DENIED ? ERR_NO_SPACE : fr_status(fr));
}

static status_t op_unlink(void *ctx, const uint8_t path[256])
{
    (void)ctx;
    char p[FS_PATH_MAX];
    status_t st = writable();
    if (st == OK)
        st = path_resolve(path, p);
    if (st != OK)
        return st;
    if (path_is_root(p))
        return ERR_ACCESS_DENIED;
    dirs_forget();
    FRESULT fr = f_unlink(p);
    /* FR_DENIED: a directory that is not empty, or a read-only file. */
    if (fr == FR_DENIED && is_dir(p))
        return ERR_BAD_STATE;
    return settled(fr_status(fr));
}

static status_t op_rename(void *ctx, const uint8_t from[256], const uint8_t to[256])
{
    (void)ctx;
    char pf[FS_PATH_MAX], pt[FS_PATH_MAX];
    status_t st = writable();
    if (st == OK)
        st = path_resolve(from, pf);
    if (st == OK)
        st = path_resolve(to, pt);
    if (st != OK)
        return st;
    /* FatFs would move a directory into itself and lose it: a loop no
     * path reaches. */
    if (path_is_root(pf) || path_is_root(pt) || path_inside(pt, pf))
        return ERR_INVALID_ARGS;
    dirs_forget();
    FRESULT fr = f_rename(pf, pt);
    /* FR_DENIED: no room for the new entry. */
    return settled(fr == FR_DENIED ? ERR_NO_SPACE : fr_status(fr));
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    if (vol.read_only)
        return OK;
    status_t st = files_sync_all();
    status_t st2 = files_unsynced() ? OK : disk_settle(true);
    return st != OK ? st : st2;
}

static status_t op_statfs(void *ctx, uint64_t *out_total, uint64_t *out_free,
                          uint8_t *out_read_only, uint8_t out_label[16])
{
    (void)ctx;
    DWORD nfree = 0;
    FATFS *fs = NULL;
    char label[40];   /* 11 characters, each up to 3 bytes of UTF-8 */
    FRESULT fr = f_getfree("", &nfree, &fs);
    if (fr == FR_OK)
        fr = f_getlabel("", label, NULL);
    if (fr != FR_OK)
        return fr_status(fr);
    uint64_t cluster = (uint64_t)fs->csize * FAT_SECTOR;
    *out_total = (uint64_t)(fs->n_fatent - 2) * cluster;
    *out_free = (uint64_t)nfree * cluster;
    *out_read_only = vol.read_only ? 1 : 0;
    name_shown(label);
    size_t n = strnlen(label, 15);
    while (n && ((unsigned char)label[n] & 0xc0) == 0x80)
        n--;   /* not in the middle of a character */
    memcpy(out_label, label, n);   /* out_label came zeroed */
    return OK;
}

const struct fs_ops fat_fs_ops = {
    .open = op_open, .stat = op_stat, .readdir = op_readdir, .mkdir = op_mkdir,
    .unlink = op_unlink, .rename = op_rename, .sync = op_sync, .statfs = op_statfs,
};
