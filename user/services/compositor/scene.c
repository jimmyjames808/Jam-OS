/* The scene (comp.h): the output's size and damage, and the windows in
 * stacking order, bottom to top. Mechanism only: where a window goes,
 * which one is raised and how big it is are the window manager's
 * decisions (wm.c); this file keeps them and damages what each change
 * uncovers or covers, so the next paint composes exactly that.
 *
 * Every change damages the window's whole extent (its frame, its shadow
 * and its surface) before and after it (moving, mapping, raising, going): cheap,
 * since it is one box, and never wrong. Damage from windows is also
 * `under` damage: what the desktop's frosted cards must blur again
 * (frost.c); the cursor's and the cards' own is not (scene_damage_over).
 *
 * A window sliding with its screen or gliding to its tile (anim.c) is at
 * (x + slide_x, y + slide_y) for everything: painting, damage and what is
 * under a point (window_shown_x/y). */
#include "desk.h"

struct comp_scene scene;

static struct comp_box output_box(void)
{
    return (struct comp_box){ 0, 0, scene.width, scene.height };
}

void scene_init(int32_t w, int32_t h, uint32_t background)
{
    scene.width = w;
    scene.height = h;
    scene.background = background;
    scene.bottom = scene.top = NULL;
    scene.nwindows = 0;
    scene.layout = COMP_FLOATING;
    damage_init(&scene.damage, scene.damage_boxes, COMP_OUTPUT_DAMAGE_MAX);
    damage_init(&scene.under, scene.under_boxes, COMP_OUTPUT_DAMAGE_MAX);
    scene_damage(output_box());
}

void scene_damage(struct comp_box b)
{
    b = box_intersect(b, output_box());
    damage_add(&scene.damage, b);
    damage_add(&scene.under, b);
}

void scene_damage_over(struct comp_box b)
{
    damage_add(&scene.damage, box_intersect(b, output_box()));
}

struct comp_box window_surface_box(const struct comp_window *w)
{
    if (w->view_w > 0 && w->view_h > 0)
        return box_make(window_shown_x(w), window_shown_y(w), w->view_w, w->view_h);
    return box_make(window_shown_x(w), window_shown_y(w), w->surface->width, w->surface->height);
}

void window_view(struct comp_window *w, int32_t view_w, int32_t view_h)
{
    if (w->view_w == view_w && w->view_h == view_h)
        return;
    window_damage(w);
    w->view_w = view_w;
    w->view_h = view_h;
    window_damage(w);
}

/* ---- subsurface trees ------------------------------------------------------------------ */

/* The walk under s, depth parents above it (a tree is never deeper than
 * COMP_SUB_DEPTH_MAX: subsurface.c refuses one that would be; the bound
 * here only keeps the stack bounded whatever happens). */
static bool walk(const struct comp_surface *s, int32_t x, int32_t y, bool top_first,
                 unsigned depth, surface_fn fn, void *arg)
{
    const struct comp_tree *t = s->tree;
    uint32_t n = t ? t->ncur : 1;
    for (uint32_t k = 0; k < n; k++) {
        const struct comp_surface *e = t ? t->cur[top_first ? n - 1 - k : k] : s;
        if (e == s) {
            if (!fn(s, x, y, arg))
                return false;
        } else if (e->buffer && e->sub && depth < COMP_SUB_DEPTH_MAX &&
                   !walk(e, x + e->sub->x, y + e->sub->y, top_first, depth + 1, fn, arg)) {
            return false;
        }
    }
    return true;
}

bool surface_tree_walk(const struct comp_surface *s, int32_t x, int32_t y, bool top_first,
                       surface_fn fn, void *arg)
{
    return walk(s, x, y, top_first, 0, fn, arg);
}

static bool grow(const struct comp_surface *s, int32_t x, int32_t y, void *arg)
{
    struct comp_box *b = arg;
    *b = box_bounds(*b, box_make(x, y, s->width, s->height));
    return true;
}

struct comp_box window_tree_box(const struct comp_window *w)
{
    const struct comp_surface *s = w->surface;
    struct comp_box b = box_make(0, 0, s->width, s->height);
    if (s->tree)
        (void)surface_tree_walk(s, 0, 0, false, grow, &b);
    return b;
}

bool window_has_subsurfaces(const struct comp_window *w)
{
    return w->surface->tree && w->surface->tree->ncur > 1;
}

struct comp_window *surface_tree_window(const struct comp_surface *s)
{
    for (unsigned d = 0; d <= COMP_SUB_DEPTH_MAX && s; d++) {
        if (s->role != COMP_ROLE_SUBSURFACE)
            return s->window;
        s = s->sub ? s->sub->parent : NULL;
    }
    return NULL;
}

struct comp_window *surface_on_output(const struct comp_surface *s, int32_t *x, int32_t *y)
{
    int32_t dx = 0, dy = 0;
    for (unsigned d = 0; d <= COMP_SUB_DEPTH_MAX; d++) {
        if (s->role != COMP_ROLE_SUBSURFACE) {
            struct comp_window *w = s->window;
            if (!w || !(w->flags & COMP_WIN_MAPPED))
                return NULL;
            *x = window_shown_x(w) + dx;
            *y = window_shown_y(w) + dy;
            return w;
        }
        const struct comp_sub *sub = s->sub;
        if (!s->buffer || !sub || !sub->parent || !sub->added)
            return NULL;
        dx += sub->x;
        dy += sub->y;
        s = sub->parent;   /* (a parent with no buffer hides its children: the next turn) */
    }
    return NULL;
}

/* Does s, its (0, 0) at output (sx, sy), take input at (x, y)? */
static bool surface_takes(const struct comp_surface *s, int32_t sx, int32_t sy, int32_t x,
                          int32_t y)
{
    return s->input_all || region_contains(&s->input, x - sx, y - sy);
}

/* What window_input_at looks for, and what it found. */
struct hit {
    const struct comp_window *w;
    int32_t x, y;                  /* the point on the output */
    struct comp_box clip;          /* where subsurfaces are drawn: the window geometry */
    const struct comp_surface *found;
    int32_t sx, sy;
};

static bool hit_one(const struct comp_surface *s, int32_t sx, int32_t sy, void *arg)
{
    struct hit *h = arg;
    struct comp_box box = s == h->w->surface ? window_surface_box(h->w)
                                             : box_intersect(box_make(sx, sy, s->width, s->height),
                                                             h->clip);
    if (!box_contains(box, h->x, h->y) || !surface_takes(s, sx, sy, h->x, h->y))
        return true;   /* on to the next one down */
    h->found = s;
    h->sx = sx;
    h->sy = sy;
    return false;
}

struct comp_surface *window_input_at(const struct comp_window *w, int32_t x, int32_t y,
                                     int32_t *sx, int32_t *sy)
{
    struct hit h = { w, x, y, { 0, 0, 0, 0 }, NULL, 0, 0 };
    if (w->surface->tree)
        h.clip = window_geometry(w);
    (void)surface_tree_walk(w->surface, window_shown_x(w), window_shown_y(w), true, hit_one, &h);
    if (h.found && sx)
        *sx = h.sx;
    if (h.found && sy)
        *sy = h.sy;
    return (struct comp_surface *)h.found;
}

void surface_damage(const struct comp_surface *s, struct comp_box b)
{
    if (s->role != COMP_ROLE_SUBSURFACE) {
        if (s->window)
            window_damage_surface(s->window, b);
        return;
    }
    int32_t x, y;
    struct comp_window *w = surface_on_output(s, &x, &y);
    if (!w)
        return;
    struct comp_box in = box_intersect(b, box_make(0, 0, s->width, s->height));
    scene_damage(box_intersect(box_translate(in, x, y), window_geometry(w)));
}

/* ---- the window's boxes ---------------------------------------------------------------- */

struct comp_box window_geom(const struct comp_window *w)
{
    struct comp_box t = window_tree_box(w);
    struct comp_box g = box_intersect(w->geom, t);   /* xdg-shell: clamped to the tree */
    return box_empty(g) ? t : g;
}

struct comp_box window_geometry(const struct comp_window *w)
{
    if (w->view_w > 0 && w->view_h > 0)
        return window_surface_box(w);   /* a tile's view (wm.c sets one only at (0, 0)) */
    return box_translate(window_geom(w), window_shown_x(w), window_shown_y(w));
}

bool window_inset(const struct comp_window *w)
{
    struct comp_box g = window_geom(w);
    return g.x1 > 0 || g.y1 > 0 || g.x2 < w->surface->width || g.y2 < w->surface->height;
}

void window_set_geom(struct comp_window *w, struct comp_box geom)
{
    if (w->geom.x1 == geom.x1 && w->geom.y1 == geom.y1 && w->geom.x2 == geom.x2 &&
        w->geom.y2 == geom.y2)
        return;
    window_damage(w);
    w->geom = geom;
    window_damage(w);
}

struct comp_box window_frame(const struct comp_window *w)
{
    struct comp_box s = window_geometry(w);
    if (box_empty(s))
        return s;
    struct comp_box f = { s.x1 - w->deco_left, s.y1 - w->deco_top, s.x2 + w->deco_right,
                          s.y2 + w->deco_bottom };
    return box_empty(f) ? s : f;
}

struct comp_box window_extent(const struct comp_window *w)
{
    struct comp_box f = window_frame(w), s = window_surface_box(w);
    if (box_empty(f) || !look_shadowed(look_of(w)))
        return box_bounds(f, s);
    f = (struct comp_box){ f.x1 - LOOK_SHADOW_SIDE, f.y1 - LOOK_SHADOW_ABOVE,
                           f.x2 + LOOK_SHADOW_SIDE, f.y2 + LOOK_SHADOW_BELOW };
    return box_bounds(f, s);
}

void window_damage(struct comp_window *w)
{
    if (w->flags & COMP_WIN_MAPPED)
        scene_damage(window_extent(w));
}

void window_damage_surface(struct comp_window *w, struct comp_box b)
{
    if (!(w->flags & COMP_WIN_MAPPED))
        return;
    struct comp_box s = box_make(0, 0, w->surface->width, w->surface->height);
    scene_damage(box_translate(box_intersect(b, s), window_shown_x(w), window_shown_y(w)));
}

/* Take w out of the stacking order. */
static void unlink(struct comp_window *w)
{
    if (w->below)
        w->below->above = w->above;
    else
        scene.bottom = w->above;
    if (w->above)
        w->above->below = w->below;
    else
        scene.top = w->below;
    w->below = w->above = NULL;
}

/* Put w on top; a window that is no boot overlay goes under the overlays
 * on top (COMP_WIN_OVERLAY: they stay over every other window). */
static void push_top(struct comp_window *w)
{
    struct comp_window *over = NULL;   /* the lowest overlay of those on top */
    if (!(w->flags & COMP_WIN_OVERLAY))
        for (struct comp_window *t = scene.top; t && (t->flags & COMP_WIN_OVERLAY); t = t->below)
            over = t;
    w->above = over;
    w->below = over ? over->below : scene.top;
    if (w->below)
        w->below->above = w;
    else
        scene.bottom = w;
    if (over)
        over->below = w;
    else
        scene.top = w;
}

status_t window_create(struct comp_surface *s, int32_t x, int32_t y, struct comp_window **out)
{
    struct comp_window *w = calloc(1, sizeof(*w));
    if (!w)
        return ERR_NO_MEMORY;
    w->surface = s;
    w->x = x;
    w->y = y;
    push_top(w);
    scene.nwindows++;
    s->window = w;
    *out = w;
    return OK;
}

void window_destroy(struct comp_window *w)
{
    anim_forget(w);        /* an animation drawing it ends */
    seat_window_gone(w);   /* its focus moves on while it is still in the order */
    window_damage(w);
    unlink(w);
    scene.nwindows--;
    w->surface->window = NULL;
    free(w);
}

void window_map(struct comp_window *w, bool mapped)
{
    if (mapped == !!(w->flags & COMP_WIN_MAPPED))
        return;
    if (!mapped)
        seat_window_gone(w);   /* its focus moves on */
    window_damage(w);   /* the frame it leaves (unmapping) */
    w->flags = mapped ? w->flags | COMP_WIN_MAPPED : w->flags & ~COMP_WIN_MAPPED;
    window_damage(w);   /* the frame it covers (mapping) */
    if (mapped)
        seat_window_mapped(w);   /* a client's first window takes the keyboard */
}

void window_move(struct comp_window *w, int32_t x, int32_t y)
{
    if (w->x == x && w->y == y)
        return;
    window_damage(w);
    w->x = x;
    w->y = y;
    window_damage(w);
}

void window_raise(struct comp_window *w)
{
    if (scene.top == w)
        return;
    unlink(w);
    push_top(w);
    window_damage(w);
}

struct comp_window *window_at(int32_t x, int32_t y)
{
    for (struct comp_window *w = scene.top; w; w = w->below)
        if ((w->flags & COMP_WIN_MAPPED) && window_input_at(w, x, y, NULL, NULL))
            return w;
    return NULL;
}

bool surface_visible(const struct comp_surface *s)
{
    int32_t x, y;
    const struct comp_window *w = surface_on_output(s, &x, &y);
    if (!w)
        return false;
    return !box_empty(box_intersect(window_surface_box(w), output_box())) && !window_covered(w);
}
