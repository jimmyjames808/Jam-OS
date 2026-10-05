/* The animations (desk.h), the owner's picks (docs/G1-PLAN.md "The look";
 * timings in look.h, every one eased out as the prototype's
 * cubic-bezier(.2, .8, .2, 1)):
 *   - a window opens growing from 92% of its size while it fades in
 *     (150 ms); closing is the reverse;
 *   - minimising shrinks it into its chip on the strip while it fades to
 *     15% (260 ms); restoring grows it back out of the chip;
 *   - switching screens slides the windows of both sideways by the
 *     output's width (260 ms), the wallpaper and the strip staying put.
 *
 * One animation runs at a time: starting one, or anything the user does
 * that changes what is shown (desk.c, screens.c), first makes the running
 * one jump to its end. So a quick second key or click never waits for a
 * picture to finish.
 *
 * Opening, closing, minimising and restoring draw a snapshot of the window
 * (animdraw.c: its frame as it was, rounded corners clear) scaled about a
 * moving centre; the window itself is not drawn meanwhile (COMP_WIN_ANIMATED
 * while it is mapped, or it is unmapped or gone already). A slide moves the
 * windows themselves (struct comp_window's slide_x), so the clients' own
 * pixels keep coming.
 *
 * Driven by the loop's clock (desk_tick): each tick computes the frame for
 * its time, and damages only what changed: the picture's old and new boxes,
 * each sliding window's old and new extent. Nothing is allocated per frame;
 * a snapshot is made when an animation starts and freed when it ends. With
 * animations off (anim_init(false): the window manager alone, tests) every
 * change is at its end at once. */
#include "desk.h"

static struct {
    bool on;
    enum anim_kind kind;
    uint64_t start, ms;
    struct comp_window *w;         /* open, restore: the window the picture stands for */
    struct anim_snap snap;
    struct comp_box frame;         /* the picture's box at full size, unmoved */
    int32_t cx0, cy0, cx1, cy1;    /* its centre at the start and the end (2x: half pixels) */
    int32_t s0, s1;                /* its scale at the start and the end, thousandths */
    uint32_t a0, a1;               /* its alpha */
    const struct desk_screen *from, *to;   /* a slide's screens */
    int dir;
    struct anim_draw now;          /* the frame the next paint draws */
} an;

void anim_init(bool on)
{
    anim_finish();
    an.on = on;
}

bool anim_enabled(void)
{
    return an.on;
}

enum anim_kind anim_running(void)
{
    return an.kind;
}

bool anim_slides(const struct desk_screen *s)
{
    return an.kind == ANIM_SLIDE && s && (s == an.from || s == an.to);
}

/* ---- easing ------------------------------------------------------------------------------ */

/* One coordinate of the bezier (0, p1, p2, 1) at u. */
static float bez(float u, float p1, float p2)
{
    float v = 1.0f - u;
    return 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u;
}

/* cubic-bezier(.2, .8, .2, 1) at progress p (0..1): x(u) = p found by
 * halving (x rises with u), y(u) the eased value. */
static float ease(float p)
{
    if (p <= 0.0f)
        return 0.0f;
    if (p >= 1.0f)
        return 1.0f;
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 24; i++) {
        float mid = (lo + hi) / 2;
        if (bez(mid, 0.2f, 0.2f) < p)
            lo = mid;
        else
            hi = mid;
    }
    return bez((lo + hi) / 2, 0.8f, 1.0f);
}

static int32_t lerp(int32_t a, int32_t b, float e)
{
    float v = (float)a + ((float)b - (float)a) * e;
    return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* ---- the frames ------------------------------------------------------------------------- */

/* The picture's box at eased progress e. */
static struct comp_box picture_at(float e)
{
    int32_t s = lerp(an.s0, an.s1, e), cx = lerp(an.cx0, an.cx1, e), cy = lerp(an.cy0, an.cy1, e);
    int32_t w = (int32_t)((int64_t)an.snap.w * s / 1000);
    int32_t h = (int32_t)((int64_t)an.snap.h * s / 1000);
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    return (struct comp_box){ (cx - w) / 2, (cy - h) / 2, (cx - w) / 2 + w, (cy - h) / 2 + h };
}

/* Every window of a slide's two screens at offset by progress e. */
static void slide_to(float e)
{
    int32_t out = lerp(0, -an.dir * scene.width, e), in = lerp(an.dir * scene.width, 0, e);
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next) {
        struct comp_window *w = ww->win;
        if (!w || (ww->screen != an.from && ww->screen != an.to))
            continue;
        int32_t x = ww->screen == an.to ? in : out;
        if (w->slide_x == x)
            continue;
        window_damage(w);
        w->slide_x = x;
        window_damage(w);
    }
}

static void frame_at(uint64_t t)
{
    float p = an.ms ? (float)(t - an.start) / (float)(an.ms * NS_PER_MS) : 1.0f;
    float e = ease(p);
    if (an.kind == ANIM_SLIDE) {
        slide_to(e);
        return;
    }
    struct comp_box b = picture_at(e);
    scene_damage_over(an.now.at);
    scene_damage_over(b);
    an.now.at = b;
    an.now.alpha = (uint32_t)lerp((int32_t)an.a0, (int32_t)an.a1, e);
}

static void end(void)
{
    enum anim_kind kind = an.kind;
    if (kind == ANIM_NONE)
        return;
    an.kind = ANIM_NONE;
    scene_damage_over(an.now.at);
    an.now = (struct anim_draw){ .kind = ANIM_NONE };
    if (an.w) {
        an.w->flags &= ~COMP_WIN_ANIMATED;
        window_damage(an.w);   /* drawn as itself again */
        an.w = NULL;
    }
    anim_snapshot_free(&an.snap);
    if (kind == ANIM_SLIDE) {
        an.dir = 0;   /* every offset back to 0 */
        for (struct wm_window *ww = wm_first(); ww; ww = ww->next)
            if (ww->win && ww->win->slide_x) {
                window_damage(ww->win);
                ww->win->slide_x = 0;
                window_damage(ww->win);
            }
        an.from = an.to = NULL;
        screens_slide_done();
    }
}

void anim_finish(void)
{
    end();
}

void anim_forget(const struct comp_window *w)
{
    if (an.w != w || !w)
        return;
    an.w = NULL;   /* nothing to give back to it: it goes */
    end();
}

void anim_tick(uint64_t t)
{
    if (an.kind == ANIM_NONE)
        return;
    if (t >= an.start + an.ms * NS_PER_MS) {
        end();
        return;
    }
    frame_at(t);
}

uint64_t anim_deadline(void)
{
    if (an.kind == ANIM_NONE)
        return DEADLINE_NEVER;
    return scene.last_paint_ns + comp.period_ns;   /* the next paint's frame */
}

bool anim_now(struct anim_draw *out)
{
    *out = an.now;
    return an.now.kind != ANIM_NONE && an.now.snap && an.now.snap->px;
}

/* ---- starting --------------------------------------------------------------------------- */

/* A picture animation of w: its snapshot from the frame, centres and
 * scales from and to. False: none (animations off, no memory). */
static bool begin(enum anim_kind kind, struct comp_window *w, uint64_t ms)
{
    anim_finish();
    if (!an.on || !w)
        return false;
    struct comp_box f = window_frame(w);
    if (box_empty(f) || anim_snapshot(w, &an.snap) != OK)
        return false;
    an.kind = kind;
    an.start = now();
    an.ms = ms;
    an.frame = f;
    an.cx0 = an.cx1 = f.x1 + f.x2;
    an.cy0 = an.cy1 = f.y1 + f.y2;
    an.now = (struct anim_draw){ .kind = kind, .snap = &an.snap, .at = f, .alpha = 255,
                                 .above_strip = kind == ANIM_MINIMISE || kind == ANIM_RESTORE };
    return true;
}

void anim_open(struct comp_window *w)
{
    if (!begin(ANIM_OPEN, w, LOOK_ANIM_OPEN_MS))
        return;
    an.s0 = LOOK_ANIM_OPEN_FROM;
    an.s1 = 1000;
    an.a0 = 0;
    an.a1 = 255;
    an.w = w;
    w->flags |= COMP_WIN_ANIMATED;
    window_damage(w);
    frame_at(an.start);
}

void anim_close(struct comp_window *w)
{
    if (!begin(ANIM_CLOSE, w, LOOK_ANIM_OPEN_MS))
        return;
    an.s0 = 1000;
    an.s1 = LOOK_ANIM_OPEN_FROM;
    an.a0 = 255;
    an.a1 = 0;
    frame_at(an.start);
}

/* Minimise and restore: between the window's frame and the chip's centre. */
static void chip_ends(struct comp_box chip, bool into)
{
    int32_t cx = an.cx0, cy = an.cy0;
    if (!box_empty(chip)) {
        cx = chip.x1 + chip.x2;
        cy = chip.y1 + chip.y2;
    }
    an.cx0 = into ? an.cx0 : cx;
    an.cy0 = into ? an.cy0 : cy;
    an.cx1 = into ? cx : an.cx1;
    an.cy1 = into ? cy : an.cy1;
    an.s0 = into ? 1000 : LOOK_ANIM_MIN_TO;
    an.s1 = into ? LOOK_ANIM_MIN_TO : 1000;
    an.a0 = into ? 255 : LOOK_ANIM_MIN_ALPHA;
    an.a1 = into ? LOOK_ANIM_MIN_ALPHA : 255;
}

void anim_minimise(struct comp_window *w, struct comp_box chip)
{
    if (!begin(ANIM_MINIMISE, w, LOOK_ANIM_MIN_MS))
        return;
    chip_ends(chip, true);
    frame_at(an.start);
}

void anim_restore(struct comp_window *w, struct comp_box chip)
{
    if (!begin(ANIM_RESTORE, w, LOOK_ANIM_MIN_MS))
        return;
    chip_ends(chip, false);
    an.w = w;
    w->flags |= COMP_WIN_ANIMATED;
    window_damage(w);
    frame_at(an.start);
}

void anim_slide(const struct desk_screen *from, const struct desk_screen *to, int dir)
{
    anim_finish();
    if (!an.on || from == to)
        return;
    an.kind = ANIM_SLIDE;
    an.start = now();
    an.ms = LOOK_ANIM_SLIDE_MS;
    an.from = from;
    an.to = to;
    an.dir = dir < 0 ? -1 : 1;
    frame_at(an.start);
}
