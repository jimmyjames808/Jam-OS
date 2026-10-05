/* libfun: the mouse (fun.h): a pointer position made from the console's
 * relative mouse reports, the buttons with their edges, and the arrow
 * drawn over the app's frame.
 *
 * The console sends what the mouse driver sent: counts moved, the wheel,
 * the buttons held (struct input_mouse_event, <jam/abi.h>), and only after
 * gfx_mouse_open asked for it. The position is kept here in 1/256 pixel,
 * clamped to the screen. Acceleration goes by the size of one report: up
 * to ACCEL_FROM counts it is 1:1, so slow movement is exact (a cell of a
 * board can be hit on a 2560x1440 screen); from there the gain rises to
 * ACCEL_MAX at ACCEL_FULL counts, so a quick push crosses the screen. The
 * whole gain is then scaled by $POINTER_SPEED, a percentage (25..400,
 * default 100): `export POINTER_SPEED=70` in the shell slows every app's
 * pointer.
 *
 * In a window the compositor owns the pointer: it moves and accelerates
 * it and draws the arrow, and tells where it is over the window (wl.c
 * hands that to mouse_at); none of the above is used.
 *
 * The arrow is not part of the app's picture: a present paints it into the
 * back buffer, copies what changed to the screen, and takes it out again
 * (the pixels under it are kept meanwhile). The present's own compare
 * against what the screen shows then moves it: the rows it left are
 * rewritten as the frame, the rows it is on now as the arrow. */
#include "internal.h"

/* ---- the pointer's position ----------------------------------------------------------- */

#define ACCEL_FROM 6     /* counts in one report: up to here 1:1 */
#define ACCEL_FULL 30    /* ... and from here the full gain */
#define ACCEL_MAX  512   /* the full gain, x256 (2:1) */
#define SPEED_MIN  25    /* $POINTER_SPEED, percent */
#define SPEED_MAX  400

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

/* $POINTER_SPEED as a percentage, 100 if unset or not a number. */
static int speed_from_env(void)
{
    static const char key[] = "POINTER_SPEED=";
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, key, sizeof(key) - 1))
            continue;
        int v = 0;
        for (const char *d = *e + sizeof(key) - 1; *d >= '0' && *d <= '9' && v < 10000; d++)
            v = v * 10 + (*d - '0');
        return v < SPEED_MIN ? (v ? SPEED_MIN : 100) : v > SPEED_MAX ? SPEED_MAX : v;
    }
    return 100;
}

static struct pointer ptr;
static struct mouse state;   /* what gfx_mouse hands out: edges gather here until it does */
static bool wanted;          /* gfx_mouse_open asked the console for the mouse */
static bool visible;         /* the arrow is drawn */
static bool hidden;          /* the app asked for no arrow (gfx_pointer_show) */

status_t gfx_mouse_open(bool accel)
{
    if (!scr.open)
        return ERR_BAD_STATE;
    struct input_want w = { .events = INPUT_WANT_MOUSE };
    status_t st = scr.windowed ? OK : jam_channel_write(scr.keys, &w, sizeof(w), NULL, 0);
    if (st != OK)
        return st;
    pointer_init(&ptr, scr.w, scr.h, accel);
    ptr.speed = speed_from_env();
    memset(&state, 0, sizeof(state));
    state.x = pointer_x(&ptr);
    state.y = pointer_y(&ptr);
    wanted = true;
    visible = hidden = false;   /* the arrow comes with the first report: is there a mouse? */
    return OK;
}

void mouse_close(void)
{
    wanted = visible = false;
}

bool mouse_wanted(void)
{
    return wanted;
}

bool mouse_report(const struct input_mouse_event *ev)
{
    if (!wanted)
        return false;
    pointer_move(&ptr, ev->dx, ev->dy);
    return mouse_at(pointer_x(&ptr), pointer_y(&ptr), ev->buttons, ev->wheel);
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
    visible = !hidden;
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
#define ARROW_MAX_SCALE 4
/* '#' the outline, 'o' the fill; the hot spot is the top-left corner
 * (fun.h: the compositor draws it too). */
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

/* The back buffer's pixels under the painted arrow, row after row of the
 * rectangle (px, py, pw, ph), which is clipped to the screen. */
static uint32_t under[ARROW_W * ARROW_H * ARROW_MAX_SCALE * ARROW_MAX_SCALE];
static int px, py, pw, ph;
static bool painted;
static int shown_y = -1;   /* the row the screen shows the arrow at; -1: it shows none */

static int arrow_scale(void)
{
    return scr.ui < ARROW_MAX_SCALE ? scr.ui : ARROW_MAX_SCALE;
}

void pointer_paint(void)
{
    painted = false;
    shown_y = visible ? state.y : -1;
    if (!visible)
        return;
    int k = arrow_scale();
    px = state.x;
    py = state.y;
    pw = px + ARROW_W * k > scr.w ? scr.w - px : ARROW_W * k;
    ph = py + ARROW_H * k > scr.h ? scr.h - py : ARROW_H * k;
    for (int j = 0; j < ph; j++) {
        uint32_t *row = scr.s.px + (uint64_t)(py + j) * scr.s.stride + px;
        for (int i = 0; i < pw; i++) {
            under[j * pw + i] = row[i];
            char c = pointer_arrow[j / k][i / k];
            if (c != ' ')
                row[i] = c == '#' ? 0x000000 : 0xffffff;
        }
    }
    painted = true;
}

void pointer_unpaint(void)
{
    if (!painted)
        return;
    painted = false;
    for (int j = 0; j < ph; j++) {
        uint32_t *row = scr.s.px + (uint64_t)(py + j) * scr.s.stride + px;
        for (int i = 0; i < pw; i++)
            row[i] = under[j * pw + i];
    }
}

void gfx_pointer_show(bool on)
{
    hidden = !on;
    visible = on && wanted && state.reports;
}

void gfx_present_pointer(void)
{
    if (scr.windowed)
        return;   /* the compositor's arrow */
    int from = shown_y, to = visible ? state.y : -1;
    if (from < 0 && to < 0)
        return;
    if (from < 0)
        from = to;
    if (to < 0)
        to = from;
    gfx_present_rows(from < to ? from : to, (from > to ? from : to) + ARROW_H * arrow_scale());
}
