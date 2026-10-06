/* The status popovers (desk.h; the owner's picks in docs/G1-PLAN.md "The
 * look"): the strip's volume and network icons and its clock each open a
 * frosted card LOOK_POP_GAP pixels under the strip, its right edge on the
 * right edge of what opened it (kept LOOK_POP_EDGE from the output's
 * side); a click on the same item, on anything else, Esc or a screen
 * switch closes it. One is open at a time.
 *   volume   a slider on the mixer's volume (a click or a drag sets it) and
 *            its percentage | the output | what is playing;
 *   network  the link's state | the address, the card and its speed | the
 *            rates down and up, read again every second;
 *   clock    the time and the full date | the month's calendar, weeks
 *            starting on Monday, today marked.
 * The data come through the plumbing's hooks (ctl_volume, ctl_network
 * ...: comp.h); until a later track defines them the popovers show what a
 * desktop with nothing behind it knows (the volume set here, "Not
 * connected"). popdraw.c draws them from the boxes here. */
#include "desk.h"

#define LOOK_POP_EDGE 4   /* a popover's right edge from the output's, at least */

struct pop_state pop;

static void slider_motion(void *data, int32_t x, int32_t y);
static void slider_end(void *data);
static const struct comp_grab_ops slider_ops = { slider_motion, slider_end };

/* ---- the data ------------------------------------------------------------------------------ */

static uint32_t volume_here = 70;   /* the volume without a mixer to ask */

void pop_refresh(uint64_t t)
{
    pop.refreshed = t;
    uint32_t v;
    pop.volume = ctl_volume(&v) && v <= 100 ? v : volume_here;
    if (!ctl_audio_output(pop.output, sizeof(pop.output)))
        snprintf(pop.output, sizeof(pop.output), "Default output");
    if (!ctl_now_playing(pop.playing, sizeof(pop.playing)))
        snprintf(pop.playing, sizeof(pop.playing), "Nothing playing");
    struct desk_net n = { 0 };
    if (!ctl_network(&n))
        n = (struct desk_net){ 0 };
    n.address[sizeof(n.address) - 1] = '\0';   /* the hook's strings, ended for sure */
    n.nic[sizeof(n.nic) - 1] = '\0';
    pop.net = n;
    (void)desk_time(&pop.now);
    if (pop.kind != POP_NONE)
        desk_damage(pop.box);
}

/* ---- opening and closing ----------------------------------------------------------------- */

/* A popover's height, by what it holds (popdraw.c lays it out the same). */
static int32_t height(enum pop_kind kind)
{
    const int32_t div = 2 * LOOK_POP_DIV + 1, label = 16;
    switch (kind) {
    case POP_VOLUME:
        return 2 * LOOK_POP_PAD + LOOK_POP_LINE + 2 * (div + label + LOOK_POP_LINE);
    case POP_NETWORK:
        return 2 * LOOK_POP_PAD + LOOK_POP_LINE + 2 * (div + 2 * LOOK_POP_LINE);
    default: {
        uint8_t cells[42];
        unsigned n = pop_calendar(&pop.now, cells);
        int32_t weeks = (int32_t)(n + 6) / 7;
        return 2 * LOOK_POP_PAD + 24 + label + div + LOOK_POP_LINE * (1 + weeks);
    }
    }
}

void pop_toggle(enum pop_kind kind, struct comp_box opener)
{
    bool same = pop.kind == kind;
    pop_close();
    if (same || kind == POP_NONE)
        return;
    pop.kind = kind;
    pop_refresh(now());
    int32_t right = opener.x2;
    if (right > scene.width - LOOK_POP_EDGE)
        right = scene.width - LOOK_POP_EDGE;
    int32_t top = LOOK_STRIP_H + LOOK_POP_GAP;
    pop.box = (struct comp_box){ right - LOOK_POP_W, top, right, top + height(kind) };
    desk_damage(pop.box);
    strip_dirty();   /* its opener lit */
}

void pop_close(void)
{
    if (pop.kind == POP_NONE)
        return;
    if (pop.dragging)
        seat_grab_cancel();
    pop.dragging = false;
    desk_damage(pop.box);
    pop.kind = POP_NONE;
    pop.box = (struct comp_box){ 0, 0, 0, 0 };
    strip_dirty();
}

/* ---- the volume slider ------------------------------------------------------------------- */

#define SLIDER_ICON 16   /* the speaker before it */

struct comp_box pop_pct_box(void)
{
    if (pop.kind != POP_VOLUME)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t x2 = pop.box.x2 - LOOK_POP_PAD, y = pop.box.y1 + LOOK_POP_PAD;
    return (struct comp_box){ x2 - desk_text_w(desk_font.r12, "100%"), y, x2,
                              y + LOOK_POP_LINE };
}

struct comp_box pop_slider_box(void)
{
    if (pop.kind != POP_VOLUME)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t x1 = pop.box.x1 + LOOK_POP_PAD + SLIDER_ICON + 9;
    int32_t x2 = pop_pct_box().x1 - LOOK_POP_PCT_GAP - LOOK_POP_KNOB;   /* the knob at 100%
                                                                         * stops short of it */
    int32_t y = pop.box.y1 + LOOK_POP_PAD;
    return (struct comp_box){ x1, y, x2, y + LOOK_POP_LINE };
}

/* The volume for the pointer at x on the slider. */
static void slide_to(int32_t x)
{
    struct comp_box s = pop_slider_box();
    int32_t w = s.x2 - s.x1;
    if (w <= 0)
        return;
    int32_t v = (x - s.x1) * 100 / w;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if ((uint32_t)v == pop.volume)
        return;
    pop.volume = volume_here = (uint32_t)v;
    ctl_set_volume(pop.volume);
    desk_damage(pop.box);
}

static void slider_motion(void *data, int32_t x, int32_t y)
{
    (void)data;
    (void)y;
    if (pop.dragging)
        slide_to(x);
}

static void slider_end(void *data)
{
    (void)data;
    pop.dragging = false;
}

bool pop_press(int32_t x, int32_t y)
{
    if (pop.kind == POP_NONE || !box_contains(pop.box, x, y))
        return false;
    struct comp_box s = pop_slider_box();
    if (box_contains(s, x, y)) {
        slide_to(x);
        pop.dragging = seat_grab_begin(&slider_ops, NULL) == OK;   /* drag on from here */
    }
    return true;
}

/* ---- the calendar ------------------------------------------------------------------------ */

static bool leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

unsigned pop_calendar(const struct civil *c, uint8_t cells[42])
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned month = c->month >= 1 && c->month <= 12 ? c->month : 1;
    unsigned n = days[month - 1] + (month == 2 && leap(c->year));
    /* The weekday of the 1st: from the days since 1970-01-01, a Thursday. */
    int64_t d = civil_days(c->year, month, 1);
    unsigned wday = (unsigned)(((d % 7) + 7 + 4) % 7);   /* 0: Sunday */
    unsigned lead = (wday + 6) % 7;                      /* Monday first */
    memset(cells, 0, 42);
    for (unsigned i = 0; i < n; i++)
        cells[lead + i] = (uint8_t)(i + 1);
    return lead + n;
}
