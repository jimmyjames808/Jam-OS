/* The desktop's glue (comp.h, desk.h): what the seat, the loop and the
 * window manager ask of it, and the hooks the plumbing fills in.
 *
 * Keys (desk_key, before any client sees them; focus.c asks):
 *   - Super tapped alone (pressed and let go within half a second, no
 *     other key or click between) opens the search box, or closes it;
 *   - Alt+Tab, Alt+Shift+Tab: Alt+Tab's list (alttab_step; an open search
 *     box closes first); letting go of Alt goes to the selected window
 *     (desk_key_up, before the release reaches a client, so the window it
 *     goes to gets that release), Esc while Alt is held cancels;
 *   - while the search box is open every other key is its own (search_key);
 *   - Esc closes an open popover;
 *   - Super+Left/Right: the screen before or after (past the last: a new
 *     one); with Shift the focused window goes there too; Super+1..9: that
 *     screen.
 * Super itself always reaches the client (it is a modifier), and so do
 * Alt's press and release. Every key press, and every button press, first
 * ends a running animation (anim.c).
 *
 * Presses (desk_press, before the window manager's): on the strip, its
 * item's action (strip.c); on a card, the card's; elsewhere, an open menu
 * or popover closes and the press goes on to whatever is there.
 *
 * The loop's clock (desk_tick): animations, Alt+Tab's list after it has
 * been held LOOK_ALTTAB_SHOW_MS, notifications coming and going, the
 * strip's clock each minute, the network popover's rates each second, the
 * busy cursor's end.
 *
 * Off (desk_init(false, ...)), only the screen keys work: the window
 * manager alone, as tests and `nodesk` want it. */
#include <fun.h>
#include "desk.h"

/* HID usages (USB HID Usage Tables 1.4, section 10). */
#define U_1      0x1e
#define U_9      0x26
#define U_ESC    0x29
#define U_TAB    0x2b
#define U_RIGHT  0x4f
#define U_LEFT   0x50
#define U_LALT   0xe2
#define U_LGUI   0xe3
#define U_RALT   0xe6
#define U_RGUI   0xe7
#define SUPER_TAP_NS (500 * NS_PER_MS)   /* a tap of Super: let go within this */
#define NET_EVERY    NS_PER_S            /* the network popover's rates read again */

struct desk_fonts desk_font;

static struct {
    bool on;
    bool super_down, super_used;   /* Super held, and something else happened meanwhile */
    uint64_t super_at;
    bool fixed;                    /* desk_time_fix */
    struct civil fixed_time;
    int minute;                    /* the strip's clock's minute (-1: not drawn yet) */
    char launching[16];            /* the app being launched ("" none) */
    uint64_t busy_until;           /* 0: not busy */
} dk;

/* ---- start --------------------------------------------------------------------------------- */

static void open_font(enum font_weight wt, int px, struct font **out)
{
    if (font_open(wt, px, out) != OK)
        *out = NULL;   /* measured as 7 a character, not drawn */
}

void desk_init(bool on, bool animate)
{
    search_close();   /* every card closed: a fresh start (and each test's) */
    alttab_end(false);
    pop_close();
    memset(&notes, 0, sizeof(notes));
    bool fixed = dk.fixed;
    struct civil when = dk.fixed_time;
    memset(&dk, 0, sizeof(dk));
    dk.fixed = fixed;
    dk.fixed_time = when;
    dk.on = on;
    dk.minute = -1;
    anim_init(on && animate);
    if (on && !desk_font.r12) {
        open_font(FONT_REGULAR, LOOK_LABEL_PX, &desk_font.r11);
        open_font(FONT_MEDIUM, LOOK_LABEL_PX, &desk_font.m11);
        open_font(FONT_REGULAR, LOOK_STRIP_PX, &desk_font.r12);
        open_font(FONT_MEDIUM, LOOK_STRIP_PX, &desk_font.m12);
        open_font(FONT_REGULAR, LOOK_MENU_PX, &desk_font.r13);
        open_font(FONT_MEDIUM, LOOK_MENU_PX, &desk_font.m13);
        open_font(FONT_REGULAR, LOOK_SEARCH_PX, &desk_font.r16);
        open_font(FONT_MEDIUM, LOOK_POP_BIG_PX, &desk_font.m17);
    }
    strip_dirty();
}

bool desk_on(void)
{
    return dk.on;
}

int desk_text_w(const struct font *f, const char *str)
{
    return f ? font_width(f, str) : 7 * (int)strlen(str);
}

/* ---- the time ------------------------------------------------------------------------------ */

bool desk_time(struct civil *out)
{
    if (dk.fixed) {
        *out = dk.fixed_time;
        return true;
    }
    return clock_local(out);
}

void desk_time_fix(const struct civil *c)
{
    dk.fixed = c != NULL;
    if (c)
        dk.fixed_time = *c;
    dk.minute = -1;
    strip_dirty();
}

/* ---- keys ---------------------------------------------------------------------------------- */

/* Super with an arrow or a digit: the screens. True if taken. */
static bool screen_key(uint16_t usage, bool shift)
{
    if (usage == U_RIGHT || usage == U_LEFT) {
        int dir = usage == U_RIGHT ? 1 : -1;
        if (shift)
            screens_move(wm_focused(), dir);
        else
            screens_step(dir);
        return true;
    }
    if (usage >= U_1 && usage <= U_9 && !shift) {
        screens_go(usage - U_1);
        return true;
    }
    return false;
}

bool desk_key(uint16_t usage, uint8_t mods, uint32_t xkb_mods)
{
    bool alt = mods & INPUT_MOD_ALT, ctrl = mods & INPUT_MOD_CTRL;
    bool shift = mods & INPUT_MOD_SHIFT, super = mods & (INPUT_MOD_LGUI | INPUT_MOD_RGUI);
    if (usage == U_LGUI || usage == U_RGUI) {
        dk.super_down = true;
        dk.super_used = false;
        dk.super_at = now();
        return false;   /* a modifier: its client sees it */
    }
    dk.super_used = true;
    anim_finish();
    if (dk.on && usage == U_TAB && alt && !ctrl) {   /* even from the search box: it closes */
        search_close();
        pop_close();
        alttab_step(shift);
        return true;
    }
    if (dk.on && search.open) {
        search_key(usage, xkb_mods);
        return true;
    }
    if (dk.on && usage == U_ESC && alttab.active) {
        alttab_end(false);
        return true;
    }
    if (dk.on && usage == U_ESC && pop.kind != POP_NONE) {
        pop_close();
        return true;
    }
    return super && !ctrl && !alt && screen_key(usage, shift);
}

void desk_key_up(uint16_t usage)
{
    if ((usage == U_LALT || usage == U_RALT) && alttab.active) {
        alttab_end(true);
        return;
    }
    if (usage != U_LGUI && usage != U_RGUI)
        return;
    bool tap = dk.super_down && !dk.super_used && now() - dk.super_at < SUPER_TAP_NS;
    dk.super_down = false;
    if (tap && dk.on && !alttab.active) {
        pop_close();
        search_toggle();
    }
}

/* ---- presses ------------------------------------------------------------------------------- */

bool desk_press(int32_t x, int32_t y, uint32_t button)
{
    (void)button;
    dk.super_used = true;
    anim_finish();
    if (!dk.on)
        return false;
    if (alttab.active)
        return alttab_press(x, y);
    bool on_strip = box_contains(strip_box(), x, y);
    if (search.open && !on_strip) {
        if (search_press(x, y))
            return true;
        search_close();   /* and the press goes on */
    }
    if (pop.kind != POP_NONE && !on_strip) {
        if (pop_press(x, y))
            return true;
        pop_close();
    }
    if (notify_press(x, y))
        return true;
    if (!on_strip)
        return false;
    const struct strip_item *it = strip_at(x, y);
    if (it)
        strip_click(it);
    return true;   /* the strip is nobody else's */
}

bool desk_covers(int32_t x, int32_t y)
{
    if (!dk.on)
        return false;
    if (box_contains(strip_box(), x, y) || (search.open && box_contains(search_box(), x, y)) ||
        (alttab.shown && box_contains(alttab_box(), x, y)) ||
        (pop.kind != POP_NONE && box_contains(pop.box, x, y)))
        return true;
    for (unsigned i = 0; i < notes.n; i++)
        if (box_contains(notes.cards[i].box, x, y))
            return true;
    return false;
}

/* Is (x, y) on one of the boxes a click does something with: a strip item
 * or island, a menu's row, a notification's card, a popover's slider? */
static bool clickable(int32_t x, int32_t y)
{
    for (int i = 0; i < 3; i++)
        if (box_contains(strip.islands[i], x, y))
            return true;
    for (unsigned i = 0; i < search.nrows && search.open; i++)
        if (box_contains(search_row_box(i), x, y))
            return true;
    for (unsigned i = 0; i < alttab.n && alttab.shown; i++)
        if (box_contains(alttab_row_box(i), x, y))
            return true;
    for (unsigned i = 0; i < notes.n; i++) {
        const struct notify_card *c = &notes.cards[i];
        if (!box_contains(c->box, x, y))
            continue;
        if (!c->nbuttons)
            return true;   /* a click sends it */
        for (unsigned b = 0; b < c->nbuttons; b++)
            if (box_contains(notify_button_box(c, b), x, y))
                return true;
        return false;
    }
    return box_contains(pop_slider_box(), x, y);
}

enum cursor_shape desk_cursor_at(int32_t x, int32_t y)
{
    if (!desk_covers(x, y))
        return CURSOR_SHAPES;   /* nothing of the desktop's there */
    return clickable(x, y) ? CURSOR_HAND : CURSOR_ARROW;
}

void desk_damage(struct comp_box b)
{
    if (box_empty(b))
        return;
    /* with the shadow a card casts (ui_glass): a card that goes takes it along */
    scene_damage_over((struct comp_box){ b.x1 - LOOK_SHADOW_SIDE, b.y1 - LOOK_SHADOW_ABOVE,
                                         b.x2 + LOOK_SHADOW_SIDE, b.y2 + LOOK_SHADOW_BELOW });
}

/* ---- launching ------------------------------------------------------------------------------ */

static void busy_set(bool on)
{
    uint64_t until = on ? now() + DESK_BUSY_NS : 0;
    bool was = dk.busy_until != 0;
    dk.busy_until = until;
    if (!on)
        dk.launching[0] = '\0';
    if (was != on)
        seat_cursor_changed();   /* busy, or the arrow again */
}

void desk_launch(const char *app)
{
    snprintf(dk.launching, sizeof(dk.launching), "%s", app);
    busy_set(true);
    ctl_launch(app);
}

void desk_run(const char *cmd)
{
    snprintf(dk.launching, sizeof(dk.launching), "%s", "terminal");
    busy_set(true);   /* until its window shows: the answer names it (desk_launch_awaits) */
    ctl_run_in_terminal(cmd);
}

void desk_launch_awaits(const char *title)
{
    if (dk.busy_until)
        snprintf(dk.launching, sizeof(dk.launching), "%s", title);
}

void desk_launch_failed(void)
{
    busy_set(false);
}

bool desk_busy(void)
{
    return dk.busy_until != 0;
}

/* a and b the same, letters of either case alike. */
static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return false;
    return !*a && !*b;
}

void desk_window_mapped(const struct wm_window *ww)
{
    if (dk.busy_until &&
        (same_name(ww->app_id, dk.launching) || same_name(ww->title, dk.launching)))
        busy_set(false);
}

/* ---- the clock ------------------------------------------------------------------------------ */

/* The strip's clock: laid out again when the minute changes. */
static void clock_tick(void)
{
    struct civil c;
    if (!strip.shown && pop.kind != POP_CLOCK)
        return;
    (void)desk_time(&c);   /* unset: the RTC's reading, as `date` shows */
    if ((int)c.minute == dk.minute)
        return;
    dk.minute = (int)c.minute;
    strip_dirty();
    if (pop.kind == POP_CLOCK)
        pop_refresh(now());
}

void desk_tick(uint64_t t)
{
    anim_tick(t);
    if (alttab.active && !alttab.shown && t >= alttab.since + LOOK_ALTTAB_SHOW_MS * NS_PER_MS) {
        alttab.shown = true;
        desk_damage(alttab_box());
    }
    notify_tick(t);
    clock_tick();
    if (pop.kind == POP_NETWORK && t >= pop.refreshed + NET_EVERY)
        pop_refresh(t);
    if (dk.busy_until && t >= dk.busy_until)
        busy_set(false);
    else if (dk.busy_until)
        cursor_moved(cursor.x, cursor.y);   /* the busy ring turns: its box only */
    strip_update();
}

static uint64_t earliest(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

uint64_t desk_deadline(void)
{
    uint64_t d = anim_deadline();
    if (!dk.on)
        return d;
    if (alttab.active && !alttab.shown)
        d = earliest(d, alttab.since + LOOK_ALTTAB_SHOW_MS * NS_PER_MS);
    d = earliest(d, notify_deadline());
    if (pop.kind == POP_NETWORK)
        d = earliest(d, pop.refreshed + NET_EVERY);
    if (dk.busy_until)   /* the busy ring turns: a frame each paint */
        d = earliest(d, earliest(dk.busy_until, scene.last_paint_ns + comp.period_ns));
    struct civil c;
    if ((strip.shown || pop.kind == POP_CLOCK) && desk_time(&c) && !dk.fixed)
        d = earliest(d, now() + (uint64_t)(60 - (c.second < 60 ? c.second : 59)) * NS_PER_S);
    return d;
}

/* ---- the plumbing's hooks: what a desktop with nothing behind it does ------------------------ */

__attribute__((weak)) void ctl_launch(const char *app)
{
    printf("compositor: launch %s: nothing runs apps for the desktop yet\n", app);
}

__attribute__((weak)) void ctl_run_in_terminal(const char *cmd)
{
    printf("compositor: run \"%s\" in a terminal: nothing runs commands for the desktop yet\n",
           cmd);
}

__attribute__((weak)) void ctl_notify_answered(uint32_t id, uint32_t button)
{
    (void)id;
    (void)button;
}

__attribute__((weak)) bool ctl_volume(uint32_t *percent)
{
    (void)percent;
    return false;
}

__attribute__((weak)) void ctl_set_volume(uint32_t percent)
{
    (void)percent;
}

__attribute__((weak)) bool ctl_audio_output(char *buf, size_t n)
{
    (void)buf;
    (void)n;
    return false;
}

__attribute__((weak)) bool ctl_now_playing(char *buf, size_t n)
{
    (void)buf;
    (void)n;
    return false;
}

__attribute__((weak)) bool ctl_network(struct desk_net *out)
{
    (void)out;
    return false;
}
