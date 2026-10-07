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
 *     one reached by always taking b: the newest corner of the spiral. If
 *     the halves would be narrower than WM_TILE_MIN_W (split across) or
 *     shorter than WM_TILE_MIN_H (split down), about 20 columns or 5 lines
 *     of a terminal, the largest tile splits instead:
 *     with many windows (16 terminals) no tile shrinks to a sliver, and
 *     the spiral turns into a grid.
 *   - A screen switched from floating (its tree made from nothing) takes
 *     its windows in the order they opened, each splitting the last.
 *   - A window that goes (closed, unmapped, minimised, moved to another
 *     screen) leaves its sibling the whole of their parent's room.
 *   - Two windows swap by trading leaves; a gap moves by changing its
 *     split's ratio (dragged: wmgrab.c; Super+Alt+direction: tiles_push).
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

/* Split n's halves' boxes from its own. */
static void split_boxes(struct tile_node *n)
{
    struct comp_box r = n->box;
    int32_t part = part_of(n->across ? (int64_t)r.x2 - r.x1 : (int64_t)r.y2 - r.y1, n->ratio);
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
        a->ww->leaf = a;
        t->ww = NULL;
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

/* Would halving t (along its longer side) make halves narrower than
 * WM_TILE_MIN_W, or shorter than WM_TILE_MIN_H? (The other side stays.) */
static bool too_small_to_split(const struct tile_node *t)
{
    int32_t w = t->box.x2 - t->box.x1, h = t->box.y2 - t->box.y1;
    return w >= h ? (w - WM_GAP) / 2 < WM_TILE_MIN_W : (h - WM_GAP) / 2 < WM_TILE_MIN_H;
}

/* s's largest tile by area (the first in pre-order of equals). */
static struct tile_node *largest_leaf(struct tile_node *root)
{
    struct tile_node *best = NULL;
    int64_t most = -1;
    for (struct tile_node *n = root; n; n = next_node(n)) {
        int64_t area = n->a ? -1
                            : (int64_t)(n->box.x2 - n->box.x1) * (n->box.y2 - n->box.y1);
        if (area > most) {
            most = area;
            best = n;
        }
    }
    return best;
}

/* The tile a new window on s splits: the focused one if it is on s, else
 * the last; if halving that one would make a tile too small to use, the
 * largest instead (many windows: no slivers, a spiral turns into a grid);
 * NULL: none (the tree is empty). */
static struct tile_node *split_target(const struct desk_screen *s, bool fresh)
{
    const struct wm_window *f = wm_focused();
    struct tile_node *t = !fresh && f && f->leaf && f->tiled_on == s ? f->leaf
                                                                     : last_leaf(s->tree);
    return t && too_small_to_split(t) ? largest_leaf(s->tree) : t;
}

static void add_missing(struct desk_screen *s)
{
    bool fresh = !s->tree;
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (!ww->leaf && tiled_here(ww, s) && !insert(s, ww, split_target(s, fresh)))
            printf("compositor: no memory for a tile: a window over the others\n");
}

void tiles_update(void)
{
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (ww->leaf && (!tiled_here(ww, ww->tiled_on) || !tiling(ww->tiled_on)))
            remove_leaf(ww);   /* gone, hidden, moved, or its screen floats now */
    for (unsigned i = 0; i < screens_count(); i++) {
        struct desk_screen *s = screens_nth(i);
        if (!tiling(s))
            continue;
        add_missing(s);
        layout(s);
    }
}

/* ---- what the window manager asks ----------------------------------------------------------- */

/* The half of t's box a window splitting it gets (b's). */
static struct comp_box new_half(const struct tile_node *t)
{
    struct comp_box r = t->box;
    bool across = r.x2 - r.x1 >= r.y2 - r.y1;
    int32_t part = part_of(across ? (int64_t)r.x2 - r.x1 : (int64_t)r.y2 - r.y1, TILE_ONE / 2);
    if (across)
        r.x1 += part + WM_GAP;
    else
        r.y1 += part + WM_GAP;
    return r;
}

struct comp_box wm_tile(const struct wm_window *ww)
{
    const struct desk_screen *s = ww->screen ? ww->screen : screens_cur();
    if (ww->leaf && ww->tiled_on == s)
        return ww->leaf->box;
    const struct tile_node *t = s->tree ? split_target(s, false) : NULL;
    return t ? new_half(t) : tile_area(s);
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
    int64_t v = ((pos - start - WM_GAP / 2) * TILE_ONE + (len - WM_GAP) / 2) / (len - WM_GAP);
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
