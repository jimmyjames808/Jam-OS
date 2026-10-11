/* The RESULTS box's keeping rules (report.c): the first 192 lines and the
 * newest 64 are kept, the lines between are lost and counted, and the
 * box says so. Run on a box of its own, not the real one. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/report.h>
#include <jam/string.h>

#define HEAD 192
#define TAIL 64

static bool has(const char *s, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; s[i]; i++)
        if (!memcmp(s + i, needle, m))
            return true;
    return false;
}

static void fill(struct report_box *b, unsigned n)
{
    char s[32];
    for (unsigned i = 0; i < n; i++) {
        ksnprintf(s, sizeof(s), "line %u", i);
        report_box_keep(b, s);
    }
}

static bool is(const struct report_box *b, unsigned i, const char *want)
{
    char s[240];
    report_box_line(b, i, s, sizeof(s));
    return !strcmp(s, want);
}

KTEST(report_box_keeps_all_that_fit)
{
    struct report_box *b = report_box_new();
    KT_ASSERT(b != NULL);
    fill(b, HEAD + TAIL);   /* exactly full: nothing lost, no note */
    KT_EQ(report_box_shown(b), HEAD + TAIL);
    KT_ASSERT(is(b, 0, "line 0"));
    KT_ASSERT(is(b, HEAD - 1, "line 191"));
    KT_ASSERT(is(b, HEAD, "line 192"));
    KT_ASSERT(is(b, HEAD + TAIL - 1, "line 255"));
    report_box_free(b);
}

KTEST(report_box_loses_the_middle_and_says_so)
{
    struct report_box *b = report_box_new();
    KT_ASSERT(b != NULL);
    fill(b, 1000);   /* 744 too many */
    KT_EQ(report_box_shown(b), HEAD + 1 + TAIL);
    KT_ASSERT(is(b, 0, "line 0"));
    KT_ASSERT(is(b, HEAD - 1, "line 191"));
    char s[240];
    report_box_line(b, HEAD, s, sizeof(s));
    KT_ASSERT(has(s, "744 result lines didn't fit"));
    KT_ASSERT(is(b, HEAD + 1, "line 936"));    /* the newest 64: 936 .. 999 */
    KT_ASSERT(is(b, HEAD + TAIL, "line 999"));
    report_box_free(b);
}

KTEST(report_box_one_over)
{
    struct report_box *b = report_box_new();
    KT_ASSERT(b != NULL);
    fill(b, HEAD + TAIL + 1);
    KT_EQ(report_box_shown(b), HEAD + 1 + TAIL);
    char s[240];
    report_box_line(b, HEAD, s, sizeof(s));
    KT_ASSERT(has(s, "1 result line didn't fit"));
    KT_ASSERT(is(b, HEAD + 1, "line 193"));
    KT_ASSERT(is(b, HEAD + TAIL, "line 256"));
    report_box_free(b);
}

KTEST(report_box_cuts_long_lines)
{
    struct report_box *b = report_box_new();
    KT_ASSERT(b != NULL);
    char longline[300];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    report_box_keep(b, longline);
    report_box_keep(b, "two\nlines");   /* stops at the newline */
    char s[240];
    report_box_line(b, 0, s, sizeof(s));
    KT_EQ(strlen(s), 119);
    KT_ASSERT(is(b, 1, "two"));
    report_box_free(b);
}
