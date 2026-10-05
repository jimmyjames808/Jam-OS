/* The keyboard focus and the keys no client sees (seat.h).
 *
 * The focus is one window (COMP_WIN_FOCUSED on it, for its title bar). It
 * moves only by the user's action or by a client's first window:
 *   - a click (a button press) into a window's surface focuses it, and the
 *     window manager is told (wm_clicked: floating raises it);
 *   - Alt+Tab (Alt+Shift+Tab backward) focuses the window manager's next
 *     (wm_cycle);
 *   - a client's FIRST window takes the focus when it is mapped (a program
 *     the shell just started); its later windows never do, so a program in
 *     the background can't keep stealing the keys;
 *   - when the focused window is unmapped or goes, the next mapped window
 *     down the stacking order takes it (else the top one), skipping
 *     windows of clients being torn down;
 *   - the window manager may focus a window itself (seat_focus: a title
 *     bar click).
 * No client can grab the keyboard: there is no request that moves it.
 *
 * Keys no client sees, taken on their press before any focus is looked at
 * (the press and its release both go nowhere):
 *   Ctrl+Alt+Del   init reboots the machine (ctl.c asks it, as the console
 *                  did);
 *   Alt+Tab        the next window (Alt+Shift+Tab: the one before);
 *   Super+F        the focused window full screen, or back;
 *   Super+T        the screen's arrangement: floating or tiling;
 *   Super+Q        the focused window asked to close (as its close box);
 *   Super+Enter    another terminal (ctl.c asks init), whichever window
 *                  has the focus, a terminal or not.
 * Ctrl+C is an ordinary key: it goes to the focused window, so a program
 * can trap the keys of its own window and never another's.
 *
 * The window manager's hooks (comp.h) have weak defaults here, standing in
 * until wm.c defines them: the seat works on its own (and in tests) with
 * the plainest arrangement. */
#include "seat.h"

/* HID usages (USB HID Usage Tables 1.4, section 10) and modifier bits. */
#define U_F        0x09
#define U_Q        0x14
#define U_T        0x17
#define U_ENTER    0x28
#define U_TAB      0x2b
#define U_DELETE   0x4c
#define U_KP_ENTER 0x58
#define MOD_SUPER  (INPUT_MOD_LGUI | INPUT_MOD_RGUI)

static struct comp_window *focused;   /* NULL: no window has the keys */

struct comp_window *seat_focused(void)
{
    return focused;
}

/* Can w take the focus: mapped, and its client alive. */
static bool focusable(const struct comp_window *w)
{
    return w && (w->flags & COMP_WIN_MAPPED) && client_alive(w->surface->client);
}

void seat_focus(struct comp_window *w)
{
    if (w == focused || (w && !focusable(w)))
        return;
    struct comp_window *old = focused;
    focused = w;
    if (old) {
        old->flags &= ~COMP_WIN_FOCUSED;
        window_damage(old);   /* its title bar changes colour */
        keyboard_leave(old);
    }
    if (w) {
        w->flags |= COMP_WIN_FOCUSED;
        window_damage(w);
        keyboard_enter(w);
    }
    wm_focus_changed(w);   /* raised, and the toplevels' activated state */
}

void focus_click(struct comp_window *w)
{
    seat_focus(w);
    wm_clicked(w);
}

void focus_window_mapped(struct comp_window *w)
{
    struct seat_client *sc = seat_of(w->surface->client);
    if (sc->had_window)
        return;
    sc->had_window = true;
    seat_focus(w);
}

/* The window to take the focus from w: the next one down, else the top. */
static struct comp_window *successor(const struct comp_window *w)
{
    for (struct comp_window *b = w->below; b; b = b->below)
        if (focusable(b))
            return b;
    for (struct comp_window *t = scene.top; t; t = t->below)
        if (t != w && focusable(t))
            return t;
    return NULL;
}

void focus_window_gone(struct comp_window *w)
{
    if (w != focused)
        return;
    /* Leave first, while w is still what the client knows. */
    w->flags &= ~COMP_WIN_FOCUSED;
    keyboard_leave(w);
    focused = NULL;
    seat_focus(successor(w));
}

/* ---- the keys no client sees -------------------------------------------------------- */

bool focus_reserved_key(uint16_t usage, uint8_t mods)
{
    bool ctrl = mods & INPUT_MOD_CTRL, alt = mods & INPUT_MOD_ALT;
    bool shift = mods & INPUT_MOD_SHIFT, super = mods & MOD_SUPER;
    if (usage == U_DELETE && ctrl && alt) {
        ctl_reboot();
        return true;
    }
    if (usage == U_TAB && alt && !ctrl) {
        struct comp_window *w = wm_cycle(focused, shift);
        if (w)
            seat_focus(w);
        return true;
    }
    if (usage == U_F && super) {
        if (focused)
            wm_toggle_fullscreen(focused);
        return true;
    }
    if (usage == U_T && super) {
        wm_toggle_layout();
        return true;
    }
    if (usage == U_Q && super) {
        if (focused)
            wm_close(focused);
        return true;
    }
    if ((usage == U_ENTER || usage == U_KP_ENTER) && super) {
        ctl_terminal();
        return true;
    }
    return false;
}

/* ---- the window manager's defaults ---------------------------------------------------- */

__attribute__((weak)) struct comp_window *wm_cycle(struct comp_window *from, bool backward)
{
    struct comp_window *w = from;
    for (uint32_t n = 0; n <= scene.nwindows; n++) {
        if (backward)
            w = w && w->above ? w->above : scene.bottom;
        else
            w = w && w->below ? w->below : scene.top;
        if (w != from && focusable(w))
            return w;
    }
    return NULL;
}

__attribute__((weak)) void wm_clicked(struct comp_window *w)
{
    window_raise(w);
}

__attribute__((weak)) bool wm_press(int32_t x, int32_t y, uint32_t button)
{
    (void)x;
    (void)y;
    (void)button;
    return false;
}

__attribute__((weak)) void wm_toggle_fullscreen(struct comp_window *w)
{
    (void)w;
}

__attribute__((weak)) void wm_toggle_layout(void)
{
}

__attribute__((weak)) void wm_focus_changed(struct comp_window *w)
{
    (void)w;
}

__attribute__((weak)) void wm_close(struct comp_window *w)
{
    (void)w;
}

__attribute__((weak)) void cursor_moved(int32_t old_x, int32_t old_y)
{
    (void)old_x;
    (void)old_y;
}
