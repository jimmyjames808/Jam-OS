/* Keys into the bytes a terminal sends (libos: user/lib/keybytes.c): a key
 * event of the console's (a HID usage, the modifiers, the character it
 * types) as the bytes xterm sends for it, for a program that reads its
 * terminal as bytes (the console's POSIX side). <termkeys.h> is the
 * inverse: what it decodes, keybytes encodes the same way, so a key
 * through both comes back as itself (a utest checks it).
 *
 *   text                       the character as UTF-8
 *   Enter, keypad Enter        CR (the line discipline's ICRNL makes it NL)
 *   Backspace                  DEL (0x7f); Tab: HT; Shift+Tab: ESC [ Z; Escape: ESC
 *   Ctrl+letter, Ctrl+@ [ \ ] ^ _ and space   the control byte (Ctrl+C: 3)
 *   arrows, Home, End          ESC [ A B C D H F (application cursor mode,
 *                              DECCKM, ESC [ ? 1 h: ESC O ...); with Shift,
 *                              Alt or Ctrl ESC [ 1 ; <m> <final>
 *   Insert, Delete, Page Up, Page Down   ESC [ 2 ~, 3 ~, 5 ~, 6 ~ (with
 *                              modifiers ESC [ <n> ; <m> ~)
 *   F1 to F4                   ESC O P Q R S (with modifiers ESC [ 1 ; <m> P ...)
 *   F5 to F12                  ESC [ 15 ~, 17, 18, 19, 20, 21, 23, 24 ~
 *   Alt with a character       ESC, then the character's bytes
 * where <m> is 1 + 1 for Shift + 2 for Alt + 4 for Ctrl (xterm's). A key
 * released, a modifier alone and a key with no bytes give none. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KEYBYTES_MAX 16   /* the most bytes one key gives */

/* The bytes of one key (usage: a HID usage or 0 for a character alone;
 * state: INPUT_KEY_*; mods: INPUT_MOD_*; cp: the character it types, 0 for
 * none) into out: how many (0: none). app_cursor: DECCKM is on. */
unsigned keybytes(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp, bool app_cursor,
                  uint8_t out[KEYBYTES_MAX]);
