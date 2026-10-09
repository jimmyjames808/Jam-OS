/* utest: the terminal's line discipline (<ldisc.h>) and keys into bytes
 * (<keybytes.h>), both pure: no process, no console.
 *
 * t_ldisc_table: byte sequences typed under termios settings, each with
 * the echo it must show and the records a reader must get (canonical
 * lines, erase, word erase, kill with and without ECHOKE, reprint, end of
 * file, ECHO off and ECHONL, ECHOCTL, a tab erased, CR and NL mapped,
 * ISIG and NOFLSH, VLNEXT, raw bytes joined, MAX_CANON, a UTF-8
 * character erased).
 *
 * t_ldisc_queue: output processing (ONLCR, OCRNL, OPOST off), canonical
 * mode turned off with a line half typed, records put back at the front
 * (ld_unread), the queue's limits.
 *
 * t_ldisc_read_rules: MIN and TIME, canonical mode (ld_read_deadline).
 *
 * t_keybytes: keys into bytes against xterm's table, and both round trips
 * with <termkeys.h>: a key into bytes into the same key, and a sequence
 * into a key into the same bytes. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/input.h>
#include <keybytes.h>
#include <ldisc.h>
#include <os.h>
#include <termkeys.h>
#include "utest.h"

static struct ldisc ld;
static char echoed[16384];
static size_t nechoed;

static void collect(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n && nechoed < sizeof(echoed) - 1; i++)
        echoed[nechoed++] = (char)p[i];
    echoed[nechoed] = '\0';
}

/* A fresh line discipline with the defaults, lflag's clear bits cleared
 * and set bits set (iflag likewise). */
static void fresh(uint32_t lclear, uint32_t lset, uint32_t iclear, uint32_t iset)
{
    ld_init(&ld, collect, NULL);
    struct ld_termios t = ld.t;
    t.lflag = (t.lflag & ~lclear) | lset;
    t.iflag = (t.iflag & ~iclear) | iset;
    ld_set(&ld, &t);
    nechoed = 0;
    echoed[0] = '\0';
}

/* Every record taken, each followed by '|' ("|" alone: one empty record). */
static void records(char *out, size_t cap)
{
    static uint8_t rec[LD_REC_MAX];
    size_t k = 0;
    for (long n; (n = ld_peek(&ld, rec, sizeof(rec))) >= 0; ld_pop(&ld)) {
        for (long i = 0; i < n && k + 2 < cap; i++)
            out[k++] = (char)rec[i];
        if (k + 1 < cap)
            out[k++] = '|';
    }
    out[k] = '\0';
}

/* Type s; the last ld_input's result. */
static enum ld_sig type(const char *s)
{
    enum ld_sig sig = LD_SIG_NONE;
    for (; *s; s++)
        sig = ld_input(&ld, (uint8_t)*s);
    return sig;
}

struct ld_case {
    const char *what;
    uint32_t lclear, lset, iclear, iset;
    const char *typed;
    const char *echo;      /* what the terminal shows */
    const char *recs;      /* what readers get, records ended by '|' */
    enum ld_sig sig;       /* the last byte's */
};

#define BS3 "\b \b"
static const struct ld_case cases[] = {
    { "a line", 0, 0, 0, 0, "abc\r", "abc\r\n", "abc\n|", 0 },
    { "erase", 0, 0, 0, 0, "abx\x7f" "c\r", "abx" BS3 "c\r\n", "abc\n|", 0 },
    { "erase at the start", 0, 0, 0, 0, "\x7f\x7f" "a\r", "a\r\n", "a\n|", 0 },
    { "kill, ECHOKE", 0, 0, 0, 0, "abc\x15" "d\r", "abc" BS3 BS3 BS3 "d\r\n", "d\n|", 0 },
    { "kill, ECHOK", LD_ECHOKE, 0, 0, 0, "ab\x15", "ab^U\r\n", "", 0 },
    { "word erase", 0, 0, 0, 0, "foo bar\x17x\r", "foo bar" BS3 BS3 BS3 "x\r\n", "foo x\n|", 0 },
    { "word erase over spaces", 0, 0, 0, 0, "ab  \x17\r", "ab  " BS3 BS3 BS3 BS3 "\r\n",
      "\n|", 0 },
    { "reprint", 0, 0, 0, 0, "ab\x12", "ab^R\r\nab", "", 0 },
    { "end of file at the start", 0, 0, 0, 0, "\x04", "", "|", 0 },
    { "end of file in a line", 0, 0, 0, 0, "ab\x04", "ab", "ab|", 0 },
    { "two lines", 0, 0, 0, 0, "a\rb\r", "a\r\nb\r\n", "a\n|b\n|", 0 },
    { "ECHO off", LD_ECHO, 0, 0, 0, "secret\r", "", "secret\n|", 0 },
    { "ECHO off, ECHONL", LD_ECHO, LD_ECHONL, 0, 0, "pw\r", "\r\n", "pw\n|", 0 },
    { "ECHOCTL", 0, 0, 0, 0, "a\x01\r", "a^A\r\n", "a\x01\n|", 0 },
    { "ECHOCTL erased", 0, 0, 0, 0, "a\x01\x7f", "a^A" BS3 BS3, "", 0 },
    { "ECHOCTL off", LD_ECHOCTL, 0, 0, 0, "a\x01\r", "a\x01\r\n", "a\x01\n|", 0 },
    { "a tab erased", 0, 0, 0, 0, "ab\t\x7f", "ab\t\b\b\b\b\b\b", "", 0 },
    { "ICRNL off", 0, 0, LD_ICRNL, 0, "a\rb\n", "a^Mb\r\n", "a\rb\n|", 0 },
    { "IGNCR", 0, 0, 0, LD_IGNCR, "a\rb\r\n", "ab\r\n", "ab\n|", 0 },
    { "INLCR", LD_ICANON | LD_ECHO, 0, LD_ICRNL, LD_INLCR, "a\n\r", "", "a\r\r|", 0 },
    { "ISIG: Ctrl+C", 0, 0, 0, 0, "x\rab\x03", "x\r\nab^C", "", LD_SIG_INT },
    { "ISIG: Ctrl+\\", 0, 0, 0, 0, "ab\x1c", "ab^\\", "", LD_SIG_QUIT },
    { "ISIG: Ctrl+Z does nothing", 0, 0, 0, 0, "a\x1a" "b\r", "ab\r\n", "ab\n|", 0 },
    { "NOFLSH", 0, LD_NOFLSH, 0, 0, "x\ry\x03", "x\r\ny^C", "x\n|", LD_SIG_INT },
    { "ISIG off", LD_ISIG, 0, 0, 0, "a\x03\r", "a^C\r\n", "a\x03\n|", 0 },
    { "VLNEXT", 0, 0, 0, 0, "\x16\x03\r", "^\b^C\r\n", "\x03\n|", 0 },
    { "VLNEXT of erase", 0, 0, 0, 0, "a\x16\x7f\r", "a^\b^?\r\n", "a\x7f\n|", 0 },
    { "IEXTEN off", LD_IEXTEN, 0, 0, 0, "a\x17\x16\r", "a^W^V\r\n", "a\x17\x16\n|", 0 },
    { "raw", LD_ICANON | LD_ECHO, 0, 0, 0, "ab\x7f\x15", "", "ab\x7f\x15|", 0 },
    { "raw, echo", LD_ICANON, 0, 0, 0, "ab\r", "ab\r\n", "ab\n|", 0 },
    { "raw, ISIG", LD_ICANON, 0, 0, 0, "ab\x03", "ab^C", "", LD_SIG_INT },
    { "utf-8 erased", 0, 0, 0, 0, "x\xc3\xa9\x7f\r", "x\xc3\xa9" BS3 "\r\n", "x\n|", 0 },
};

static bool one_case(const struct ld_case *c)
{
    static char got[8192];
    fresh(c->lclear, c->lset, c->iclear, c->iset);
    enum ld_sig sig = type(c->typed);
    records(got, sizeof(got));
    bool ok = !strcmp(echoed, c->echo) && !strcmp(got, c->recs) && sig == c->sig;
    if (!ok)
        printf("utest: ldisc: \"%s\": echo \"%s\" records \"%s\" signal %d\n", c->what, echoed,
               got, (int)sig);
    return ok;
}

static bool max_canon(void)
{
    static char got[LD_MAX_CANON + 16];
    fresh(LD_ECHO, 0, 0, 0);
    for (unsigned i = 0; i < LD_MAX_CANON + 100; i++)
        ld_input(&ld, 'a');
    ld_input(&ld, '\r');
    records(got, sizeof(got));
    size_t n = strlen(got);
    CHECK_EQ(n, LD_MAX_CANON + 1);   /* 4095 characters, the newline and the '|' */
    CHECK(got[LD_MAX_CANON - 1] == '\n');
    return true;
}

bool t_ldisc_table(void)
{
    bool ok = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        ok &= one_case(&cases[i]);
    CHECK(ok);
    return max_canon();
}

/* ---- output, records, settings ----------------------------------------------------------- */

static bool output(void)
{
    uint8_t out[32];
    fresh(0, 0, 0, 0);
    size_t n = ld_output(&ld, (const uint8_t *)"a\nb", 3, out);
    CHECK(n == 4 && !memcmp(out, "a\r\nb", 4));
    CHECK_EQ(ld.col, 1u);
    struct ld_termios t = ld.t;
    t.oflag |= LD_OCRNL;
    ld_set(&ld, &t);
    n = ld_output(&ld, (const uint8_t *)"x\r", 2, out);
    CHECK(n == 2 && !memcmp(out, "x\n", 2));
    t.oflag &= ~LD_OPOST;
    ld_set(&ld, &t);
    n = ld_output(&ld, (const uint8_t *)"a\nb\r", 4, out);
    CHECK(n == 4 && !memcmp(out, "a\nb\r", 4));
    /* The column the echo starts from: a prompt of 4 columns, then a tab
     * (to column 8) erased with 4 backspaces. */
    fresh(0, 0, 0, 0);
    (void)ld_output(&ld, (const uint8_t *)"\n$$> ", 5, out);
    type("\t\x7f");
    CHECK(!strcmp(echoed, "\t\b\b\b\b"));
    return true;
}

static bool canon_off(void)
{
    char got[64];
    fresh(LD_ECHO, 0, 0, 0);
    type("half");
    struct ld_termios t = ld.t;
    t.lflag &= ~LD_ICANON;
    ld_set(&ld, &t);   /* the line so far becomes raw input */
    type("way");
    records(got, sizeof(got));
    CHECK(!strcmp(got, "halfway|"));
    /* A record peeked is closed: what is typed after it is a new one. */
    type("a");
    uint8_t rec[LD_REC_MAX];
    CHECK_EQ(ld_peek(&ld, rec, sizeof(rec)), 1);
    type("b");
    records(got, sizeof(got));
    CHECK(!strcmp(got, "a|b|"));
    return true;
}

static bool unread(void)
{
    char got[64];
    fresh(LD_ECHO, 0, 0, 0);
    type("third\r");
    CHECK(ld_unread(&ld, (const uint8_t *)"second\n", 7));
    CHECK(ld_unread(&ld, (const uint8_t *)"first\n", 6));
    CHECK(ld_unread(&ld, (const uint8_t *)"", 0));   /* an end of file put back */
    records(got, sizeof(got));
    CHECK(!strcmp(got, "|first\n|second\n|third\n|"));
    /* The queue's limits: records, then bytes. */
    for (unsigned i = 0; i < LD_RECS_MAX; i++)
        CHECK(ld_unread(&ld, (const uint8_t *)"x", 1));
    CHECK(!ld_unread(&ld, (const uint8_t *)"y", 1));
    CHECK_EQ(ld_ready(&ld), LD_RECS_MAX);
    ld_flush_input(&ld);
    CHECK_EQ(ld_ready(&ld), 0u);
    static uint8_t big[LD_REC_MAX];
    memset(big, 'b', sizeof(big));
    for (unsigned i = 0; i < LD_READY_MAX / LD_REC_MAX; i++)
        CHECK(ld_unread(&ld, big, sizeof(big)));
    CHECK(!ld_unread(&ld, big, 1));
    /* Typed input past the queue is dropped, not wrapped over it. */
    type("lost\r");
    CHECK_EQ(ld_ready(&ld), LD_READY_MAX / LD_REC_MAX);
    return true;
}

bool t_ldisc_queue(void)
{
    return output() && canon_off() && unread();
}

/* ---- read rules ------------------------------------------------------------------------- */

#define TENTH (100 * NS_PER_MS)

static uint64_t rule(bool canon, uint8_t min, uint8_t time, size_t got, size_t want)
{
    struct ld_termios t;
    ld_default(&t);
    if (!canon)
        t.lflag &= ~LD_ICANON;
    t.cc[LD_VMIN] = min;
    t.cc[LD_VTIME] = time;
    return ld_read_deadline(&t, got, want, 1000, 5000);   /* started at 1000, a byte at 5000 */
}

bool t_ldisc_read_rules(void)
{
    CHECK_EQ(rule(true, 1, 0, 0, 10), LD_NEVER);   /* canonical: a line */
    CHECK_EQ(rule(true, 1, 0, 3, 10), 0u);
    CHECK_EQ(rule(false, 0, 0, 0, 10), 0u);        /* MIN 0, TIME 0: a poll */
    CHECK_EQ(rule(false, 3, 0, 2, 10), LD_NEVER);  /* MIN 3: wait for the third */
    CHECK_EQ(rule(false, 3, 0, 3, 10), 0u);
    CHECK_EQ(rule(false, 3, 0, 2, 2), 0u);         /* MIN past what was asked for */
    CHECK_EQ(rule(false, 0, 5, 0, 10), 1000 + 5 * TENTH);   /* TIME from the start */
    CHECK_EQ(rule(false, 0, 5, 1, 10), 0u);
    CHECK_EQ(rule(false, 4, 5, 0, 10), LD_NEVER);  /* TIME starts at the first byte */
    CHECK_EQ(rule(false, 4, 5, 2, 10), 5000 + 5 * TENTH);   /* ... and after each */
    CHECK_EQ(rule(false, 4, 5, 4, 10), 0u);
    return true;
}

/* ---- keys into bytes -------------------------------------------------------------------- */

struct kb_case {
    uint16_t usage;
    uint8_t  mods;
    uint32_t cp;
    bool     app;
    const char *bytes;
};

#define CTRL  INPUT_MOD_LCTRL
#define SHIFT INPUT_MOD_LSHIFT
#define ALT   INPUT_MOD_LALT
static const struct kb_case kb[] = {
    { 0x04, 0, 'a', false, "a" },
    { 0x04, SHIFT, 'A', false, "A" },
    { 0, 0, 0xe9, false, "\xc3\xa9" },          /* a character alone: UTF-8 */
    { 0, 0, 0x20ac, false, "\xe2\x82\xac" },
    { 0x28, 0, '\n', false, "\r" },             /* Enter */
    { 0x58, 0, '\n', false, "\r" },             /* keypad Enter */
    { 0x2a, 0, 0x08, false, "\x7f" },           /* Backspace */
    { 0x2a, CTRL, 0x08, false, "\x08" },
    { 0x2b, 0, '\t', false, "\t" },
    { 0x2b, SHIFT, '\t', false, "\033[Z" },
    { 0x29, 0, 0x1b, false, "\033" },
    { 0x06, CTRL, 'c', false, "\x03" },         /* Ctrl+C */
    { 0x06, CTRL, 3, false, "\x03" },
    { 0, 0, 3, false, "\x03" },                 /* a serial terminal's */
    { 0x2c, CTRL, ' ', false, "" },             /* Ctrl+Space: NUL (checked below) */
    { 0x31, CTRL, '\\', false, "\x1c" },
    { 0x04, ALT, 'a', false, "\033a" },
    { 0x06, CTRL | ALT, 'c', false, "\033\x03" },
    { 0x52, 0, 0, false, "\033[A" },
    { 0x51, 0, 0, false, "\033[B" },
    { 0x4f, 0, 0, false, "\033[C" },
    { 0x50, 0, 0, false, "\033[D" },
    { 0x52, 0, 0, true, "\033OA" },
    { 0x50, CTRL, 0, false, "\033[1;5D" },
    { 0x52, SHIFT, 0, true, "\033[1;2A" },
    { 0x4a, 0, 0, false, "\033[H" },
    { 0x4d, 0, 0, false, "\033[F" },
    { 0x4a, 0, 0, true, "\033OH" },
    { 0x49, 0, 0, false, "\033[2~" },
    { 0x4c, 0, 0, false, "\033[3~" },
    { 0x4c, CTRL, 0, false, "\033[3;5~" },
    { 0x4b, 0, 0, false, "\033[5~" },
    { 0x4e, 0, 0, false, "\033[6~" },
    { 0x3a, 0, 0, false, "\033OP" },
    { 0x3d, 0, 0, false, "\033OS" },
    { 0x3a, SHIFT, 0, false, "\033[1;2P" },
    { 0x3e, 0, 0, false, "\033[15~" },
    { 0x45, 0, 0, false, "\033[24~" },
    { 0x44, ALT, 0, false, "\033[23;3~" },
    { 0xe0, CTRL, 0, false, "" },               /* Ctrl alone */
};

static bool kb_table(void)
{
    for (unsigned i = 0; i < sizeof(kb) / sizeof(kb[0]); i++) {
        uint8_t out[KEYBYTES_MAX];
        unsigned n = keybytes(kb[i].usage, INPUT_KEY_DOWN, kb[i].mods, kb[i].cp, kb[i].app, out);
        size_t want = strlen(kb[i].bytes);
        if (kb[i].usage == 0x2c && kb[i].mods == CTRL)
            want = 1;   /* the NUL byte */
        if (n != want || memcmp(out, kb[i].bytes, want)) {
            printf("utest: keybytes: case %u: %u bytes\n", i, n);
            CHECK(false);
        }
    }
    uint8_t out[KEYBYTES_MAX];
    CHECK_EQ(keybytes(0x04, INPUT_KEY_UP, 0, 'a', false, out), 0u);   /* a release */
    CHECK_EQ(keybytes(0x04, INPUT_KEY_REPEAT, 0, 'a', false, out), 1u);
    return true;
}

/* Keys <termkeys.h> knows: into bytes and back, the same key. */
static bool kb_round_trip(void)
{
    static const struct { uint16_t usage; uint32_t cp; uint16_t back_usage; uint32_t back_cp; } keys[] = {
        { 0x52, 0, 0x52, 0 }, { 0x51, 0, 0x51, 0 }, { 0x4f, 0, 0x4f, 0 }, { 0x50, 0, 0x50, 0 },
        { 0x4a, 0, 0x4a, 0 }, { 0x4d, 0, 0x4d, 0 }, { 0x4c, 0, 0x4c, 0 }, { 0x4b, 0, 0x4b, 0 },
        { 0x4e, 0, 0x4e, 0 }, { 0x28, '\n', 0x28, '\n' }, { 0x2a, 0x08, 0x2a, 0x08 },
        { 0x2b, '\t', 0x2b, '\t' }, { 0x04, 'a', 0, 'a' }, { 0, '~', 0, '~' },
        { 0x06, 3, 0, 3 },
    };
    for (unsigned app = 0; app < 2; app++)
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            uint8_t b[KEYBYTES_MAX];
            uint8_t mods = keys[i].cp == 3 ? CTRL : 0;
            unsigned n = keybytes(keys[i].usage, INPUT_KEY_DOWN, mods, keys[i].cp, app, b);
            struct termkeys t = { 0 };
            struct termkey k[2], last = { 0 };
            unsigned got = 0;
            for (unsigned j = 0; j < n; j++) {
                unsigned m = termkeys_byte(&t, b[j], k);
                got += m;
                if (m)
                    last = k[m - 1];
            }
            if (got != 1 || last.usage != keys[i].back_usage || last.cp != keys[i].back_cp) {
                printf("utest: keybytes round trip: key %u (app %u): %u keys, usage %#x cp %#x\n",
                       i, app, got, last.usage, last.cp);
                CHECK(false);
            }
        }
    /* And the other way: a terminal's sequence, a key, the same bytes. */
    static const char *const seqs[] = { "\033[A", "\033[D", "\033[H", "\033[F", "\033[3~",
                                        "\033[5~", "\033[6~", "\t", "x", "\x03" };
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        struct termkeys t = { 0 };
        struct termkey k[2];
        unsigned got = 0;
        for (const char *s = seqs[i]; *s; s++)
            got = termkeys_byte(&t, (uint8_t)*s, k);
        CHECK_EQ(got, 1u);
        uint8_t b[KEYBYTES_MAX];
        unsigned n = keybytes(k[0].usage, INPUT_KEY_DOWN, 0, k[0].cp, false, b);
        CHECK(n == strlen(seqs[i]) && !memcmp(b, seqs[i], n));
    }
    return true;
}

bool t_keybytes(void)
{
    return kb_table() && kb_round_trip();
}
