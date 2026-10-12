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
 * fs.open of one file shares one FIL (struct fat_open, counted): a file
 * being written can be opened again to read, and the reader sees what the
 * writer has written so far, flushed or not. Every request seeks first,
 * so the shared position is nobody's.
 *
 * Which file a path is: its directory entry, by the entry's place (the
 * sector that holds it and its slot there: ffx.h), never by the path. A
 * file reached by its 8.3 alias, or by another case, is the same file; and
 * an entry doesn't move while its file is open (renaming or removing an
 * open file is refused), even when a directory above it is renamed or
 * moved, so no open file is ever known by a path that went stale (the
 * M11.6 review's finding 5). file.stat's time is read from that entry.
 *
 * The lock. FatFs's own lock (its table of open objects) is off, since it
 * is state FatFs keeps outside the structs in kept (ffconf.h); fat keeps
 * its rule instead, by entry: a file open at all can't be opened for
 * writing (opening it to read shares the FIL above, a writer's too), and
 * an open file can't be removed or renamed (files_may_change, fsops.c).
 * Each is refused with ERR_BUSY.
 *
 * A file is `unsynced` from its first change until its next f_sync; when
 * the last unsynced file is flushed the volume is settled (marked clean,
 * disk.c).
 *
 * A mapped file (file.map, pager.c) keeps its entry in opens[] too: its
 * slot of maps[] names it as a slot of files[] does, and the FIL stays
 * until its last mapping goes. The lock counts a mapping as no open: a
 * file mapped to read may be opened for writing (its FIL, open to read
 * since its first open, is made to write: open_to_write), written,
 * truncated, renamed or removed, and a file open for writing may be
 * mapped to read; every change reaches its mappings (changed, and
 * pager.c's header for the order against the hold). A file mapped to run
 * (FILE_MAP_EXEC, a program) is the exception: none of these while that
 * VMO lives (ERR_BUSY), and a file open for writing isn't mapped to run.
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
/* Bit i: slot i's client has left too many answers unread (file_unread_full):
 * it is not armed until it has read them (files_unheld). */
static uint32_t held;
_Static_assert(FAT_MAX_FILES <= 32, "held is a word");

static unsigned slot_of(const struct fat_file *f)
{
    return (unsigned)(f - kept->files);
}

static struct fat_open *open_of(const struct fat_file *f)
{
    return &kept->opens[f->open];
}

bool files_open_used(uint32_t o)
{
    return kept->opens[o].refs || maps_use_open(o);
}

bool files_unsynced(void)
{
    for (unsigned i = 0; i < FAT_OPENS; i++)
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
    for (unsigned i = 0; i < FAT_OPENS; i++) {
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

/* The place of the entry fil was opened from (it points into the one
 * volume's window, kept->fs.win, at f_open's time). */
static uint64_t place_of(const FIL *fil)
{
    return ffx_place(fil->dir_sect, (UINT)(fil->dir_ptr - kept->fs.win));
}

/* The open file (in use) whose entry is at `place`, or NULL. */
static struct fat_open *open_at(uint64_t place)
{
    for (unsigned i = 0; i < FAT_OPENS; i++)
        if (files_open_used(i) && kept->opens[i].place == place)
            return &kept->opens[i];
    return NULL;
}

status_t files_may_change(const char *path, int *mapped)
{
    FILINFO fi;
    uint64_t place = 0;
    bool any = false;
    *mapped = -1;
    for (unsigned i = 0; i < FAT_OPENS && !any; i++)
        any = files_open_used(i);
    struct fat_open *o = any && ffx_stat(path, &fi, &place) == FR_OK ? open_at(place) : NULL;
    if (!o)
        return OK;   /* not in use (or not there: the caller's own call says) */
    uint32_t oi = (uint32_t)(o - kept->opens);
    if (o->refs || maps_exec(oi))
        return ERR_BUSY;
    *mapped = (int)oi;
    return OK;
}

void files_moved(uint32_t o, const char *to)
{
    FILINFO fi;
    uint64_t place = 0;
    struct fat_open *f = &kept->opens[o];
    if (ffx_stat(to, &fi, &place) != FR_OK) {
        /* Its entry can't be found again: a later open of the file would
         * be a second FIL, not this one. Its mappings go instead. */
        maps_cut_open(o);
        return;
    }
    undo_open(f);
    f->place = place;
    f->fil.dir_sect = (LBA_t)(place >> 4);   /* as f_open sets them (ffx.h's place) */
    f->fil.dir_ptr = kept->fs.win + (place & 15u) * 32u;
}

bool files_place_ok(const struct fat_open *o)
{
    const FATFS *fs = &kept->fs;
    return o->fil.dir_ptr >= fs->win && o->fil.dir_ptr < fs->win + FAT_SECTOR &&
           o->place == place_of(&o->fil);
}

/* ---- the file protocol ---------------------------------------------------------------- */

/* f_truncate at fp's position, but a truncate to 0 of a file of more than
 * one cluster in two: to one byte (its first cluster made the chain's end,
 * the rest freed), then to 0. FatFs truncates to 0 by freeing the chain
 * from its head, and the directory entry naming that head changes only at
 * the file's next sync; when the freed chain doesn't fit the hold, its FAT
 * sectors go out in steps before the request commits (hold.c's "Room"),
 * and a death between steps leaves the successor, started fresh from the
 * disk, an entry naming free clusters that the next allocation gives to
 * another file. Cut first, the first FAT sector to go out ends the chain
 * at its head: at worst clusters no file reaches. The disk ends the same. */
static FRESULT truncate_fil(FIL *fp)
{
    FSIZE_t cluster = (FSIZE_t)kept->fs.csize * FAT_SECTOR;
    if (f_tell(fp) == 0 && f_size(fp) > cluster) {
        FRESULT fr = f_lseek(fp, 1);
        if (fr == FR_OK)
            fr = f_truncate(fp);
        if (fr == FR_OK)
            fr = f_lseek(fp, 0);
        if (fr != FR_OK)
            return fr;
    }
    return f_truncate(fp);
}

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

/* f's bytes [lo, hi) changed (hi: the end before or after, whichever is
 * further): its mappings see it. First what FatFs left in memory goes into
 * the hold (ffx_flush), where the pages are read from; then pager.c drops
 * the pages and follows the size. Both inside the change's operation:
 * pager.c's header says why that order, there. A flush that fails is the
 * change's failure (the pages are dropped anyway: a mapper that fetches
 * them again fails, as read_pages refuses a FIL left dirty). */
static status_t changed(struct fat_open *f, uint64_t lo, uint64_t hi)
{
    uint32_t o = (uint32_t)(f - kept->opens);
    if (!maps_use_open(o))
        return OK;
    status_t st = fr_status(ffx_flush(&f->fil));
    maps_changed(o, lo, hi);
    return st;
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
    if (st != OK)
        return st;
    uint64_t old = f_size(&f->fil);
    st = grow_to(f, offset);
    UINT put = 0;
    FRESULT fr = st == OK ? f_lseek(&f->fil, (FSIZE_t)offset) : FR_OK;
    if (st == OK && fr == FR_OK && length) {
        if (h->flags & FS_GATHER)
            op_gather();   /* its writes stay held after this request */
        fr = f_write(&f->fil, bounce, length, &put);
        f->unsynced = true;
    }
    /* What a failure changed too: grow_to's zeros, what f_write put. */
    status_t sc = changed(f, old < offset ? old : offset, offset + put);
    if (st != OK || fr != FR_OK)
        return st != OK ? st : fr_status(fr);
    if (length && put == 0)
        return ERR_NO_SPACE;   /* FatFs writes what fits: nothing did */
    if (sc != OK)
        return sc;
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
    uint64_t old = f_size(&f->fil);
    status_t st = OK;
    if (size >= old) {
        st = grow_to(f, size);
    } else {
        FRESULT fr = f_lseek(&f->fil, (FSIZE_t)size);
        if (fr == FR_OK)
            fr = truncate_fil(&f->fil);
        f->unsynced = true;
        st = fr_status(fr);
    }
    status_t sc = changed(f, size < old ? size : old, size < old ? old : size);
    return st != OK ? st : sc;
}

static status_t op_stat(void *ctx, uint64_t *out_size, uint8_t *out_is_dir, uint64_t *out_mtime,
                        uint64_t *out_id)
{
    struct fat_open *f = open_of(ctx);
    WORD date = 0, time = 0;
    *out_size = f_size(&f->fil);
    *out_is_dir = 0;   /* fs.open opens files only */
    *out_id = f->place;
    /* The directory entry's time: that of the last sync or close that
     * followed a write. The entry stays where it is while the file is open
     * (above). */
    FRESULT fr = ffx_entry_time(&kept->fs, f->place, &date, &time);
    if (fr != FR_OK)
        return fr_status(fr);
    *out_mtime = fat_unix_time(date, time);
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

/* Is a slot of files[] that may write using opens[o]? */
static bool written(uint32_t o)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++)
        if (kept->files[i].used && kept->files[i].open == o && (kept->files[i].flags & FS_WRITE))
            return true;
    return false;
}

/* file.map: the pages come from pager.c. A file open for writing is
 * mapped to read (its writes reach the pages: changed), never to run (a
 * program's bytes don't change under it); what its writer left in
 * FatFs's memory goes into the hold first, where the pages are read. */
static status_t op_map(void *ctx, uint32_t flags, handle_t *out_vmo, uint64_t *out_size)
{
    struct fat_file *h = ctx;
    if (flags != FILE_MAP_READ && flags != FILE_MAP_EXEC)
        return ERR_INVALID_ARGS;
    if (!(h->flags & FS_READ))
        return ERR_ACCESS_DENIED;
    if (written(h->open) && flags == FILE_MAP_EXEC)
        return ERR_BUSY;
    if (written(h->open)) {
        if (hold_pending())
            op_gather();   /* as a read's: an FS_GATHER file's writes stay held */
        status_t st = fr_status(ffx_flush(&open_of(h)->fil));   /* (its open: in the undo) */
        if (st != OK)
            return st;
    }
    return maps_make(h->open, flags, out_vmo, out_size);
}

const struct file_ops fat_file_ops = {
    .read = op_read, .write = op_write, .truncate = op_truncate, .stat = op_stat,
    .sync = op_sync, .map = op_map,
};

/* ---- open and close ------------------------------------------------------------------- */

/* Slot i's channel goes, and its binding. */
static void close_channel(unsigned i)
{
    held &= ~(1u << i);
    if (fh[i].ch != HANDLE_INVALID) {
        if (fh[i].armed)
            (void)jam_port_unbind(vol.port, fh[i].ch, FAT_KEY_FILE(i, fh[i].gen));
        jam_handle_close(fh[i].ch);
    }
    fh[i].ch = HANDLE_INVALID;
    fh[i].armed = false;
}

/* Slot i's handles go: the buffer, the channel, the binding, and the
 * keeper's (after the commit, if an operation runs: keeper.c). */
static void release_handles(unsigned i)
{
    if (fh[i].vmo != HANDLE_INVALID)
        jam_handle_close(fh[i].vmo);
    fh[i].vmo = HANDLE_INVALID;
    close_channel(i);
    kept_drop(FAT_KEEP_FILE(i));
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

/* The slot goes; its file is closed with its last slot, unless it is
 * mapped. A writer that leaves readers or mappings behind has its changes
 * flushed as a close would. */
static void close_file(struct fat_file *f)
{
    struct fat_open *o = open_of(f);
    undo_open(o);
    bool was = o->unsynced, last = --o->refs == 0;
    if (last)
        maps_let_go(f->open);   /* its shared VMOs live on in their mappers alone */
    last = last && !maps_use_open(f->open);
    FRESULT fr = last ? f_close(&o->fil) : was ? f_sync(&o->fil) : FR_OK;
    if (fr != FR_OK && !vol.disk_gone)
        printf("fat %s: closing the file at entry %lx: FatFs error %d\n", vol.name,
               (unsigned long)o->place, (int)fr);
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

/* The slot's buffer and channel (put with the keeper before the request
 * that made them commits: keeper.c); the client's ends into *out_ch,
 * *out_vmo. */
static status_t attach(struct fat_file *f, handle_t *out_ch, handle_t *out_vmo)
{
    unsigned i = slot_of(f);
    handle_t client = HANDLE_INVALID, buf = HANDLE_INVALID;
    status_t st = jam_vmo_create(FAT_FILE_BUF, 0, &fh[i].vmo);
    if (st == OK)
        st = jam_handle_duplicate(fh[i].vmo, CLIENT_BUF_RIGHTS, &buf);
    if (st == OK)
        st = jam_channel_create(&fh[i].ch, &client);
    if (st == OK)
        st = kept_put(FAT_KEEP_FILE(i), (const handle_t[]){ fh[i].ch, fh[i].vmo }, 2);
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

/* f_open for FS_* flags, of a file nobody has open. FatFs's FR_DENIED: an
 * existing file is read-only (its attribute); a new one found no room (the
 * volume or a fixed root directory is full). */
static status_t open_fil(struct fat_open *f, const char *path, uint32_t flags, bool exists)
{
    /* Always readable: a later reader shares this FIL. */
    BYTE mode = FA_READ | (flags & FS_WRITE ? FA_WRITE : 0) |
                (exists ? FA_OPEN_EXISTING : FA_CREATE_NEW);
    if (!exists)
        dirs_forget();   /* a new entry: listings start again */
    FRESULT fr = f_open(&f->fil, path, mode);
    if (fr == FR_DENIED)
        return exists ? ERR_ACCESS_DENIED : ERR_NO_SPACE;
    if (fr != FR_OK)
        return fr_status(fr);
    f->place = place_of(&f->fil);
    f->unsynced = !exists;
    if ((flags & FS_TRUNCATE) && f_size(&f->fil) > 0) {
        fr = truncate_fil(&f->fil);   /* the file pointer is 0 after f_open */
        f->unsynced = true;
        if (fr != FR_OK)
            (void)f_close(&f->fil);   /* the truncate's error is the result */
    }
    return fr_status(fr);
}

/* o, a file nobody has open but mapped to read (pager.c), opened for
 * writing (flags, fi its entry): its FIL, open to read since its first
 * open, may write from now on, as f_open would have made it (FatFs refuses
 * a read-only file), and FS_TRUNCATE empties it; its mappings see that. */
static status_t open_to_write(struct fat_open *o, const FILINFO *fi, uint32_t flags)
{
    if (fi->fattrib & AM_RDO)
        return ERR_ACCESS_DENIED;
    o->fil.flag |= FA_WRITE;
    uint64_t old = f_size(&o->fil);
    if (!(flags & FS_TRUNCATE) || old == 0)
        return OK;
    FRESULT fr = f_lseek(&o->fil, 0);
    if (fr == FR_OK)
        fr = truncate_fil(&o->fil);
    o->unsynced = true;
    status_t sc = changed(o, 0, old);
    return fr != FR_OK ? fr_status(fr) : sc;
}

/* A free slot of files[] into *f, and a free entry of opens[] into *spare. */
static void find_slots(struct fat_file **f, struct fat_open **spare)
{
    for (unsigned i = 0; i < FAT_MAX_FILES && !*f; i++)
        if (!kept->files[i].used)
            *f = &kept->files[i];
    for (unsigned i = 0; i < FAT_OPENS && !*spare; i++)
        if (!files_open_used(i))
            *spare = &kept->opens[i];
}

status_t files_open(const char *path, uint32_t flags, handle_t *out_ch, handle_t *out_vmo,
                    uint64_t *out_size)
{
    FILINFO fi;
    uint64_t place = 0;
    FRESULT fr = ffx_stat(path, &fi, &place);
    if (fr != FR_OK && fr != FR_NO_FILE)
        return fr_path_status(fr, path);
    bool exists = fr == FR_OK;
    if (exists && (fi.fattrib & AM_DIR))
        return ERR_IS_DIR;
    if (!exists && !(flags & FS_CREATE))
        return ERR_NOT_FOUND;
    if (exists && (flags & FS_EXCL))
        return ERR_ALREADY_EXISTS;
    struct fat_file *f = NULL;
    struct fat_open *o = exists ? open_at(place) : NULL, *spare = NULL;
    find_slots(&f, &spare);
    /* Open already: a writer has its file to itself. Mapped to run: never
     * written while it runs. Only mapped to read: written under its
     * mappings. */
    if (o && (flags & FS_WRITE) && (o->refs || maps_exec((uint32_t)(o - kept->opens))))
        return ERR_BUSY;
    if (!f || (!o && !spare))
        return ERR_NO_RESOURCES;
    undo_open(o ? o : spare);
    if (!o) {
        o = spare;
        memset(o, 0, sizeof(*o));
        status_t st = open_fil(o, path, flags, exists);
        if (st != OK)
            return st;
    } else if (flags & FS_WRITE) {
        status_t st = open_to_write(o, &fi, flags);
        if (st != OK)
            return st;
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
    op_made(FAT_MADE_FILE, slot_of(f));
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
    pager_reap();
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
    if (file_unread_full(fh[slot].ch)) {
        held |= 1u << slot;   /* it reads its answers first: files_unheld looks again */
        idl_hold(&vol.held);
        return;
    }
    const struct fat_chan c = chan_of(f);
    status_t st = serve_one(&c);   /* one per turn: main.c's loop */
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(f);   /* fires at once if more is queued */
    if (st != OK)
        close_op(f);   /* the client closed it (ERR_PEER_CLOSED), or we can't go on */
}

void files_unheld(void)
{
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_file *f = &kept->files[i];
        if (!(held & (1u << i)))
            continue;
        if (f->used && file_unread_full(fh[i].ch)) {
            idl_hold(&vol.held);   /* still behind */
            continue;
        }
        held &= ~(1u << i);
        if (f->used && arm(f) != OK)   /* fires at once: its requests are queued */
            close_op(f);
    }
}

/* ---- a successor's files (adopt.c) ------------------------------------------------- */

bool files_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= FAT_MAX_FILES || !kept->files[i].used || n != 2 || fh[i].ch || fh[i].vmo)
        return false;
    fh[i].ch = hs[0];
    fh[i].vmo = hs[1];
    fh[i].armed = false;
    return true;
}

unsigned files_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < FAT_MAX_FILES; i++) {
        struct fat_file *f = &kept->files[i];
        if (!f->used)
            continue;
        /* Not handed back (the keeper refused its put): its client's end
         * is closed, so the file is too. */
        if (!fh[i].ch || !fh[i].vmo || arm(f) != OK) {
            close_op(f);
            continue;
        }
        n++;
    }
    return n;
}

status_t files_remake(uint32_t i, handle_t *out_ch, handle_t *out_vmo)
{
    handle_t client = HANDLE_INVALID, buf = HANDLE_INVALID;
    if (fh[i].ch)
        jam_handle_close(fh[i].ch);   /* not bound yet: files_adopt comes after */
    fh[i].ch = HANDLE_INVALID;
    status_t st = jam_handle_duplicate(fh[i].vmo, CLIENT_BUF_RIGHTS, &buf);
    if (st == OK)
        st = jam_channel_create(&fh[i].ch, &client);
    if (st == OK)
        st = kept_put(FAT_KEEP_FILE(i), (const handle_t[]){ fh[i].ch, fh[i].vmo }, 2);
    if (st != OK) {
        if (buf)
            jam_handle_close(buf);
        if (client)
            jam_handle_close(client);
        if (fh[i].ch)
            jam_handle_close(fh[i].ch);
        fh[i].ch = HANDLE_INVALID;   /* files_adopt closes the file */
        return st;
    }
    *out_ch = client;
    *out_vmo = buf;
    return OK;
}

bool files_chan(uint32_t i, uint32_t gen16, struct fat_chan *out)
{
    if (i >= FAT_MAX_FILES || !kept->files[i].used || !fh[i].ch || !fh[i].vmo ||
        (kept->files[i].gen & 0xffffu) != gen16)
        return false;
    *out = chan_of(&kept->files[i]);
    return true;
}

void files_forget(uint32_t i)
{
    if (i >= FAT_MAX_FILES || !kept->files[i].used)
        return;
    struct fat_open *o = open_of(&kept->files[i]);
    uint32_t oi = kept->files[i].open;
    if (o->refs && --o->refs == 0)
        maps_let_go(oi);
    if (!files_open_used(oi))
        memset(o, 0, sizeof(*o));   /* its FIL with it, never closed through FatFs */
    release(&kept->files[i]);
}
