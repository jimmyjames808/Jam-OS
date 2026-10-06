/* The shell's side of a bracketed paste (sh_paste.c): telling the
 * markers a terminal puts around pasted text from typed keys, and what a
 * pasted key puts on the line. No state and no system calls: the line
 * editor (main.c) reads the keys, utest checks these (shpaste.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>

#define SH_PASTE_MARK_LEN 6u   /* ESC [ 2 0 0 ~ and ESC [ 2 0 1 ~ */

enum sh_paste_mark {
    SH_PASTE_NO,      /* not a marker: the keys are typed keys */
    SH_PASTE_MORE,    /* a marker's start so far: more keys say */
    SH_PASTE_BEGIN,   /* ESC [ 2 0 0 ~: pasted text follows */
    SH_PASTE_END,     /* ESC [ 2 0 1 ~: the pasted text is over */
};

/* Is this key Escape (the Escape key, or a terminal's ESC byte)? */
bool sh_paste_is_esc(const struct input_key_event *k);
/* What the keys k[0..n) (n at most SH_PASTE_MARK_LEN) are, as far as
 * they go. */
enum sh_paste_mark sh_paste_marker(const struct input_key_event *k, unsigned n);
/* The byte a pasted key puts on the line: printable ASCII as it is; a
 * newline, carriage return or tab a space (a pasted line break never
 * runs anything: the line runs when the user presses Enter); anything
 * else 0, dropped (the line holds ASCII only). */
char sh_paste_byte(const struct input_key_event *k);
