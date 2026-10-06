/* Virtual screens (desk.h), the owner's rules (docs/G1-PLAN.md "The
 * look"):
 *   - the desktop starts with one screen; a new one is made past the last
 *     (Super+Ctrl+Right or L, the strip's "+") or by moving a window there
 *     (Super+Ctrl+Shift+Right or L, Super+Shift+1..9 past the last); a
 *     screen other than the current one that has no windows left goes away
 *     (minimised windows count: they live there);
 *   - Super+1..9 and the strip's dots go to a screen that exists;
 *   - each normal screen has its own arrangement, floating or tiling
 *     (Super+T switches the current one; new screens get the last choice);
 *   - a window made full screen moves to a screen of its own just right of
 *     the one it came from (its home), and back to it when it leaves full
 *     screen (a new screen in its place if the home went); its own screen
 *     then goes;
 *   - a new window opens on the current screen; on a full-screen screen,
 *     on the normal screen left of it, which becomes current;
 *   - minimised windows stay on their screen, hidden, listed by its chips;
 *   - a full-screen window whose client takes no keys (the boot splash's:
 *     libjwl's no_keyboard) is a boot overlay instead, not an app to
 *     switch between: on no screen, over whichever is current, the strip
 *     included (strip.c shows none under it) and every window (the scene
 *     keeps it on top: COMP_WIN_OVERLAY); no screen, dot, chip or Alt+Tab
 *     row, no slide; never the keys (focus.c), so typing reaches the
 *     window under it; when it goes it fades out (anim_fade, wm.c).
 * Going from one screen to another slides (anim.c), the windows of both
 * shown while it does; when it ends, the old screen's windows are hidden
 * and screens with nothing left go.
 *
 * Shown or not is the scene's COMP_WIN_MAPPED: a window of a screen that
 * isn't current (nor sliding) is unmapped, so it is neither painted nor
 * under the pointer, and the seat's focus never stays on it. The focus
 * after a switch is the screen's most recently focused window.
 *
 * The screens are slots of a small pool, in order in `order`; a window
 * points at its screen's slot, which never moves while the screen lives. */
#include "desk.h"

static struct {
    struct desk_screen pool[DESK_SCREENS_MAX];
    struct desk_screen *order[DESK_SCREENS_MAX];   /* left to right */
    unsigned n, cur;
    enum comp_layout deflt;        /* new screens' arrangement */
} sc;

static void cleanup(void);

/* ---- the list --------------------------------------------------------------------------- */

static struct desk_screen *new_screen(enum screen_kind kind, unsigned at)
{
    if (sc.n == DESK_SCREENS_MAX || at > sc.n)
        return NULL;
    struct desk_screen *s = NULL;
    for (unsigned i = 0; i < DESK_SCREENS_MAX && !s; i++)
        if (!sc.pool[i].used)
            s = &sc.pool[i];
    *s = (struct desk_screen){ .used = true, .kind = kind, .layout = sc.deflt };
    memmove(&sc.order[at + 1], &sc.order[at], (sc.n - at) * sizeof(sc.order[0]));
    sc.order[at] = s;
    sc.n++;
    if (at <= sc.cur && sc.n > 1)
        sc.cur++;   /* the current screen moved right */
    strip_dirty();
    return s;
}

static void drop_screen(unsigned i)
{
    tiles_drop(sc.order[i]);
    sc.order[i]->used = false;
    memmove(&sc.order[i], &sc.order[i + 1], (sc.n - i - 1) * sizeof(sc.order[0]));
    sc.n--;
    if (i < sc.cur)
        sc.cur--;
    strip_dirty();
}

void screens_init(enum comp_layout layout)
{
    for (unsigned i = 0; i < DESK_SCREENS_MAX; i++)
        if (sc.pool[i].used)
            tiles_drop(&sc.pool[i]);   /* a fresh start's (each test's) */
    memset(&sc, 0, sizeof(sc));
    sc.deflt = layout;
    sc.pool[0] = (struct desk_screen){ .used = true, .kind = SCREEN_NORMAL, .layout = layout };
    sc.order[0] = &sc.pool[0];
    sc.n = 1;
    scene.layout = layout;
}

void screens_set_default(enum comp_layout layout)
{
    sc.deflt = layout;
    if (sc.order[sc.cur]->kind == SCREEN_NORMAL)
        sc.order[sc.cur]->layout = layout;
    scene.layout = layout;
}

unsigned screens_count(void)
{
    return sc.n;
}

unsigned screens_cur_index(void)
{
    return sc.cur;
}

struct desk_screen *screens_cur(void)
{
    return sc.order[sc.cur];
}

struct desk_screen *screens_nth(unsigned i)
{
    return i < sc.n ? sc.order[i] : NULL;
}

int screens_index(const struct desk_screen *s)
{
    for (unsigned i = 0; i < sc.n; i++)
        if (sc.order[i] == s)
            return (int)i;
    return -1;
}

unsigned screens_windows(const struct desk_screen *s)
{
    unsigned n = 0;
    for (const struct wm_window *ww = wm_first(); ww; ww = ww->next)
        n += ww->win && ww->screen == s;
    return n;
}

struct comp_box screens_room(const struct desk_screen *s)
{
    struct comp_box b = { 0, 0, scene.width, scene.height };
    if (desk_on() && (!s || s->kind == SCREEN_NORMAL))
        b.y1 = LOOK_STRIP_H < scene.height ? LOOK_STRIP_H : 0;
    return b;
}

/* ---- shown or not ------------------------------------------------------------------------ */

bool screens_shown(const struct wm_window *ww)
{
    if (ww->overlay)
        return ww->win != NULL;
    return ww->win && !ww->minimised && ww->screen &&
           (ww->screen == sc.order[sc.cur] || anim_slides(ww->screen));
}

bool screens_overlay(void)
{
    for (const struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (ww->overlay && ww->win && (ww->win->flags & COMP_WIN_MAPPED))
            return true;
    return false;
}

void screens_sync(void)
{
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next) {
        if (!ww->win)
            continue;
        bool want = screens_shown(ww);
        if (want != !!(ww->win->flags & COMP_WIN_MAPPED))
            window_map(ww->win, want);
    }
}

struct wm_window *screens_mru(const struct desk_screen *s)
{
    struct wm_window *best = NULL;
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next)
        if (ww->win && ww->screen == s && !ww->minimised &&
            (!best || ww->focused_at >= best->focused_at))
            best = ww;
    return best;
}

/* The focus to s's most recently focused window, or to none. */
static void refocus(const struct desk_screen *s)
{
    struct wm_window *ww = screens_mru(s);
    seat_focus(ww && ww->win && (ww->win->flags & COMP_WIN_MAPPED) ? ww->win : NULL);
}

/* ---- going from screen to screen --------------------------------------------------------- */

/* To screen i, sliding if animate (and animations are on). */
static void go_to(unsigned i, bool animate)
{
    anim_finish();   /* a slide still running ends first: its screens settle */
    if (i >= sc.n || i == sc.cur)
        return;
    struct desk_screen *from = sc.order[sc.cur], *to = sc.order[i];
    int dir = i > sc.cur ? 1 : -1;
    pop_close();
    sc.cur = i;
    if (to->kind == SCREEN_NORMAL)
        scene.layout = to->layout;
    bool slide = animate && anim_enabled();
    if (slide)
        anim_slide(from, to, dir);
    screens_sync();
    refocus(to);
    strip_dirty();
    if (!slide)
        screens_slide_done();
}

void screens_slide_done(void)
{
    screens_sync();   /* the old screen's windows hidden */
    cleanup();
    strip_dirty();
}

/* Screens with no windows go, but the current one; while a full-screen
 * screen is current and no normal screen has windows, one empty normal
 * screen stays (the desktop to come back to). */
static void cleanup(void)
{
    bool spare = sc.order[sc.cur]->kind == SCREEN_FULL;
    for (unsigned i = 0; i < sc.n && spare; i++)
        if (sc.order[i]->kind == SCREEN_NORMAL && screens_windows(sc.order[i]))
            spare = false;
    for (unsigned i = 0; i < sc.n;) {
        struct desk_screen *s = sc.order[i];
        if (i == sc.cur || anim_slides(s) || screens_windows(s)) {
            i++;
        } else if (spare && s->kind == SCREEN_NORMAL) {
            spare = false;   /* this one stays */
            i++;
        } else {
            drop_screen(i);
        }
    }
}

void screens_go(unsigned i)
{
    go_to(i, true);
}

/* A normal screen at the end, or NULL if there is no room for one. */
static struct desk_screen *append(void)
{
    return new_screen(SCREEN_NORMAL, sc.n);
}

void screens_step(int dir)
{
    anim_finish();
    if (dir < 0 && sc.cur == 0)
        return;
    unsigned i = dir < 0 ? sc.cur - 1 : sc.cur + 1;
    if (i >= sc.n && !append())
        return;
    go_to(i, true);
}

void screens_add(void)
{
    anim_finish();
    if (append())
        go_to(sc.n - 1, true);
}

/* Can ww move to another screen: a mapped window on a normal one. */
static bool movable(const struct wm_window *ww)
{
    return ww && ww->win && ww->screen && ww->screen->kind == SCREEN_NORMAL;
}

/* ww onto screen i (past the last: a new one at the end), which becomes current. */
static void move_window(struct wm_window *ww, int i)
{
    if (i >= (int)sc.n && !append())
        return;
    if (i >= (int)sc.n)
        i = (int)sc.n - 1;
    ww->screen = sc.order[i];
    ww->minimised = false;
    wm_relayout();   /* both screens' tiles */
    go_to((unsigned)i, true);
    screens_sync();
    seat_focus(ww->win);
}

void screens_move(struct wm_window *ww, int dir)
{
    anim_finish();
    if (!movable(ww))
        return;
    int i = screens_index(ww->screen) + dir;
    while (i >= 0 && i < (int)sc.n && sc.order[i]->kind == SCREEN_FULL)
        i += dir;
    if (i >= 0)
        move_window(ww, i);
}

void screens_move_to(struct wm_window *ww, unsigned i)
{
    anim_finish();
    if (!movable(ww) || (i < sc.n && sc.order[i]->kind == SCREEN_FULL) ||
        (i < sc.n && sc.order[i] == ww->screen))
        return;
    move_window(ww, i < sc.n ? (int)i : (int)sc.n);
}

/* ---- minimising ------------------------------------------------------------------------- */

void screens_minimise(struct wm_window *ww)
{
    if (!ww || !ww->win || ww->minimised || ww->overlay)
        return;
    anim_finish();
    if (ww->screen && ww->screen->kind == SCREEN_FULL)
        screens_fullscreen(ww, false);   /* back to its home screen first */
    struct comp_box chip = strip_chip_box(ww);
    anim_minimise(ww->win, chip);        /* its picture, while it is still mapped */
    ww->minimised = true;
    screens_sync();
    if (wm_layout_of(ww) == COMP_TILING)
        wm_relayout();   /* the others take its tile */
    refocus(sc.order[sc.cur]);
    strip_dirty();
}

void screens_restore(struct wm_window *ww)
{
    if (!ww || !ww->win || !ww->minimised)
        return;
    anim_finish();
    int i = screens_index(ww->screen);
    if (i >= 0 && (unsigned)i != sc.cur)
        go_to((unsigned)i, false);   /* the restore is the animation to see */
    ww->minimised = false;
    if (wm_layout_of(ww) == COMP_TILING)
        wm_relayout();
    screens_sync();
    seat_focus(ww->win);
    strip_dirty();
    anim_restore(ww->win, strip_chip_box(ww));
}

void screens_activate(struct wm_window *ww)
{
    if (!ww || !ww->win)
        return;
    if (ww->minimised) {
        screens_restore(ww);
        return;
    }
    int i = screens_index(ww->screen);
    if (i >= 0 && (unsigned)i != sc.cur)
        go_to((unsigned)i, true);
    seat_focus(ww->win);
}

/* ---- what the window manager tells ------------------------------------------------------ */

/* The normal screen nearest left of i (else right of it), or a new one at i. */
static unsigned normal_near(unsigned i)
{
    for (unsigned k = i + 1; k-- > 0;)
        if (sc.order[k]->kind == SCREEN_NORMAL)
            return k;
    for (unsigned k = i + 1; k < sc.n; k++)
        if (sc.order[k]->kind == SCREEN_NORMAL)
            return k;
    return new_screen(SCREEN_NORMAL, i) ? i : 0;
}

/* Is ww to be a boot overlay: asked to be full screen by a client that
 * takes no keys. */
static bool overlay_wanted(const struct wm_window *ww)
{
    return ww->want == WM_FULLSCREEN && !seat_takes_keys(ww->surface->client);
}

/* ww a boot overlay, or no more (onto the current screen, a normal one). */
static void set_overlay(struct wm_window *ww, bool on)
{
    ww->overlay = on;
    ww->home = NULL;
    ww->minimised = false;
    if (on) {
        ww->screen = NULL;
        ww->win->flags |= COMP_WIN_OVERLAY;
        window_raise(ww->win);
    } else {
        ww->win->flags &= ~COMP_WIN_OVERLAY;
        if (sc.order[sc.cur]->kind == SCREEN_FULL)
            go_to(normal_near(sc.cur), true);
        ww->screen = sc.order[sc.cur];
    }
    strip_dirty();
}

void screens_window_new(struct wm_window *ww)
{
    if (overlay_wanted(ww)) {
        set_overlay(ww, true);   /* no screen to go to */
        return;
    }
    unsigned i = sc.cur;
    if (sc.order[i]->kind == SCREEN_FULL) {
        i = normal_near(sc.cur);
        go_to(i, true);
    }
    ww->screen = sc.order[sc.cur];
    ww->home = NULL;
    ww->minimised = false;
}

void screens_window_gone(struct wm_window *ww)
{
    struct desk_screen *s = ww->screen, *home = ww->home;
    ww->screen = ww->home = NULL;
    ww->minimised = false;
    if (ww->overlay)
        strip_dirty();   /* the strip again, under its fade */
    ww->overlay = false;
    if (!s)
        return;
    int i = screens_index(s);
    if (s->kind == SCREEN_FULL && i == (int)sc.cur) {
        int h = screens_index(home);
        go_to(h >= 0 ? (unsigned)h : normal_near(sc.cur), true);   /* its screen goes after */
        return;
    }
    cleanup();
    strip_dirty();
}

/* ww onto a full-screen screen of its own, right of the one it is on. */
static void enter_full(struct wm_window *ww)
{
    struct desk_screen *home = ww->screen;
    struct desk_screen *s = new_screen(SCREEN_FULL, (unsigned)screens_index(home) + 1);
    if (!s)
        return;   /* no room for a screen: full screen where it is */
    ww->home = home;
    ww->screen = s;
    if (home->layout == COMP_TILING)
        wm_relayout();   /* its tile goes to the others */
    go_to((unsigned)screens_index(s), true);
    screens_sync();
    seat_focus(ww->win);
}

/* ww back to its home screen (a new one where its own is, if that went). */
static void leave_full(struct wm_window *ww)
{
    struct desk_screen *s = ww->screen, *back = ww->home;
    int i = screens_index(s);
    if (screens_index(back) < 0)
        back = new_screen(SCREEN_NORMAL, (unsigned)i);
    if (!back)
        back = sc.order[normal_near((unsigned)screens_index(s))];
    ww->screen = back;
    ww->home = NULL;
    wm_relayout();
    if (screens_index(s) == (int)sc.cur)
        go_to((unsigned)screens_index(back), true);   /* its own screen goes after the slide */
    else
        cleanup();
    screens_sync();
    seat_focus(ww->win);
    strip_dirty();
}

void screens_fullscreen(struct wm_window *ww, bool on)
{
    if (ww->win && ww->overlay != (on && overlay_wanted(ww))) {
        anim_finish();
        set_overlay(ww, !ww->overlay);
        wm_relayout();   /* its tile goes to the others, or comes back */
        cleanup();
        screens_sync();
        return;
    }
    if (!ww->win || !ww->screen || ww->minimised)
        return;   /* placed when it is mapped (screens_window_new, then wm.c asks again) */
    anim_finish();
    if (on && ww->screen->kind == SCREEN_NORMAL)
        enter_full(ww);
    else if (!on && ww->screen->kind == SCREEN_FULL)
        leave_full(ww);
}
