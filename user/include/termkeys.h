/* Keys from a terminal's bytes (libos: user/lib/termkeys.c): what a serial
 * terminal sends (the `input` protocol's `text`: printable characters,
 * control characters and VT100 escape sequences) turned into key presses,
 * for every program that takes keys from a terminal (the console, the
 * compositor), so they read a terminal the same way.
 *
 * A key comes out as a USB HID usage (Keyboard/Keypad page) for the keys
 * a terminal names by a sequence or a control byte (arrows, Home, End,
 * Delete, Page Up/Down, Enter, Backspace, Tab, Escape), each with the
 * character the key types (Enter '\n', Backspace 0x08, Tab '\t', Escape
 * 0x1b; 0 for the others), or as usage 0 and the byte itself for anything
 * else: a printable character, or a control character (Ctrl+C is 3).
 *
 * Bytes: ESC [ <digits and ;> <final> and ESC O <final> are sequences
 * (A B C D arrows, H Home, F End; ~ after 1/7 Home, 4/8 End, 3 Delete, 5
 * Page Up, 6 Page Down; any other final is dropped); an ESC followed by
 * anything else is the Escape key and then that byte as itself; CR, LF
 * and CR LF are one Enter; 0x7f and 0x08 Backspace. The state between
 * bytes is in struct termkeys, one per terminal; nothing else is kept. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One key a terminal typed. */
struct termkey {
    uint16_t usage;   /* the HID usage, or 0: cp is a character as it came */
    uint32_t cp;      /* the character it types (0: none) */
};

/* A terminal's decoding state: zero it to start. */
struct termkeys {
    uint8_t esc;          /* 0: plain bytes; 1: after ESC; 2: in ESC [ or ESC O */
    uint8_t np;           /* parameter bytes so far */
    bool    last_cr;      /* the last byte was CR: an LF right after it is dropped */
    char    params[8];    /* the sequence's parameter bytes (digits and ';') */
};

/* Decode byte b: the keys it completes go to out (at most two: a lone
 * ESC's Escape, then b's own key). Returns how many. */
unsigned termkeys_byte(struct termkeys *t, uint8_t b, struct termkey out[2]);
