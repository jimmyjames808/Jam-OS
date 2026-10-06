/* The window keys (comp.h's wm_key), the owner's whole set (docs/G1-PLAN.md
 * "The look": "Keys"). Super is the logo key, apart from Ctrl, so apps
 * keep Ctrl+C, Ctrl+L and the rest. Every "direction" is both H/J/K/L and
 * the arrow keys (left, down, up, right):
 *
 *   Super+direction             the focus to the nearest window that way
 *                               (by where the windows are: floating too)
 *   Super+Shift+direction       tiling: swap with the nearest tile that way
 *   Super+Alt+direction         tiling: push the focused tile's edge that
 *                               way (no edge there: its other edge);
 *                               floating: its right or bottom edge moves
 *                               that way (grows or shrinks it)
 *   Super+Q, Super+M, Super+F   close, minimise, full screen (and back)
 *   Super+T                     this screen floating or tiling
 *   Super+Enter                 a new terminal (init's)
 *   Super+1..9                  to screen N (one that exists)
 *   Super+Shift+1..9            the focused window to screen N (past the
 *                               last: a new one)
 *   Super+Ctrl+Left/Right, H/L  the screen before or after (past the last:
 *                               a new one)
 *   Super+Ctrl+Shift+the same   the focused window to the screen before or
 *                               after
 *
 * Super+Left and Super+Right move the focus; they do not switch
 * screens. The seat (focus.c) asks after Ctrl+Alt+Del and the desktop's
 * own keys (Super tapped alone, Alt+Tab, the menus: desk.c), before any
 * client sees the key; a key taken here never reaches a client, nor its
 * release. Every key press has already ended a running animation
 * (desk_key), so a glide these start begins from where windows rest. */
#include "desk.h"
#include "seat.h"

/* HID usages (USB HID Usage Tables 1.4, section 10). */
#define U_F        0x09
#define U_H        0x0b
#define U_J        0x0d
#define U_K        0x0e
#define U_L        0x0f
#define U_M        0x10
#define U_Q        0x14
#define U_T        0x17
#define U_1        0x1e
#define U_9        0x26
#define U_ENTER    0x28
#define U_RIGHT    0x4f
#define U_LEFT     0x50
#define U_DOWN     0x51
#define U_UP       0x52
#define U_KP_ENTER 0x58
#define MOD_SUPER  (INPUT_MOD_LGUI | INPUT_MOD_RGUI)
#define NEAR       5   /* pixels a window's centre must be past ours to be that way */

/* A direction key's way (dx, dy each -1, 0 or 1). False: no direction. */
static bool direction(uint16_t usage, int *dx, int *dy)
{
    *dx = usage == U_RIGHT || usage == U_L ? 1 : usage == U_LEFT || usage == U_H ? -1 : 0;
    *dy = usage == U_DOWN || usage == U_J ? 1 : usage == U_UP || usage == U_K ? -1 : 0;
    return *dx || *dy;
}

/* ---- the nearest window that way --------------------------------------------------------- */

/* Twice the centre of ww's frame (whole numbers). */
static void centre2(const struct wm_window *ww, int64_t *x, int64_t *y)
{
    struct comp_box f = window_frame(ww->win);
    *x = (int64_t)f.x1 + f.x2;
    *y = (int64_t)f.y1 + f.y2;
}

/* Can the focus or a swap go to ww from `from`: shown on its screen (and a
 * tile of the same tree, if tiles). */
static bool candidate(const struct wm_window *ww, const struct wm_window *from, bool tiles)
{
    if (ww == from || !ww->win || !(ww->win->flags & COMP_WIN_MAPPED) || ww->minimised ||
        ww->overlay || ww->screen != from->screen)
        return false;
    return !tiles || (ww->leaf && ww->tiled_on == from->tiled_on);
}

/* The window nearest `from` in way (dx, dy): its centre at least NEAR
 * pixels that way, scored by the distance along the way plus twice the
 * distance across it (the prototype's rule), the lowest score first. */
static struct wm_window *neighbour(const struct wm_window *from, int dx, int dy, bool tiles)
{
    int64_t cx, cy, best = INT64_MAX;
    struct wm_window *pick = NULL;
    centre2(from, &cx, &cy);
    for (struct wm_window *ww = wm_first(); ww; ww = ww->next) {
        if (!candidate(ww, from, tiles))
            continue;
        int64_t x, y;
        centre2(ww, &x, &y);
        int64_t along = dx ? (x - cx) * dx : (y - cy) * dy, across = dx ? y - cy : x - cx;
        if (along <= 2 * NEAR)
            continue;   /* not that way */
        int64_t score = along + 2 * (across < 0 ? -across : across);
        if (score < best) {
            best = score;
            pick = ww;
        }
    }
    return pick;
}

/* ---- what the keys do ---------------------------------------------------------------------- */

/* The focused toplevel, if it is on the current screen and shown. */
static struct wm_window *focused_here(void)
{
    struct wm_window *f = wm_focused();
    return f && f->win && (f->win->flags & COMP_WIN_MAPPED) && !f->overlay ? f : NULL;
}

static void focus_way(int dx, int dy)
{
    struct wm_window *f = focused_here(), *to;
    if (!f) {   /* nothing focused here: the screen's last focused window */
        to = screens_mru(screens_cur());
        if (to && to->win && (to->win->flags & COMP_WIN_MAPPED))
            seat_focus(to->win);
        return;
    }
    if ((to = neighbour(f, dx, dy, false)))
        seat_focus(to->win);
}

static void swap_way(int dx, int dy)
{
    struct wm_window *f = focused_here(), *to;
    if (f && f->leaf && (to = neighbour(f, dx, dy, true)) && tiles_swap(f, to))
        wm_reflow();
}

/* A floating window's right or bottom edge moved WM_PUSH that way. */
static void grow(struct wm_window *ww, int dx, int dy)
{
    if (!wm_floating_normal(ww) || !wm_resizable(ww))
        return;
    int32_t w = ww->float_w ? ww->float_w : ww->surface->width;
    int32_t h = ww->float_h ? ww->float_h : ww->surface->height;
    int32_t nw = w + dx * WM_PUSH, nh = h + dy * WM_PUSH;
    if (ww->max_w > 0 && nw > ww->max_w)
        nw = ww->max_w;
    if (ww->max_h > 0 && nh > ww->max_h)
        nh = ww->max_h;
    nw = nw < ww->min_w ? ww->min_w : nw < WM_MIN_SIDE ? WM_MIN_SIDE : nw;
    nh = nh < ww->min_h ? ww->min_h : nh < WM_MIN_SIDE ? WM_MIN_SIDE : nh;
    if (nw > COMP_BUFFER_SIDE_MAX || nh > COMP_BUFFER_SIDE_MAX || (nw == w && nh == h))
        return;
    ww->anchor = 0;   /* its top left stays */
    ww->float_w = nw;
    ww->float_h = nh;
    wm_reconfigure(ww);
}

static void push_way(int dx, int dy)
{
    struct wm_window *f = focused_here();
    if (!f)
        return;
    if (f->leaf && tiles_push(f, dx, dy))
        wm_reflow();
    else if (!f->leaf)
        grow(f, dx, dy);
}

/* Super with a direction (and Shift, or Alt). True if taken. */
static bool direction_key(uint16_t usage, bool shift, bool alt)
{
    int dx, dy;
    if (!direction(usage, &dx, &dy))
        return false;
    if (alt)
        push_way(dx, dy);
    else if (shift)
        swap_way(dx, dy);
    else
        focus_way(dx, dy);
    return true;
}

/* Super+Ctrl with Left/Right or H/L (and Shift): the screens. True if taken. */
static bool screen_key(uint16_t usage, bool shift)
{
    int dx, dy;
    if (!direction(usage, &dx, &dy) || !dx)
        return false;
    if (shift)
        screens_move(wm_focused(), dx);
    else
        screens_step(dx);
    return true;
}

/* Super with a letter, Enter or a digit. True if taken. */
static bool window_key(uint16_t usage, bool shift)
{
    struct wm_window *f = wm_focused();
    struct comp_window *w = f ? f->win : NULL;
    if (usage >= U_1 && usage <= U_9) {
        if (shift)
            screens_move_to(wm_focused(), usage - U_1);
        else
            screens_go(usage - U_1);
    } else if (usage == U_F) {
        if (w)
            wm_toggle_fullscreen(w);
    } else if (usage == U_T) {
        wm_toggle_layout();
    } else if (usage == U_Q) {
        if (w)
            wm_close(w);
    } else if (usage == U_M) {
        if (w)
            wm_minimise(w);
    } else if (usage == U_ENTER || usage == U_KP_ENTER) {
        ctl_terminal();
    } else {
        return false;
    }
    return true;
}

bool wm_key(uint16_t usage, uint8_t mods)
{
    bool ctrl = mods & INPUT_MOD_CTRL, alt = mods & INPUT_MOD_ALT;
    bool shift = mods & INPUT_MOD_SHIFT;
    if (!(mods & MOD_SUPER))
        return false;
    if (ctrl)
        return !alt && screen_key(usage, shift);
    if (direction_key(usage, shift, alt))
        return true;
    return !alt && window_key(usage, shift);
}
