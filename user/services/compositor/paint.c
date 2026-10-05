/* Painting (comp.h): the scene's damage composed onto the output.
 *
 * A paint, run by the loop's thread when the clock says (clock.c):
 *   1. the damage boxes (which may overlap) become disjoint boxes, so no
 *      pixel is composed twice (more than COMP_REGION_RECTS_MAX pieces:
 *      their bounding box, correct and only more pixels);
 *   2. each box is cut into tiles of at most TILE_W by TILE_H pixels;
 *   3. libfun's pool runs the tiles on its workers (the loop's thread is
 *      one of them) and the loop waits, so the scene can't change under
 *      a paint and nothing needs a lock: the workers only read it.
 *
 * One tile, by one worker, into a buffer of its own (cached memory):
 *   - where to start: the topmost window that covers the whole tile with
 *     pixels that hide what is below (paint_opaque_over, decorations are
 *     opaque too, a rounded corner's square is not: shape.c);
 *     everything under it is skipped, the wallpaper too. With none, the
 *     wallpaper first (wallpaper.c; black while blank);
 *   - then each mapped window from there up: its shadow (shape.c), the
 *     pixels below its corners kept, its decorations (title.c), then its
 *     buffer: copied (xrgb8888, or inside its opaque region), or blended
 *     over what is below (argb8888, premultiplied, with libfun's
 *     px_over_row: px_over's exact rounding), then its corners cut round;
 *   - the desktop over the windows (deskpaint.c): animations, the
 *     strip, then the cards;
 *   - the cursor last (cursor.c);
 *   - the tile's rows to the output (output_put), which is never read.
 * A tile wholly inside the strip starts with it (desk_hides): nothing
 * under it shows. A window an animation draws (COMP_WIN_ANIMATED) is
 * skipped, and hides nothing.
 * The wallpaper is copied only where nothing opaque is, so a tile under
 * one opaque window costs one copy in and one write out.
 *
 * Before a paint, the window whose title bar circles the pointer is over
 * is looked up (title_hovered), and the circles damaged when it changes.
 *
 * Before the tiles, the desktop lays out its strip and remakes the
 * blurred backdrops of its cards that went stale (frost.c), on the same
 * workers. The `under` damage is cleared with the rest.
 *
 * The full-screen path: when the topmost visible window is opaque and its
 * surface covers the whole output (the splash, a full-screen game), and
 * the desktop draws nothing over the windows, its
 * damage is copied straight from its buffer to the output, row by row,
 * with no tile buffer: today's libfun present, one copy. The cursor's box
 * still goes through a tile, so the arrow is drawn over it.
 *
 * Client pixels are read only through the compositor's VMAR_KEPT_ONLY
 * mapping of the pool, inside the bounds the buffer was checked against
 * (shm.c): a client can change what its pixels are, never where we read,
 * and a read never faults. A pixel is only ever a pixel: nothing read
 * from it decides anything here. */
#include <fun.h>
#include <jwl/wayland.h>
#include "desk.h"

#define THREADS_DEFAULT 4   /* the framebuffer saturates at two (fbbench); blends want a few */

/* A tile to paint: its box, and whether it is copied straight from the
 * full-screen window's buffer (no tile buffer). */
struct tile {
    struct comp_box b;
    bool direct;
};

/* Each worker's count, on a cache line of its own. */
struct worker_count {
    uint64_t layer_px;
    uint64_t pad[7];
};

static struct {
    struct tile *t;               /* this paint's tiles */
    uint32_t n, cap;
    uint32_t threads;             /* the pool's */
    uint32_t *buf[FUN_MAX_THREADS];   /* each worker's tile pixels, TILE_W * TILE_H, then
                                       * SHAPE_SAVE_PX for the pixels below a window's corners */
    struct worker_count count[FUN_MAX_THREADS];
    const struct comp_window *full;   /* this paint's full-screen window, or NULL */
} pt;

struct paint_stats paint_last;

/* ---- what hides what ------------------------------------------------------------------ */

/* Is all of a inside b? */
static bool inside(struct comp_box a, struct comp_box b)
{
    return a.x1 >= b.x1 && a.y1 >= b.y1 && a.x2 <= b.x2 && a.y2 <= b.y2;
}

bool paint_opaque_over(const struct comp_window *w, struct comp_box in)
{
    const struct comp_surface *s = w->surface;
    if (!s->buffer)
        return false;
    if (s->buffer->format != JWL_WL_SHM_FORMAT_ARGB8888)
        return true;
    struct comp_box rel = box_translate(in, -w->x - w->slide_x, -w->y);
    for (uint32_t i = 0; i < s->opaque.n; i++)
        if (inside(rel, s->opaque.b[i]))
            return true;
    return false;
}

/* Does w hide all of b (b not empty)? Its frame holds b, b is clear of its
 * round corners, and the part of b on its surface is opaque (decorations
 * always are). */
static bool hides(const struct comp_window *w, struct comp_box b)
{
    if (!(w->flags & COMP_WIN_MAPPED) || (w->flags & COMP_WIN_ANIMATED) || !w->surface->buffer ||
        !inside(b, window_frame(w)) || shape_corner_meets(w, b))
        return false;
    struct comp_box in = box_intersect(b, window_surface_box(w));
    return box_empty(in) || paint_opaque_over(w, in);
}

/* The topmost window hiding all of b, or NULL. */
static const struct comp_window *cull(struct comp_box b)
{
    for (const struct comp_window *w = scene.top; w; w = w->below)
        if (hides(w, b))
            return w;
    return NULL;
}

bool window_covered(const struct comp_window *w)
{
    struct comp_box out = { 0, 0, scene.width, scene.height };
    struct comp_box b = box_intersect(window_surface_box(w), out);
    if (!(w->flags & COMP_WIN_MAPPED) || box_empty(b))
        return true;
    for (const struct comp_window *a = w->above; a; a = a->above)
        if (hides(a, b))
            return true;
    return false;
}

/* The full-screen window: the topmost one shown, if its surface covers the
 * whole output with opaque pixels (no round corner on the output either). */
static const struct comp_window *full_screen(void)
{
    struct comp_box out = { 0, 0, scene.width, scene.height };
    if (comp.blanked || desk_over_windows())
        return NULL;
    for (const struct comp_window *w = scene.top; w; w = w->below) {
        if (!(w->flags & COMP_WIN_MAPPED) || box_empty(box_intersect(window_extent(w), out)))
            continue;
        if (w->flags & COMP_WIN_ANIMATED)
            return NULL;
        bool all = inside(out, window_surface_box(w)) && paint_opaque_over(w, out) &&
                   !shape_corner_meets(w, out);
        return all ? w : NULL;
    }
    return NULL;
}

/* ---- one tile ------------------------------------------------------------------------- */

/* n pixels from src to dst, the top byte cleared (an xrgb pixel's x is
 * anything; a composed pixel's top byte is always 0). */
static void copy_rgb(uint32_t *restrict dst, const uint32_t *restrict src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = src[i] & 0xffffff;
}

/* Row y of window w's buffer, from output column x. */
static const uint32_t *buffer_at(const struct comp_window *w, int32_t x, int32_t y)
{
    const struct comp_buffer *b = w->surface->buffer;
    return (const uint32_t *)(const void *)(comp_buffer_data(b) +
                                            (uint64_t)(y - w->y) * b->stride) +
           (x - w->x - w->slide_x);
}

/* Window w's buffer where it meets t (me: the worker, counting; ~0u: none). */
static void draw_buffer(const struct comp_window *w, const struct tile_buf *t, uint32_t me)
{
    struct comp_box in = box_intersect(window_surface_box(w), t->b);
    if (!w->surface->buffer || box_empty(in))
        return;
    bool copy = paint_opaque_over(w, in);
    int n = in.x2 - in.x1;
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint32_t *dst = tile_row(t, y) + (in.x1 - t->b.x1);
        const uint32_t *src = buffer_at(w, in.x1, y);
        if (copy)
            copy_rgb(dst, src, n);
        else
            px_over_row(dst, src, n);
    }
    if (me < FUN_MAX_THREADS)
        pt.count[me].layer_px += (uint64_t)n * (uint64_t)(in.y2 - in.y1);
}

void paint_window(const struct comp_window *w, const struct tile_buf *t, uint32_t *save)
{
    shape_save(w, t, save);
    title_draw(w, t);
    draw_buffer(w, t, ~0u);
    shape_clip(w, t, save);
}

/* Window w where it meets t: its shadow, decorations and buffer, its
 * corners cut round. */
static void draw_window(const struct comp_window *w, const struct tile_buf *t, uint32_t me)
{
    if (!(w->flags & COMP_WIN_MAPPED) || (w->flags & COMP_WIN_ANIMATED) ||
        box_empty(box_intersect(window_extent(w), t->b)))
        return;
    shadow_draw(w, t);
    if (box_empty(box_intersect(window_frame(w), t->b)))
        return;
    uint32_t *save = pt.buf[me] + TILE_W * TILE_H;
    shape_save(w, t, save);
    title_draw(w, t);
    draw_buffer(w, t, me);
    shape_clip(w, t, save);
}

/* t all one colour. */
static void fill_tile(const struct tile_buf *t, uint32_t c)
{
    uint64_t n = (uint64_t)(t->b.x2 - t->b.x1) * (uint64_t)(t->b.y2 - t->b.y1);
    for (uint64_t i = 0; i < n; i++)
        t->px[i] = c;
}

void paint_under(const struct tile_buf *t, uint32_t me)
{
    if (!desk_hides(t->b)) {
        const struct comp_window *from = cull(t->b);
        if (!from)
            wallpaper_fill(t);
        for (const struct comp_window *w = from ? from : scene.bottom; w; w = w->above)
            draw_window(w, t, me);
    }
    desk_draw_low(t);
}

static void compose(const struct tile_buf *t, uint32_t me)
{
    if (comp.blanked) {
        fill_tile(t, LOOK_BLANK);
        return;
    }
    paint_under(t, me);
    desk_draw_high(t);
    cursor_draw(t);
}

static void paint_tile(uint32_t item, uint32_t me, void *arg)
{
    (void)arg;
    const struct tile *t = &pt.t[item];
    struct comp_box b = t->b;
    int n = b.x2 - b.x1;
    if (t->direct) {   /* the full-screen window's own rows */
        for (int32_t y = b.y1; y < b.y2; y++)
            output_put(b.x1, y, buffer_at(pt.full, b.x1, y), n);
        pt.count[me].layer_px += (uint64_t)n * (uint64_t)(b.y2 - b.y1);
        return;
    }
    struct tile_buf tb = { b, pt.buf[me] };
    compose(&tb, me);
    for (int32_t y = b.y1; y < b.y2; y++)
        output_put(b.x1, y, tile_row(&tb, y), n);
}

/* ---- planning a paint ------------------------------------------------------------------ */

/* One more tile; false if there is no memory (the tile is lost: its pixels
 * stay as they were until they are damaged again). */
static bool add_tile(struct comp_box b, bool direct)
{
    if (pt.n == pt.cap) {
        uint32_t cap = pt.cap ? pt.cap * 2 : 256;
        struct tile *t = malloc(cap * sizeof(*t));
        if (!t)
            return false;
        memcpy(t, pt.t, pt.n * sizeof(*t));
        free(pt.t);
        pt.t = t;
        pt.cap = cap;
    }
    pt.t[pt.n++] = (struct tile){ b, direct };
    return true;
}

/* Box b cut into tiles. */
static void add_tiles(struct comp_box b, bool direct)
{
    for (int32_t y = b.y1; y < b.y2; y += TILE_H)
        for (int32_t x = b.x1; x < b.x2; x += TILE_W) {
            struct comp_box t = { x, y, x + TILE_W < b.x2 ? x + TILE_W : b.x2,
                                  y + TILE_H < b.y2 ? y + TILE_H : b.y2 };
            if (!add_tile(t, direct))
                return;
        }
}

/* Box b's tiles; on the full-screen path, the part under the cursor gets
 * composed tiles and the rest direct ones. */
static void plan_box(struct comp_box b)
{
    if (!pt.full) {
        add_tiles(b, false);
        return;
    }
    struct comp_box c = box_intersect(b, cursor_box());
    if (box_empty(c)) {
        add_tiles(b, true);
        return;
    }
    add_tiles(c, false);
    struct comp_box rest[4] = {
        { b.x1, b.y1, b.x2, c.y1 }, { b.x1, c.y2, b.x2, b.y2 },   /* above, below */
        { b.x1, c.y1, c.x1, c.y2 }, { c.x2, c.y1, b.x2, c.y2 },   /* left, right */
    };
    for (int i = 0; i < 4; i++)
        if (!box_empty(rest[i]))
            add_tiles(rest[i], true);
}

/* The damage as disjoint boxes, each cut into tiles. */
static void plan(void)
{
    struct comp_region dis;
    region_init(&dis, NULL);
    bool ok = true;
    struct comp_box all = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < scene.damage.n; i++) {
        all = box_bounds(all, scene.damage.b[i]);
        ok = ok && region_add(&dis, scene.damage.b[i]) == OK;
    }
    pt.n = 0;
    pt.full = full_screen();
    if (ok) {
        for (uint32_t i = 0; i < dis.n; i++)
            plan_box(dis.b[i]);
    } else {
        plan_box(all);
    }
    region_fini(&dis);
}

/* The window whose circles the pointer is over (title_hovered), its
 * circles damaged when that changes: they show their symbols, or stop. */
static void hover_update(void)
{
    static struct comp_box last;   /* the circles hovered at the last paint */
    const struct comp_window *w = NULL;
    struct comp_box now_on = { 0, 0, 0, 0 };
    bool on_surface;
    struct comp_window *u = comp.blanked || box_empty(cursor_box())
                                ? NULL
                                : wm_window_at(cursor.x, cursor.y, &on_surface);
    if (u && !on_surface && title_button_at(u, cursor.x, cursor.y) != TITLE_NONE) {
        w = u;
        now_on = title_buttons_hit(u);
    }
    if (now_on.x1 != last.x1 || now_on.y1 != last.y1 || now_on.x2 != last.x2 ||
        now_on.y2 != last.y2) {
        scene_damage(last);
        scene_damage(now_on);
        last = now_on;
    }
    title_hovered = w;
}

uint64_t paint_frame(void)
{
    uint64_t t0 = now();
    memset(&paint_last, 0, sizeof(paint_last));
    hover_update();
    desk_paint_prepare();
    if (damage_empty(&scene.damage)) {
        damage_clear(&scene.under);
        return 0;
    }
    plan();
    desk_paint_backdrops();
    for (uint32_t i = 0; i < pt.threads; i++)
        pt.count[i].layer_px = 0;
    if (pt.n)
        pool_run(paint_tile, NULL, pt.n);
    pool_rest();   /* the workers sleep until the next paint instead of spinning */
    for (uint32_t i = 0; i < pt.n; i++) {
        struct comp_box b = pt.t[i].b;
        paint_last.px += (uint64_t)(b.x2 - b.x1) * (uint64_t)(b.y2 - b.y1);
        paint_last.direct += pt.t[i].direct;
    }
    for (uint32_t i = 0; i < pt.threads; i++)
        paint_last.layer_px += pt.count[i].layer_px;
    paint_last.tiles = pt.n;
    damage_clear(&scene.damage);
    damage_clear(&scene.under);
    paint_last.ns = now() - t0;
    comp.stats.painted_px += paint_last.px;
    return paint_last.px;
}

status_t paint_init(uint32_t threads)
{
    if (!threads)
        threads = THREADS_DEFAULT;
    uint32_t cpus = fun_cpu_count();
    if (title_init() != OK)   /* before the workers: they only read the fonts */
        printf("compositor: no memory for the titles' fonts: the 8x16 text\n");
    pt.threads = pool_start(threads < cpus ? threads : cpus);
    for (uint32_t i = 0; i < pt.threads; i++) {
        pt.buf[i] = big_alloc(((uint64_t)TILE_W * TILE_H + SHAPE_SAVE_PX) * 4);
        if (!pt.buf[i])
            return ERR_NO_MEMORY;
    }
    (void)text_width(1, "");   /* libfun's glyph tables, made now: the workers only read them */
    mask_init();
    shape_init();
    ui_init();
    cursor_init();
    if (wallpaper_init(output.width, output.height) != OK)
        printf("compositor: no memory for the wallpaper: a flat background\n");
    if (desk_on() && frost_init(output.width, output.height) != OK)
        printf("compositor: no memory for the strip's frosting: a flat strip\n");
    return OK;
}
