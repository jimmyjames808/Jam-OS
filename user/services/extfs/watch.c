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
 * **Restarts** (2.8). The library's table, each channel's view (its flags
 * and root in the table's record, the root's place here) and each watched
 * file's directory are in the state VMO (struct xf_watches); each channel
 * is with the keeper from its fs.watch on (slot XF_KEEP_WATCH: the channel
 * alone), and with its bell once given (the two). A successor takes the
 * table over before its mount (watch_adopt_table), the handles back from
 * the keeper (watch_take), and waits on every channel again (watch_adopt):
 * each watcher's queue starts with IN_Q_OVERFLOW, its bell rung, and its
 * watches carry on. A channel whose bell the keeper doesn't hold (a bell
 * without RIGHT_DUPLICATE, which a keeper can't hold, or a death between
 * the give and its put) is closed then: its client sees the mount's
 * watches end (libc's IN_UNMOUNT and IN_IGNORED) and asks again. A fresh
 * start (the state given up) closes every channel the same way, and so
 * does the service's end (watch_stop), each bell rung first. The queued
 * events are the instance's memory, as are the open files' directories
 * and names (files.c): after a restart a file opened before it makes no
 * events in its directory's watch. */
#include "extfs.h"

_Static_assert(FSWATCH_REQ_MAX <= FS_REQ_MAX && FSWATCH_REP_MAX <= FS_REP_MAX,
               "fswatch's requests and replies fit the slots fs's do (state.c)");

#define EV_HELD 8u   /* events one request makes at most (a rename's exchange: 6) */

static struct fsw w;   /* over kept->watch.table */

/* A watcher channel's part here, by the library's number. */
static struct wchan {
    bool          armed;
    struct xf_ctx ctx;              /* the view it came from (its root's place: the state's) */
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

bool watching(void)
{
    return w.t && fsw_watching(&w);
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
    if (!watching() || len > EXT_NAME_MAX)
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
    if (!watching())
        return;
    const struct held e = { .gone = true, .self = *self };
    put(&e);
}

uint32_t ev_cookie(void)
{
    return watching() ? fsw_cookie(&w) : 0;
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

static uint32_t gen_of(unsigned i)
{
    return kept->watch.views[i].gen;
}

static status_t arm(unsigned i)
{
    status_t st = jam_port_bind(vol.port, w.chans[i].ch, XF_KEY_WATCH(i, gen_of(i)),
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
    wc[i].armed = st == OK;
    return st;
}

/* Channel i ends: its watches, its queue, its handles here and with the
 * keeper (its client sees the close: libc's IN_UNMOUNT and IN_IGNORED; the
 * library rings its bell first if the client is still there). */
static void end(unsigned i)
{
    if (wc[i].armed && w.chans[i].ch != HANDLE_INVALID)
        (void)jam_port_unbind(vol.port, w.chans[i].ch, XF_KEY_WATCH(i, gen_of(i)));
    wc[i].armed = false;
    held_bits &= ~(1u << i);
    fsw_chan_close(&w, i);
    kept_drop(XF_KEEP_WATCH(i));
}

static bool live(unsigned i)
{
    return w.t && i < FSW_CHANNELS && w.t->chans[i].live;
}

/* Channel i's view, from the state: the walks of its adds. */
static void ctx_of(unsigned i)
{
    const struct fsw_chan_rec *r = &w.t->chans[i];
    const struct xf_wview *v = &kept->watch.views[i];
    wc[i].ctx = (struct xf_ctx){
        .flags = r->view, .root_ino = (uint32_t)r->root, .root_igen = r->root_gen,
        .root = v->root, .root_len = v->root_len, .view = v->view,
    };
}

/* Channel i's bell given: kept with the channel (one slot), if the keeper
 * can hold it (a keeper takes only what it can duplicate and hand on). If
 * not, the slot keeps the channel alone, and a restart closes it. */
static void keep_bell(unsigned i)
{
    const rights_t need = RIGHT_DUPLICATE | RIGHT_TRANSFER;
    const handle_t hs[2] = { w.chans[i].ch, w.chans[i].bell };
    struct handle_info info;
    if (jam_handle_info(hs[1], &info) == OK && (info.rights & need) == need)
        (void)kept_put(XF_KEEP_WATCH(i), hs, 2);
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
    struct xf_wview *v = &kept->watch.views[i];
    v->gen++;
    v->view = c->view < 0 ? -1 : 0;   /* a view's root is checked by its generation */
    v->root_len = c->root_len;
    memcpy(v->root, c->root, c->root_len);
    v->root[c->root_len] = '\0';
    ctx_of(i);
    wc[i].armed = false;
    st = kept_put(XF_KEEP_WATCH(i), &w.chans[i].ch, 1);
    if (st == OK)
        st = arm(i);
    if (st != OK) {
        end(i);
        jam_handle_close(client);
        return st;
    }
    op_made(XF_MADE_WATCH, i);
    *out = client;
    return OK;
}

void watch_event(uint64_t key)
{
    unsigned i = XF_KEY_SLOT(key);
    if (!live(i) || gen_of(i) != XF_KEY_GEN(key))
        return;
    wc[i].armed = false;
    status_t st;
    if (!fsw_belled(&w, i)) {
        st = fsw_chan_bell(&w, i);   /* the first message: anything else ends it */
        if (st == OK)
            keep_bell(i);
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
    const struct xf_chan c = { .ch = w.chans[i].ch, .id = XF_CHAN_WATCH(i, gen_of(i)),
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

/* ---- starts, restarts and the end (2.8) ---------------------------------------- */

void watch_reset(void)
{
    for (unsigned i = 0; i < FSW_CHANNELS; i++)
        if (live(i))
            end(i);   /* a fresh start after the keeper's handles came back */
    memset(&kept->watch, 0, sizeof(kept->watch));
    fsw_init(&w, &kept->watch.table);
    memset(wc, 0, sizeof(wc));
    held_bits = 0;
}

/* The dead instance's record of channel i: its view in range. */
static bool view_ok(unsigned i)
{
    const struct fsw_chan_rec *r = &kept->watch.table.chans[i];
    const struct xf_wview *v = &kept->watch.views[i];
    return !(r->view & ~FS_VIEW_FLAGS) && r->root && r->root <= UINT32_MAX &&
           v->root_len < XF_ROOT_MAX && strnlen(v->root, v->root_len) == v->root_len &&
           (v->view == -1 || v->view == 0);
}

void watch_adopt_table(void)
{
    struct xf_watches *k = &kept->watch;
    bool ok = fsw_adopt(&w, &k->table);
    for (unsigned i = 0; ok && i < FSW_CHANNELS; i++)
        ok = !k->table.chans[i].live || view_ok(i);
    if (!ok) {
        printf("extfs %s: the watches couldn't be taken over: their channels close\n", vol.name);
        memset(k, 0, sizeof(*k));
        fsw_init(&w, &k->table);
    }
    memset(wc, 0, sizeof(wc));
    held_bits = 0;
    for (unsigned i = 0; i < FSW_CHANNELS; i++)
        if (live(i))
            ctx_of(i);
}

void watch_refused(const handle_t *hs, unsigned n)
{
    if (n == 2)
        (void)jam_event_signal(hs[1], 0, FSWATCH_BELL);   /* a poll wakes to see it closed */
}

bool watch_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (!live(i) || w.chans[i].ch != HANDLE_INVALID || n != (w.t->chans[i].belled ? 2u : 1u)) {
        watch_refused(hs, n);
        return false;
    }
    fsw_chan_handles(&w, i, hs[0], n == 2 ? hs[1] : HANDLE_INVALID);   /* the overflow rings */
    return true;
}

unsigned watch_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < FSW_CHANNELS; i++) {
        if (!live(i))
            continue;
        if (w.chans[i].ch == HANDLE_INVALID || arm(i) != OK)
            end(i);   /* its handles (or its bell) didn't come back */
        else
            n++;
    }
    return n;
}

bool watch_chan(uint32_t i, uint32_t gen16, struct xf_chan *out)
{
    if (!live(i) || w.chans[i].ch == HANDLE_INVALID || (gen_of(i) & 0xffffu) != gen16)
        return false;
    *out = (struct xf_chan){ .ch = w.chans[i].ch, .id = XF_CHAN_WATCH(i, gen_of(i)),
                             .proto = XF_PROTO_WATCH, .watch = &wc[i] };
    return true;
}

status_t watch_remake(uint32_t i, handle_t *out)
{
    handle_t mine = HANDLE_INVALID, client = HANDLE_INVALID;
    if (!live(i))
        return ERR_IO;   /* its record didn't come through */
    /* The end the dead instance kept has no client: the reply that held
     * the other died unsent. The channel never had its bell. */
    handle_t bell = w.chans[i].bell;
    if (w.chans[i].ch != HANDLE_INVALID)
        jam_handle_close(w.chans[i].ch);
    fsw_chan_handles(&w, i, HANDLE_INVALID, bell);
    status_t st = jam_channel_create(&mine, &client);
    if (st == OK) {
        fsw_chan_handles(&w, i, mine, bell);
        st = kept_put(XF_KEEP_WATCH(i), &mine, 1);
    }
    if (st != OK) {
        if (client != HANDLE_INVALID)
            jam_handle_close(client);
        end(i);
        return st;
    }
    *out = client;
    return OK;
}

void watch_unmake(uint32_t i)
{
    if (live(i))
        end(i);
}

void watch_stop(void)
{
    for (unsigned i = 0; i < FSW_CHANNELS; i++)
        if (live(i))
            end(i);
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
