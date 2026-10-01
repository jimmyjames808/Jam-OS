/* UTF-8 checks (utf8.h): Unicode 15, section 3.9, Table 3-7 ("Well-Formed
 * UTF-8 Byte Sequences"). The second byte's allowed range depends on the
 * lead byte (that is what rules out overlong forms, surrogates and code
 * points past U+10FFFF); every later byte is 80..BF. */
#include <jam/utf8.h>

unsigned utf8_lead_len(uint8_t b)
{
    if (b < 0x80)
        return 1;
    if (b >= 0xc2 && b <= 0xdf)
        return 2;
    if (b >= 0xe0 && b <= 0xef)
        return 3;
    if (b >= 0xf0 && b <= 0xf4)
        return 4;
    return 0;   /* 80..BF continue a sequence; C0, C1 and F5..FF are never used */
}

/* The range the second byte of a sequence led by b must be in. */
static void second_range(uint8_t b, uint8_t *lo, uint8_t *hi)
{
    *lo = 0x80;
    *hi = 0xbf;
    if (b == 0xe0)
        *lo = 0xa0;   /* below: overlong */
    else if (b == 0xed)
        *hi = 0x9f;   /* above: a surrogate */
    else if (b == 0xf0)
        *lo = 0x90;   /* below: overlong */
    else if (b == 0xf4)
        *hi = 0x8f;   /* above: past U+10FFFF */
}

int utf8_seq(const uint8_t *s, size_t n, uint32_t *cp)
{
    unsigned len = utf8_lead_len(s[0]);
    if (len == 1) {
        *cp = s[0];
        return 1;
    }
    if (len == 0)
        return -1;
    uint8_t lo, hi;
    second_range(s[0], &lo, &hi);
    uint32_t c = s[0] & (0x7fu >> len);
    for (unsigned i = 1; i < len; i++) {
        if (i >= n || s[i] < lo || s[i] > hi)
            return -(int)i;   /* the valid start so far is one bad piece */
        c = c << 6 | (s[i] & 0x3fu);
        lo = 0x80;
        hi = 0xbf;
    }
    *cp = c;
    return (int)len;
}
