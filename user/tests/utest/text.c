/* utest: text that comes from somewhere else (libos <utf8.h>): well-formed
 * UTF-8, and how a bad run is cut into pieces, by the same rules as the
 * kernel's log (kernel/test/test_utf8.c checks that side). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <utf8.h>
#include "utest.h"

/* utf8_seq of the n bytes at s, and the code point it gave. */
static int seq(const char *s, size_t n, uint32_t *cp)
{
    *cp = 0xffffffffu;
    return utf8_seq((const uint8_t *)s, n, cp);
}

bool t_utf8_well_formed(void)
{
    uint32_t cp;
    CHECK_EQ(seq("\xc5\xb8-Z", 4, &cp), 2);     /* Y with a diaeresis */
    CHECK_EQ(cp, 0x178);
    CHECK_EQ(seq("\xe2\x82\xac", 3, &cp), 3);
    CHECK_EQ(cp, 0x20ac);
    CHECK_EQ(seq("\xf0\x9f\x98\x80", 4, &cp), 4);
    CHECK_EQ(cp, 0x1f600);
    static const struct { const char *s; size_t n; uint32_t cp; } edges[] = {
        { "\xc2\x80", 2, 0x80 },         { "\xdf\xbf", 2, 0x7ff },
        { "\xe0\xa0\x80", 3, 0x800 },     { "\xed\x9f\xbf", 3, 0xd7ff },
        { "\xee\x80\x80", 3, 0xe000 },    { "\xef\xbf\xbf", 3, 0xffff },
        { "\xf0\x90\x80\x80", 4, 0x10000 }, { "\xf4\x8f\xbf\xbf", 4, 0x10ffff },
    };
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        CHECK_EQ(seq(edges[i].s, edges[i].n, &cp), (int)edges[i].n);
        CHECK_EQ(cp, edges[i].cp);
    }
    CHECK(utf8_is_control(0x9b) && utf8_is_control(0x1b) && utf8_is_control(0x7f));
    CHECK(!utf8_is_control(0xa0) && !utf8_is_control(' '));
    return true;
}

bool t_utf8_bad_pieces(void)
{
    uint32_t cp;
    /* Overlong forms of '/' and of U+07FF, U+FFFF, U+10FFFF. */
    CHECK_EQ(seq("\xc0\xaf", 2, &cp), -1);
    CHECK_EQ(seq("\xe0\x80\xaf", 3, &cp), -1);
    CHECK_EQ(seq("\xe0\x9f\xbf", 3, &cp), -1);
    CHECK_EQ(seq("\xf0\x80\x80\xaf", 4, &cp), -1);
    CHECK_EQ(seq("\xf0\x8f\xbf\xbf", 4, &cp), -1);
    /* Surrogates (a CESU-8 pair too), past U+10FFFF, never-used bytes. */
    CHECK_EQ(seq("\xed\xa0\x80", 3, &cp), -1);
    CHECK_EQ(seq("\xed\xbf\xbf", 3, &cp), -1);
    CHECK_EQ(seq("\xed\xa0\xbd\xed\xb8\x80", 6, &cp), -1);
    CHECK_EQ(seq("\xf4\x90\x80\x80", 4, &cp), -1);
    CHECK_EQ(seq("\xf8\x88\x80\x80\x80", 5, &cp), -1);
    CHECK_EQ(seq("\xfe", 1, &cp), -1);
    /* Stray continuation bytes: one piece each. */
    CHECK_EQ(seq("\x80", 1, &cp), -1);
    CHECK_EQ(seq("\xbf\x80", 2, &cp), -1);
    /* A good start cut short is one piece, however far it got. */
    CHECK_EQ(seq("\xe2\x82", 2, &cp), -2);
    CHECK_EQ(seq("\xe2\x82(", 3, &cp), -2);
    CHECK_EQ(seq("\xf0\x9f\x98", 3, &cp), -3);
    CHECK_EQ(seq("\xc5 ", 2, &cp), -1);
    CHECK_EQ(utf8_lead_len(0xc1), 0);
    CHECK_EQ(utf8_lead_len(0xc2), 2);
    CHECK_EQ(utf8_lead_len(0xf4), 4);
    CHECK_EQ(utf8_lead_len(0xf5), 0);
    return true;
}
