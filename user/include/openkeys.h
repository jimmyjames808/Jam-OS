/* The messages of a console.open_keys channel (abi/idl/console.idl), and
 * through <idl/input.h> the input protocol's constants they carry
 * (INPUT_KEY_*, INPUT_MOD_*, INPUT_BTN_*: abi/idl/input.idl).
 *
 * A console.open_keys channel carries plain messages, no protocol header,
 * and its message kinds are told apart by their size:
 *
 *   console -> client   struct input_key_event (8 bytes): one key, to the
 *                       focused client. Every client gets these.
 *   console -> client   struct input_mouse_event (12 bytes): one mouse
 *                       report, to the focused client, and only if it asked.
 *   client -> console   struct input_want (8 bytes): which events besides
 *                       keys the client wants from now on. The console
 *                       reads it before it passes on the next mouse report.
 *
 * A client that never writes on the channel only ever reads key events,
 * so one that knows nothing of the mouse needs no size check. Anything
 * else a client writes (another size, unknown bits, a non-zero reserved
 * field, handles) loses it the channel: the console closes its end. While
 * the focused client wants the mouse it gets every report, the wheel
 * included; otherwise the wheel scrolls the console's text back (while
 * the console has the screen) and the rest of the report is dropped. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <idl/input.h>

struct input_key_event {
    uint16_t usage;       /* HID keyboard page usage id, 0 for text from a terminal */
    uint8_t  state;       /* INPUT_KEY_* */
    uint8_t  mods;        /* INPUT_MOD_* */
    uint32_t codepoint;   /* the character typed, or 0 */
};

#define INPUT_EVENT_MOUSE 1u         /* input_mouse_event.kind */

/* One mouse report as its driver sent it (input.idl `mouse`): movement is
 * relative and in the mouse's own counts, so a pointer position, its
 * speed and its limits are the client's business. A report lost to a full
 * channel loses movement only: each one carries every button's state. */
struct input_mouse_event {
    uint32_t kind;        /* INPUT_EVENT_MOUSE */
    int16_t  dx, dy;      /* counts moved since the last report: +x right, +y down */
    int8_t   wheel;       /* notches turned: + away from the user */
    uint8_t  buttons;     /* INPUT_BTN_* held down now */
    uint16_t reserved;    /* 0 */
};

#define INPUT_WANT_MOUSE (1u << 0)   /* input_want.events: mouse reports */

struct input_want {
    uint32_t events;      /* INPUT_WANT_*: replaces what was asked for before */
    uint32_t reserved;    /* 0 */
};

/* The kinds are told apart by size. */
_Static_assert(sizeof(struct input_key_event) == 8, "input_key_event layout");
_Static_assert(sizeof(struct input_mouse_event) == 12, "input_mouse_event layout");
_Static_assert(offsetof(struct input_mouse_event, buttons) == 9, "input_mouse_event layout");
_Static_assert(sizeof(struct input_want) == 8, "input_want layout");
