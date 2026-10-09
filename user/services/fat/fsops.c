/* fat: the `fs` protocol (abi/idl/fs.idl) on the volume. Every method
 * resolves its paths first (path.c), so FatFs only sees canonical paths
 * inside the volume, and every method that would change the volume is
 * refused with ERR_ACCESS_DENIED on a read-only one before FatFs is called.
 *
 * FatFs flushes its own state at the end of mkdir, unlink and rename; the
 * volume is then settled (marked clean, disk.c) unless an open file still
 * has unwritten changes. Each method runs as an operation (request.c): its
 * writes are held (hold.c) and go out together after its commit, before
 * it is answered, and so do the flush and the clean mark. An open file
 * can't be removed or renamed (ERR_BUSY: fat's lock, fileops.c). */
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

/* What is at p (resolved): FatFs's answer, and *is_dir. The root is a
 * directory (FatFs has no entry for it). */
static FRESULT kind_of(const char *p, bool *is_dir)
{
    FILINFO fi;
    *is_dir = true;
    if (path_is_root(p))
        return FR_OK;
    FRESULT fr = f_stat(p, &fi);
    *is_dir = fr == FR_OK && (fi.fattrib & AM_DIR);
    return fr;
}

static status_t op_open(void *ctx, const char *path, uint32_t path_len, uint32_t flags,
                        handle_t *out_file, handle_t *out_buffer, uint64_t *out_size)
{
    (void)ctx;
    if ((flags & ~FS_FLAGS) || !(flags & (FS_READ | FS_WRITE)))
        return ERR_INVALID_ARGS;
    if ((flags & (FS_CREATE | FS_TRUNCATE | FS_APPEND | FS_GATHER | FS_EXCL)) &&
        !(flags & FS_WRITE))
        return ERR_INVALID_ARGS;
    if ((flags & FS_EXCL) && !(flags & FS_CREATE))
        return ERR_INVALID_ARGS;
    if ((flags & FS_WRITE) && vol.read_only)
        return ERR_ACCESS_DENIED;
    char p[FAT_PATH_MAX];
    status_t st = path_resolve(path, path_len, p);
    if (st != OK)
        return st;
    if (path_is_root(p))
        return ERR_IS_DIR;
    return files_open(p, flags, out_file, out_buffer, out_size);
}

static status_t op_stat(void *ctx, const char *path, uint32_t path_len, uint64_t *out_size,
                        uint8_t *out_is_dir, uint64_t *out_mtime, uint64_t *out_id)
{
    (void)ctx;
    char p[FAT_PATH_MAX];
    FILINFO fi;
    uint64_t place = 0;
    status_t st = path_resolve(path, path_len, p);
    if (st != OK)
        return st;
    test_crash(p);
    if (path_is_root(p)) {   /* FatFs has no entry for the root */
        *out_size = 0;
        *out_is_dir = 1;
        *out_mtime = 0;
        *out_id = FAT_ROOT_ID;
        return OK;
    }
    st = fr_path_status(ffx_stat(p, &fi, &place), p);
    if (st != OK)
        return st;
    *out_is_dir = (fi.fattrib & AM_DIR) ? 1 : 0;
    *out_size = *out_is_dir ? 0 : fi.fsize;
    *out_mtime = fat_unix_time(fi.fdate, fi.ftime);
    *out_id = place;
    return OK;
}

/* From a cursor (dirs.c): a listing costs one entry read per entry. The
 * cookie is the index of the next entry. A disk error after the batch's
 * first entry ends the batch there (the next call gets it). */
static status_t op_readdir(void *ctx, const char *path, uint32_t path_len, uint32_t cookie,
                           uint32_t *out_next, uint16_t *out_count, uint8_t *out_entries,
                           uint32_t *out_entries_len)
{
    (void)ctx;
    char p[FAT_PATH_MAX];
    FILINFO fi;
    uint32_t len = 0;
    uint16_t count = 0;
    status_t st = path_resolve(path, path_len, p);
    *out_next = 0;
    for (uint32_t i = cookie; st == OK; i++) {
        uint64_t place = 0;
        status_t r = dirs_read(p, i, &fi, &place);
        if (r == ERR_NOT_FOUND && i == cookie) {
            /* Nothing at the cookie: the end, if p is a directory. */
            bool dir = false;
            st = fr_path_status(kind_of(p, &dir), p);
            if (st == OK && !dir)
                st = ERR_NOT_DIR;
            break;
        }
        if (r == ERR_NOT_FOUND)
            break;   /* the end */
        if (r != OK) {
            if (count)
                *out_next = i;   /* what was read; the error comes at the next call */
            else
                st = r;
            break;
        }
        name_shown(fi.fname);
        bool dir = fi.fattrib & AM_DIR;
        const struct fs_dirent e = {
            .id = place, .size = dir ? 0 : fi.fsize,
            .mtime = fat_unix_time(fi.fdate, fi.ftime), .is_dir = dir,
            .name_len = (uint32_t)strnlen(fi.fname, sizeof(fi.fname)), .name = fi.fname,
        };
        if (!fs_dirent_put(out_entries, FS_READDIR_BYTES, &len, &e)) {
            *out_next = i;   /* first in the next batch */
            break;
        }
        count++;
    }
    *out_count = count;
    *out_entries_len = len;
    return st;
}

static status_t op_mkdir(void *ctx, const char *path, uint32_t path_len)
{
    (void)ctx;
    char p[FAT_PATH_MAX];
    status_t st = writable();
    if (st == OK)
        st = path_resolve(path, path_len, p);
    if (st != OK)
        return st;
    if (path_is_root(p))
        return ERR_ALREADY_EXISTS;
    dirs_forget();
    FRESULT fr = f_mkdir(p);
    /* FR_DENIED: no free cluster, or no room in a fixed root directory. */
    return settled(fr == FR_DENIED ? ERR_NO_SPACE : fr_path_status(fr, p));
}

static status_t op_unlink(void *ctx, const char *path, uint32_t path_len, uint32_t flags)
{
    (void)ctx;
    char p[FAT_PATH_MAX];
    bool dir = false;
    status_t st = flags & ~FS_UNLINK_DIR ? ERR_INVALID_ARGS : writable();
    if (st == OK)
        st = path_resolve(path, path_len, p);
    if (st != OK)
        return st;
    if (path_is_root(p))
        return ERR_ACCESS_DENIED;
    st = fr_path_status(kind_of(p, &dir), p);
    if (st == OK && dir != !!(flags & FS_UNLINK_DIR))
        st = dir ? ERR_IS_DIR : ERR_NOT_DIR;   /* not the kind the caller meant */
    if (st != OK)
        return st;
    dirs_forget();
    /* Held (hold.c) and sent together after the commit, before the answer:
     * freeing a file changes a FAT sector per 128 clusters in each FAT
     * copy, a write each otherwise (on the ESP's one-sector clusters, 200
     * for a boot image). The directory entry still goes out first, as
     * FatFs wrote it. A send that fails is the answer (request.c). */
    FRESULT fr = files_is_open(p) ? FR_LOCKED : f_unlink(p);   /* fat's lock (fileops.c) */
    /* FR_DENIED: a directory that is not empty, or a read-only file. */
    if (fr == FR_DENIED && dir)
        return ERR_NOT_EMPTY;
    return settled(fr_status(fr));
}

/* A replacing rename's first half: what is at pt goes, unless it is pf's
 * own entry (another case of its name, or its 8.3 alias), or nothing is
 * there. A file replaces a file, a directory an empty directory. Both
 * halves are one request, so its writes go out together after its commit
 * (request.c): a crash never leaves `to` gone and `from` not renamed (but
 * a power cut in the middle of their going out can, as FAT has no
 * journal). */
static status_t make_way(const char *pf, const char *pt)
{
    FILINFO ff, ft;
    uint64_t place_f = 0, place_t = 0;
    FRESULT fr = ffx_stat(pt, &ft, &place_t);
    if (fr != FR_OK)
        return fr == FR_NO_FILE || fr == FR_NO_PATH ? OK : fr_status(fr);   /* f_rename says */
    fr = ffx_stat(pf, &ff, &place_f);
    if (fr != FR_OK)
        return fr_path_status(fr, pf);
    if (place_f == place_t)
        return OK;   /* the same entry: a rename of its name only */
    bool from_dir = ff.fattrib & AM_DIR, to_dir = ft.fattrib & AM_DIR;
    if (from_dir != to_dir)
        return to_dir ? ERR_IS_DIR : ERR_NOT_DIR;
    if (files_is_open(pt))
        return ERR_BUSY;
    fr = f_unlink(pt);
    /* FR_DENIED: a directory with entries, or a read-only file. */
    if (fr == FR_DENIED)
        return to_dir ? ERR_NOT_EMPTY : ERR_ACCESS_DENIED;
    return fr_status(fr);
}

/* Is `to` (resolved) inside the directory whose entry is at place_dir:
 * does any directory on its way have that entry? Told by the entries, not
 * the names: FatFs finds a directory by its 8.3 alias ("LONGDI~1") or any
 * case of its name as well, and a path string can't say. to is cut at
 * each '/' in turn and put back. */
static bool inside_entry(char *to, uint64_t place_dir)
{
    for (size_t i = 1; to[i]; i++) {
        if (to[i] != '/')
            continue;
        FILINFO fi;
        uint64_t place = 0;
        to[i] = '\0';
        FRESULT fr = ffx_stat(to, &fi, &place);
        to[i] = '/';
        if (fr == FR_OK && place == place_dir)
            return true;
    }
    return false;
}

static status_t op_rename(void *ctx, const char *from, uint32_t from_len, const char *to,
                          uint32_t to_len, uint32_t flags)
{
    (void)ctx;
    char pf[FAT_PATH_MAX], pt[FAT_PATH_MAX];
    status_t st = flags & ~FS_RENAME_REPLACE ? ERR_INVALID_ARGS : writable();
    if (st == OK)
        st = path_resolve(from, from_len, pf);
    if (st == OK)
        st = path_resolve(to, to_len, pt);
    if (st != OK)
        return st;
    /* FatFs would move a directory into itself and lose it: a loop no
     * path reaches. Into itself by any of its names (its 8.3 alias, another
     * case): by its entry. */
    if (path_is_root(pf) || path_is_root(pt))
        return ERR_INVALID_ARGS;
    FILINFO ff;
    uint64_t place_f = 0;
    if (path_inside(pt, pf) ||
        (ffx_stat(pf, &ff, &place_f) == FR_OK && (ff.fattrib & AM_DIR) &&
         inside_entry(pt, place_f))) {
        bool from_dir = false;
        if (kind_of(pf, &from_dir) == FR_OK && from_dir)
            return ERR_INVALID_ARGS;   /* (a file "into itself": FatFs finds no path) */
    }
    if (files_is_open(pf))
        return ERR_BUSY;   /* fat's lock (fileops.c) */
    dirs_forget();
    if (flags & FS_RENAME_REPLACE) {
        st = make_way(pf, pt);
        if (st != OK)
            return settled(st);
    }
    FRESULT fr = f_rename(pf, pt);
    /* FR_DENIED: no room for the new entry. FR_NO_PATH: on either path. */
    if (fr == FR_NO_PATH) {
        status_t sf = fr_path_status(fr, pf);
        return sf == ERR_NOT_DIR ? sf : fr_path_status(fr, pt);
    }
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
