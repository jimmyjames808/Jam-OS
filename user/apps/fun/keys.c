/* libfun: keys (fun.h): key events decoded, and the mouse's news, both
 * from the window (wl.c's wl_next): a key, the pointer, the window's new
 * size (KEY_RESIZE). gfx_key is the one place an app waits for all of
 * them: the mouse's news comes back from it as KEY_MOUSE. Movement is
 * merged (every report already queued is folded in before KEY_MOUSE is
 * returned), but a report that changes a button or the wheel ends the
 * merge, so gfx_mouse shows each press and release with the position it
 * happened at. */
#include "internal.h"

/* ---- keys ---------------------------------------------------------------------------- */

int key_decode(const struct fun_key *ev)
{
    if (ev->state == INPUT_KEY_UP)
        return KEY_NONE;
    uint32_t cp = ev->codepoint;
    if (cp == 0x1b || ev->usage == 0x29 || cp == 3)
        return KEY_QUIT;
    if ((ev->mods & INPUT_MOD_CTRL) && (cp == 'c' || ev->usage == 0x06))
        return KEY_QUIT;
    switch (ev->usage) {
    case 0x52: return KEY_UP;
    case 0x51: return KEY_DOWN;
    case 0x50: return KEY_LEFT;
    case 0x4f: return KEY_RIGHT;
    case 0x28: case 0x58: return KEY_ENTER;
    case 0x4b: return KEY_PGUP;
    case 0x4e: return KEY_PGDN;
    case 0x4a: return KEY_HOME;
    case 0x4d: return KEY_END;
    case 0x2a: return KEY_BACKSPACE;
    case 0x2b: return KEY_TAB;
    case 0x4c: return KEY_DELETE;
    case 0x57: return '+';   /* keypad */
    case 0x56: return '-';
    }
    if (cp == '\n' || cp == '\r')
        return KEY_ENTER;
    if (cp == 8 || cp == 0x7f)   /* a serial terminal's Backspace */
        return KEY_BACKSPACE;
    if (cp == '\t')
        return KEY_TAB;
    if (cp >= 0x20 && cp < 0x7f)
        return (int)cp;
    return KEY_NONE;
}

/* ---- the window's messages ------------------------------------------------------------ */

static struct fun_key held;   /* a key read while mouse news waited to be told */
static bool have_held;
static bool mouse_news;               /* reports folded in since the last KEY_MOUSE */
static bool resize_news;              /* a new size gfx_key_event read, not told yet */

/* The next message before deadline (0: only what is queued). OK: *kind
 * says what it was; a key is in *ev, the mouse's news went to mouse.c.
 * ERR_TIMED_OUT when none came; another error when the window broke. */
static status_t next_msg(uint64_t deadline, struct fun_key *ev, enum msg_kind *kind)
{
    return scr.open ? wl_next(deadline, ev, kind) : ERR_BAD_HANDLE;
}

status_t gfx_key_event(uint64_t deadline, struct fun_key *ev)
{
    if (have_held) {
        *ev = held;
        have_held = false;
        return OK;
    }
    for (;;) {
        enum msg_kind kind;
        status_t st = next_msg(deadline, ev, &kind);
        if (st != OK || kind == MSG_KEY)
            return st;
        if (kind == MSG_RESIZE)
            resize_news = true;   /* gfx_key tells */
        else
            mouse_news = true;
    }
}

/* The mouse news is told: KEY_MOUSE. */
static int tell_mouse(void)
{
    mouse_news = false;
    return KEY_MOUSE;
}

int gfx_key(uint64_t deadline)
{
    if (resize_news) {
        resize_news = false;
        return KEY_RESIZE;
    }
    for (;;) {
        struct fun_key ev;
        enum msg_kind kind = MSG_KEY;
        if (have_held) {
            ev = held;
            have_held = false;
        } else {
            /* With mouse news waiting, only what is already queued is
             * taken (more movement to merge), never waited for. */
            status_t st = next_msg(mouse_news ? 0 : deadline, &ev, &kind);
            if (st == ERR_TIMED_OUT)
                return mouse_news ? tell_mouse() : KEY_NONE;
            if (st != OK)
                return KEY_QUIT;
        }
        if (kind == MSG_RESIZE)
            return KEY_RESIZE;   /* at once: scr is new already; mouse news waits */
        if (kind == MSG_BUTTON)
            return tell_mouse();
        if (kind == MSG_MOVE) {
            mouse_news = true;
            continue;
        }
        if (mouse_news) {   /* the mouse news came first: the key waits its turn */
            held = ev;
            have_held = true;
            return tell_mouse();
        }
        int k = key_decode(&ev);
        if (k != KEY_NONE)
            return k;
    }
}
