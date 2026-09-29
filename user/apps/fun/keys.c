/* libfun: key events from the console, decoded (fun.h). */
#include "fun.h"

/* ---- keys ---------------------------------------------------------------------------- */

int key_decode(const struct input_key_event *ev)
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
    case 0x57: return '+';   /* keypad */
    case 0x56: return '-';
    }
    if (cp == '\n' || cp == '\r')
        return KEY_ENTER;
    if (cp >= 0x20 && cp < 0x7f)
        return (int)cp;
    return KEY_NONE;
}

status_t gfx_key_event(uint64_t deadline, struct input_key_event *ev)
{
    if (!scr.keys)
        return ERR_BAD_HANDLE;
    for (;;) {
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = scr.keys, .bytes_cap = sizeof(*ev), .bytes = (uint64_t)(uintptr_t)ev,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            if (n == sizeof(*ev))
                return OK;
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return st;   /* the console went away */
        if (!deadline)
            return ERR_TIMED_OUT;
        signals_t seen;
        st = jam_object_wait_one(scr.keys, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st != OK)
            return st;
    }
}

int gfx_key(uint64_t deadline)
{
    for (;;) {
        struct input_key_event ev;
        status_t st = gfx_key_event(deadline, &ev);
        if (st == ERR_TIMED_OUT)
            return KEY_NONE;
        if (st != OK)
            return KEY_QUIT;
        int k = key_decode(&ev);
        if (k != KEY_NONE)
            return k;
    }
}
