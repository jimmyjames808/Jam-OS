/* Tiling's layout (wm.h): dwindle, the owner's pick (docs/G1-PLAN.md "The
 * look", after trying six layouts in the prototype).
 *
 * Each tiling screen (screens.c) has a binary tree. A leaf is a window's
 * tile; a split cuts its room in two, side by side (across) or one over
 * the other, with WM_GAP pixels of background between the halves and
 * around the whole: the room is the screen's (screens_room: below the
 * strip while the desktop is on) less WM_GAP on every side. A split's
 * ratio is a's share of its room less the gap, in TILE_ONE parts, kept
 * between 15% and 85%; each split keeps its own, so the tree is the
 * screen's memory of how the user sized it.
 *
 *   - The first window fills the room.
 *   - A new window splits the focused tile in half (the last one, if the
 *     focus is elsewhere) along its longer side: the old window keeps the
 *     left or top half, the new one takes the other. The last tile is the
 *     one reached by always taking b: the newest corner of the spiral.
 *   - No tile is made too small: if that split would leave halves
 *     narrower than WM_TILE_MIN_W (split across) or shorter than
 *     WM_TILE_MIN_H (split down), about 20 columns or 5 lines of a
 *     terminal, or if either window's own minimum (xdg_toplevel's
 *     set_min_size, wm_set_limits; a fixed size is min = max) wouldn't fit
 *     inside its half, the window doesn't go on this screen: it goes to a new
 *     screen at the end (screens_spill), tiling, where it has all the
 *     room; this screen stays as the user arranged it. The same wherever
 *     a window comes into a tree: a new window (wm.c then takes the view
 *     there: screens_follow), one moved to the screen (Super+Shift+N,
 *     Super+Ctrl+Shift+direction: the view follows it to the new screen),
 *     one restored or back from full screen (followed), and Super+T
 *     building a tree from a floating screen's windows (those that don't
 *     fit go to one new screen, in order, the view staying; that screen
 *     spills again if it fills). With no room for another screen
 *     (DESK_SCREENS_MAX) the split is made, however small.
 *   - A screen switched from floating (its tree made from nothing) takes
 *     its windows in the order they opened, each splitting the last.
 *   - A window that goes (closed, unmapped, minimised, moved to another
 *     screen) leaves its sibling the whole of their parent's room.
 *   - Two windows swap by trading leaves; a gap moves by changing its
 *     split's ratio (dragged: wmgrab.c; Super+Alt+direction: tiles_push).
 *   - No tile gets less than its window needs (the tiler's minimum, or
 *     the window's own and its border): each node's need is worked out at
 *     every layout, a split's halves are held to theirs while its room
 *     has it, and a gap stops there (the owner's PC: a terminal's tile
 *     grew over Jamjar's when the gap was dragged past Jamjar's minimum).
 *
 * Every tile is a frame box: the window's decorations (a border) go inside
 * it, and a window that can't resize sits centred in it (wm.c).
 *
 * The tree is walked without recursion (its depth is the number of
 * windows on the screen, which clients choose): a pre-order step uses the
 * parent pointers. Memory: two nodes a window, from the heap, freed as
 * windows go. */
#include "desk.h"

#define GAP_BAR_W     3     /* a lit gap's bar: pixels wide, */
#define GAP_BAR_INSET 150   /* ... thousandths of its length left out at either end */

static uint64_t generation;   /* tree changes: grabs holding a node check it */

/* The tiler's minimum for a half (tiles_set_min: a test's small output). */
static int32_t min_w = WM_TILE_MIN_W, min_h = WM_TILE_MIN_H;

void tiles_set_min(int32_t w, int32_t h)
{
    min_w = w;
    min_h = h;
}

uint64_t tiles_generation(void)
{
    return generation;
}

/* ---- walking --------------------------------------------------------------------------- */

/* The node after n in pre-order (a split before its halves), or NULL. */
static struct tile_node *next_node(const struct tile_node *n)
{
    if (n->a)
        return n->a;
    for (; n->up; n = n->up)
        if (n == n->up->a)
            return n->up->b;
    return NULL;
}

/* The first node of n's subtree in post-order (halves before their split). */
static struct tile_node *first_post(struct tile_node *n)
{
    while (n->a)
        n = n->a;
    return n;
}

/* The node after n in post-order, or NULL after the root. */
static struct tile_node *next_post(struct tile_node *n)
{
    if (!n->up)
        return NULL;
    return n == n->up->a ? first_post(n->up->b) : n->up;
}

/* The last tile: from the root, always b. */
static struct tile_node *last_leaf(struct tile_node *n)
{
    while (n && n->a)
        n = n->b;
    return n;
}

/* ---- layout ------------------------------------------------------------------------------ */

/* a's length when a room len long is split at ratio, the gap between. */
static int32_t part_of(int64_t len, int32_t ratio)
{
    int64_t room = len - WM_GAP < 0 ? 0 : len - WM_GAP;
    return (int32_t)((room * ratio + TILE_ONE / 2) / TILE_ONE);
}

/* What leaf l needs: the tiler's minimum, or more if its window's own
 * minimum (a fixed size's too) and its border need it. */
static void leaf_need(struct tile_node *l)
{
    struct deco_sizes d = deco_sizes(0, COMP_TILING);
    const struct wm_window *ww = l->ww;
    int32_t mw = ww ? ww->min_w : l->hold_min_w, mh = ww ? ww->min_h : l->hold_min_h;
    int32_t w = mw > 0 ? mw + d.left + d.right : 0;   /* (a placeholder's: its window's) */
    int32_t h = mh > 0 ? mh + d.top + d.bottom : 0;
    l->need_w = w > min_w ? w : min_w;
    l->need_h = h > min_h ? h : min_h;
}

/* Every node's need in s's tree, the leaves' first (post-order). */
static void needs(struct desk_screen *s)
{
    for (struct tile_node *n = s->tree ? first_post(s->tree) : NULL; n; n = next_post(n)) {
        if (!n->a) {
            leaf_need(n);
            continue;
        }
        const struct tile_node *a = n->a, *b = n->b;
        int64_t w = n->across ? (int64_t)a->need_w + WM_GAP + b->need_w
                              : (a->need_w > b->need_w ? a->need_w : b->need_w);
        int64_t h = n->across ? (a->need_h > b->need_h ? a->need_h : b->need_h)
                              : (int64_t)a->need_h + WM_GAP + b->need_h;
        n->need_w = w > INT32_MAX / 2 ? INT32_MAX / 2 : (int32_t)w;   /* deep trees: no overflow */
        n->need_h = h > INT32_MAX / 2 ? INT32_MAX / 2 : (int32_t)h;
    }
}

/* a's length of n's room (len long) at n's ratio, held to what each half
 * needs while the room has it; with less room than both need, shared in
 * proportion to their needs (both short: their windows are clipped to
 * their tiles, wm.c). */
static int32_t split_part(const struct tile_node *n, int64_t len)
{
    int32_t part = part_of(len, n->ratio);
    int64_t room = len - WM_GAP < 0 ? 0 : len - WM_GAP;
    int64_t na = n->across ? n->a->need_w : n->a->need_h;
    int64_t nb = n->across ? n->b->need_w : n->b->need_h;
    if (na + nb <= room) {
        if (part < na)
            part = (int32_t)na;
        if (part > room - nb)
            part = (int32_t)(room - nb);
    } else if (na + nb > 0) {
        part = (int32_t)(room * na / (na + nb));
    }
    return part;
}

/* Split n's halves' boxes from its own. */
static void split_boxes(struct tile_node *n)
{
    struct comp_box r = n->box;
    int32_t part = split_part(n, n->across ? (int64_t)r.x2 - r.x1 : (int64_t)r.y2 - r.y1);
    n->a->box = r;
    n->b->box = r;
    if (n->across) {
        n->a->box.x2 = r.x1 + part;
        n->b->box.x1 = r.x1 + part + WM_GAP;
        if (n->b->box.x1 > r.x2)
            n->b->box.x1 = r.x2;   /* a room narrower than the gap: b empty, never inside out */
    } else {
        n->a->box.y2 = r.y1 + part;
        n->b->box.y1 = r.y1 + part + WM_GAP;
        if (n->b->box.y1 > r.y2)
            n->b->box.y1 = r.y2;
    }
}

/* s's room for tiles: its room less a gap all round. */
static struct comp_box tile_area(const struct desk_screen *s)
{
    struct comp_box r = screens_room(s);
    return (struct comp_box){ r.x1 + WM_GAP, r.y1 + WM_GAP, r.x2 - WM_GAP, r.y2 - WM_GAP };
}

static void layout(struct desk_screen *s)
{
    if (!s->tree)
        return;
    needs(s);
    s->tree->box = tile_area(s);
    for (struct tile_node *n = s->tree; n; n = next_node(n))
        if (n->a)
            split_boxes(n);
}

/* ---- changing the tree -------------------------------------------------------------------- */

/* Leaf l out of s's tree, freed: its sibling takes their parent's place. */
static void remove_node(struct desk_screen *s, struct tile_node *l)
{
    struct tile_node *p = l->up;
    generation++;
    if (!p) {
        s->tree = NULL;
        free(l);
        return;
    }
    struct tile_node *sib = p->a == l ? p->b : p->a;
    sib->up = p->up;
    if (!p->up)
        s->tree = sib;
    else if (p->up->a == p)
        p->up->a = sib;
    else
        p->up->b = sib;
    sib->box = p->box;
    free(l);
    free(p);
}

/* ww's leaf out of its tree. */
static void remove_leaf(struct wm_window *ww)
{
    struct tile_node *l = ww->leaf;
    struct desk_screen *s = ww->tiled_on;
    ww->leaf = NULL;
    ww->tiled_on = NULL;
    remove_node(s, l);
}

static struct tile_node *new_leaf(struct wm_window *ww, struct tile_node *up)
{
    struct tile_node *l = calloc(1, sizeof(*l));
    if (l) {
        l->ww = ww;
        l->up = up;
    }
    return l;
}

/* ww into s's tree, splitting leaf t (NULL: as the root). False: no memory
 * (it stays untiled: wm_tile gives it the whole room). */
static bool insert(struct desk_screen *s, struct wm_window *ww, struct tile_node *t)
{
    generation++;
    if (!t) {
        if (!(s->tree = new_leaf(ww, NULL)))
            return false;
        ww->leaf = s->tree;
    } else {
        struct tile_node *a = new_leaf(t->ww, t), *b = new_leaf(ww, t);
        if (!a || !b) {
            free(a);
            free(b);
            return false;
        }
        t->across = t->box.x2 - t->box.x1 >= t->box.y2 - t->box.y1;
        t->ratio = TILE_ONE / 2;
        t->a = a;
        t->b = b;
        if (a->ww)
            a->ww->leaf = a;
        a->hold = t->hold;   /* a placeholder split keeps its place in a */
        a->hold_min_w = t->hold_min_w;
        a->hold_min_h = t->hold_min_h;
        t->ww = NULL;
        t->hold = 0;
        ww->leaf = b;
    }
    ww->tiled_on = s;
    layout(s);   /* the next insert splits a tile of known size */
    return true;
}

void tiles_forget(struct wm_window *ww)
{
    if (ww->leaf)
        remove_leaf(ww);
}

void tiles_drop(struct desk_screen *s)
{
    while (s->tree) {   /* the first leaf each time, its window untiled */
        struct tile_node *l = s->tree;
        while (l->a)
            l = l->a;
        if (l->ww && l->ww->leaf == l) {
            l->ww->leaf = NULL;
            l->ww->tiled_on = NULL;
        }
        remove_node(s, l);
    }
}

/* ---- matching the windows ------------------------------------------------------------------ */

/* Is ww tiled on s: mapped on it, shown there (not minimised, no overlay). */
static bool tiled_here(const struct wm_window *ww, const struct desk_screen *s)
{
    return ww->win && !ww->minimised && !ww->overlay && ww->screen == s;
}

static bool tiling(const struct desk_screen *s)
{
    return s->kind == SCREEN_NORMAL && s->layout == COMP_TILING;
}

/* The halves of t's box when a window splits it at half (split_boxes' sums):
 * *a the old window's, *b the new one's. */
static void halves(const struct tile_node *t, struct comp_box *a, struct comp_box *b)
{
    struct comp_box r = t->box;
    bool across = r.x2 - r.x1 >= r.y2 - r.y1;
    int32_t part = part_of(across ? (int64_t)r.x2 - r.x1 : (int64_t)r.y2 - r.y1, TILE_ONE / 2);
    *a = *b = r;
    if (across) {
        a->x2 = r.x1 + part;
        b->x1 = r.x1 + part + WM_GAP;
    } else {
        a->y2 = r.y1 + part;
        b->y1 = r.y1 + part + WM_GAP;
    }
}

/* Does ww's declared minimum (xdg_toplevel.set_min_size; a fixed size's
 * too) fit inside tile b, its border taken off? */
static bool min_fits(const struct wm_window *ww, struct comp_box b)
{
    struct comp_box in = deco_inner(b, 0, COMP_TILING);
    return !ww || (in.x2 - in.x1 >= ww->min_w && in.y2 - in.y1 >= ww->min_h);
}

/* May ww split leaf t: on the side split, both halves at least the
 * tiler's minimum (the other side stays as it is), and each window's own
 * minimum fits its half (t's in a, ww in b)? */
static bool split_fits(const struct tile_node *t, const struct wm_window *ww)
{
    struct comp_box a, b;
    halves(t, &a, &b);
    bool across = t->box.x2 - t->box.x1 >= t->box.y2 - t->box.y1;   /* as halves() splits */
    bool big = across ? a.x2 - a.x1 >= min_w && b.x2 - b.x1 >= min_w
                      : a.y2 - a.y1 >= min_h && b.y2 - b.y1 >= min_h;
    struct comp_box in = deco_inner(a, 0, COMP_TILING);   /* a placeholder: its window's minimum */
    bool held_fits = t->ww || (in.x2 - in.x1 >= t->hold_min_w && in.y2 - in.y1 >= t->hold_min_h);
    return big && min_fits(t->ww, a) && held_fits && min_fits(ww, b);
}

/* The tile a new window on s splits: the focused one if it is on s, else
 * the last; NULL: none (the tree is empty). */
static struct tile_node *split_target(const struct desk_screen *s, bool fresh)
{
    const struct wm_window *f = wm_focused();
    return !fresh && f && f->leaf && f->tiled_on == s ? f->leaf : last_leaf(s->tree);
}

/* Windows that went to a new screen in this update (the caller's screens
 * then sync: one left the current screen). */
static bool spilled;

/* s's windows not in its tree yet go in; one whose split would leave a
 * half too small goes to a new screen instead, and the others that don't
 * fit after it too, in order (that screen's own update places them). */
static void add_missing(struct desk_screen *s)
{
    bool fresh = !s->tree;
    struct desk_screen *spill = NULL;
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next) {
        if (ww->leaf || !tiled_here(ww, s))
            continue;
        struct tile_node *t = split_target(s, fresh);
        if (t && !split_fits(t, ww) && (spill || (spill = screens_spill()))) {
            printf("compositor: no room for another tile on screen %d: a new screen, %d\n",
                   screens_index(s) + 1, screens_index(spill) + 1);
            ww->screen = spill;   /* (screens_spill: at the end, so its turn comes) */
            spilled = true;
            continue;
        }
        if (!insert(s, ww, t))   /* (no room for a screen: the split, however small) */
            printf("compositor: no memory for a tile: a window over the others\n");
    }
}

void tiles_update(void)
{
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (ww->leaf && (!tiled_here(ww, ww->tiled_on) || !tiling(ww->tiled_on)))
            remove_leaf(ww);   /* gone, hidden, moved, or its screen floats now */
    spilled = false;
    for (unsigned i = 0; i < screens_count(); i++) {   /* a screen spilled to is the last */
        struct desk_screen *s = screens_nth(i);
        if (!tiling(s))
            continue;
        add_missing(s);
        layout(s);
    }
    if (spilled)
        screens_sync();   /* a window that left the current screen is hidden */
}

/* ---- placeholders: a restarted compositor's remembered places (wmsave.c) -------------------- */

/* Every node of s's tree freed, whole or not (a build cut short): from
 * the bottom, one childless node at a time. */
static void free_all(struct desk_screen *s)
{
    while (s->tree) {
        struct tile_node *t = s->tree;
        for (;;) {
            if (t->a)
                t = t->a;
            else if (t->b)
                t = t->b;
            else
                break;
        }
        if (t->ww && t->ww->leaf == t) {
            t->ww->leaf = NULL;
            t->ww->tiled_on = NULL;
        }
        if (!t->up)
            s->tree = NULL;
        else if (t->up->a == t)
            t->up->a = NULL;
        else
            t->up->b = NULL;
        free(t);
    }
    generation++;
}

bool tiles_build(struct desk_screen *s, const struct tile_spec *spec, unsigned n)
{
    /* a whole tree in pre-order, nothing after it: each split asks one
     * more node than it gives, each leaf gives one */
    unsigned need = 1;
    for (unsigned i = 0; i < n; i++) {
        if (!need)
            return false;
        need = spec[i].split ? need + 1 : need - 1;
    }
    if (need || s->tree)
        return false;
    struct tile_node *open = NULL;   /* the deepest split still missing a half */
    for (unsigned i = 0; i < n; i++) {
        struct tile_node *t = calloc(1, sizeof(*t));
        if (!t) {
            free_all(s);
            return false;
        }
        t->up = open;
        if (!open)
            s->tree = t;
        else if (!open->a)
            open->a = t;
        else
            open->b = t;
        if (spec[i].split) {
            t->across = spec[i].across;
            t->ratio = spec[i].ratio;
            open = t;
            continue;
        }
        t->hold = spec[i].hold;
        t->hold_min_w = spec[i].min_w;
        t->hold_min_h = spec[i].min_h;
        while (open && open->b)   /* whole: up to the next split missing a half */
            open = open->up;
    }
    generation++;
    /* the places of windows not remembered go now, as closed windows' */
    for (bool again = true; again;) {
        again = false;
        for (struct tile_node *t = s->tree; t; t = next_node(t))
            if (!t->a && !t->hold) {
                remove_node(s, t);
                again = true;
                break;
            }
    }
    layout(s);
    return true;
}

/* The placeholder keeping key's place, on s (NULL: any screen). */
static struct tile_node *held(uint64_t key, const struct desk_screen *s)
{
    for (unsigned i = 0; key && i < screens_count(); i++) {
        struct desk_screen *sc = screens_nth(i);
        if (s && sc != s)
            continue;
        for (struct tile_node *t = sc->tree; t; t = next_node(t))
            if (!t->a && !t->ww && t->hold == key)
                return t;
    }
    return NULL;
}

bool tiles_held_box(uint64_t key, struct comp_box *out)
{
    const struct tile_node *t = held(key, NULL);
    if (t)
        *out = t->box;
    return t != NULL;
}

void tiles_take_hold(struct wm_window *ww)
{
    for (unsigned i = 0; i < screens_count(); i++) {
        struct desk_screen *s = screens_nth(i);
        struct tile_node *t = held(ww->key, s);
        if (!t)
            continue;
        if (s == ww->screen && !ww->leaf && tiling(s) && tiled_here(ww, s)) {
            t->ww = ww;
            t->hold = 0;
            ww->leaf = t;
            ww->tiled_on = s;
            generation++;
        } else {
            remove_node(s, t);   /* minimised, moved, its screen floating: the place goes */
            layout(s);
        }
        return;
    }
}

unsigned tiles_drop_holds(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < screens_count(); i++) {
        struct desk_screen *s = screens_nth(i);
        for (bool again = true; again;) {
            again = false;
            for (struct tile_node *t = s->tree; t; t = next_node(t))
                if (!t->a && !t->ww) {
                    remove_node(s, t);
                    n++;
                    again = true;
                    break;
                }
        }
        layout(s);
    }
    return n;
}

/* ---- what the window manager asks ----------------------------------------------------------- */


struct comp_box wm_tile(const struct wm_window *ww)
{
    const struct desk_screen *s = ww->screen ? ww->screen : screens_cur();
    if (ww->leaf && ww->tiled_on == s)
        return ww->leaf->box;
    struct comp_box place;
    if (!ww->win && ww->restoring && tiles_held_box(ww->key, &place))
        return place;   /* a remembered window not back yet: its place */
    const struct tile_node *t = s->tree ? split_target(s, false) : NULL;
    if (!t || !split_fits(t, ww))
        return tile_area(s);   /* the first; or it will go to a new screen, all its room */
    struct comp_box a, b;
    halves(t, &a, &b);
    return b;   /* the half it gets */
}

bool tiles_swap(struct wm_window *a, struct wm_window *b)
{
    if (a == b || !a->leaf || !b->leaf || a->tiled_on != b->tiled_on)
        return false;
    struct tile_node *la = a->leaf, *lb = b->leaf;
    la->ww = b;
    lb->ww = a;
    a->leaf = lb;
    b->leaf = la;
    generation++;
    return true;
}

/* ---- gaps ---------------------------------------------------------------------------------- */

/* Split n's gap, its hit box and its bar. */
struct tile_gap tiles_gap(struct tile_node *n)
{
    struct tile_gap g = { .split = n };
    struct comp_box r = n->box;
    if (n->across) {
        int32_t x = n->a->box.x2, mid = x + WM_GAP / 2, len = r.y2 - r.y1;
        int32_t in = (int32_t)((int64_t)len * GAP_BAR_INSET / 1000);
        g.gap = (struct comp_box){ x, r.y1, x + WM_GAP, r.y2 };
        g.hit = (struct comp_box){ mid - WM_GAP_HIT / 2, r.y1, mid + WM_GAP_HIT / 2, r.y2 };
        g.bar = (struct comp_box){ mid - GAP_BAR_W / 2, r.y1 + in, mid - GAP_BAR_W / 2 + GAP_BAR_W,
                                   r.y2 - in };
    } else {
        int32_t y = n->a->box.y2, mid = y + WM_GAP / 2, len = r.x2 - r.x1;
        int32_t in = (int32_t)((int64_t)len * GAP_BAR_INSET / 1000);
        g.gap = (struct comp_box){ r.x1, y, r.x2, y + WM_GAP };
        g.hit = (struct comp_box){ r.x1, mid - WM_GAP_HIT / 2, r.x2, mid + WM_GAP_HIT / 2 };
        g.bar = (struct comp_box){ r.x1 + in, mid - GAP_BAR_W / 2, r.x2 - in,
                                   mid - GAP_BAR_W / 2 + GAP_BAR_W };
    }
    return g;
}

bool tiles_gap_at(const struct desk_screen *s, int32_t x, int32_t y, struct tile_gap *out)
{
    if (!s || !tiling(s))
        return false;
    for (struct tile_node *n = s->tree; n; n = next_node(n)) {
        if (!n->a)
            continue;
        struct tile_gap g = tiles_gap(n);
        if (box_contains(g.hit, x, y)) {
            *out = g;   /* pre-order: the outermost split first */
            return true;
        }
    }
    return false;
}

void tiles_gap_move(struct tile_node *split, int32_t pos)
{
    struct comp_box r = split->box;
    int64_t start = split->across ? r.x1 : r.y1;
    int64_t len = split->across ? (int64_t)r.x2 - r.x1 : (int64_t)r.y2 - r.y1;
    if (len - WM_GAP <= 0)
        return;
    /* a's length the pointer asks for, stopped where either half would get
     * less than it needs; a gap already past that (the room shrank, a
     * minimum came later) may only move back towards it */
    int64_t p = (int64_t)pos - start - WM_GAP / 2;
    int64_t now_a = split->across ? (int64_t)split->a->box.x2 - split->a->box.x1
                                  : (int64_t)split->a->box.y2 - split->a->box.y1;
    int64_t lo = split->across ? split->a->need_w : split->a->need_h;
    int64_t hi = len - WM_GAP - (split->across ? split->b->need_w : split->b->need_h);
    int64_t least = lo < hi ? lo : hi, most = lo < hi ? hi : lo;
    least = now_a < least ? now_a : least;
    most = now_a > most ? now_a : most;
    p = p < least ? least : p > most ? most : p;
    int64_t v = (p * TILE_ONE + (len - WM_GAP) / 2) / (len - WM_GAP);
    split->ratio = (int32_t)(v < TILE_MIN ? TILE_MIN : v > TILE_MAX ? TILE_MAX : v);
}

/* The nearest split above leaf l cut the way given (across or not) with l
 * on the side `on_a` says: the gap on that edge of l's tile. */
static struct tile_node *edge_split(struct tile_node *l, bool across, bool on_a)
{
    for (struct tile_node *n = l; n->up; n = n->up)
        if (n->up->across == across && (n->up->a == n) == on_a)
            return n->up;
    return NULL;
}

bool tiles_push(struct wm_window *ww, int dx, int dy)
{
    if (!ww->leaf || (!dx && !dy))
        return false;
    bool across = dx != 0;
    int sign = (across ? dx : dy) > 0 ? 1 : -1;
    /* the edge on that side: right or bottom is a's gap, left or top b's */
    struct tile_node *s = edge_split(ww->leaf, across, sign > 0);
    if (!s)
        s = edge_split(ww->leaf, across, sign < 0);   /* none there: the other edge */
    if (!s)
        return false;
    struct tile_gap g = tiles_gap(s);
    int32_t mid = across ? g.gap.x1 + WM_GAP / 2 : g.gap.y1 + WM_GAP / 2;
    int32_t before = s->ratio;
    tiles_gap_move(s, mid + sign * WM_PUSH);
    return s->ratio != before;
}
