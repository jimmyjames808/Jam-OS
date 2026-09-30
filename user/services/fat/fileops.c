/* fat: the open files. Each fs.open takes a slot of files[]: FatFs's FIL,
 * a 64 KiB transfer buffer (a VMO; the client gets a handle to map it) and
 * a channel speaking the `file` protocol, bound to fat's port. The client
 * closing its end closes the file. The table's size (FAT_MAX_FILES) bounds
 * what clients can make fat hold: 64 KiB and two handles per slot.
 *
 * fat never maps a transfer buffer: the client's handle can shrink the VMO
 * (RIGHT_WRITE allows vmo_set_size), and a mapping of it would fault under
 * FatFs. Data goes between the VMO and one buffer of fat's own (bounce)
 * with vmo_read / vmo_write, which answer a shrunken buffer with an error.
 *
 * FatFs's f_lseek past the end of a file open for writing grows it with
 * whatever the clusters held before. Here a write or truncate past the end
 * writes zeros into the gap instead (grow_to), at most FAT_GROW_MAX per
 * call, and reads never seek past the end.
 *
 * A file is `unsynced` from its first change until its next f_sync; when
 * the last unsynced file is flushed the volume is settled (marked clean,
 * disk.c). */
#include "fat.h"

#define CLIENT_BUF_RIGHTS (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER)

static struct fat_file files[FAT_MAX_FILES];
static uint8_t bounce[FAT_FILE_BUF];   /* between FatFs and a file's buffer VMO */

bool files_unsynced(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++)
        if (files[i].used && files[i].unsynced)
            return true;
    return false;
}

/* Flush f; once nothing is left unsynced, settle the volume. */
static status_t sync_file(struct fat_file *f)
{
    status_t st = fr_status(f_sync(&f->fil));
    if (st != OK)
        return st;
    bool was = f->unsynced;
    f->unsynced = false;
    return was && !files_unsynced() ? disk_settle() : OK;
}

status_t files_sync_all(void)
{
    status_t st = OK;
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_file *f = &files[i];
        if (!f->used || !f->unsynced)
            continue;
        status_t s = fr_status(f_sync(&f->fil));
        if (s == OK)
            f->unsynced = false;
        else if (st == OK)
            st = s;
    }
    return st;
}

/* ---- the file protocol ---------------------------------------------------------------- */

/* Make f at least `size` bytes long, the new part zeros. */
static status_t grow_to(struct fat_file *f, uint64_t size)
{
    static const uint8_t zeros[4096];
    FSIZE_t cur = f_size(&f->fil);
    if (size <= cur)
        return OK;
    if (size - cur > FAT_GROW_MAX)
        return ERR_OUT_OF_RANGE;
    FRESULT fr = f_lseek(&f->fil, cur);
    while (fr == FR_OK && cur < size) {
        UINT n = size - cur < sizeof(zeros) ? (UINT)(size - cur) : (UINT)sizeof(zeros), put = 0;
        fr = f_write(&f->fil, zeros, n, &put);
        f->unsynced = true;
        if (fr == FR_OK && put < n)
            return ERR_NO_SPACE;
        cur += put;
    }
    return fr_status(fr);
}

static status_t op_read(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct fat_file *f = ctx;
    if (!(f->flags & FS_READ))
        return ERR_ACCESS_DENIED;
    if (length > FAT_FILE_BUF)
        return ERR_INVALID_ARGS;
    UINT got = 0;
    if (length && offset < f_size(&f->fil)) {
        FRESULT fr = f_lseek(&f->fil, (FSIZE_t)offset);
        if (fr == FR_OK)
            fr = f_read(&f->fil, bounce, length, &got);
        if (fr != FR_OK)
            return fr_status(fr);
    }
    status_t st = got ? jam_vmo_write(f->vmo, 0, bounce, got) : OK;
    if (st != OK)
        return st;
    *out_actual = got;
    return OK;
}

static status_t op_write(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct fat_file *f = ctx;
    if (!(f->flags & FS_WRITE) || vol.read_only)
        return ERR_ACCESS_DENIED;
    if (length > FAT_FILE_BUF)
        return ERR_INVALID_ARGS;
    if (f->flags & FS_APPEND)
        offset = f_size(&f->fil);
    if (offset > FAT_FILE_MAX || length > FAT_FILE_MAX - offset)
        return ERR_OUT_OF_RANGE;
    status_t st = length ? jam_vmo_read(f->vmo, 0, bounce, length) : OK;
    if (st == OK)
        st = grow_to(f, offset);
    if (st != OK)
        return st;
    UINT put = 0;
    FRESULT fr = f_lseek(&f->fil, (FSIZE_t)offset);
    if (fr == FR_OK && length) {
        fr = f_write(&f->fil, bounce, length, &put);
        f->unsynced = true;
    }
    if (fr != FR_OK)
        return fr_status(fr);
    if (length && put == 0)
        return ERR_NO_SPACE;   /* FatFs writes what fits: nothing did */
    *out_actual = put;
    return OK;
}

static status_t op_truncate(void *ctx, uint64_t size)
{
    struct fat_file *f = ctx;
    if (!(f->flags & FS_WRITE) || vol.read_only)
        return ERR_ACCESS_DENIED;
    if (size > FAT_FILE_MAX)
        return ERR_OUT_OF_RANGE;
    if (size >= f_size(&f->fil))
        return grow_to(f, size);
    FRESULT fr = f_lseek(&f->fil, (FSIZE_t)size);
    if (fr == FR_OK)
        fr = f_truncate(&f->fil);
    f->unsynced = true;
    return fr_status(fr);
}

static status_t op_stat(void *ctx, uint64_t *out_size, uint64_t *out_mtime)
{
    struct fat_file *f = ctx;
    FILINFO fi;
    *out_size = f_size(&f->fil);
    /* The directory entry's time: that of the last sync or close that
     * followed a write. FatFs's lock keeps the path ours while it is open. */
    *out_mtime = f_stat(f->path, &fi) == FR_OK ? fat_unix_time(fi.fdate, fi.ftime) : 0;
    return OK;
}

static status_t op_sync(void *ctx)
{
    return sync_file(ctx);
}

static const struct file_ops file_ops = {
    .read = op_read, .write = op_write, .truncate = op_truncate, .stat = op_stat,
    .sync = op_sync,
};

/* ---- open and close ------------------------------------------------------------------- */

/* Give the slot back: the buffer, the channel, the binding. */
static void release(struct fat_file *f)
{
    if (f->vmo != HANDLE_INVALID)
        jam_handle_close(f->vmo);
    if (f->ch != HANDLE_INVALID) {
        if (f->armed)
            (void)jam_port_unbind(vol.port, f->ch, FAT_KEY_FILE(f - files, f->gen));
        jam_handle_close(f->ch);
    }
    f->used = false;
}

static void close_file(struct fat_file *f)
{
    bool was = f->unsynced;
    FRESULT fr = f_close(&f->fil);   /* flushes what was written */
    if (fr != FR_OK && !vol.disk_gone)
        printf("fat %s: closing %s: FatFs error %d\n", vol.name, f->path, (int)fr);
    f->unsynced = false;
    release(f);
    if (was && !vol.disk_gone && !files_unsynced())
        (void)disk_settle();   /* failures are logged there; nobody is left to tell */
}

void files_close_all(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++)
        if (files[i].used)
            close_file(&files[i]);
}

/* Wait (ONCE) for the next request or the client's close. */
static status_t arm(struct fat_file *f)
{
    status_t st = jam_port_bind(vol.port, f->ch, FAT_KEY_FILE(f - files, f->gen),
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
    f->armed = st == OK;
    return st;
}

/* The slot's buffer and channel; the client's ends into *out_ch, *out_vmo. */
static status_t attach(struct fat_file *f, handle_t *out_ch, handle_t *out_vmo)
{
    handle_t client = HANDLE_INVALID, buf = HANDLE_INVALID;
    status_t st = jam_vmo_create(FAT_FILE_BUF, 0, HANDLE_INVALID, &f->vmo);
    if (st == OK)
        st = jam_handle_duplicate(f->vmo, CLIENT_BUF_RIGHTS, &buf);
    if (st == OK)
        st = jam_channel_create(&f->ch, &client);
    if (st == OK)
        st = arm(f);
    if (st != OK) {
        if (buf != HANDLE_INVALID)
            jam_handle_close(buf);
        if (client != HANDLE_INVALID)
            jam_handle_close(client);
        return st;
    }
    *out_ch = client;
    *out_vmo = buf;
    return OK;
}

/* f_open for FS_* flags. FatFs's FR_DENIED: an existing file is read-only
 * (its attribute); a new one found no room (the volume or a fixed root
 * directory is full). */
static status_t open_fil(struct fat_file *f, const char *path, uint32_t flags, bool exists)
{
    BYTE mode = (flags & FS_READ ? FA_READ : 0) | (flags & FS_WRITE ? FA_WRITE : 0) |
                (exists ? FA_OPEN_EXISTING : FA_CREATE_NEW);
    FRESULT fr = f_open(&f->fil, path, mode);
    if (fr == FR_DENIED)
        return exists ? ERR_ACCESS_DENIED : ERR_NO_SPACE;
    if (fr != FR_OK)
        return fr_status(fr);
    f->unsynced = !exists;
    if ((flags & FS_TRUNCATE) && f_size(&f->fil) > 0) {
        fr = f_truncate(&f->fil);   /* the file pointer is 0 after f_open */
        f->unsynced = true;
        if (fr != FR_OK)
            (void)f_close(&f->fil);   /* the truncate's error is the result */
    }
    return fr_status(fr);
}

status_t files_open(const char *path, uint32_t flags, handle_t *out_ch, handle_t *out_vmo,
                    uint64_t *out_size)
{
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK && fr != FR_NO_FILE)
        return fr_status(fr);
    bool exists = fr == FR_OK;
    if (exists && (fi.fattrib & AM_DIR))
        return ERR_WRONG_TYPE;
    if (!exists && !(flags & FS_CREATE))
        return ERR_NOT_FOUND;
    struct fat_file *f = NULL;
    for (unsigned i = 0; i < FAT_MAX_FILES && !f; i++)
        if (!files[i].used)
            f = &files[i];
    if (!f)
        return ERR_NO_RESOURCES;
    uint32_t gen = f->gen + 1;
    memset(f, 0, sizeof(*f));
    f->gen = gen;
    f->flags = flags;
    status_t st = open_fil(f, path, flags, exists);
    if (st != OK)
        return st;
    f->used = true;
    memcpy(f->path, path, strlen(path) + 1);
    st = attach(f, out_ch, out_vmo);
    if (st != OK) {
        close_file(f);
        return st;
    }
    *out_size = f_size(&f->fil);
    return OK;
}

/* A client that closes a file and then calls `fs` (unlink it, stat it, open
 * it again) must find the file closed, but the two arrive on different
 * channels. The close is visible at once as the channel's PEER_CLOSED, so
 * before each fs request every file whose client is gone is finished:
 * what it still had queued, then the close. */
void files_reap(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_file *f = &files[i];
        if (!f->used || jam_object_wait_one(f->ch, SIG_PEER_CLOSED, 0, NULL) != OK)
            continue;
        /* At most a channel's queue (the kernel caps it) of requests. */
        while (!vol.disk_gone && file_serve_one(f->ch, &file_ops, f) == OK)
            ;
        close_file(f);
    }
}

void files_event(uint64_t key)
{
    unsigned slot = FAT_KEY_SLOT(key);
    if (slot >= FAT_MAX_FILES)
        return;
    struct fat_file *f = &files[slot];
    if (!f->used || f->gen != FAT_KEY_GEN(key))
        return;   /* a packet of a file closed since */
    f->armed = false;
    status_t st = OK;
    for (unsigned i = 0; i < FAT_BATCH && st == OK && !vol.disk_gone; i++)
        st = file_serve_one(f->ch, &file_ops, f);
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(f);   /* fires at once if more is queued */
    if (st != OK)
        close_file(f);   /* the client closed it (ERR_PEER_CLOSED), or we can't go on */
}
