/* libfun: UTF-8 decoding for the text drawers (fun.h "utf8_next"), in a
 * file of its own so a program (or a host build) that only decodes text
 * links nothing else of text.c's.
 *
 * Malformed: a lead byte whose continuation bytes are missing, a stray
 * continuation byte, overlong forms, surrogates and anything past
 * U+10FFFF. Each such byte is one UTF8_BAD, and decoding goes on at the
 * next byte. */
#include "internal.h"

uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = p[0];
    if (c < 0x80) {
        *s += c != 0;
        return c;
    }
    unsigned n = c >= 0xf0 && c < 0xf5 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : 0;
    if (c >= 0xf5)
        n = 0;
    uint32_t cp = c & (0x3f >> n);
    for (unsigned i = 1; i <= n; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            n = 0;   /* cut short: the lead byte alone is the bad one */
            break;
        }
        cp = cp << 6 | (p[i] & 0x3f);
    }
    /* Overlong forms, surrogates and beyond U+10FFFF are malformed. */
    if (!n || (n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp < 0xe000))) ||
        (n == 3 && (cp < 0x10000 || cp > 0x10ffff))) {
        *s += 1;
        return UTF8_BAD;
    }
    *s += n + 1;
    return cp;
}
