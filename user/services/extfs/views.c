/* extfs: views (fs.view, <fsview.h>) and subviews (fs.subview): narrower
 * `fs` channels onto the volume. A view has flags (read-only, `etc`
 * guarded or hidden) and a root: the volume's, or a directory's for a
 * subview, above which nothing can be named ("..", and links, stop there:
 * walk.c), so a mount of /jam/usr as /usr reaches nothing above it.
 *
 * Each request on a view goes through the views' shared code
 * (fs_view_dispatch: the lexical first line, on the path as written) and
 * then walk.c's judgment on where the path resolves, with the view's
 * flags. A subview's paths are relative to its root, which the shared
 * code's checks of `etc` (the volume's top-level directory) can't know
 * about: they are given only the read-only bit (views_lexical), as a
 * directory channel's are, and walk.c judges `etc` on the region
 * (<fsview.h>) its walk keeps, which starts at the root's: where the root
 * lies, found by climbing from it (walk_place) and found again after a
 * rename moves a directory, so a root renamed into `etc` is judged there.
 * A new channel made from a view never has less than its flags: fs.view's
 * add ORs in the parent's.
 *
 * Each slot's flags, generation and root are in the state; its channel
 * here and with the keeper. Dropping a view is one store (its `used`). */
#include "extfs.h"

static handle_t view_ch[XF_VIEWS];
static uint64_t held[(XF_VIEWS + 63) / 64];   /* held back by the unread bound */
static struct xf_place view_at[XF_VIEWS];     /* where each root lies */

void views_whole(struct xf_ctx *c)
{
    *c = (struct xf_ctx){ .flags = 0, .root_ino = EXT_ROOT_INO, .view = -1 };
}

uint32_t views_lexical(const struct xf_ctx *c)
{
    return c->root_ino != EXT_ROOT_INO || c->dir_ino ? c->flags & FS_VIEW_READ_ONLY : c->flags;
}

static void ctx_of(unsigned i, struct xf_ctx *c)
{
    const struct xf_view *v = &kept->views[i];
    *c = (struct xf_ctx){
        .flags = v->flags, .root_ino = v->root_ino, .root_igen = v->root_igen,
        .root_at = &view_at[i], .view = (int)i,
    };
}

static status_t arm(unsigned i)
{
    return jam_port_bind(vol.port, view_ch[i], XF_KEY_VIEW(i, kept->views[i].gen),
                         SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

static void close_channel(unsigned i)
{
    if (view_ch[i] != HANDLE_INVALID)
        jam_handle_close(view_ch[i]);
    view_ch[i] = HANDLE_INVALID;
    kept_drop(XF_KEEP_VIEW(i));
}

void views_drop(uint32_t i)
{
    if (i >= XF_VIEWS)
        return;
    kept->views[i].used = false;
    held[i / 64] &= ~(1ull << (i % 64));
    close_channel(i);
}

/* A new view of `flags` rooted where parent is rooted, or at w's directory
 * (w not NULL), served on ch (consumed). */
static status_t add(const struct xf_ctx *parent, const struct xf_walk *w, handle_t ch,
                    uint32_t flags)
{
    for (unsigned i = 0; i < XF_VIEWS; i++) {
        struct xf_view *v = &kept->views[i];
        if (v->used)
            continue;
        uint32_t gen = v->gen + 1;
        *v = (struct xf_view){ .flags = parent->flags | flags, .gen = gen };
        view_at[i] = parent->root_at ? *parent->root_at : (struct xf_place){ 0 };
        if (w) {
            v->root_ino = w->in.ino;
            v->root_igen = w->in.d.generation;
            view_at[i] = walk_place_of(w);
        } else {
            v->root_ino = parent->root_ino;
            v->root_igen = parent->view >= 0 ? parent->root_igen : 0;
            if (parent->view < 0) {   /* the volume's root: its generation, for the check */
                struct ext_inode root;
                if (ext_iget(&vol.fs, EXT_ROOT_INO, 0, &root) == OK)
                    v->root_igen = root.d.generation;
            }
        }
        v->used = true;
        view_ch[i] = ch;
        status_t st = kept_put(XF_KEEP_VIEW(i), &ch, 1);
        if (st == OK)
            st = arm(i);
        if (st != OK)
            views_drop(i);
        else
            op_made(XF_MADE_VIEW, i);
        return st;
    }
    jam_handle_close(ch);
    return ERR_NO_RESOURCES;
}

status_t views_add(void *host, handle_t ch, uint32_t flags)
{
    return add(host, NULL, ch, flags);
}

status_t views_sub(const struct xf_ctx *parent, const struct xf_walk *w, uint32_t flags,
                   handle_t *out)
{
    /* A root at or under etc: only where the shared code would allow it
     * for that place (fs_view_check's subview rule, judged where it
     * resolves). */
    uint32_t pf = parent->flags;
    if (fs_region_in_etc(w->region) &&
        ((pf & FS_VIEW_HIDE_KEYS) ||
         ((pf & FS_VIEW_GUARD_ETC) && !((pf | flags) & FS_VIEW_READ_ONLY))))
        return ERR_ACCESS_DENIED;
    if (fs_region_in_pkg(w->region) && (pf & FS_VIEW_GUARD_PKG) &&
        !((pf | flags) & FS_VIEW_READ_ONLY))
        return ERR_ACCESS_DENIED;   /* the same for pkg */
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = add(parent, w, mine, flags);
    if (st != OK) {
        jam_handle_close(theirs);
        return st;
    }
    *out = theirs;
    return OK;
}

void views_drop_unknown(void)
{
    for (unsigned i = 0; i < XF_VIEWS; i++)
        if (!kept->views[i].used && view_ch[i] != HANDLE_INVALID)
            close_channel(i);
}

bool views_chan(uint32_t i, uint32_t gen16, struct xf_chan *out)
{
    if (i >= XF_VIEWS || !kept->views[i].used || !view_ch[i] ||
        (kept->views[i].gen & 0xffffu) != gen16)
        return false;
    *out = (struct xf_chan){
        .ch = view_ch[i], .id = XF_CHAN_VIEW(i, kept->views[i].gen), .proto = XF_PROTO_FS,
    };
    ctx_of(i, &out->ctx);
    return true;
}

void views_event(uint64_t key)
{
    unsigned i = XF_KEY_SLOT(key);
    if (i >= XF_VIEWS || !kept->views[i].used || kept->views[i].gen != XF_KEY_GEN(key))
        return;
    struct xf_chan c;
    if (!views_chan(i, kept->views[i].gen & 0xffffu, &c))
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
        views_drop(i);
}

void views_unheld(void)
{
    for (unsigned i = 0; i < XF_VIEWS; i++) {
        if (!(held[i / 64] >> (i % 64) & 1))
            continue;
        if (fs_unread_full(view_ch[i])) {
            idl_hold(&vol.held);
            continue;
        }
        held[i / 64] &= ~(1ull << (i % 64));
        if (arm(i) != OK)
            views_drop(i);
    }
}

/* ---- a successor's views (adopt.c) -------------------------------------------------- */

const char *views_check(void)
{
    for (unsigned i = 0; i < XF_VIEWS; i++) {
        const struct xf_view *v = &kept->views[i];
        if (v->used && ((v->flags & ~FS_VIEW_FLAGS) || !v->root_ino))
            return "a view out of range";
    }
    return NULL;
}

bool views_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= XF_VIEWS || !kept->views[i].used || n != 1 || view_ch[i])
        return false;
    view_ch[i] = hs[0];
    return true;
}

unsigned views_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < XF_VIEWS; i++) {
        if (!kept->views[i].used)
            continue;
        if (!view_ch[i] || arm(i) != OK)
            views_drop(i);
        else
            n++;
    }
    return n;
}

status_t views_remake(uint32_t i, handle_t *out)
{
    handle_t mine = HANDLE_INVALID, client = HANDLE_INVALID;
    if (view_ch[i])
        jam_handle_close(view_ch[i]);
    view_ch[i] = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &client);
    if (st == OK) {
        view_ch[i] = mine;
        st = kept_put(XF_KEEP_VIEW(i), &mine, 1);
    }
    if (st != OK) {
        if (client)
            jam_handle_close(client);
        views_drop(i);
        return st;
    }
    *out = client;
    return OK;
}
