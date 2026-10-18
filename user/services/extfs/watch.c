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
 * watches carry on. A rename's reach check, held for its commit, is in the
 * state too until it ran: a successor that answers the rename from the
 * journal runs it (watch_moves_again). A channel whose bell the keeper
 * doesn't hold (a bell without RIGHT_DUPLICATE, which a keeper can't hold,
 * or a death between the give and its put) is closed then: its client sees the mount's
 * watches end (libc's IN_UNMOUNT and IN_IGNORED) and asks again. A fresh
 * start (the state given up) closes every channel the same way, and so
 * does the service's end (watch_stop), each bell rung first. The queued
 * events are the instance's memory, as are the open files' directories
 * and names (files.c): after a restart a file opened before it makes no
 * events in its directory's watch. */
#include "extfs.h"

_Static_assert(FSWATCH_REQ_MAX <= FS_REQ_MAX && FSWATCH_REP_MAX <= FS_REP_MAX,
               "fswatch's requests and replies fit the slots fs's do (state.c)");

#define EV_HELD 8u   /* events one request makes at most (a rename's exchange: 6, and
                      * its two moves' reach checks) */

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
    bool           moved;           /* watch_moved's: self moved from `from` into dir */
    bool           has_dir, has_self;
    struct fsw_key dir, self, from;
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

/* ---- what a view hides (2.6) ---------------------------------------------------- */

/* The places the hiding flags look at (<fsview.h>): the volume's top-level
 * `etc` (any entry of the root fs_view_in_etc calls etc, of any kind), and
 * in it `ssh` and `authorized_keys`. They are known by their inodes, found
 * by reading the root and each etc again after a name in them changed
 * (ev() marks it; so does the first watch, since no event is made while
 * nothing is watched). The views judge where a path lies, so these do too:
 * an event never names an entry of a watched directory that the watcher's
 * view hides from a listing, and a watch whose inode a rename takes out
 * of its view's reach ends (Q2). */
#define SPECIAL_MAX 16u   /* more etc-like names than this: the rest unknown */
enum { SP_NONE, SP_ETC, SP_SSH, SP_AUTH };
static struct special {
    struct fsw_key key;
    unsigned       kind;
} special[SPECIAL_MAX];
static unsigned nspecial;
static bool     special_ok;   /* found since the last change of a name in them */

static bool is_dir(const struct ext_inode *in)
{
    return (in->d.mode & EXT_S_IFMT) == EXT_S_IFDIR;
}

static bool same_key(const struct fsw_key *a, const struct fsw_key *b)
{
    return a->ino == b->ino && a->gen == b->gen;
}

/* What k is of those (as last found). */
static unsigned kind_of(const struct fsw_key *k)
{
    for (unsigned i = 0; i < nspecial; i++)
        if (same_key(&special[i].key, k))
            return special[i].kind;
    return SP_NONE;
}

static bool dots(const struct ext_dentry *e)
{
    return e->name[0] == '.' && (e->name_len == 1 || (e->name_len == 2 && e->name[1] == '.'));
}

static void special_add(uint32_t ino, unsigned kind)
{
    struct ext_inode in;
    if (nspecial < SPECIAL_MAX && ext_iget(&vol.fs, ino, 0, &in) == OK)
        special[nspecial++] = (struct special){ .key = xf_key(&in), .kind = kind };
}

static void special_find(void)
{
    static struct ext_dentry e, f;
    static char place[EXT_NAME_MAX + 8];
    struct ext_fs *fs = &vol.fs;
    struct ext_inode root, etc;
    uint64_t pos = 0;
    nspecial = 0;
    special_ok = true;
    if (ext_iget(fs, EXT_ROOT_INO, 0, &root) != OK)
        return;
    while (ext_dir_next(fs, &root, &pos, &e) == OK && e.ino) {
        if (dots(&e) || !fs_view_in_etc(e.name))
            continue;
        special_add(e.ino, SP_ETC);
        uint64_t at = 0;
        if (ext_iget(fs, e.ino, 0, &etc) != OK || !is_dir(&etc))
            continue;
        while (ext_dir_next(fs, &etc, &at, &f) == OK && f.ino) {
            if (dots(&f))
                continue;
            snprintf(place, sizeof(place), "etc/%s", f.name);
            if (fs_view_in_keys(place))
                special_add(f.ino, SP_SSH);
            else if (!fs_view_hides(FS_VIEW_HIDE_ETC, place, false))
                special_add(f.ino, SP_AUTH);   /* the one name HIDE_ETC leaves */
        }
    }
}

/* The library's hook: does the view of channel rec hide entry `name` of
 * directory dir from a listing? Only the root's entries, an etc's and
 * those of etc's ssh and authorized_keys can be hidden from a view that
 * may watch their directory. */
static bool hidden(void *ctx, const struct fsw_chan_rec *rec, const struct fsw_key *dir,
                   const char *name, uint32_t len)
{
    static char place[EXT_NAME_MAX + 32];
    (void)ctx;
    if (!(rec->view & FS_VIEW_NO_ETC))
        return false;
    if (!special_ok)
        special_find();
    const char *in = "";
    if (dir->ino != EXT_ROOT_INO) {
        unsigned k = kind_of(dir);
        in = k == SP_ETC ? "etc/" : k == SP_SSH ? "etc/ssh/" : k == SP_AUTH ? "etc/authorized_keys/"
                                                                            : NULL;
        if (!in)
            return false;
    }
    snprintf(place, sizeof(place), "%s%.*s", in, (int)len, name);
    return fs_view_hides(rec->view, place, true);
}

/* ---- the reach (Q2) --------------------------------------------------------------- */

#define CHAIN_MAX  2048u    /* directories deeper than this: not placed (a judged watch ends) */
#define WALK_STEPS 16384u   /* directories one channel's watches may read in one move's
                             * check: past it, its watches left to judge end */
#define NOT_UNDER  4096u    /* directories a check found not under the moved inode (a
                             * power of two; filled to half) */

static struct fsw_key chain_to[CHAIN_MAX], chain_from[CHAIN_MAX], chain_w[CHAIN_MAX];
static uint32_t       not_under[NOT_UNDER];   /* inode numbers; 0: free */
static unsigned       nnot_under;

/* Directory in's parent, from its ".." (an entry of its first block): 0 if
 * not found. */
static uint32_t parent_of(struct ext_inode *in)
{
    static struct ext_dentry e;
    uint64_t pos = 0;
    uint32_t ino = 0;
    for (unsigned i = 0; i < 2 && ext_dir_next(&vol.fs, in, &pos, &e) == OK && e.ino; i++)
        if (e.name_len == 2 && e.name[0] == '.' && e.name[1] == '.')
            ino = e.ino;
    return ino;
}

/* The directories from (ino, gen) up to the volume's root, the root last:
 * how many, or 0 if it can't be followed (gone, its inode used again, an
 * error, too deep). gen 0: not checked. */
static unsigned chain_of(uint32_t ino, uint32_t gen, struct fsw_key *out)
{
    struct ext_inode in;
    for (unsigned n = 0; n < CHAIN_MAX; n++) {
        if (!ino || ext_iget(&vol.fs, ino, 0, &in) != OK || (gen && in.d.generation != gen) ||
            !is_dir(&in))
            return 0;
        out[n] = xf_key(&in);
        if (ino == EXT_ROOT_INO)
            return n + 1;
        ino = parent_of(&in);
        gen = 0;
    }
    return 0;
}

static uint32_t nu_hash(uint32_t ino)
{
    return (ino * 0x9e3779b1u) & (NOT_UNDER - 1);
}

static bool nu_has(uint32_t ino)
{
    for (uint32_t h = nu_hash(ino); not_under[h]; h = (h + 1) & (NOT_UNDER - 1))
        if (not_under[h] == ino)
            return true;
    return false;
}

/* The first k of chain_w are not under the moved inode. */
static void nu_add(unsigned k)
{
    for (unsigned i = 0; i < k && nnot_under < NOT_UNDER / 2; i++) {
        uint32_t ino = (uint32_t)chain_w[i].ino, h = nu_hash(ino);
        while (not_under[h] && not_under[h] != ino)
            h = (h + 1) & (NOT_UNDER - 1);
        if (!not_under[h]) {
            not_under[h] = ino;
            nnot_under++;
        }
    }
}

enum { AT_NOT, AT_UNDER, AT_UNKNOWN };

/* Where directory (ino, gen) lies against x, a directory just moved into
 * `to` (chain_to: nto of them): x or under it (AT_UNDER: chain_w holds its
 * chain up to the volume's root, *n of them: its own part up to x, then
 * to's), not under it (AT_NOT: its walk up met stop, the volume's root or
 * a directory found not under x before), or AT_UNKNOWN (gone, unreadable,
 * deeper than CHAIN_MAX, or the *left reads spent). Every directory it
 * finds not under x is kept (not_under), so watches sharing a path walk
 * it once. stop: a subview's root that x left (never under x). */
static int where(uint32_t ino, uint32_t gen, const struct fsw_key *x, const struct fsw_key *stop,
                 unsigned nto, unsigned *left, unsigned *n)
{
    struct ext_inode in;
    for (unsigned k = 0;; k++) {
        if (k && nu_has(ino)) {
            nu_add(k);
            return AT_NOT;
        }
        if (k == CHAIN_MAX || !*left || !ino)
            return AT_UNKNOWN;
        (*left)--;
        if (ext_iget(&vol.fs, ino, 0, &in) != OK || (gen && in.d.generation != gen) ||
            !is_dir(&in))
            return AT_UNKNOWN;
        chain_w[k] = xf_key(&in);
        if (same_key(&chain_w[k], x)) {
            if (!nto || k + 1 + nto > CHAIN_MAX)
                return AT_UNKNOWN;
            memcpy(&chain_w[k + 1], chain_to, nto * sizeof(chain_w[0]));
            *n = k + 1 + nto;
            return AT_UNDER;
        }
        if (ino == EXT_ROOT_INO || (stop && same_key(&chain_w[k], stop))) {
            nu_add(k + 1);
            return AT_NOT;
        }
        ino = parent_of(&in);
        gen = 0;
    }
}

static bool in_chain(const struct fsw_key *chain, unsigned n, const struct fsw_key *k)
{
    for (unsigned i = 0; i < n; i++)
        if (same_key(&chain[i], k))
            return true;
    return false;
}

/* What the hiding flags see of where an inode lies, as a cleaned path for
 * fs_view_hides: "" for the root, "x" outside etc, "etc", then etc's entry
 * ("ssh", "authorized_keys" or "x" for any other) and "/x" if deeper.
 * chain: its directory's, up to the root (n of them; a directory's own
 * starts it); file: the inode, a file in chain[0] (NULL: chain[0] is it). */
static void place_of(const struct fsw_key *chain, unsigned n, const struct fsw_key *file,
                     char out[32])
{
    unsigned depth = n - 1 + (file != NULL);
#define AT(k) ((file && (k) == depth) ? file : &chain[n - 1 - (k)])
    if (!depth) {
        out[0] = '\0';
    } else if (kind_of(AT(1)) != SP_ETC) {
        snprintf(out, 32, "x");
    } else if (depth == 1) {
        snprintf(out, 32, "etc");
    } else {
        unsigned k = kind_of(AT(2));
        snprintf(out, 32, "etc/%s%s", k == SP_SSH ? "ssh" : k == SP_AUTH ? "authorized_keys" : "x",
                 depth > 2 ? "/x" : "");
    }
#undef AT
}

static void dir_now(void *ctx, uint32_t slot)
{
    const struct fsw_key *to = ctx;
    kept->watch.dir[slot][0] = (uint32_t)to->ino;
    kept->watch.dir[slot][1] = to->gen;
}

/* Inode x (a directory if xdir) was moved from directory `from` into `to`,
 * its watches' IN_MOVE_SELF queued. Every watch of a narrower view (a
 * subview, or the hiding flags) on x or under it that the view can no
 * longer reach ends: IN_MOVE_SELF if asked (x's own had the rename's),
 * then IN_IGNORED. Only the channels whose reach the move may change look
 * at their watches: a subview's whose root `from` was in or under (so x
 * was under it) and `to` isn't; a hiding view's, when x went under etc.
 * Each such watch walks up its ".." chain to x (under it: judged), to that
 * root or a directory found not under x (left), with WALK_STEPS reads a
 * channel. A place that can't be found (a chain too deep or unreadable, a
 * file's directory gone, the channel's reads spent) counts as out of
 * reach: a subview's client can build a place too deep to judge (IR's 3),
 * and a watch the check can't judge must not outlive its view. */
static void moved(const struct fsw_key *x, bool xdir, const struct fsw_key *from,
                  const struct fsw_key *to)
{
    const struct fsw_table *t = w.t;
    if (!xdir)
        fsw_each(&w, x, dir_now, (void *)to);   /* a file's watches: its directory now */
    uint32_t judge = 0, stops = 0;
    unsigned nto = 0, nfrom = 0, left[FSW_CHANNELS];
    bool placed = false, etc_top = false;
    for (unsigned c = 0; c < FSW_CHANNELS; c++) {
        const struct fsw_chan_rec *r = &t->chans[c];
        bool sub = r->root != EXT_ROOT_INO, hides = r->view & FS_VIEW_NO_ETC;
        if (!r->live || !r->watches || (!sub && !hides))
            continue;
        if (!placed) {
            placed = true;
            nto = chain_of((uint32_t)to->ino, to->gen, chain_to);
            nfrom = chain_of((uint32_t)from->ino, from->gen, chain_from);
            if (!special_ok)
                special_find();
            /* Not placed: as if x went under etc, and out of every subview. */
            etc_top = !nto || kind_of(nto == 1 ? x : &chain_to[nto - 2]) == SP_ETC;
        }
        const struct fsw_key root = { .ino = r->root, .gen = r->root_gen };
        bool in_root = nfrom && in_chain(chain_from, nfrom, &root);
        bool left_root = sub && (in_root || !nfrom) && (!nto || !in_chain(chain_to, nto, &root));
        if (left_root && in_root)
            stops |= 1u << c;   /* x was under the root: the root isn't under x */
        if (left_root || (hides && etc_top))
            judge |= 1u << c;
        left[c] = WALK_STEPS;
    }
    if (judge) {
        memset(not_under, 0, sizeof(not_under));
        nnot_under = 0;
    }
    for (unsigned s = 0; judge && s < FSW_WATCHES; s++) {
        const struct fsw_watch_rec *r = &t->w[s];
        if (!r->chan || !(judge >> (r->chan - 1) & 1))
            continue;
        unsigned c = r->chan - 1, n = 0;
        const struct fsw_key k = { .ino = r->ino, .gen = r->gen };
        bool file = !(r->flags & FSW_W_DIR), self = same_key(&k, x);
        if (!self && !xdir)
            continue;   /* nothing is under a file */
        const struct fsw_chan_rec *cr = &t->chans[c];
        const struct fsw_key root = { .ino = cr->root, .gen = cr->root_gen };
        int at = AT_UNKNOWN;
        if (self && file) {   /* x itself: its directory is `to` */
            if (nto) {
                memcpy(chain_w, chain_to, nto * sizeof(chain_w[0]));
                n = nto;
                at = AT_UNDER;
            }
        } else {
            at = file ? where(kept->watch.dir[s][0], kept->watch.dir[s][1], x,
                              stops >> c & 1 ? &root : NULL, nto, &left[c], &n)
                      : where((uint32_t)r->ino, r->gen, x, stops >> c & 1 ? &root : NULL, nto,
                              &left[c], &n);
        }
        if (at == AT_NOT)
            continue;
        bool out = at == AT_UNKNOWN || (cr->root != EXT_ROOT_INO && !in_chain(chain_w, n, &root));
        if (!out && (cr->view & FS_VIEW_NO_ETC)) {
            char place[32];
            place_of(chain_w, n, file ? &k : NULL, place);
            out = fs_view_hides(cr->view, place, false);
        }
        if (out)
            fsw_end(&w, s, !self);
    }
}

/* ---- events ------------------------------------------------------------------- */

static void deliver(const struct held *e)
{
    if (e->gone) {
        fsw_gone(&w, &e->self);
        return;
    }
    if (e->moved) {
        moved(&e->self, e->flags & FSW_EV_DIR, &e->from, &e->dir);
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
    if (dir && (dir->ino == EXT_ROOT_INO || kind_of(dir) == SP_ETC))
        special_ok = false;   /* a name of the root or an etc: found again when asked */
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

void watch_moved(const struct ext_inode *in, const struct ext_inode *from,
                 const struct ext_inode *to)
{
    if (!watching())
        return;
    const struct held e = { .moved = true, .self = xf_key(in), .from = xf_key(from),
                            .dir = xf_key(to), .flags = is_dir(in) ? FSW_EV_DIR : 0 };
    /* Kept in the state too, until the commit's ev_settle: a death between
     * the journal's commit and it leaves the successor to run it (IR's 1). */
    struct xf_watches *k = &kept->watch;
    if (holding && k->nmoves < 2)
        k->moves[k->nmoves++] = (struct xf_wmove){
            .x = (uint32_t)e.self.ino, .x_gen = e.self.gen, .from = (uint32_t)e.from.ino,
            .from_gen = e.from.gen, .to = (uint32_t)e.dir.ino, .to_gen = e.dir.gen,
            .xdir = is_dir(in),
        };
    put(&e);
}

void watch_moves_again(bool committed)
{
    struct xf_watches *k = &kept->watch;
    unsigned n = k->nmoves < 2 ? k->nmoves : 2;
    for (unsigned i = 0; committed && watching() && i < n; i++) {
        const struct xf_wmove *m = &k->moves[i];
        const struct fsw_key x = { .ino = m->x, .gen = m->x_gen };
        const struct fsw_key from = { .ino = m->from, .gen = m->from_gen };
        const struct fsw_key to = { .ino = m->to, .gen = m->to_gen };
        moved(&x, m->xdir != 0, &from, &to);
    }
    k->nmoves = 0;
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
    if (kept)
        kept->watch.nmoves = 0;
}

void ev_settle(bool done)
{
    holding = false;
    for (unsigned i = 0; done && i < nheld; i++)
        deliver(&held[i]);
    nheld = 0;
    if (kept)
        kept->watch.nmoves = 0;   /* run (or given up with the request) */
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
    w.hidden = hidden;
    memset(wc, 0, sizeof(wc));
    held_bits = 0;
    special_ok = false;
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
    w.hidden = hidden;
    memset(wc, 0, sizeof(wc));
    held_bits = 0;
    special_ok = false;
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
    bool first = !watching();
    st = fsw_add(&w, index_of(k), &key, dir, mask, out_id, out_existed);
    if (st != OK)
        return st;
    if (first)
        special_ok = false;   /* names changed unseen while nothing was watched */
    /* A file's place is its directory's (the reach check): the one named. */
    uint32_t *d = kept->watch.dir[*out_id & ((1u << FSW_SLOT_BITS) - 1)];
    d[0] = dir ? 0 : aw.dir.ino;
    d[1] = dir ? 0 : aw.dir.d.generation;
    return OK;
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
