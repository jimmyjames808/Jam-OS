/* The compositor's memory across its own restarts (wm.h; the owner's G1
 * sign-off, 2026-10-07: after `kill compositor` the windows came back
 * in whatever order their clients reconnected, so the arrangement
 * changed). docs/G1-PLAN.md "As built: the compositor remembers the
 * arrangement" has the design and why.
 *
 * Where it lives: the state VMO init keeps for the compositor (M11.6's
 * <svcstate.h>, SR_STATE), mapped at SVCSTATE_ADDR. The memory outlives
 * the process because init holds the VMO; nothing is sent anywhere. Its
 * own area holds two copies of a struct wm_save and a commit word: each
 * loop turn the arrangement is described, and if it changed it is written
 * over the copy that isn't current and the word moved with one released
 * store (as the mixer's numbers: a process dies between instructions, so
 * the word always names a whole copy).
 *
 * Who is who: a window's key (jam_window_memory_v1, memory.c), 64 random
 * bits the compositor gives the window's own client, which presents it
 * again for the window it makes after a reconnect. Two windows with one
 * title have two keys; a client can't learn another's, and the worst a
 * client presenting a key it was given can do is put its own window in
 * a place its own window had.
 *
 * Coming back: a new compositor that finds a description takes it whole
 * or not at all (every count, index, ratio, layout, mode, box and key
 * checked, a whole tree per screen, the checksum): its normal screens and
 * their layouts, the one shown, each tiling screen's tree with a
 * placeholder in each remembered window's tile. A window presenting a
 * remembered key before it is mapped takes that place: its screen, its
 * tile (wm_tile gives its first configure the placeholder's box), its
 * floating box, minimised, maximised or full screen; the focus goes back
 * to the window that had it once that one is back. Windows not
 * remembered are new windows. After WM_SAVE_HOLD_NS the places of
 * windows not back go, as closed windows' do; the last one back ends the
 * wait sooner. Nothing is written while the wait is on (a second restart
 * then finds the same description), but a compositor that dies during
 * the wait marks that description as one not to try again (`trying`):
 * should putting it back be what kills it, init's restarts don't loop.
 *
 * Bounds: WM_SAVE_WINDOWS windows, WM_SAVE_NODES nodes for every screen's
 * tree together (a tree that doesn't fit is left out: its windows come
 * back as new windows), DESK_SCREENS_MAX screens; struct wm_save is a
 * fixed size. */
#include <svcstate.h>
#include "desk.h"

_Static_assert(WM_SAVE_SCREENS == DESK_SCREENS_MAX, "a description holds every screen");
_Static_assert(WM_SAVE_WINDOWS < WM_SAVE_GONE, "a node's window index");

#define STATE_KIND   0x504d4f43u   /* "COMP": svcstate's kind */
#define SIDE_MAX     8192          /* a size, a minimum (COMP_BUFFER_SIDE_MAX's) */
#define PLACE_MAX    65536         /* a floating place, either way */
#define FNV_OFFSET   0xcbf29ce484222325ull
#define FNV_PRIME    0x100000001b3ull

/* The state VMO's own area. */
struct save_area {
    uint64_t commits;              /* copy[commits & 1] is whole; 0: none yet */
    uint64_t trying;               /* the commit being put back (0: none): one a compositor
                                    * died putting back isn't tried again (no restart loop) */
    struct wm_save copy[2];
};

static const struct svcstate_layout state_layout = {
    .kind = STATE_KIND,
    .layout = WM_SAVE_VERSION << 16 | (uint32_t)(sizeof(struct wm_save) & 0xffff),
    .req_cap = 4,   /* no requests are kept: the least svcstate takes */
    .rep_cap = 4,
    .user_size = sizeof(struct save_area),
};

/* A remembered window, until it is back or the wait ends. */
struct entry {
    uint64_t key, rank;
    struct desk_screen *screen;
    int32_t fx, fy, fw, fh;
    uint8_t want, before_fs, flags;
    bool claimed;                  /* a window presented the key */
    bool placed;                   /* ... and is mapped in its place */
};

static struct {
    bool pending;                  /* remembered windows awaited */
    uint64_t deadline;
    struct entry e[WM_SAVE_WINDOWS];
    unsigned n, placed;
    uint64_t focused, cur_full;
} mem;

static struct svcstate state;
static struct save_area *area;     /* mapped; NULL: no memory */
static struct wm_save scratch, last;
static bool written;               /* last is what the current copy holds */

/* ---- describing ------------------------------------------------------------------------- */

uint64_t wm_save_sum(const struct wm_save *d)
{
    const uint8_t *p = (const uint8_t *)d;
    size_t at = offsetof(struct wm_save, sum);
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < sizeof(*d); i++) {
        uint8_t b = i >= at && i < at + sizeof(d->sum) ? 0 : p[i];
        h = (h ^ b) * FNV_PRIME;
    }
    return h;
}

/* The normal screen a window is on (a full-screen one's home), as an
 * index among the normal screens; -1: none. */
static int normal_index(const struct desk_screen *s, const int *map)
{
    int i = s ? screens_index(s) : -1;
    return i >= 0 ? map[i] : -1;
}

/* s's tree into d's nodes (none if it doesn't fit); si: s's index among
 * the normal screens. */
static void describe_tree(struct wm_save *d, unsigned si, const struct desk_screen *s,
                          const struct wm_window *const *saved)
{
    struct wm_save_screen *out = &d->screens[si];
    unsigned first = d->nnodes, n = 0;
    for (const struct tile_node *t = s->tree; t; n++) {
        if (first + n == WM_SAVE_NODES) {
            for (unsigned k = first; k < first + n; k++)
                d->nodes[k] = (struct wm_save_node){ 0 };
            return;   /* no room: this screen's windows come back as new ones */
        }
        struct wm_save_node *o = &d->nodes[first + n];
        if (t->a) {
            *o = (struct wm_save_node){ .ratio = t->ratio, .win = WM_SAVE_SPLIT,
                                        .across = t->across };
        } else {
            o->win = WM_SAVE_GONE;   /* (a window not remembered, or not one a tile keeps:
                                      * what the check would refuse is never written) */
            for (unsigned k = 0; k < d->nwins; k++)
                if (saved[k] == t->ww && t->ww->want != WM_FULLSCREEN && !t->ww->minimised &&
                    d->wins[k].screen == si)
                    o->win = (uint16_t)k;
        }
        /* pre-order: a split's a, else up to the first a whose b is next */
        if (t->a) {
            t = t->a;
            continue;
        }
        while (t->up && t == t->up->b)
            t = t->up;
        t = t->up ? t->up->b : NULL;
    }
    out->first = (uint16_t)first;
    out->n = (uint16_t)n;
    d->nnodes = first + n;
}

void wm_save_describe(struct wm_save *d)
{
    memset(d, 0, sizeof(*d));
    d->magic = WM_SAVE_MAGIC;
    d->version = WM_SAVE_VERSION;
    d->bytes = sizeof(*d);
    int map[DESK_SCREENS_MAX];
    unsigned ns = 0;
    for (unsigned i = 0; i < screens_count(); i++) {
        const struct desk_screen *s = screens_nth(i);
        map[i] = -1;
        if (s->kind != SCREEN_NORMAL)
            continue;
        map[i] = (int)ns;
        d->screens[ns].layout = (uint8_t)s->layout;
        d->screens[ns].spill = s->spill;
        ns++;
    }
    d->nscreens = ns;
    d->deflt = (uint32_t)screens_default();
    const struct desk_screen *cur = screens_cur();
    const struct wm_window *saved[WM_SAVE_WINDOWS];
    for (const struct wm_window *ww = wm_first(); ww && ns; ww = ww->next) {
        if (!ww->win || ww->overlay || !ww->key || d->nwins == WM_SAVE_WINDOWS)
            continue;
        bool full = ww->screen && ww->screen->kind == SCREEN_FULL;
        int si = normal_index(full ? ww->home : ww->screen, map);
        if (full && ww->screen == cur)
            d->cur_full = ww->key;
        if (si < 0)
            si = 0;   /* a full screen whose home went: the first, full screen again */
        saved[d->nwins] = ww;
        d->wins[d->nwins++] = (struct wm_save_window){
            .key = ww->key, .rank = ww->focused_at,
            .fx = ww->float_x, .fy = ww->float_y, .fw = ww->float_w, .fh = ww->float_h,
            .min_w = ww->min_w, .min_h = ww->min_h, .screen = (uint8_t)si,
            .want = (uint8_t)ww->want, .before_fs = (uint8_t)ww->before_fs,
            .flags = (uint8_t)((ww->minimised ? WM_SAVE_MINIMISED : 0) |
                               (ww->float_placed ? WM_SAVE_FLOAT_PLACED : 0) |
                               (ww->float_w || ww->float_h ? WM_SAVE_FLOAT_SIZE : 0)),
        };
        if (ww == wm_focused())
            d->focused = ww->key;
    }
    int ci = normal_index(cur, map);
    if (ci < 0) {   /* a full screen: its window's home */
        ci = 0;
        for (unsigned k = 0; k < d->nwins; k++)
            if (d->wins[k].key == d->cur_full)
                ci = d->wins[k].screen;
    }
    d->cur = (uint32_t)ci;
    for (unsigned i = 0; i < screens_count(); i++) {
        const struct desk_screen *s = screens_nth(i);
        if (map[i] >= 0 && s->tree && s->layout == COMP_TILING)
            describe_tree(d, (unsigned)map[i], s, saved);
    }
    d->sum = 0;
}

/* ---- checking --------------------------------------------------------------------------- */

static bool side_ok(int32_t v)
{
    return v >= 0 && v <= SIDE_MAX;
}

static bool place_ok(int32_t v)
{
    return v >= -PLACE_MAX && v <= PLACE_MAX;
}

/* Why d can't be used, or NULL. */
static const char *check(const struct wm_save *d)
{
    if (d->magic != WM_SAVE_MAGIC || d->bytes != sizeof(*d))
        return "not a description";
    if (d->version != WM_SAVE_VERSION)
        return "another version's";
    if (wm_save_sum(d) != d->sum)
        return "its checksum";
    if (!d->nscreens || d->nscreens > WM_SAVE_SCREENS || d->nwins > WM_SAVE_WINDOWS ||
        d->nnodes > WM_SAVE_NODES || d->cur >= d->nscreens || d->deflt > COMP_TILING)
        return "a count";
    for (unsigned i = 0; i < d->nwins; i++) {
        const struct wm_save_window *w = &d->wins[i];
        if (!w->key || w->screen >= d->nscreens || w->want > WM_FULLSCREEN ||
            w->before_fs > WM_MAXIMIZED || w->flags & ~7u)
            return "a window";
        if (!side_ok(w->fw) || !side_ok(w->fh) || !place_ok(w->fx) || !place_ok(w->fy) ||
            !side_ok(w->min_w) || !side_ok(w->min_h))
            return "a window's box";
        for (unsigned k = 0; k < i; k++)
            if (d->wins[k].key == w->key)
                return "a key twice";
    }
    bool used[WM_SAVE_WINDOWS] = { false };
    unsigned next = 0;
    for (unsigned i = 0; i < d->nscreens; i++) {
        const struct wm_save_screen *s = &d->screens[i];
        if (s->layout > COMP_TILING || s->spill > 1)
            return "a screen";
        if (!s->n)
            continue;
        if (s->first != next || s->n > d->nnodes - next || s->layout != COMP_TILING)
            return "a screen's tree";
        unsigned need = 1;   /* a whole tree in pre-order (as tiles_build checks) */
        for (unsigned k = s->first; k < s->first + s->n; k++) {
            const struct wm_save_node *nd = &d->nodes[k];
            if (!need)
                return "a tree with nodes after its end";
            need = nd->win == WM_SAVE_SPLIT ? need + 1 : need - 1;
            if (nd->win == WM_SAVE_SPLIT) {
                if (nd->ratio < TILE_MIN || nd->ratio > TILE_MAX || nd->across > 1)
                    return "a split";
            } else if (nd->win != WM_SAVE_GONE) {
                const struct wm_save_window *w = nd->win < d->nwins ? &d->wins[nd->win] : NULL;
                if (!w || used[nd->win] || w->screen != i || (w->flags & WM_SAVE_MINIMISED) ||
                    w->want == WM_FULLSCREEN)
                    return "a tile's window";
                used[nd->win] = true;
            }
        }
        if (need)
            return "a tree cut short";
        next += s->n;
    }
    if (next != d->nnodes)
        return "nodes of no screen";
    bool focused = !d->focused, full = !d->cur_full;
    for (unsigned i = 0; i < d->nwins; i++) {
        focused = focused || d->wins[i].key == d->focused;
        full = full || (d->wins[i].key == d->cur_full && d->wins[i].want == WM_FULLSCREEN);
    }
    if (!focused || !full)
        return "the focus or the screen shown";
    return NULL;
}

/* ---- coming back ------------------------------------------------------------------------- */

void wm_save_forget(void)
{
    memset(&mem, 0, sizeof(mem));
}

status_t wm_save_load(const struct wm_save *d, uint64_t t)
{
    const char *why = check(d);
    if (!why && (wm_first() || screens_count() != 1))
        return ERR_BAD_STATE;
    if (why) {
        printf("compositor: the remembered layout is ignored (%s)\n", why);
        return ERR_INVALID_ARGS;
    }
    enum comp_layout layouts[WM_SAVE_SCREENS];
    bool spill[WM_SAVE_SCREENS];
    struct desk_screen *s[WM_SAVE_SCREENS] = { NULL };
    for (unsigned i = 0; i < d->nscreens; i++) {
        layouts[i] = (enum comp_layout)d->screens[i].layout;
        spill[i] = d->screens[i].spill;
    }
    screens_load(d->nscreens, layouts, spill, d->cur, (enum comp_layout)d->deflt, s);
    wm_save_forget();
    uint64_t top = 0;
    for (unsigned i = 0; i < d->nwins; i++) {
        const struct wm_save_window *w = &d->wins[i];
        mem.e[mem.n++] = (struct entry){
            .key = w->key, .rank = w->rank, .screen = s[w->screen], .fx = w->fx, .fy = w->fy,
            .fw = w->fw, .fh = w->fh, .want = w->want, .before_fs = w->before_fs,
            .flags = w->flags,
        };
        s[w->screen]->held++;
        top = w->rank > top ? w->rank : top;
    }
    static struct tile_spec spec[WM_SAVE_NODES];
    for (unsigned i = 0; i < d->nscreens; i++) {
        const struct wm_save_screen *sc = &d->screens[i];
        for (unsigned k = 0; k < sc->n; k++) {
            const struct wm_save_node *nd = &d->nodes[sc->first + k];
            const struct wm_save_window *w = nd->win < d->nwins ? &d->wins[nd->win] : NULL;
            spec[k] = (struct tile_spec){ .split = nd->win == WM_SAVE_SPLIT,
                                          .across = nd->across, .ratio = nd->ratio,
                                          .hold = w ? w->key : 0,
                                          .min_w = w ? w->min_w : 0, .min_h = w ? w->min_h : 0 };
        }
        if (sc->n && !tiles_build(s[i], spec, sc->n))
            printf("compositor: no memory for screen %u's remembered tiles\n", i + 1);
    }
    wm_focus_count_at_least(top);
    mem.focused = d->focused;
    mem.cur_full = d->cur_full;
    mem.pending = mem.n > 0;
    mem.deadline = t + WM_SAVE_HOLD_NS;
    printf("compositor: layout remembered: %u windows on %u screens\n", mem.n, d->nscreens);
    if (!mem.pending)
        screens_tidy();
    return OK;
}

bool wm_save_pending(void)
{
    return mem.pending;
}

uint64_t wm_save_deadline(void)
{
    return mem.pending ? mem.deadline : DEADLINE_NEVER;
}

/* The wait is over: the places of windows not back go, as closed ones'. */
static void finish(void)
{
    mem.pending = false;
    (void)tiles_drop_holds();
    for (unsigned i = 0; i < screens_count(); i++)
        screens_nth(i)->held = 0;
    printf("compositor: layout restored: %u windows in place, %u not back\n", mem.placed,
           mem.n - mem.placed);
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next)
        ww->restoring = false;   /* one still to map is a new window */
    wm_reflow();   /* the windows beside a place that went glide into it */
    screens_tidy();
}

void wm_save_tick(uint64_t t)
{
    if (mem.pending && t >= mem.deadline)
        finish();
}

static struct entry *find(uint64_t key)
{
    for (unsigned i = 0; key && i < mem.n; i++)
        if (mem.e[i].key == key)
            return &mem.e[i];
    return NULL;
}

static bool in_use(uint64_t key)
{
    for (const struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (ww->key == key)
            return true;
    return find(key) != NULL;
}

uint64_t wm_save_key(struct wm_window *ww, uint64_t presented)
{
    if (ww->key)
        return ww->key;   /* asked again: the key it has */
    struct entry *e = mem.pending && !ww->win ? find(presented) : NULL;
    if (e && !e->claimed && screens_index(e->screen) >= 0) {
        e->claimed = true;
        ww->key = presented;
        ww->restoring = true;
        ww->screen = e->screen;   /* its screen's layout and room for its first configure */
        if (e->flags & WM_SAVE_FLOAT_SIZE) {
            ww->float_w = e->fw;
            ww->float_h = e->fh;
        }
        if (e->flags & WM_SAVE_FLOAT_PLACED) {
            ww->float_x = e->fx;
            ww->float_y = e->fy;
            ww->float_placed = true;
        }
        if (e->want == WM_MAXIMIZED || (e->want == WM_FULLSCREEN && e->before_fs == WM_MAXIMIZED))
            wm_request_maximized(ww, true);
        if (e->want == WM_FULLSCREEN)
            wm_request_fullscreen(ww, true);
        wm_reconfigure(ww);
        return presented;
    }
    uint64_t k = 0;
    for (unsigned tries = 0; tries < 8 && (!k || in_use(k)); tries++)
        os_random(&k, sizeof(k));
    ww->key = k && !in_use(k) ? k : 0;   /* (a generator that fails: no key, a new window) */
    return ww->key;
}

bool wm_save_place(struct wm_window *ww)
{
    if (!ww->restoring)
        return false;
    ww->restoring = false;
    struct entry *e = find(ww->key);
    if (!mem.pending || !e || !e->claimed || e->placed || screens_index(e->screen) < 0 ||
        e->screen->kind != SCREEN_NORMAL ||
        (ww->want == WM_FULLSCREEN && !seat_takes_keys(ww->surface->client)))
        return false;   /* (a boot overlay is no screen's) */
    e->placed = true;
    mem.placed++;
    if (e->screen->held)
        e->screen->held--;
    ww->screen = e->screen;
    ww->home = NULL;
    ww->overlay = false;
    ww->minimised = (e->flags & WM_SAVE_MINIMISED) != 0;
    tiles_take_hold(ww);
    return true;
}

bool wm_save_shown_full(const struct wm_window *ww)
{
    return ww->key && ww->key == mem.cur_full;
}

void wm_save_mapped(struct wm_window *ww)
{
    struct entry *e = find(ww->key);
    if (!mem.pending || !e || !e->placed)
        return;
    struct wm_window *f = NULL;
    for (struct wm_window *w = wm_first(); w && mem.focused; w = w->next)
        if (w->key == mem.focused && w->win && screens_shown(w))
            f = w;
    if (f && f->win)
        seat_focus(f->win);   /* the window that had the keys, once it is back */
    if (ww != f)
        ww->focused_at = e->rank;   /* its place in the focus order, not its mapping's */
    if (mem.placed == mem.n) {
        mem.pending = false;
        for (unsigned i = 0; i < screens_count(); i++)
            screens_nth(i)->held = 0;
        printf("compositor: layout restored: %u windows in place, the keys with \"%s\"\n",
               mem.placed, f ? f->title : "");
        screens_tidy();
    }
}

/* ---- the state VMO ------------------------------------------------------------------------ */

status_t wm_save_open(handle_t vmo, uint64_t t)
{
    if (vmo == HANDLE_INVALID)
        return OK;   /* (a test's compositor): no memory */
    enum svcstate_start how = SVCSTATE_FRESH;
    status_t st = svcstate_open(vmo, &state_layout, &state, &how);
    if (st != OK) {
        printf("compositor: no memory of the layout (%s)\n", status_str(st));
        return st;
    }
    area = svcstate_user(&state);
    uint64_t c = __atomic_load_n(&area->commits, __ATOMIC_ACQUIRE);
    if (how == SVCSTATE_ADOPTED && c && area->trying == c) {
        printf("compositor: the remembered layout is ignored (the last compositor ended "
               "putting it back)\n");
        area->trying = 0;
    } else if (how == SVCSTATE_ADOPTED && c) {
        static struct wm_save copy;   /* ours: checked as it is, not as it may change */
        memcpy(&copy, &area->copy[c & 1], sizeof(copy));
        __atomic_store_n(&area->trying, c, __ATOMIC_RELEASE);
        (void)wm_save_load(&copy, t);
    } else {
        memset(area, 0, sizeof(*area));   /* a refused state's bytes are whatever it left */
    }
    return OK;
}

void wm_save_turn(uint64_t t)
{
    wm_save_tick(t);
    if (!area || mem.pending)
        return;   /* (while the wait is on, the description it came from stays) */
    if (area->trying)
        __atomic_store_n(&area->trying, 0, __ATOMIC_RELEASE);   /* put back: safe to try again */
    wm_save_describe(&scratch);
    if (written && !memcmp(&scratch, &last, sizeof(scratch)))
        return;
    memcpy(&last, &scratch, sizeof(last));
    scratch.sum = wm_save_sum(&scratch);
    uint64_t next = area->commits + 1;
    memcpy(&area->copy[next & 1], &scratch, sizeof(scratch));
    __atomic_store_n(&area->commits, next, __ATOMIC_RELEASE);
    written = true;
}
