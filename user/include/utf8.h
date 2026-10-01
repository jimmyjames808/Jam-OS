/* UTF-8 checks (user/lib/utf8.c), for text that came from somewhere else:
 * the kernel log on the console, names from someone else's stick.
 *
 * Well-formed is Unicode's Table 3-7: no overlong forms, no surrogates
 * (U+D800..U+DFFF), nothing past U+10FFFF. An ill-formed run is cut by
 * Unicode's "maximal subpart" rule into bad pieces that each become one
 * replacement character: a start of a valid sequence that is cut short is
 * one piece, and so is each byte that can never start or continue a
 * sequence (C0, C1, F5..FF, a stray continuation byte). The kernel's log
 * applies the same rules (kernel/lib/utf8.c, <jam/utf8.h>): the kernel and
 * user programs share no code. (The apps library's utf8_next, <fun.h>,
 * counts each malformed byte instead: one box a byte in a drawn name.) */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The sequence at s (n > 0 bytes there): its length (1..4) if it is
 * well-formed, else minus the length of the bad piece (1..3). A sequence
 * that would be valid but runs past n is a bad piece of what is there.
 * *cp gets the code point of a good one. */
int utf8_seq(const uint8_t *s, size_t n, uint32_t *cp);

/* How long a sequence starting with byte b is meant to be: 1 for ASCII,
 * 2..4 for a lead byte that can start a well-formed one, 0 for a byte that
 * can't start one. */
unsigned utf8_lead_len(uint8_t b);

/* cp is a control character: C0 (below U+0020), DEL or C1
 * (U+0080..U+009F; U+009B is a terminal's CSI). */
static inline bool utf8_is_control(uint32_t cp)
{
    return cp < 0x20 || (cp >= 0x7f && cp < 0xa0);
}
