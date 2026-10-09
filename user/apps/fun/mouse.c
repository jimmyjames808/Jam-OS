/* libfun: the mouse (fun.h): the pointer's position over the window, the
 * buttons with their edges, and the wheel. The compositor owns the
 * pointer: it moves it, accelerates it and draws the arrow, and tells
 * where it is over the window (wl.c hands that to mouse_at).
 *
 * Also here, for the compositor's pointer and its test: a position made
 * from relative mouse counts (struct pointer). The position is kept in
 * 1/256 pixel, clamped to the screen. Acceleration goes by the size of
 * one report: up to ACCEL_FROM counts it is 1:1, so slow movement is
 * exact (a cell of a board can be hit on a 2560x1440 screen); from there
 * the gain rises to ACCEL_MAX at ACCEL_FULL counts, so a quick push
 * crosses the screen. The whole gain is then scaled by the pointer's
 * speed (a percentage, 100 as it is). And the arrow's picture, which the
 * compositor draws. */
#include "internal.h"

/* ---- the pointer's position ----------------------------------------------------------- */

#define ACCEL_FROM 6     /* counts in one report: up to here 1:1 */
#define ACCEL_FULL 30    /* ... and from here the full gain */
#define ACCEL_MAX  512   /* the full gain, x256 (2:1) */

void pointer_init(struct pointer *p, int w, int h, bool accel)
{
    p->w = w;
    p->h = h;
    p->accel = accel;
    p->speed = 100;
    p->x256 = (w / 2) * 256 + 128;
    p->y256 = (h / 2) * 256 + 128;
}

/* The gain (x256) for a report that moved m counts on its longer axis. */
static int32_t gain(int m)
{
    if (m <= ACCEL_FROM)
        return 256;
    if (m >= ACCEL_FULL)
        return ACCEL_MAX;
    return 256 + (ACCEL_MAX - 256) * (m - ACCEL_FROM) / (ACCEL_FULL - ACCEL_FROM);
}

static int32_t clamp256(int64_t v, int pixels)
{
    int64_t max = (int64_t)pixels * 256 - 1;
    return (int32_t)(v < 0 ? 0 : v > max ? max : v);
}

void pointer_move(struct pointer *p, int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int32_t g = p->accel ? gain(ax > ay ? ax : ay) * p->speed / 100 : 256;
    p->x256 = clamp256((int64_t)p->x256 + (int64_t)dx * g, p->w);
    p->y256 = clamp256((int64_t)p->y256 + (int64_t)dy * g, p->h);
}

/* ---- the mouse as the app sees it ----------------------------------------------------- */

static struct mouse state;   /* what gfx_mouse hands out: edges gather here until it does */
static bool wanted;          /* gfx_mouse_open asked for the mouse */

status_t gfx_mouse_open(bool accel)
{
    (void)accel;   /* the compositor's pointer: its acceleration */
    if (!scr.open)
        return ERR_BAD_STATE;
    memset(&state, 0, sizeof(state));
    state.x = scr.w / 2;
    state.y = scr.h / 2;
    wanted = true;
    return OK;
}

void mouse_close(void)
{
    wanted = false;
}

bool mouse_wanted(void)
{
    return wanted;
}

bool mouse_at(int x, int y, uint8_t buttons, int wheel)
{
    if (!wanted)
        return false;
    buttons &= MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE;
    x = x < 0 ? 0 : x >= scr.w ? scr.w - 1 : x;
    y = y < 0 ? 0 : y >= scr.h ? scr.h - 1 : y;
    state.moved |= x != state.x || y != state.y;
    state.x = x;
    state.y = y;
    state.pressed |= buttons & ~state.buttons;
    state.released |= state.buttons & ~buttons;
    bool edge = buttons != state.buttons || wheel;
    state.buttons = buttons;
    state.wheel += wheel;
    state.reports++;
    return edge;
}

void gfx_mouse(struct mouse *out)
{
    *out = state;
    state.pressed = state.released = 0;
    state.wheel = 0;
    state.moved = false;
}

/* ---- the arrow ------------------------------------------------------------------------ */

#define ARROW_W POINTER_ARROW_W
#define ARROW_H POINTER_ARROW_H
/* '#' the outline, 'o' the fill; the hot spot is the top-left corner
 * (fun.h): the compositor draws it. */
const char pointer_arrow[ARROW_H][ARROW_W + 1] = {
    "#           ",
    "##          ",
    "#o#         ",
    "#oo#        ",
    "#ooo#       ",
    "#oooo#      ",
    "#ooooo#     ",
    "#oooooo#    ",
    "#ooooooo#   ",
    "#oooooooo#  ",
    "#ooooooooo# ",
    "#oooooo#####",
    "#ooo#oo#    ",
    "#oo# #oo#   ",
    "#o#  #oo#   ",
    "##    #oo#  ",
    "#     #oo#  ",
    "       ##   ",
};
