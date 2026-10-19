/* extfs: directory channels (fs.opendir, docs/DIRHANDLE-PLAN.md 2.1 to
 * 2.5). A directory channel is an `fs` channel whose paths start at its
 * directory unless they start with '/' (walk.c), so a call names a place
 * by one short relative path however deep the directory lies.
 *
 * **Its view.** It keeps the view of the channel it was opened through, a
 * copy (flags, root and the root's generation): that channel may close
 * first, and a channel opened from a directory channel keeps that one's,
 * so it is never wider. ".." climbs to the view's root and stops there;
 * the view's flags are judged where each path resolves (walk.c's
 * regions), and the shared view code is given only the read-only bit
 * (views_lexical: a relative `etc` is not the volume's). view, subview and
 * watch are refused (ERR_NOT_SUPPORTED).
 *
 * **Where it lies.** Each channel keeps where its directory lies
 * (struct xf_place: the region, found when it is opened and again by
 * climbing after a rename moves a directory, walk.c's epoch). A directory
 * moved where the view doesn't reach (above a subview's root) or hides
 * (`etc/ssh` for a view that hides the keys) cuts the channel off: every
 * request ERR_NOT_FOUND while it lies there (the plan's Q2); moved back,
 * it works again.
 *
 * **Removed** (3.3: an rmdir, or a rename over an empty directory). Its
 * name goes and its parent's link count drops, but while a channel holds
 * it the inode stays, with no name, on the orphan list (on the disk, so a
 * cut frees it at the next mount), as an open unlinked file does
 * (files_held asks dirs_has): never freed, never used again, so a
 * directory made again at its name is another one. Each channel holding
 * it remembers the parent it was removed from (dirs_removed, in the state
 * before the removal commits). Its channel then answers "" and "." (getattr
 * with a link count of 0, stat, sync, statfs, opendir: W_GONE), ".." the
 * remembered parent (ERR_NOT_FOUND once that inode is free or used again;
 * a parent removed too and held answers as this one does), and
 * ERR_NOT_FOUND to everything else, readdir and creations too (walk.c). It
 * has no place, so no view hides it. Its last channel closing drops it
 * (let_go: defer_drop).
 *
 * **Limits.** XF_DIRS per volume, XF_DIRS_PROC from one process (the
 * opendir request's writer: a channel handed to another process still
 * counts as its opener's). Full: the channels whose clients closed them
 * and whose close the loop hasn't seen yet are let go first.
 *
 * **Restarts** (2.5). Each slot's record (the directory's inode and
 * generation, the view's flags and root, the opener, a removed one's
 * parent) is in the state (struct xf_dir); its channel here and with the
 * keeper (slot XF_KEEP_DIR), put before the request that made it commits.
 * A successor's mount keeps the removed directories its records hold
 * (recover_orphans asks files_held); it takes the channels back as
 * views.c's are (dirs_take, dirs_adopt), and climbs once for each at its
 * first request. A fresh start closes them, and its mount drops the
 * removed ones: the client sees ERR_PEER_CLOSED and opens the directory
 * again. */
#include "extfs.h"

static handle_t dir_ch[XF_DIRS];
static uint64_t held[(XF_DIRS + 63) / 64];   /* held back by the unread bound */
static struct xf_place dir_at[XF_DIRS];      /* where each directory lies ... */
static struct xf_place root_at[XF_DIRS];     /* ... and its view's root */

static void ctx_of(unsigned i, struct xf_ctx *c)
{
    const struct xf_dir *d = &kept->dirs[i];
    *c = (struct xf_ctx){
        .flags = d->flags, .root_ino = d->root_ino, .root_igen = d->root_igen,
        .root_at = &root_at[i], .view = d->view, .dir_ino = d->ino, .dir_igen = d->igen,
        .dir_at = &dir_at[i],
    };
}

static status_t arm(unsigned i)
{
    return jam_port_bind(vol.port, dir_ch[i], XF_KEY_DIR(i, kept->dirs[i].gen),
                         SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

static void close_channel(unsigned i)
{
    if (dir_ch[i] != HANDLE_INVALID)
        jam_handle_close(dir_ch[i]);
    dir_ch[i] = HANDLE_INVALID;
    kept_drop(XF_KEEP_DIR(i));
}

/* Directory ino's last channel went: removed, it goes now, or after the
 * running request's answer (serve_slot's defer_run). The state forgot the
 * slot first, so a death between leaves an orphan the next mount drops.
 * Never one freed already (its bitmap asked: ext_iget reads it as an
 * orphan), which ext would call damage if it were dropped again. */
static void let_go(uint32_t ino)
{
    struct ext_inode in;
    if (!vol.mounted || files_held(ino) ||
        ext_iget(&vol.fs, ino, EXT_IGET_UNLINKED, &in) != OK || in.d.links_count ||
        (in.d.mode & EXT_S_IFMT) != EXT_S_IFDIR || !inode_used(ino))
        return;
    defer_drop(ino);
    if (!op_running())
        defer_run();
}

void dirs_drop(uint32_t i)
{
    if (i >= XF_DIRS)
        return;
    bool was = kept->dirs[i].used;
    kept->dirs[i].used = false;
    held[i / 64] &= ~(1ull << (i % 64));
    close_channel(i);
    if (was)
        let_go(kept->dirs[i].ino);
}

void dirs_close_all(void)
{
    for (unsigned i = 0; i < XF_DIRS; i++)
        if (kept->dirs[i].used)
            dirs_drop(i);
}

bool dirs_has(uint32_t ino)
{
    for (unsigned i = 0; i < XF_DIRS; i++)
        if (kept->dirs[i].used && kept->dirs[i].ino == ino)
            return true;
    return false;
}

bool dirs_up(uint32_t ino, uint32_t gen, uint32_t *up, uint32_t *up_gen)
{
    for (unsigned i = 0; i < XF_DIRS; i++) {
        const struct xf_dir *d = &kept->dirs[i];
        if (d->used && d->ino == ino && d->igen == gen && d->up_ino) {
            *up = d->up_ino;
            *up_gen = d->up_igen;
            return true;
        }
    }
    return false;
}

void dirs_removed(const struct ext_inode *in, const struct ext_inode *parent)
{
    bool any = false;
    for (unsigned i = 0; i < XF_DIRS; i++) {
        struct xf_dir *d = &kept->dirs[i];
        if (d->used && d->ino == in->ino && d->igen == in->d.generation) {
            d->up_ino = parent->ino;
            d->up_igen = parent->d.generation;
            any = true;
        }
    }
    if (any)
        test_die(XF_DIE_REMOVED);
}

/* Every channel its client has closed, let go now (a close whose packet
 * still waits behind this request): did any go? */
static bool reap(void)
{
    bool any = false;
    for (unsigned i = 0; i < XF_DIRS; i++) {
        if (kept->dirs[i].used && dir_ch[i] != HANDLE_INVALID &&
            jam_object_wait_one(dir_ch[i], SIG_PEER_CLOSED, 0, NULL) == OK) {
            dirs_drop(i);
            any = true;
        }
    }
    return any;
}

/* A free slot for process pid, or -1 (XF_DIRS used, or pid's share). */
static int free_slot(uint32_t pid)
{
    int at = -1;
    unsigned mine = 0;
    for (unsigned i = 0; i < XF_DIRS; i++) {
        const struct xf_dir *d = &kept->dirs[i];
        if (d->used)
            mine += pid && d->pid == pid;
        else if (at < 0)
            at = (int)i;
    }
    return mine >= XF_DIRS_PROC ? -1 : at;
}

status_t dirs_open(const struct xf_ctx *c, const struct xf_walk *w, handle_t *out)
{
    uint32_t pid = op_pid();
    int i = free_slot(pid);
    if (i < 0 && reap())
        i = free_slot(pid);
    if (i < 0)
        return ERR_NO_RESOURCES;
    uint32_t up = 0, up_gen = 0;
    if (w->gone && !dirs_up(w->in.ino, w->in.d.generation, &up, &up_gen))
        return ERR_NOT_FOUND;   /* (walk.c's answer: one a channel holds) */
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    struct xf_dir *d = &kept->dirs[i];
    uint32_t gen = d->gen + 1;
    *d = (struct xf_dir){
        .gen = gen, .ino = w->in.ino, .igen = w->in.d.generation, .flags = c->flags,
        .root_ino = c->root_ino, .root_igen = c->root_igen, .view = c->view < 0 ? -1 : 0,
        .pid = pid, .up_ino = up, .up_igen = up_gen,   /* removed: the same ".." */
    };
    if (c->view < 0 && c->root_ino == EXT_ROOT_INO) {   /* the volume's root: its generation */
        struct ext_inode root;
        if (ext_iget(&vol.fs, EXT_ROOT_INO, 0, &root) == OK) {
            d->root_igen = root.d.generation;
            d->view = 0;
        }
    }
    dir_at[i] = walk_place_of(w);   /* found by the walk that reached it */
    root_at[i] = c->root_at ? *c->root_at : (struct xf_place){ 0 };
    d->used = true;
    test_die(XF_DIE_SLOT);
    dir_ch[i] = mine;
    st = kept_put(XF_KEEP_DIR(i), &mine, 1);
    if (st == OK)
        st = arm((unsigned)i);
    if (st != OK) {
        dirs_drop((uint32_t)i);
        jam_handle_close(theirs);
        return st;
    }
    op_made(XF_MADE_DIR, (uint32_t)i);
    *out = theirs;
    return OK;
}

status_t dirs_of_id(const struct xf_ctx *c, uint64_t id, struct xf_ctx *out, struct xf_place *at)
{
    struct ext_inode in;
    if (!id || id > UINT32_MAX)
        return ERR_NOT_FOUND;
    status_t st = ext_iget(&vol.fs, (uint32_t)id, 0, &in);
    if (st != OK)
        return st == ERR_IO ? ERR_IO : ERR_NOT_FOUND;   /* free, out of range */
    if ((in.d.mode & EXT_S_IFMT) != EXT_S_IFDIR)
        return ERR_NOT_FOUND;
    *at = (struct xf_place){ 0 };
    st = walk_place(at, in.ino, in.d.generation, c->root_ino, c->view >= 0 ? c->root_igen : 0);
    if (st == OK && fs_region_hides(c->flags, at->region, false))
        st = ERR_NOT_FOUND;   /* a place the view hides: it doesn't reach it */
    if (st != OK)
        return st;
    *out = *c;
    out->dir_ino = in.ino;
    out->dir_igen = in.d.generation;
    out->dir_at = at;
    return OK;
}

uint64_t dirs_volume(void)
{
    uint64_t a, b;
    memcpy(&a, vol.fs.sb.uuid, 8);
    memcpy(&b, vol.fs.sb.uuid + 8, 8);
    return a ^ b;
}

status_t dirs_no_view(void *host, handle_t ch, uint32_t flags)
{
    (void)host;
    (void)flags;
    jam_handle_close(ch);
    return ERR_NOT_SUPPORTED;
}

bool dirs_chan(uint32_t i, uint32_t gen16, struct xf_chan *out)
{
    if (i >= XF_DIRS || !kept->dirs[i].used || !dir_ch[i] ||
        (kept->dirs[i].gen & 0xffffu) != gen16)
        return false;
    *out = (struct xf_chan){
        .ch = dir_ch[i], .id = XF_CHAN_DIR(i, kept->dirs[i].gen), .proto = XF_PROTO_FS,
    };
    ctx_of(i, &out->ctx);
    return true;
}

void dirs_event(uint64_t key)
{
    unsigned i = XF_KEY_SLOT(key);
    if (i >= XF_DIRS || !kept->dirs[i].used || kept->dirs[i].gen != XF_KEY_GEN(key))
        return;
    struct xf_chan c;
    if (!dirs_chan(i, kept->dirs[i].gen & 0xffffu, &c))
        return;
    if (fs_unread_full(c.ch)) {
        held[i / 64] |= 1ull << (i % 64);
        idl_hold(&vol.held);
        return;
    }
    files_reap();
    status_t st = serve_one(&c);
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(i);
    if (st != OK)
        dirs_drop(i);   /* closed: a removed directory goes with its last channel */
}

void dirs_unheld(void)
{
    for (unsigned i = 0; i < XF_DIRS; i++) {
        if (!(held[i / 64] >> (i % 64) & 1))
            continue;
        if (fs_unread_full(dir_ch[i])) {
            idl_hold(&vol.held);
            continue;
        }
        held[i / 64] &= ~(1ull << (i % 64));
        if (arm(i) != OK)
            dirs_drop(i);
    }
}

/* ---- a successor's channels (adopt.c) ----------------------------------------------- */

const char *dirs_check(void)
{
    for (unsigned i = 0; i < XF_DIRS; i++) {
        const struct xf_dir *d = &kept->dirs[i];
        if (d->used && ((d->flags & ~FS_VIEW_FLAGS) || !d->ino || !d->root_ino ||
                        (d->view != -1 && d->view != 0)))
            return "a directory channel out of range";
    }
    return NULL;
}

bool dirs_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= XF_DIRS || !kept->dirs[i].used || n != 1 || dir_ch[i])
        return false;
    dir_ch[i] = hs[0];
    return true;
}

void dirs_drop_unknown(void)
{
    for (unsigned i = 0; i < XF_DIRS; i++)
        if (!kept->dirs[i].used && dir_ch[i] != HANDLE_INVALID)
            close_channel(i);
}

unsigned dirs_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < XF_DIRS; i++) {
        if (!kept->dirs[i].used)
            continue;
        if (!dir_ch[i] || arm(i) != OK)
            dirs_drop(i);
        else
            n++;
    }
    return n;
}

status_t dirs_remake(uint32_t i, handle_t *out)
{
    handle_t mine = HANDLE_INVALID, client = HANDLE_INVALID;
    if (i >= XF_DIRS)
        return ERR_INVALID_ARGS;
    if (dir_ch[i])
        jam_handle_close(dir_ch[i]);
    dir_ch[i] = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &client);
    if (st == OK) {
        dir_ch[i] = mine;
        st = kept_put(XF_KEEP_DIR(i), &mine, 1);
    }
    if (st != OK) {
        if (client)
            jam_handle_close(client);
        dirs_drop(i);
        return st;
    }
    *out = client;
    return OK;
}
