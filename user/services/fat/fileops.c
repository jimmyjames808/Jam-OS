/* fat: the open files. Each fs.open takes a slot of files[]: FatFs's FIL,
 * a 64 KiB transfer buffer (a VMO; the client gets a handle to map it) and
 * a channel speaking the `file` protocol, bound to fat's port. The client
 * closing its end closes the file. The table's size (FAT_MAX_FILES) bounds
 * what clients can make fat hold: 64 KiB and two handles per slot.
 *
 * The tables (files[], opens[]) are in fat's state (kept, fat.h), so that
 * they can outlive fat; each slot's channel and buffer, handles, are here
 * in fh[], by the same index.
 *
 * fat never maps a transfer buffer: data goes between the VMO and the
 * request's own buffer (its slot's bounce, request.c: op_bounce) with
 * vmo_read / vmo_write, which answer a buffer that is not what it was with
 * an error, so nothing a client does to its handle can make fat fault.
 * (The client's handle has no RIGHT_RESIZE, so it can't change the size
 * either; the bounce stays as the one copy that also keeps FatFs off
 * memory a client can write, and a write's bytes in the state.)
 *
 * Every request and every close is an operation (request.c): an open's
 * entry in opens[] is saved in the undo copy before anything changes it
 * (undo_open), and the close of a file whose client has gone is an
 * operation of its own (close_op).
 *
 * FatFs's f_lseek past the end of a file open for writing grows it with
 * whatever the clusters held before. Here a write or truncate past the end
 * writes zeros into the gap instead (grow_to), at most FAT_GROW_MAX per
 * call, and reads never seek past the end.
 *
 * One file, one FIL. FatFs gives every f_open a FIL of its own, with its
 * own cached sector and its own idea of the file's size. Here every
 * fs.open of one path shares one FIL (struct fat_open, counted): a file
 * being written can be opened again to read, and the reader sees what the
 * writer has written so far, flushed or not. Every request seeks first,
 * so the shared position is nobody's. Two opens are of one path when their
 * resolved paths are equal without case.
 *
 * The lock. FatFs's own lock (its table of open objects) is off, since it
 * is state FatFs keeps outside the structs in kept (ffconf.h); fat keeps
 * its rule instead, by directory entry (where a FIL's entry is: its sector
 * and offset), so that a file reached by its 8.3 alias too is still one
 * file: an entry open for writing can't be opened again (but by the same
 * path, to read: the shared FIL above), an entry open at all can't be
 * opened for writing, and an open file can't be removed or renamed
 * (files_is_open, fsops.c). Each is refused with ERR_BAD_STATE, as
 * FatFs's FR_LOCKED was.
 *
 * A file is `unsynced` from its first change until its next f_sync; when
 * the last unsynced file is flushed the volume is settled (marked clean,
 * disk.c).
 *
 * FS_GATHER (<os.h>): a write to a file opened with it stays held
 * (hold.c) after its request, instead of going out with it, and goes out
 * in big sorted writes by the file's sync or close at the latest. Its sync
 * fails if a held write ever failed to go out (disk.c: then nothing more
 * is written). */
#include "fat.h"

#define CLIENT_BUF_RIGHTS (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER)

/* Each slot's handles: by files[]'s index. */
static struct {
    handle_t ch;      /* our end of its `file` channel */
    handle_t vmo;     /* its transfer buffer, FAT_FILE_BUF bytes: never mapped here */
    bool     armed;   /* ch is bound to the port (ONCE) ... */
    uint32_t gen;     /* ... with this generation in its key */
} fh[FAT_MAX_FILES];

static FIL probe;   /* files_is_open's look at a path's entry */

static unsigned slot_of(const struct fat_file *f)
{
    return (unsigned)(f - kept->files);
}

static struct fat_open *open_of(const struct fat_file *f)
{
    return &kept->opens[f->open];
}

bool files_unsynced(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++)
        if (kept->opens[i].refs && kept->opens[i].unsynced)
            return true;
    return false;
}

/* Flush o; once nothing is left unsynced, settle the volume. */
static status_t sync_open(struct fat_open *o)
{
    undo_open(o);
    status_t st = fr_status(f_sync(&o->fil));
    if (st != OK)
        return st;
    bool was = o->unsynced;
    o->unsynced = false;
    return was && !files_unsynced() ? disk_settle(false) : OK;
}

status_t files_sync_all(void)
{
    status_t st = OK;
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_open *o = &kept->opens[i];
        if (!o->refs || !o->unsynced)
            continue;
        undo_open(o);
        status_t s = fr_status(f_sync(&o->fil));
        if (s == OK)
            o->unsynced = false;
        else if (st == OK)
            st = s;
    }
    return st;
}

/* ---- the lock ------------------------------------------------------------------------ */

/* The open file (refs > 0) whose directory entry is fil's, or NULL. Both
 * point into the one volume's window (kept->fs.win). */
static struct fat_open *open_at_entry(const FIL *fil)
{
    size_t at = (size_t)(fil->dir_ptr - kept->fs.win);
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_open *o = &kept->opens[i];
        if (o->refs && o->fil.dir_sect == fil->dir_sect &&
            (size_t)(o->fil.dir_ptr - kept->fs.win) == at)
            return o;
    }
    return NULL;
}

bool files_is_open(const char *path)
{
    bool any = false;
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        if (!kept->opens[i].refs)
            continue;
        if (path_same(kept->opens[i].path, path))
            return true;
        any = true;
    }
    /* Another path may reach an open file's entry (an 8.3 alias): look at
     * where the path's entry is. Not a file (a directory, nothing): FatFs
     * says what it is. */
    if (!any || f_open(&probe, path, FA_READ) != FR_OK)
        return false;
    bool open = open_at_entry(&probe) != NULL;
    (void)f_close(&probe);   /* read-only: nothing to write */
    return open;
}

/* ---- the file protocol ---------------------------------------------------------------- */

/* Make f at least `size` bytes long, the new part zeros. */
static status_t grow_to(struct fat_open *f, uint64_t size)
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
    struct fat_file *h = ctx;
    struct fat_open *f = open_of(h);
    if (!(h->flags & FS_READ))
        return ERR_ACCESS_DENIED;
    if (length > FAT_FILE_BUF)
        return ERR_INVALID_ARGS;
    UINT got = 0;
    uint8_t *bounce = op_bounce();
    if (length && offset < f_size(&f->fil)) {
        /* A read moves FatFs's window, which writes out the FAT sector an
         * FS_GATHER file's write left changed: while writes are held, it
         * stays held with them (hold.c) instead of sending them all out
         * early. */
        if (hold_pending())
            op_gather();
        FRESULT fr = f_lseek(&f->fil, (FSIZE_t)offset);
        if (fr == FR_OK)
            fr = f_read(&f->fil, bounce, length, &got);
        if (fr != FR_OK)
            return fr_status(fr);
    }
    status_t st = got ? jam_vmo_write(fh[slot_of(h)].vmo, 0, bounce, got) : OK;
    if (st != OK)
        return st;
    *out_actual = got;
    return OK;
}

static status_t op_write(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct fat_file *h = ctx;
    struct fat_open *f = open_of(h);
    if (!(h->flags & FS_WRITE) || vol.read_only)
        return ERR_ACCESS_DENIED;
    if (length > FAT_FILE_BUF)
        return ERR_INVALID_ARGS;
    if (h->flags & FS_APPEND)
        offset = f_size(&f->fil);
    if (offset > FAT_FILE_MAX || length > FAT_FILE_MAX - offset)
        return ERR_OUT_OF_RANGE;
    /* The bytes, into the state before FatFs sees them; a request run
     * again takes them from there, never from the client's buffer again. */
    uint8_t *bounce = op_bounce();
    status_t st = OK;
    if (length && !op_data_in()) {
        st = jam_vmo_read(fh[slot_of(h)].vmo, 0, bounce, length);
        if (st == OK)
            op_data_copied();
    }
    if (st == OK)
        st = grow_to(f, offset);
    if (st != OK)
        return st;
    UINT put = 0;
    FRESULT fr = f_lseek(&f->fil, (FSIZE_t)offset);
    if (fr == FR_OK && length) {
        if (h->flags & FS_GATHER)
            op_gather();   /* its writes stay held after this request */
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
    struct fat_file *h = ctx;
    struct fat_open *f = open_of(h);
    if (!(h->flags & FS_WRITE) || vol.read_only)
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
    struct fat_open *f = open_of(ctx);
    FILINFO fi;
    *out_size = f_size(&f->fil);
    /* The directory entry's time: that of the last sync or close that
     * followed a write. The lock (above) keeps the path ours while it is
     * open. */
    *out_mtime = f_stat(f->path, &fi) == FR_OK ? fat_unix_time(fi.fdate, fi.ftime) : 0;
    return OK;
}

/* An FS_GATHER file's sync: what is held goes out (with the request's
 * send), and a held write that failed (before, or in that send) is its
 * failure too. */
static status_t op_sync(void *ctx)
{
    struct fat_file *h = ctx;
    if (h->flags & FS_GATHER) {
        if (kept->disk.hold_failed)
            return ERR_IO;
        kept->post.release = true;
    }
    return sync_open(open_of(h));
}

const struct file_ops fat_file_ops = {
    .read = op_read, .write = op_write, .truncate = op_truncate, .stat = op_stat,
    .sync = op_sync,
};

/* ---- open and close ------------------------------------------------------------------- */

/* Slot i's handles go: the buffer, the channel, the binding. */
static void release_handles(unsigned i)
{
    if (fh[i].vmo != HANDLE_INVALID)
        jam_handle_close(fh[i].vmo);
    if (fh[i].ch != HANDLE_INVALID) {
        if (fh[i].armed)
            (void)jam_port_unbind(vol.port, fh[i].ch, FAT_KEY_FILE(i, fh[i].gen));
        jam_handle_close(fh[i].ch);
    }
    fh[i].vmo = fh[i].ch = HANDLE_INVALID;
    fh[i].armed = false;
}

/* Give the slot back. */
static void release(struct fat_file *f)
{
    release_handles(slot_of(f));
    f->used = false;
}

void files_drop_unknown(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++)
        if (!kept->files[i].used && (fh[i].ch != HANDLE_INVALID || fh[i].vmo != HANDLE_INVALID))
            release_handles(i);
}

/* The slot goes; its file is closed with its last slot. A writer that
 * leaves readers behind has its changes flushed as a close would. */
static void close_file(struct fat_file *f)
{
    struct fat_open *o = open_of(f);
    undo_open(o);
    bool was = o->unsynced, last = --o->refs == 0;
    FRESULT fr = last ? f_close(&o->fil) : was ? f_sync(&o->fil) : FR_OK;
    if (fr != FR_OK && !vol.disk_gone)
        printf("fat %s: closing %s: FatFs error %d\n", vol.name, o->path, (int)fr);
    o->unsynced = false;
    release(f);
    if (was && !vol.disk_gone && !files_unsynced())
        (void)disk_settle(false);   /* failures are logged there; nobody is left to tell */
}

void files_close_all(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++)
        if (kept->files[i].used)
            close_file(&kept->files[i]);
}

/* f's close, as an operation of its own (request.c): its client has gone,
 * or its channel failed. */
static void close_op(struct fat_file *f)
{
    op_close_begin(f);
    close_file(f);
#ifdef FAT_RERUN_CHECK
    op_close_again(f, close_file);
#endif
    op_close_end();
}

/* Wait (ONCE) for the next request or the client's close. */
static status_t arm(struct fat_file *f)
{
    unsigned i = slot_of(f);
    status_t st = jam_port_bind(vol.port, fh[i].ch, FAT_KEY_FILE(i, f->gen),
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
    fh[i].armed = st == OK;
    fh[i].gen = f->gen;
    return st;
}

/* The slot's buffer and channel; the client's ends into *out_ch, *out_vmo. */
static status_t attach(struct fat_file *f, handle_t *out_ch, handle_t *out_vmo)
{
    unsigned i = slot_of(f);
    handle_t client = HANDLE_INVALID, buf = HANDLE_INVALID;
    status_t st = jam_vmo_create(FAT_FILE_BUF, 0, HANDLE_INVALID, &fh[i].vmo);
    if (st == OK)
        st = jam_handle_duplicate(fh[i].vmo, CLIENT_BUF_RIGHTS, &buf);
    if (st == OK)
        st = jam_channel_create(&fh[i].ch, &client);
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

/* Another path's open of the entry f has just opened (an 8.3 alias), if
 * the lock refuses the two together: one of them writes. */
static bool locked_out(const struct fat_open *f)
{
    const struct fat_open *other = open_at_entry(&f->fil);
    return other && ((f->fil.flag & FA_WRITE) || (other->fil.flag & FA_WRITE));
}

/* f_open for FS_* flags. FatFs's FR_DENIED: an existing file is read-only
 * (its attribute); a new one found no room (the volume or a fixed root
 * directory is full). The lock comes before the attribute, as FatFs's
 * own did. */
static status_t open_fil(struct fat_open *f, const char *path, uint32_t flags, bool exists)
{
    /* Always readable: a later reader shares this FIL. */
    BYTE mode = FA_READ | (flags & FS_WRITE ? FA_WRITE : 0) |
                (exists ? FA_OPEN_EXISTING : FA_CREATE_NEW);
    if (!exists)
        dirs_forget();   /* a new entry: listings start again */
    FRESULT fr = f_open(&f->fil, path, mode);
    if (fr == FR_DENIED && exists && files_is_open(path))
        return ERR_BAD_STATE;
    if (fr == FR_DENIED)
        return exists ? ERR_ACCESS_DENIED : ERR_NO_SPACE;
    if (fr != FR_OK)
        return fr_status(fr);
    if (exists && locked_out(f)) {
        (void)f_close(&f->fil);   /* nothing changed yet: nothing to write */
        return ERR_BAD_STATE;
    }
    f->unsynced = !exists;
    if ((flags & FS_TRUNCATE) && f_size(&f->fil) > 0) {
        fr = f_truncate(&f->fil);   /* the file pointer is 0 after f_open */
        f->unsynced = true;
        if (fr != FR_OK)
            (void)f_close(&f->fil);   /* the truncate's error is the result */
    }
    return fr_status(fr);
}

/* A free slot of files[] into *f, and the open of `path` into *o (or, if
 * it isn't open, a free one into *spare). */
static void find_slots(const char *path, struct fat_file **f, struct fat_open **o,
                       struct fat_open **spare)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        if (!kept->files[i].used && !*f)
            *f = &kept->files[i];
        if (kept->opens[i].refs && path_same(kept->opens[i].path, path))
            *o = &kept->opens[i];
        else if (!kept->opens[i].refs && !*spare)
            *spare = &kept->opens[i];
    }
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
    struct fat_open *o = NULL, *spare = NULL;
    find_slots(path, &f, &o, &spare);
    if (o && (flags & FS_WRITE))
        return ERR_BAD_STATE;   /* open already: a writer has its file to itself */
    if (!f || (!o && !spare))
        return ERR_NO_RESOURCES;
    undo_open(o ? o : spare);
    if (!o) {
        o = spare;
        memset(o, 0, sizeof(*o));
        status_t st = open_fil(o, path, flags, exists);
        if (st != OK)
            return st;
        memcpy(o->path, path, strlen(path) + 1);
    }
    o->refs++;
    uint32_t gen = f->gen + 1;
    memset(f, 0, sizeof(*f));
    memset(&fh[slot_of(f)], 0, sizeof(fh[0]));
    f->gen = gen;
    f->flags = flags;
    f->open = (uint32_t)(o - kept->opens);
    f->used = true;
    status_t st = attach(f, out_ch, out_vmo);
    if (st != OK) {
        close_file(f);
        return st;
    }
    *out_size = f_size(&o->fil);
    return OK;
}

/* A client that closes a file and then calls `fs` (unlink it, stat it, open
 * it again) must find the file closed, but the two arrive on different
 * channels. The close is visible at once as the channel's PEER_CLOSED, so
 * before each fs request every file whose client is gone is finished:
 * what it still had queued, then the close. */
/* Slot i's channel, as serve_one takes it. */
static struct fat_chan chan_of(struct fat_file *f)
{
    unsigned i = slot_of(f);
    return (struct fat_chan){
        .ch = fh[i].ch, .id = FAT_CHAN_FILE(i, f->gen), .proto = FAT_PROTO_FILE, .file = f,
    };
}

void files_reap(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_file *f = &kept->files[i];
        if (!f->used || jam_object_wait_one(fh[i].ch, SIG_PEER_CLOSED, 0, NULL) != OK)
            continue;
        /* At most a channel's queue (the kernel caps it) of requests. */
        const struct fat_chan c = chan_of(f);
        while (!vol.disk_gone && serve_one(&c) == OK)
            ;
        close_op(f);
    }
}

void files_event(uint64_t key)
{
    unsigned slot = FAT_KEY_SLOT(key);
    if (slot >= FAT_MAX_FILES)
        return;
    struct fat_file *f = &kept->files[slot];
    if (!f->used || f->gen != FAT_KEY_GEN(key))
        return;   /* a packet of a file closed since */
    fh[slot].armed = false;
    const struct fat_chan c = chan_of(f);
    status_t st = OK;
    for (unsigned i = 0; i < FAT_BATCH && st == OK && !vol.disk_gone; i++)
        st = serve_one(&c);
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(f);   /* fires at once if more is queued */
    if (st != OK)
        close_op(f);   /* the client closed it (ERR_PEER_CLOSED), or we can't go on */
}
