/* extfs: file watching (docs/INOTIFY-PLAN.md sections 2.1 to 2.3), on the
 * library every file service shares (<fswatch.h>: the watcher channels,
 * the watches by inode, the queues, the ids, `take`'s records).
 *
 * **Channels.** fs.watch makes a watcher channel through the view it came
 * on, which keeps a copy of that view (its flags, its root and the root's
 * place: the view's own channel may close first) and judges its adds by
 * it. Its first message is the bell (fswatch_bell.give); then each
 * request (add, remove, take) is read into a request slot and run as
 * every other request is (request.c), after the order rule (main.c's
 * order_closes: a close made before the request was sent is handled
 * first). `add` walks the path in the channel's view as an fs request
 * would (links inside followed, ERR_LINK_OUT for one going out,
 * IN_DONT_FOLLOW as FS_NOFOLLOW), needs the read bit (Linux's rule), and
 * IN_ONLYDIR a directory.
 *
 * **Events.** fsops.c and files.c say what each request changed, in the
 * order of the plan's 2.2 (Linux's); request.c's begin and commit bracket
 * them: they wait here until the request's transaction commits and are
 * dropped if it is given up, so a watcher never hears of a change that
 * isn't on the disk. A close's events and an inode's going (defer_run,
 * after the answer) come outside requests, at once.
 *
 * The table is this instance's memory: a restart ends every watch (the
 * watcher channels die with the instance, and the client's end says so).
 * IE2 keeps it in the state VMO and the channels with the keeper. */
#include "extfs.h"

_Static_assert(FSWATCH_REQ_MAX <= FS_REQ_MAX && FSWATCH_REP_MAX <= FS_REP_MAX,
               "fswatch's requests and replies fit the slots fs's do (state.c)");

#define EV_HELD 8u   /* events one request makes at most (a rename's exchange: 6) */

static struct fsw_table table;
static struct fsw       w;

/* A watcher channel's part here, by the library's number. */
static struct wchan {
    uint32_t      gen;              /* its port key's, bumped at each fs.watch */
    bool          armed;
    struct xf_ctx ctx;              /* the view it came from (root -> root below) */
    char          root[XF_ROOT_MAX];
} wc[FSW_CHANNELS];
static uint32_t held_bits;          /* held back by the unread bound */
_Static_assert(FSW_CHANNELS <= 32, "held_bits has a bit for each");

/* An event waiting for its request's commit. */
static struct held {
    bool           gone;            /* ev_gone's */
    bool           has_dir, has_self;
    struct fsw_key dir, self;
    uint32_t       mask, flags, cookie;
    uint32_t       len;
    char           name[EXT_NAME_MAX];
} held[EV_HELD];
static unsigned nheld;
static bool     holding;            /* a request is running: events wait */

void watch_start(void)
{
    fsw_init(&w, &table);
}

bool watching(void)
{
    return fsw_watching(&w);
}

/* ---- events ------------------------------------------------------------------- */

static void deliver(const struct held *e)
{
    if (e->gone) {
        fsw_gone(&w, &e->self);
        return;
    }
    if (e->has_dir)
        fsw_dir_event(&w, &e->dir, e->name, e->len, e->mask, e->cookie, e->flags);
    if (e->has_self)
        fsw_self_event(&w, &e->self, e->mask, e->flags);
}

static void put(const struct held *e)
{
    if (!holding) {
        deliver(e);
        return;
    }
    if (nheld < EV_HELD) {
        held[nheld++] = *e;
        return;
    }
    /* More than a request makes: what is held goes now, in order. */
    for (unsigned i = 0; i < nheld; i++)
        deliver(&held[i]);
    nheld = 0;
    deliver(e);
}

void ev(const struct fsw_key *dir, const char *name, uint32_t len, const struct fsw_key *self,
        uint32_t mask, uint32_t flags, uint32_t cookie)
{
    if (!fsw_watching(&w) || len > EXT_NAME_MAX)
        return;
    struct held e = { .has_dir = dir != NULL, .has_self = self != NULL, .mask = mask,
                      .flags = flags, .cookie = cookie, .len = dir ? len : 0 };
    if (dir) {
        e.dir = *dir;
        memcpy(e.name, name, len);
    }
    if (self)
        e.self = *self;
    put(&e);
}

void ev_gone(const struct fsw_key *self)
{
    if (!fsw_watching(&w))
        return;
    const struct held e = { .gone = true, .self = *self };
    put(&e);
}

uint32_t ev_cookie(void)
{
    return fsw_watching(&w) ? fsw_cookie(&w) : 0;
}

uint32_t ev_attr_mask(uint32_t mask)
{
    const uint32_t times = FS_ATTR_ATIME | FS_ATTR_MTIME;
    uint32_t m = mask & (FS_ATTR_MODE | FS_ATTR_UID | FS_ATTR_GID) ? FSWATCH_IN_ATTRIB : 0;
    if ((mask & times) == times)
        m |= FSWATCH_IN_ATTRIB;    /* both times: a utime call */
    else if (mask & FS_ATTR_ATIME)
        m |= FSWATCH_IN_ACCESS;
    else if (mask & FS_ATTR_MTIME)
        m |= FSWATCH_IN_MODIFY;
    return m;
}

void ev_begin(void)
{
    holding = true;
    nheld = 0;
}

void ev_settle(bool done)
{
    holding = false;
    for (unsigned i = 0; done && i < nheld; i++)
        deliver(&held[i]);
    nheld = 0;
}

/* ---- the watcher channels ------------------------------------------------------ */

static unsigned index_of(const struct wchan *c)
{
    return (unsigned)(c - wc);
}

static status_t arm(unsigned i)
{
    status_t st = jam_port_bind(vol.port, w.chans[i].ch, XF_KEY_WATCH(i, wc[i].gen),
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
    wc[i].armed = st == OK;
    return st;
}

/* Channel i ends: its watches, its queue, its handles (its client sees the
 * close: libc's IN_UNMOUNT and IN_IGNORED). */
static void end(unsigned i)
{
    if (wc[i].armed && w.chans[i].ch != HANDLE_INVALID)
        (void)jam_port_unbind(vol.port, w.chans[i].ch, XF_KEY_WATCH(i, wc[i].gen));
    wc[i].armed = false;
    held_bits &= ~(1u << i);
    fsw_chan_close(&w, i);
}

static bool live(unsigned i)
{
    return i < FSW_CHANNELS && w.t->chans[i].live;
}

/* Every watcher channel its client has closed, ended now: a close made
 * before this fs.watch, whose packet may still wait behind it, frees its
 * share first (a program that closes its inotify descriptor and makes
 * another). */
static bool reap(void)
{
    bool any = false;
    for (unsigned i = 0; i < FSW_CHANNELS; i++) {
        if (live(i) && w.chans[i].ch != HANDLE_INVALID &&
            jam_object_wait_one(w.chans[i].ch, SIG_PEER_CLOSED, 0, NULL) == OK) {
            end(i);
            any = true;
        }
    }
    return any;
}

status_t watch_new(const struct xf_ctx *c, handle_t *out)
{
    handle_t mine = HANDLE_INVALID, client = HANDLE_INVALID;
    if (c->root_len >= XF_ROOT_MAX)
        return ERR_NAME_TOO_LONG;
    const struct fsw_key root = { .ino = c->root_ino, .gen = c->root_igen };
    uint32_t i = 0;
    status_t st = ERR_NO_RESOURCES;
    for (unsigned tries = 0; st == ERR_NO_RESOURCES && tries < 2; tries++) {
        if (tries && !reap())
            break;
        st = jam_channel_create(&mine, &client);
        if (st != OK)
            return st;
        st = fsw_chan_new(&w, op_pid(), c->flags, &root, mine, &i);   /* mine: the library's */
        if (st != OK)
            jam_handle_close(client);
    }
    if (st != OK)
        return st;
    struct wchan *k = &wc[i];
    k->gen++;
    k->armed = false;
    memcpy(k->root, c->root, c->root_len);
    k->root[c->root_len] = '\0';
    k->ctx = *c;
    k->ctx.root = k->root;
    k->ctx.view = c->view < 0 ? -1 : 0;   /* a view's root is checked by its generation */
    st = arm(i);
    if (st != OK) {
        fsw_chan_close(&w, i);
        jam_handle_close(client);
        return st;
    }
    *out = client;
    return OK;
}

void watch_event(uint64_t key)
{
    unsigned i = XF_KEY_SLOT(key);
    if (!live(i) || wc[i].gen != XF_KEY_GEN(key))
        return;
    wc[i].armed = false;
    status_t st;
    if (!fsw_belled(&w, i)) {
        st = fsw_chan_bell(&w, i);   /* the first message: anything else ends it */
        if (st == OK || st == ERR_SHOULD_WAIT)
            st = arm(i);
        if (st != OK)
            end(i);
        return;
    }
    if (fswatch_unread_full(w.chans[i].ch)) {
        held_bits |= 1u << i;
        idl_hold(&vol.held);
        return;
    }
    order_closes();   /* a close made before this request was sent: first */
    const struct xf_chan c = { .ch = w.chans[i].ch, .id = XF_CHAN_WATCH(i, wc[i].gen),
                               .proto = XF_PROTO_WATCH, .watch = &wc[i] };
    st = serve_one(&c);
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm(i);
    if (st != OK)
        end(i);
}

void watch_unheld(void)
{
    for (unsigned i = 0; i < FSW_CHANNELS; i++) {
        if (!(held_bits >> i & 1))
            continue;
        if (live(i) && fswatch_unread_full(w.chans[i].ch)) {
            idl_hold(&vol.held);
            continue;
        }
        held_bits &= ~(1u << i);
        if (live(i) && arm(i) != OK)
            end(i);
    }
}

/* ---- the fswatch methods (abi/idl/fswatch.idl) --------------------------------- */

static struct xf_walk aw;

static status_t op_add(void *ctx, const char *path, uint32_t path_len, uint32_t mask,
                       uint32_t *out_id, uint8_t *out_existed)
{
    const struct wchan *k = ctx;
    status_t st = fsw_mask_check(mask);   /* Linux checks the mask before the path */
    if (st != OK)
        return st;
    st = walk(&k->ctx, path, path_len, mask & FSWATCH_IN_DONT_FOLLOW ? W_NOFOLLOW : 0, &aw);
    if (st == OK && walk_hidden(&k->ctx, &aw, false))
        st = ERR_ACCESS_DENIED;
    if (st == OK && !aw.found)
        st = ERR_NOT_FOUND;
    if (st != OK)
        return st;
    bool dir = (aw.in.d.mode & EXT_S_IFMT) == EXT_S_IFDIR;
    if ((aw.must_dir || (mask & FSWATCH_IN_ONLYDIR)) && !dir)
        return ERR_NOT_DIR;
    if (!may(&aw.in, 4))
        return ERR_ACCESS_DENIED;   /* a watch needs the read bit, as a listing does */
    const struct fsw_key key = xf_key(&aw.in);
    return fsw_add(&w, index_of(k), &key, dir, mask, out_id, out_existed);
}

static status_t op_remove(void *ctx, uint32_t id)
{
    return fsw_remove(&w, index_of(ctx), id);
}

static status_t op_take(void *ctx, uint32_t room, uint8_t *out_more, uint8_t *out_events,
                        uint32_t *out_events_len)
{
    return fsw_take(&w, index_of(ctx), room, out_more, out_events, out_events_len);
}

const struct fswatch_ops xf_watch_ops = { .add = op_add, .remove = op_remove, .take = op_take };
