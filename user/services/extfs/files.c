/* extfs: the open files. Each fs.open takes a slot of files[]: the file's
 * inode number and generation (which file it is, whatever path reached
 * it, across renames), the flags it was opened with, a 64 KiB transfer
 * buffer (a VMO the client maps) and a channel speaking `file`, bound to
 * extfs's port. The client closing its end closes the file.
 *
 * Any number of opens per inode, writers too (the plan's "many writers":
 * one writer per file was FatFs's rule): every request reads the inode
 * afresh, so each open sees the others' writes at once. FS_APPEND writes
 * at the end as the server finds it, exact across processes.
 *
 * Unlink of an open file succeeds: the inode, with no name left, is on the
 * orphan list (on the disk, so a cut frees it at the next mount) and
 * lives while a slot names it, or a mapped file's (pager.c); its last
 * close or unmap drops it (defer_drop). A successor keeps the orphans its
 * state holds open or mapped (adopt.c).
 *
 * file.map is pager.c's; here are its checks and the changes its mappings
 * must see (a write's bytes and a truncate's size: maps_changed).
 *
 * The tables are in the state (kept->files); each slot's channel and
 * buffer, handles, are here by the same index, and with the keeper. The
 * buffer is never mapped here: data goes between it and the request's
 * bounce buffer in its slot with vmo_read and vmo_write. */
#include "extfs.h"

#define CLIENT_BUF_RIGHTS (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER)
#define TRUNC_ROOM 64u      /* blocks a truncate step may dirty, at most (a leaf, bitmaps, ...) */
#define TRUNC_STEPS 8u      /* steps a request takes before leaving the rest for after */

static struct {
    handle_t ch;
    handle_t vmo;
    bool     armed;
    uint32_t gen;
} fh[XF_MAX_FILES];
static uint64_t held[(XF_MAX_FILES + 63) / 64];   /* held back by the unread bound */

static unsigned slot_of(const struct xf_file *f)
{
    return (unsigned)(f - kept->files);
}

bool files_has(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_FILES; i++)
        if (kept->files[i].used && kept->files[i].ino == ino)
            return true;
    return false;
}

bool files_writing(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_FILES; i++)
        if (kept->files[i].used && kept->files[i].ino == ino &&
            (kept->files[i].flags & (FS_WRITE | FS_APPEND)))
            return true;
    return false;
}

bool files_held(uint32_t ino)
{
    return files_has(ino) || maps_has(ino);
}

/* The file's inode as it is now: ERR_IO if it isn't the one opened (it
 * can't be freed while a slot names it, so that is damage). */
static status_t inode_of(const struct xf_file *f, struct ext_inode *in)
{
    status_t st = ext_iget(&vol.fs, f->ino, EXT_IGET_UNLINKED, in);
    if (st == OK && in->d.generation != f->igen)
        st = ERR_NOT_FOUND;
    if (st == ERR_NOT_FOUND)
        return ext_damaged(&vol.fs, "an open file's inode was freed", f->ino, 0);
    return st;
}

static status_t stamp(struct ext_inode *in)
{
    struct ext_time t;
    time_now(&t);
    status_t st = ext_itime_set(in, EXT_MTIME, &t);
    if (st == OK)
        st = ext_itime_set(in, EXT_CTIME, &t);
    return st == OK ? ext_iput(&vol.fs, in) : st;
}

/* files_truncate's change of the inode (size at most ext's largest). */
static status_t set_size(struct ext_inode *in, uint64_t size)
{
    struct ext_fs *fs = &vol.fs;
    uint64_t old = ext_isize(in);
    bool done = false;
    status_t st;
    if (size >= old) {   /* growing: a hole after the old end, no blocks */
        st = size > old ? ext_truncate(fs, in, size, &done) : OK;
        return st == OK ? stamp(in) : st;
    }
    /* Shrinking: on the orphan list from the first step to the last, so a
     * cut between them is finished at the next mount. A file with no name
     * is on it already, and stays until its last close. */
    bool own = in->d.links_count != 0;
    st = own ? ext_orphan_add(fs, in) : OK;
    for (unsigned i = 0; st == OK && !done && i < TRUNC_STEPS; i++) {
        if (i && ext_jnl_room(fs) < TRUNC_ROOM)
            break;
        st = ext_truncate(fs, in, size, &done);
    }
    if (st == OK)
        st = stamp(in);
    if (st == OK && done && own)
        st = ext_orphan_remove(fs, in->ino);
    else if (st == OK && !done)
        defer_trunc(in->ino, own);
    if (st == OK)
        st = ext_iget(fs, in->ino, EXT_IGET_UNLINKED, in);   /* the list changed it */
    return st;
}

status_t files_truncate(struct ext_inode *in, uint64_t size)
{
    uint64_t old = ext_isize(in);
    if (size > ext_max_size(&vol.fs))
        return ERR_OUT_OF_RANGE;
    status_t st = set_size(in, size);
    /* Its mappings: the new size, and the last page (zeros past the end
     * now, or the old end's page grown). A failure's too: the last
     * block's tail may be zero on the disk already, and the size goes
     * back (maps_settle). */
    maps_changed(in->ino, size < old ? size : old, size < old ? old : size,
                 st == OK ? size : old);
    return st;
}

/* ---- the file protocol ------------------------------------------------------------ */

static status_t op_read(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct xf_file *f = ctx;
    if (!(f->flags & FS_READ))
        return ERR_ACCESS_DENIED;
    if (length > XF_FILE_BUF)
        return ERR_INVALID_ARGS;
    struct ext_inode in;
    status_t st = inode_of(f, &in);
    size_t got = 0;
    uint8_t *bounce = op_bounce();
    if (st == OK && length)
        st = ext_read(&vol.fs, &in, offset, bounce, length, &got);
    if (st == OK && got)
        st = jam_vmo_write(fh[slot_of(f)].vmo, 0, bounce, got);
    if (st != OK)
        return st;
    *out_actual = (uint32_t)got;
    return OK;
}

static status_t op_write(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct xf_file *f = ctx;
    if (!(f->flags & (FS_WRITE | FS_APPEND)) || !writable())
        return ERR_ACCESS_DENIED;
    if (length > XF_FILE_BUF)
        return ERR_INVALID_ARGS;
    uint8_t *bounce = op_bounce();
    status_t st = OK;
    if (!op_data_in()) {   /* a re-run writes the bytes its first run copied */
        st = length ? jam_vmo_read(fh[slot_of(f)].vmo, 0, bounce, length) : OK;
        if (st != OK)
            return st;
        op_data_copied();
    }
    struct ext_inode in;
    st = inode_of(f, &in);
    if (st != OK)
        return st;
    if (f->flags & FS_APPEND)
        offset = ext_isize(&in);
    size_t n = 0;
    if (length)
        st = ext_write(&vol.fs, &in, offset, bounce, length, &n);
    if (st == ERR_NO_SPACE && n)
        st = OK;   /* what was written stays: a short write */
    if (st == OK && length)
        st = stamp(&in);
    /* Its mappings see the bytes, and a failed write's too: what ext wrote
     * over the file's own blocks is on the disk whatever the answer. */
    if (length)
        maps_changed(in.ino, offset, offset + (st == OK ? n : length), ext_isize(&in));
    if (st != OK)
        return st;
    *out_actual = (uint32_t)n;
    return OK;
}

static status_t op_truncate(void *ctx, uint64_t size)
{
    struct xf_file *f = ctx;
    if (!(f->flags & (FS_WRITE | FS_APPEND)) || !writable())
        return ERR_ACCESS_DENIED;
    struct ext_inode in;
    status_t st = inode_of(f, &in);
    return st == OK ? files_truncate(&in, size) : st;
}

static status_t op_stat(void *ctx, uint64_t *out_size, uint8_t *out_is_dir, uint64_t *out_mtime,
                        uint64_t *out_id)
{
    struct ext_inode in;
    status_t st = inode_of(ctx, &in);
    if (st != OK)
        return st;
    struct ext_time t;
    ext_itime(&in, EXT_MTIME, &t);
    *out_size = ext_isize(&in);
    *out_is_dir = 0;
    *out_mtime = t.sec > 0 ? (uint64_t)t.sec : 0;
    *out_id = in.ino;
    return OK;
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    return disk_flush();   /* every answered write is committed already; a failed one's
                            * bytes too, on a disk with a cache */
}

static status_t op_getattr(void *ctx, uint32_t *out_mode, uint32_t *out_nlink, uint32_t *out_uid,
                           uint32_t *out_gid, uint64_t *out_size, uint64_t *out_blocks,
                           uint64_t *out_id, int64_t *out_atime_ns, int64_t *out_mtime_ns,
                           int64_t *out_ctime_ns)
{
    struct ext_inode in;
    status_t st = inode_of(ctx, &in);
    if (st != OK)
        return st;
    struct xf_attr a;
    attr_of(&in, &a);
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

static status_t op_setattr(void *ctx, uint32_t mask, uint32_t mode, uint32_t uid, uint32_t gid,
                           int64_t atime_ns, int64_t mtime_ns)
{
    struct xf_file *f = ctx;
    if (f->nochange)
        return ERR_ACCESS_DENIED;   /* its view may not change it */
    struct ext_inode in;
    status_t st = inode_of(f, &in);
    return st == OK ? attr_set(&in, mask, mode, uid, gid, atime_ns, mtime_ns) : st;
}

static status_t op_map(void *ctx, uint32_t flags, handle_t *out_vmo, uint64_t *out_size)
{
    struct xf_file *f = ctx;
    if (flags != FILE_MAP_READ && flags != FILE_MAP_EXEC)
        return ERR_INVALID_ARGS;
    if (!(f->flags & FS_READ))
        return ERR_ACCESS_DENIED;
    if (flags == FILE_MAP_EXEC && files_writing(f->ino))
        return ERR_BUSY;   /* a program's bytes don't change while it runs (Q7) */
    struct ext_inode in;
    status_t st = inode_of(f, &in);
    if (st == OK)
        st = maps_make(f->ino, f->igen, flags, ext_isize(&in), out_vmo);
    if (st == OK)
        *out_size = ext_isize(&in);
    return st;
}

const struct file_ops xf_file_ops = {
    .read = op_read, .write = op_write, .truncate = op_truncate, .stat = op_stat,
    .sync = op_sync, .map = op_map, .getattr = op_getattr, .setattr = op_setattr,
};

/* ---- open and close ------------------------------------------------------------- */

static void close_channel(unsigned i)
{
    held[i / 64] &= ~(1ull << (i % 64));
    if (fh[i].ch != HANDLE_INVALID) {
        if (fh[i].armed)
            (void)jam_port_unbind(vol.port, fh[i].ch, XF_KEY_FILE(i, fh[i].gen));
        jam_handle_close(fh[i].ch);
    }
    fh[i].ch = HANDLE_INVALID;
    fh[i].armed = false;
}

static void release_handles(unsigned i)
{
    if (fh[i].vmo != HANDLE_INVALID)
        jam_handle_close(fh[i].vmo);
    fh[i].vmo = HANDLE_INVALID;
    close_channel(i);
    kept_drop(XF_KEEP_FILE(i));
}

void files_release(uint32_t i)
{
    if (i >= XF_MAX_FILES)
        return;
    kept->files[i].used = false;
    release_handles(i);
}

void files_drop_unknown(void)
{
    for (unsigned i = 0; i < XF_MAX_FILES; i++)
        if (!kept->files[i].used && (fh[i].ch != HANDLE_INVALID || fh[i].vmo != HANDLE_INVALID))
            release_handles(i);
}

/* The slot goes; with the file's last open, its shared VMO is its mappers'
 * alone (pager.c), and an inode with no name left goes with its last open
 * or mapping. The state forgets the slot first, so a death after it leaves
 * an orphan the next mount drops. */
static void close_file(struct xf_file *f)
{
    uint32_t ino = f->ino;
    files_release(slot_of(f));
    if (files_has(ino))
        return;
    maps_let_go(ino);
    if (maps_has(ino) || !vol.mounted)
        return;
    struct ext_inode in;
    if (ext_iget(&vol.fs, ino, EXT_IGET_UNLINKED, &in) == OK && !in.d.links_count) {
        defer_drop(ino);
        defer_run();
    }
}

void files_close_all(void)
{
    for (unsigned i = 0; i < XF_MAX_FILES; i++)
        if (kept->files[i].used)
            close_file(&kept->files[i]);
}

static status_t arm(struct xf_file *f)
{
    unsigned i = slot_of(f);
    status_t st = jam_port_bind(vol.port, fh[i].ch, XF_KEY_FILE(i, f->gen),
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
    fh[i].armed = st == OK;
    fh[i].gen = f->gen;
    return st;
}

static status_t attach(struct xf_file *f, handle_t *out_ch, handle_t *out_vmo)
{
    unsigned i = slot_of(f);
    handle_t client = HANDLE_INVALID, buf = HANDLE_INVALID;
    status_t st = jam_vmo_create(XF_FILE_BUF, 0, &fh[i].vmo);
    if (st == OK)
        st = jam_handle_duplicate(fh[i].vmo, CLIENT_BUF_RIGHTS, &buf);
    if (st == OK)
        st = jam_channel_create(&fh[i].ch, &client);
    if (st == OK)
        st = kept_put(XF_KEEP_FILE(i), (const handle_t[]){ fh[i].ch, fh[i].vmo }, 2);
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

status_t files_open(const struct ext_inode *in, uint32_t flags, bool nochange,
                    handle_t *out_ch, handle_t *out_vmo, uint64_t *out_size)
{
    struct xf_file *f = NULL;
    for (unsigned i = 0; i < XF_MAX_FILES && !f; i++)
        if (!kept->files[i].used)
            f = &kept->files[i];
    if (!f)
        return ERR_NO_RESOURCES;
    unsigned i = slot_of(f);
    uint32_t gen = f->gen + 1;
    *f = (struct xf_file){
        .gen = gen, .flags = flags, .ino = in->ino, .igen = in->d.generation,
        .nochange = nochange,
    };
    fh[i].ch = fh[i].vmo = HANDLE_INVALID;
    fh[i].armed = false;
    f->used = true;
    status_t st = attach(f, out_ch, out_vmo);
    if (st != OK) {
        files_release(i);
        return st;
    }
    op_made(XF_MADE_FILE, i);
    *out_size = ext_isize(in);
    return OK;
}

static struct xf_chan chan_of(struct xf_file *f)
{
    unsigned i = slot_of(f);
    return (struct xf_chan){
        .ch = fh[i].ch, .id = XF_CHAN_FILE(i, f->gen), .proto = XF_PROTO_FILE, .file = f,
    };
}

/* Every file whose client has closed it: what it left queued, then the
 * close (before each fs request, so a close followed by an fs call is
 * seen in that order). The pager's requests first: a mapping let go, then
 * its file removed, is seen in that order too. */
void files_reap(void)
{
    pager_reap();
    for (unsigned i = 0; i < XF_MAX_FILES; i++) {
        struct xf_file *f = &kept->files[i];
        if (!f->used || jam_object_wait_one(fh[i].ch, SIG_PEER_CLOSED, 0, NULL) != OK)
            continue;
        const struct xf_chan c = chan_of(f);
        while (!vol.disk_gone && serve_one(&c) == OK)
            ;
        close_file(f);
    }
}

void files_event(uint64_t key)
{
    unsigned slot = XF_KEY_SLOT(key);
    if (slot >= XF_MAX_FILES)
        return;
    struct xf_file *f = &kept->files[slot];
    if (!f->used || f->gen != XF_KEY_GEN(key))
        return;
    fh[slot].armed = false;
    if (file_unread_full(fh[slot].ch)) {
        held[slot / 64] |= 1ull << (slot % 64);
        idl_hold(&vol.held);
        return;
    }
    const struct xf_chan c = chan_of(f);
    status_t st = serve_one(&c);
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(f);
    if (st != OK)
        close_file(f);
}

void files_unheld(void)
{
    for (unsigned i = 0; i < XF_MAX_FILES; i++) {
        struct xf_file *f = &kept->files[i];
        if (!(held[i / 64] >> (i % 64) & 1))
            continue;
        if (f->used && file_unread_full(fh[i].ch)) {
            idl_hold(&vol.held);
            continue;
        }
        held[i / 64] &= ~(1ull << (i % 64));
        if (f->used && arm(f) != OK)
            close_file(f);
    }
}

/* ---- a successor's files (adopt.c) ----------------------------------------------- */

const char *files_check(void)
{
    for (unsigned i = 0; i < XF_MAX_FILES; i++) {
        const struct xf_file *f = &kept->files[i];
        if (!f->used)
            continue;
        if (f->flags & ~(uint32_t)FS_FLAGS)
            return "an open file's flags out of range";
        struct ext_inode in;
        if (ext_iget(&vol.fs, f->ino, EXT_IGET_UNLINKED, &in) != OK ||
            in.d.generation != f->igen || (in.d.mode & EXT_S_IFMT) != EXT_S_IFREG)
            return "an open file that isn't on the volume";
    }
    return NULL;
}

bool files_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= XF_MAX_FILES || !kept->files[i].used || n != 2 || fh[i].ch || fh[i].vmo)
        return false;
    fh[i].ch = hs[0];
    fh[i].vmo = hs[1];
    fh[i].armed = false;
    return true;
}

unsigned files_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < XF_MAX_FILES; i++) {
        struct xf_file *f = &kept->files[i];
        if (!f->used)
            continue;
        if (!fh[i].ch || !fh[i].vmo || arm(f) != OK) {
            close_file(f);   /* not handed back: its client's end is closed */
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
        jam_handle_close(fh[i].ch);
    fh[i].ch = HANDLE_INVALID;
    status_t st = fh[i].vmo ? jam_handle_duplicate(fh[i].vmo, CLIENT_BUF_RIGHTS, &buf)
                            : ERR_BAD_STATE;
    if (st == OK)
        st = jam_channel_create(&fh[i].ch, &client);
    if (st == OK)
        st = kept_put(XF_KEEP_FILE(i), (const handle_t[]){ fh[i].ch, fh[i].vmo }, 2);
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

bool files_chan(uint32_t i, uint32_t gen16, struct xf_chan *out)
{
    if (i >= XF_MAX_FILES || !kept->files[i].used || !fh[i].ch || !fh[i].vmo ||
        (kept->files[i].gen & 0xffffu) != gen16)
        return false;
    *out = chan_of(&kept->files[i]);
    return true;
}
