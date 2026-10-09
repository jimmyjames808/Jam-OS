/* A key as a console.open_keys channel carries it: keys.key's fields
 * (abi/idl/console.idl, the `keys` protocol: review W14 made it an IDL
 * event; the mouse reports and the client's wants that shared the
 * channel went with the full-screen console, M12's Q14), and through
 * <idl/input.h> the input protocol's constants they carry (INPUT_KEY_*,
 * INPUT_MOD_*). The console and its clients keep keys in this struct
 * (keys_read_event's handler fills one). */
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

_Static_assert(sizeof(struct input_key_event) == 8, "input_key_event layout");
