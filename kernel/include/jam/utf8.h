/* UTF-8 checks for the kernel log (lib/utf8.c): which bytes may go into
 * the log as they are, and how a bad run is cut into pieces that each
 * become one '?'.
 *
 * Well-formed is Unicode's Table 3-7: no overlong forms, no surrogates
 * (U+D800..U+DFFF), nothing past U+10FFFF. An ill-formed run is replaced
 * by Unicode's "maximal subpart" rule: a start of a valid sequence that is
 * cut short counts as one bad piece, and so does each byte that can never
 * start or continue a sequence (C0, C1, F5..FF, a stray continuation
 * byte). User space has its own copy of these rules (user/lib/utf8.c,
 * <utf8.h>): the kernel and user programs share no code. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The sequence at s (n > 0 bytes there): its length (1..4) if it is
 * well-formed, else minus the length of the bad piece to replace with one
 * '?' (1..3). A sequence that would be valid but runs past n is a bad
 * piece of what is there. *cp gets the code point of a good one. */
int utf8_seq(const uint8_t *s, size_t n, uint32_t *cp);

/* How long a sequence starting with byte b is meant to be: 1 for ASCII,
 * 2..4 for a lead byte that can start a well-formed one, 0 for a byte that
 * can't start one. */
unsigned utf8_lead_len(uint8_t b);

/* Code point cp is a control character: C0 (below U+0020), DEL or C1
 * (U+0080..U+009F; U+009B is a terminal's CSI). */
static inline bool utf8_is_control(uint32_t cp)
{
    return cp < 0x20 || (cp >= 0x7f && cp < 0xa0);
}
