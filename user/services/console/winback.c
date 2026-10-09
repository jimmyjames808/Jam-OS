/* console: the window back end (console.h, struct backend).
 *
 * The terminal as a text model (text.c, term.c) drawn into a window of the
 * compositor's (window.c, winpaint.c), with the text copied to COM1. This
 * file is the front end's way in: program output goes through the escape
 * interpreter into the text, kernel log lines above the current line, and
 * the keys from the window (wlinput.c, via window.c) come here first, for
 * the scrollback's keys, before they go to the focus. Without a
 * compositor there is no window and the text goes to COM1 alone.
 *
 * The screen is drawn at most every RENDER_NS (win_flush). */
#include "console.h"

static uint64_t last_render;   /* when the screen was last drawn */
static bool have_screen;       /* something shows the text (a window); else COM1 only */

/* The screen: the window, with /svc/wayland's channel from init; the
 * text model at its size. false: out of memory. Without a compositor
 * nothing shows the text: COM1 only. */
static bool win_init(void)
{
    handle_t wayland = startup_handle(SR_USER + WAYLAND_ROLE);
    cols = WIN_COLS;   /* until the output is known (window.c regrids) */
    rows = WIN_ROWS;
    have_screen = false;
    if (wayland)
        have_screen = window_init(wayland, term_no);
    else
        printf("console: no compositor (no /svc/wayland): no screen, COM1 only\n");
    if (text_init() && paint_regrid())
        return true;
    printf("console: out of memory\n");
    return false;
}

static void win_banner(unsigned clients, bool log_off)
{
    printf("console: %ux%u cells%s, %u client channel(s)%s\n", cols, rows,
           have_screen ? "" : " (no screen)", clients,
           log_off ? "; the kernel log off the screen" : "");
}

static void win_ready(void)
{
    render();
}

static void win_write(struct client *c, const uint8_t *text, uint32_t length)
{
    bool was_alt = alt_on;
    out_writer = c;   /* the modes it asks for are its own (term.c) */
    for (unsigned i = 0; i < length; i++)
        out_char(text[i]);
    out_writer = NULL;
    if (!was_alt || !alt_on)   /* the alternate screen isn't mirrored to COM1 */
        (void)jam_serial_write(root, text, length);   /* the mirror is best effort */
    dirty = true;
}

static void win_size(uint16_t *c, uint16_t *r)
{
    *c = (uint16_t)cols;
    *r = (uint16_t)rows;
}

static void win_clear(void)
{
    clear_screen();
    jam_serial_write(root, "\033[2J\033[H", 7);
}

static void win_focus_dropped(handle_t h)
{
    if (h == alt_owner)
        alt_leave();
}

static void win_event(uint32_t kind, uint32_t i)
{
    if (kind == K_ALT)
        alt_owner_event(i);
    else if (kind == K_WL)
        window_event();
}

static uint64_t win_deadline(void)
{
    uint64_t render_at = dirty ? last_render + RENDER_NS : DEADLINE_NEVER;
    uint64_t wl_at = window_deadline();
    return wl_at < render_at ? wl_at : render_at;
}

static void win_tick(void)
{
    if (window_mode)
        window_event();   /* its own work: a key repeat, a reconnect's try */
}

static void win_flush(void)
{
    uint64_t t = now();
    if (dirty && t >= last_render + RENDER_NS) {
        render();
        last_render = t;
    }
}

const struct backend win_backend = {
    .replay_log = true,   /* the window shows the boot log so far (its last part) */
    .init = win_init, .banner = win_banner, .ready = win_ready, .write = win_write,
    .write_raw = win_write,   /* the text model reads CR and LF as they come */
    .log_line = kernel_line, .size = win_size, .clear = win_clear,
    .set_font = window_set_font, .focus_dropped = win_focus_dropped, .event = win_event,
    .deadline = win_deadline, .tick = win_tick, .flush = win_flush,
};

/* ---- keys and the mouse from the window -------------------------------------------- */

void win_key(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    struct input_key_event ev = { usage, state, mods, cp };
    /* Scrollback: Shift+PageUp/Down. */
    bool page = usage == 0x4b || usage == 0x4e;
    if (page && !alt_on && (mods & INPUT_MOD_SHIFT)) {
        if (state == INPUT_KEY_UP)
            return;
        uint32_t step = rows / 2 ? rows / 2 : 1;
        struct view v = view_now();
        uint32_t max = view_back_max(&v);
        if (usage == 0x4b)
            view_back = view_back + step < max ? view_back + step : max;
        else
            view_back = view_back > step ? view_back - step : 0;
        dirty = true;
        return;
    }
    if (state != INPUT_KEY_UP && view_back) {
        view_back = 0;
        dirty = true;
    }
    focus_key(&ev);
}

void mouse_event(const struct con_mouse *ev)
{
    /* The wheel scrolls back; the rest is the selection's (clip.c). */
    if (ev->wheel > 0)
        win_key(0x4b, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0);
    else if (ev->wheel < 0)
        win_key(0x4e, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0);
}
