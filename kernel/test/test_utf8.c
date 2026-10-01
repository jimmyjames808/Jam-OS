/* UTF-8 in the kernel log: the checks themselves (<jam/utf8.h>) and what
 * the log makes of a line that has good and bad text in it (klog.c). */
#include <stdbool.h>
#include <stdint.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/string.h>
#include <jam/utf8.h>

/* utf8_seq of the bytes in s (n of them): the result, and the code point. */
static int seq(const char *s, size_t n, uint32_t *cp)
{
    *cp = 0xffffffffu;
    return utf8_seq((const uint8_t *)s, n, cp);
}

KTEST(utf8_well_formed)
{
    uint32_t cp;
    KT_EQ(seq("A", 1, &cp), 1);
    KT_EQ(cp, 'A');
    KT_EQ(seq("\xc5\xb8-Z", 4, &cp), 2);              /* the Y with a diaeresis of JAY-Z */
    KT_EQ(cp, 0x178);
    KT_EQ(seq("\xe2\x82\xac", 3, &cp), 3);            /* the euro sign */
    KT_EQ(cp, 0x20ac);
    KT_EQ(seq("\xf0\x9f\x98\x80", 4, &cp), 4);        /* an emoji, past the BMP */
    KT_EQ(cp, 0x1f600);
    /* Each length's smallest and largest. */
    KT_EQ(seq("\xc2\x80", 2, &cp), 2);
    KT_EQ(cp, 0x80);
    KT_EQ(seq("\xdf\xbf", 2, &cp), 2);
    KT_EQ(cp, 0x7ff);
    KT_EQ(seq("\xe0\xa0\x80", 3, &cp), 3);
    KT_EQ(cp, 0x800);
    KT_EQ(seq("\xed\x9f\xbf", 3, &cp), 3);            /* just below the surrogates */
    KT_EQ(cp, 0xd7ff);
    KT_EQ(seq("\xef\xbf\xbf", 3, &cp), 3);
    KT_EQ(cp, 0xffff);
    KT_EQ(seq("\xf0\x90\x80\x80", 4, &cp), 4);
    KT_EQ(cp, 0x10000);
    KT_EQ(seq("\xf4\x8f\xbf\xbf", 4, &cp), 4);
    KT_EQ(cp, 0x10ffff);
    KT_EQ(utf8_lead_len('x'), 1u);
    KT_EQ(utf8_lead_len(0xc5), 2u);
    KT_EQ(utf8_lead_len(0xe2), 3u);
    KT_EQ(utf8_lead_len(0xf0), 4u);
    KT_EQ(utf8_lead_len(0x80), 0u);
    KT_EQ(utf8_lead_len(0xc1), 0u);
    KT_EQ(utf8_lead_len(0xf5), 0u);
}

KTEST(utf8_ill_formed)
{
    uint32_t cp;
    /* Overlong forms: C0 and C1 never start a sequence; E0 and F0 need a
     * second byte high enough. */
    KT_EQ(seq("\xc0\x80", 2, &cp), -1);
    KT_EQ(seq("\xc1\xbf", 2, &cp), -1);
    KT_EQ(seq("\xe0\x80\x80", 3, &cp), -1);
    KT_EQ(seq("\xe0\x9f\xbf", 3, &cp), -1);
    KT_EQ(seq("\xf0\x80\x80\x80", 4, &cp), -1);
    KT_EQ(seq("\xf0\x8f\xbf\xbf", 4, &cp), -1);
    /* Surrogates, and past U+10FFFF. */
    KT_EQ(seq("\xed\xa0\x80", 3, &cp), -1);
    KT_EQ(seq("\xed\xbf\xbf", 3, &cp), -1);
    KT_EQ(seq("\xf4\x90\x80\x80", 4, &cp), -1);
    KT_EQ(seq("\xf5\x80\x80\x80", 4, &cp), -1);
    KT_EQ(seq("\xff", 1, &cp), -1);
    /* A stray continuation byte. */
    KT_EQ(seq("\x80", 1, &cp), -1);
    KT_EQ(seq("\xbf\xbf", 2, &cp), -1);
    /* A good start cut short is one bad piece, however far it got. */
    KT_EQ(seq("\xe2\x82", 2, &cp), -2);
    KT_EQ(seq("\xe2\x82" "A", 3, &cp), -2);
    KT_EQ(seq("\xf0\x9f\x98", 3, &cp), -3);
    KT_EQ(seq("\xf0\x9f\x98" "A", 4, &cp), -3);
    KT_EQ(seq("\xc5", 1, &cp), -1);
    KT_EQ(seq("\xc5" "A", 2, &cp), -1);
}

/* What the log made of `line`: logged with a marker, read back from the
 * ring (other CPUs may log meanwhile: the marker finds ours). */
static bool logged_as(const char *line, const char *want)
{
    static const char marker[] = "utf8 test line: ";
    uint64_t from = klog_head();
    kprintf("%s%s\n", marker, line);
    size_t cap = 4096, got;
    char *buf = kmalloc(cap);
    if (!buf)
        return false;
    uint64_t first;
    got = klog_read_at(from, buf, cap - 1, &first);
    buf[got] = '\0';
    bool ok = false;
    for (size_t i = 0; i + sizeof(marker) - 1 <= got && !ok; i++) {
        if (memcmp(buf + i, marker, sizeof(marker) - 1))
            continue;
        const char *p = buf + i + sizeof(marker) - 1;
        size_t n = strlen(want);
        ok = (size_t)(buf + got - p) > n && !memcmp(p, want, n) && p[n] == '\n';
    }
    if (!ok)
        kprintf("utf8: the log line is not \"%s\"\n", want);
    kfree(buf);
    return ok;
}

KTEST(utf8_klog_keeps_good_text)
{
    KT_ASSERT(logged_as("JA\xc5\xb8-Z \xe2\x82\xac \xf0\x9f\x98\x80 tab\tend",
                        "JA\xc5\xb8-Z \xe2\x82\xac \xf0\x9f\x98\x80 tab\tend"));
}

KTEST(utf8_klog_replaces_bad_text)
{
    /* Escapes and C1's CSI: one '?' each, the rest of the escape as text. */
    KT_ASSERT(logged_as("a\x1b[31mb\xc2\x9b" "2Jc\x7f", "a?[31mb?2Jc?"));
    /* Overlong, surrogate, cut short, stray: one '?' per bad piece. */
    KT_ASSERT(logged_as("\xc0\x80|\xed\xa0\x80|\xe2\x82" "A|\x80\x80|\xf0\x9f\x98",
                        "??|???|?A|??|?"));
}

/* What the log holds after `marker` (up to its newline) equals want. */
static bool log_has(uint64_t from, const char *marker, const char *want)
{
    size_t cap = 8192, got;
    char *buf = kmalloc(cap);
    if (!buf)
        return false;
    uint64_t first;
    got = klog_read_at(from, buf, cap - 1, &first);
    buf[got] = '\0';
    size_t m = strlen(marker), n = strlen(want);
    bool ok = false;
    for (size_t i = 0; i + m <= got && !ok; i++)
        ok = !memcmp(buf + i, marker, m) && got - i - m > n && !memcmp(buf + i + m, want, n) &&
             buf[i + m + n] == '\n';
    kfree(buf);
    return ok;
}

/* A program's lines (debug_write) keep their UTF-8: a character written
 * in two calls is one character, and a line too long for the process's
 * line buffer (199 bytes: OUT_LINE in process.c) is split before a
 * character that doesn't fit, not through it. */
KTEST(utf8_debug_write_keeps_characters)
{
    struct job *j = kt_fresh_job();
    struct process *p;
    KT_EQ(process_create(j, "u8test", &p), OK);
    uint64_t from = klog_head();
    process_debug_write(p, "one JA\xc5", 7, false);
    process_debug_write(p, "\xb8-Z \x1b[1m\n", 9, false);
    static char longline[256];
    memset(longline, 'x', 198);
    memcpy(longline + 198, "\xc5\xb8 two\n", 7);
    process_debug_write(p, longline, 205, false);
    KT_ASSERT(log_has(from, "[u8test] one ", "JA\xc5\xb8-Z ?[1m"));
    longline[198] = '\0';
    KT_ASSERT(log_has(from, "[u8test] ", longline));
    KT_ASSERT(log_has(from, "[u8test] \xc5\xb8", " two"));
    process_kill(p, PROCESS_KILLED_CODE, true);
    kobject_unref(process_kobject(p));
    kt_job_is_empty(j);
    job_unref(j);
}
