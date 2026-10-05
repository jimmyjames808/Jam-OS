/* Tiling's layout (wm.h): master and stack.
 *
 * Each screen is tiled on its own (screens.c), in its room: the output
 * less the strip while the desktop is on. One window takes all of it.
 * With more, the oldest (the master)
 * takes the left half and the others share the right half, stacked top to
 * bottom in the order they opened; a new window joins at the bottom of
 * the stack. Every tile is a frame box: the window's decorations go
 * inside it, and WM_GAP pixels of background separate tiles and edges.
 *
 * Why master and stack and not equal columns: Jam OS's windows are mostly
 * one main program (the terminal, jamjar) with smaller ones beside it,
 * and many of them can't resize (libfun's games and demos keep their own
 * size). Columns make every window narrower with each one added (four on
 * 1280 pixels are about 300 each, too narrow for the terminal's 80 columns),
 * where master and stack keeps the first window half the screen however
 * many join, and gives the stack's fixed-size windows tiles they are
 * centred in. dwm and xmonad's default layouts are the same idea. */
#include "desk.h"

#define WM_GAP 6   /* background between tiles, and around them */

/* [lo, hi) cut into n equal parts with a gap between them: part i. */
static void split(int32_t lo, int32_t hi, unsigned n, unsigned i, int32_t *a, int32_t *b)
{
    int64_t room = (int64_t)hi - lo - (int64_t)(n - 1) * WM_GAP;
    if (room < (int64_t)n)
        room = n;   /* a silly-small output: tiles of a pixel, overlapping gaps */
    *a = lo + (int32_t)(room * i / n) + (int32_t)i * WM_GAP;
    *b = lo + (int32_t)(room * (i + 1) / n) + (int32_t)i * WM_GAP;
}

struct comp_box wm_tile_box(struct comp_box area, unsigned n, unsigned i)
{
    struct comp_box in = { area.x1 + WM_GAP, area.y1 + WM_GAP, area.x2 - WM_GAP,
                           area.y2 - WM_GAP };
    if (n <= 1 || i >= n)
        return in;
    int32_t x1, x2, y1, y2;
    if (i == 0) {
        split(in.x1, in.x2, 2, 0, &x1, &x2);
        return (struct comp_box){ x1, in.y1, x2, in.y2 };
    }
    split(in.x1, in.x2, 2, 1, &x1, &x2);
    split(in.y1, in.y2, n - 1, i - 1, &y1, &y2);
    return (struct comp_box){ x1, y1, x2, y2 };
}

struct comp_box wm_tile(const struct wm_window *ww)
{
    const struct desk_screen *s = ww->screen ? ww->screen : screens_cur();
    unsigned n = 0, i = 0;
    for (const struct wm_window *t = wm_first(); t; t = t->next) {
        if (t != ww && (!t->win || t->minimised || (t->screen ? t->screen : screens_cur()) != s))
            continue;   /* not tiled here: unmapped, minimised, or on another screen */
        if (t == ww)
            i = n;
        n++;
    }
    return wm_tile_box(screens_room(s), n, i);
}
