/* The scene (comp.h): the output's size and damage, and the windows in
 * stacking order, bottom to top. Mechanism only: where a window goes,
 * which one is raised and how big it is are the window manager's
 * decisions (wm.c); this file keeps them and damages what each change
 * uncovers or covers, so the next paint composes exactly that.
 *
 * Every change damages the window's whole extent (its frame and its
 * shadow) before and after it (moving, mapping, raising, going): cheap,
 * since it is one box, and never wrong. Damage from windows is also
 * `under` damage: what the desktop's frosted cards must blur again
 * (frost.c); the cursor's and the cards' own is not (scene_damage_over).
 *
 * A window sliding with its screen (anim.c) is at x + slide_x for
 * everything: painting, damage and what is under a point. */
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
    return box_make(w->x + w->slide_x, w->y, w->surface->width, w->surface->height);
}

struct comp_box window_frame(const struct comp_window *w)
{
    struct comp_box s = window_surface_box(w);
    if (box_empty(s))
        return s;
    struct comp_box f = { s.x1 - w->deco_left, s.y1 - w->deco_top, s.x2 + w->deco_right,
                          s.y2 + w->deco_bottom };
    return box_empty(f) ? s : f;
}

struct comp_box window_extent(const struct comp_window *w)
{
    struct comp_box f = window_frame(w);
    if (box_empty(f) || look_of(w) != LOOK_FLOATING)
        return f;
    return (struct comp_box){ f.x1 - LOOK_SHADOW_SIDE, f.y1 - LOOK_SHADOW_ABOVE,
                              f.x2 + LOOK_SHADOW_SIDE, f.y2 + LOOK_SHADOW_BELOW };
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
    scene_damage(box_translate(box_intersect(b, s), w->x + w->slide_x, w->y));
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

/* Does the surface of w take input at output (x, y)? */
static bool takes_input(const struct comp_window *w, int32_t x, int32_t y)
{
    const struct comp_surface *s = w->surface;
    if (!box_contains(window_surface_box(w), x, y))
        return false;
    return s->input_all || region_contains(&s->input, x - w->x - w->slide_x, y - w->y);
}

struct comp_window *window_at(int32_t x, int32_t y)
{
    for (struct comp_window *w = scene.top; w; w = w->below)
        if ((w->flags & COMP_WIN_MAPPED) && takes_input(w, x, y))
            return w;
    return NULL;
}

bool surface_visible(const struct comp_surface *s)
{
    const struct comp_window *w = s->window;
    if (!w || !(w->flags & COMP_WIN_MAPPED))
        return false;
    return !box_empty(box_intersect(window_surface_box(w), output_box())) && !window_covered(w);
}
