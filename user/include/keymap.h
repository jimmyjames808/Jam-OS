/* Keyboard layouts (libos: user/lib/keymap.c, and the tables
 * tools/genkeymap.py makes from abi/keymap/: keymap_keys.c, keymap_us.c).
 *
 * A key press travels as numbers until a program wants a character:
 *   - hid reports the key's USB HID usage (Keyboard/Keypad page 0x07);
 *   - the compositor sends Wayland clients the key's Linux evdev code
 *     (keymap_evdev_of_hid), and once, a keymap in XKB's text format
 *     (struct keymap's xkb) that says what each code types;
 *   - a client turns code + modifiers into a keysym and a character:
 *     Jam OS's own programs with this table (keymap_decode), ported ones
 *     with xkbcommon over the XKB text. Both come from one layout file,
 *     so they agree.
 * The XKB text's first line names its layout ("// jamos-keymap us: ..."),
 * so a client of ours finds the matching table (keymap_of_xkb) without
 * parsing XKB.
 *
 * Levels: each key has one or two keysyms, picked by its type and the
 * modifiers (keymap_level): ONE_LEVEL always the first; TWO_LEVEL the
 * second with Shift; ALPHABETIC the second with Shift or Caps Lock but
 * not both; KEYPAD the second with Num Lock. Ctrl, Alt and Super pick no
 * level: Ctrl+C decodes to 'c', with the CTRL bit for the program to see
 * (as hid's events carry it today).
 *
 * The modifier bits are XKB's real modifiers in the generated keymap's
 * order, so the compositor sends these very bits in
 * wl_keyboard.modifiers: Shift, Lock (Caps Lock), Control, Mod1 (Alt),
 * Mod2 (Num Lock), Mod4 (Super). The tables are read-only data; nothing
 * here keeps state, so any thread may call these. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYMAP_HID_USAGES 0xe8u   /* usages the tables cover: 0 .. 0xe7 (the right GUI key) */
#define KEYMAP_CODES      256u    /* evdev codes the tables cover: 0 .. 255 */
#define KEYMAP_XKB_OFFSET 8u      /* an XKB key number is its evdev code + 8 */

#define KEYMAP_MOD_SHIFT (1u << 0)   /* Shift: either Shift key held */
#define KEYMAP_MOD_CAPS  (1u << 1)   /* Lock: Caps Lock on */
#define KEYMAP_MOD_CTRL  (1u << 2)   /* Control */
#define KEYMAP_MOD_ALT   (1u << 3)   /* Mod1: either Alt key */
#define KEYMAP_MOD_NUM   (1u << 4)   /* Mod2: Num Lock on */
#define KEYMAP_MOD_SUPER (1u << 6)   /* Mod4: either GUI (Windows) key */

/* How a key's modifiers pick its level (struct keymap_key's type). */
enum keymap_type {
    KEYMAP_NONE,         /* no key has this code in the layout */
    KEYMAP_ONE_LEVEL,
    KEYMAP_TWO_LEVEL,
    KEYMAP_ALPHABETIC,
    KEYMAP_KEYPAD,
};

/* One key of a layout, by evdev code. */
struct keymap_key {
    uint8_t  type;       /* enum keymap_type */
    bool     repeats;    /* it repeats while held (not a modifier or a lock key) */
    uint16_t reserved;   /* 0 */
    uint32_t sym[2];     /* the keysym of level 1 and 2 (X's numbers; 0: none) */
    uint32_t cp[2];      /* the character each level types (a code point; 0: none) */
};

/* A layout. */
struct keymap {
    const char              *name;          /* "us": the XKB text's first line names it */
    const char              *description;   /* "English (US)" */
    const struct keymap_key *keys;          /* KEYMAP_CODES of them, by evdev code */
    const char              *xkb;           /* the XKB keymap (xkb_v1), NUL-terminated */
    uint32_t                 xkb_size;      /* its bytes, the NUL included: what
                                               wl_keyboard.keymap's size says */
};

/* The layouts (generated: user/lib/keymap_<name>.c). */
extern const struct keymap keymap_us;

/* The generated key tables (user/lib/keymap_keys.c): evdev code by HID
 * usage, and the usage of each code (the first listed in keys.txt);
 * 0: none. Read them through the functions below. */
extern const uint8_t keymap_evdev_by_hid[KEYMAP_HID_USAGES];
extern const uint8_t keymap_hid_by_evdev[KEYMAP_CODES];

/* The evdev code of HID usage `usage` (Keyboard/Keypad page), 0 if it has
 * none. */
uint32_t keymap_evdev_of_hid(uint32_t usage);
/* The HID usage of evdev code `code`, 0 if none. */
uint32_t keymap_hid_of_evdev(uint32_t code);
/* KEYMAP_MOD_* for hid's modifier byte (INPUT_MOD_*: left and right
 * Ctrl, Shift, Alt, GUI); the lock bits are the caller's to add. */
uint32_t keymap_mods_of_hid(uint8_t hid_mods);

/* The layout called `name`, NULL if there is none. */
const struct keymap *keymap_by_name(const char *name);
/* The layout an XKB keymap's first line names ("// jamos-keymap <name>:"),
 * reading at most len bytes of text; NULL if the line isn't one of ours
 * or names no layout we have. */
const struct keymap *keymap_of_xkb(const char *text, size_t len);

/* The level (0 or 1) modifiers `mods` pick on a key of type `type`. */
unsigned keymap_level(uint32_t type, uint32_t mods);

/* What a key types. */
struct keymap_sym {
    uint32_t sym;   /* the keysym (0: none at that level) */
    uint32_t cp;    /* the character, 0 for none (an arrow, a modifier, F1) */
};

/* Evdev code `code` with modifiers `mods` (KEYMAP_MOD_*) on layout km:
 * true and *out, or false (*out untouched) if the layout has no such key. */
bool keymap_decode(const struct keymap *km, uint32_t code, uint32_t mods,
                   struct keymap_sym *out);
/* The other way, for text that arrives as characters (a serial terminal):
 * the key and modifiers that type cp on layout km, the key with the
 * lowest code that types it at either level, the keypad's digits left out
 * (they need Num Lock); level 1 needs no modifiers, level 2 Shift. True
 * and *code, *mods; false (nothing written) if no key types it. */
bool keymap_typing(const struct keymap *km, uint32_t cp, uint32_t *code, uint32_t *mods);
