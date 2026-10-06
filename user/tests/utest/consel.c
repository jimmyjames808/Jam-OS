/* utest: copy and paste in a terminal window, the pure parts
 * (user/services/console/select.c, wlinput.c and cellpaint.c, linked in).
 *
 * t_consel_drag: the cell under a pixel (the padding and the edges
 * clamped); a drag across lines, forwards and backwards, its cells and its
 * text (trailing blanks dropped, lines joined by newlines, UTF-8); a press
 * alone selects nothing until the drag reaches another cell.
 * t_consel_word_line: a double click takes a word (paths and options are
 * words; blanks and other punctuation are runs of their own), a drag after
 * it goes on by words; a triple click takes the line.
 * t_consel_scrollback: a range over lines far back in the scrollback, one
 * of them no longer kept (skipped in the text, its newline kept); a cut at
 * the buffer's size; the clicks counted (within 500 ms on one cell, a
 * fourth starting again).
 * t_consel_keys: Super+C and Ctrl+Shift+C copy, Super+V and Ctrl+Shift+V
 * paste, Ctrl+C and other chords don't; which keys clear the selection;
 * pasted text as keys (CR LF one newline, tabs kept, ESC and other control
 * characters, C1 controls and bad UTF-8 dropped).
 * t_consel_tint: a selected cell's background is blackcurrant at 45% over
 * its own, its text unchanged; the cursor wins. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "console.h"
#include "utest.h"

static const struct cell_look look9 = { 9, 21, 16, NULL, NULL };

#define TCOLS  20
#define TLINES 8
/* The text: lines 100 .. 107 (as if deep in the scrollback), 103 not kept. */
static struct cell text_cells[TLINES][TCOLS];
#define FIRST 100

static const struct cell *tline(void *ctx, int64_t i)
{
    (void)ctx;
    if (i < FIRST || i >= FIRST + TLINES || i == FIRST + 3)
        return NULL;
    return text_cells[i - FIRST];
}

static void set_line(int64_t i, const char *s)
{
    struct cell *l = text_cells[i - FIRST];
    for (unsigned c = 0; c < TCOLS; c++)
        l[c] = (struct cell){ ' ', ATTR(C_WHITE, C_BLACK), 0 };
    for (unsigned c = 0; c < TCOLS && s[c]; c++)
        l[c].ch = (uint16_t)(uint8_t)s[c];
}

static void text_init_test(void)
{
    set_line(100, "jam:/> ls -l /data");
    set_line(101, "music  notes.txt");
    set_line(102, "a  (x) b");
    set_line(104, "after the gap");
    set_line(105, "");
    set_line(106, "x");
    set_line(107, "end");
    text_cells[1][0].ch = (uint16_t)(G_LATIN + 0xe9 - FONT_LATIN_FIRST);   /* "e acute" */
}

static const struct sel_text text = { tline, NULL, TCOLS };

static bool copied(const struct selection *s, const char *want)
{
    char buf[256];
    size_t n = sel_copy(s, &text, buf, sizeof(buf));
    if (n != strlen(want) || memcmp(buf, want, n))
        FAIL("copied \"%.*s\", want \"%s\"", (int)n, buf, want);
    return true;
}

bool t_consel_drag(void)
{
    text_init_test();
    uint32_t c, r;
    sel_cell_at(&look9, 80, 24, 0, 0, &c, &r);   /* the padding: the first cell */
    CHECK(c == 0 && r == 0);
    sel_cell_at(&look9, 80, 24, WIN_PAD + 9 * 3 + 8, WIN_PAD + 21 * 2, &c, &r);
    CHECK(c == 3 && r == 2);
    sel_cell_at(&look9, 80, 24, 5000, 5000, &c, &r);   /* past the grid: its last cell */
    CHECK(c == 79 && r == 23);
    struct selection s = { 0 };
    struct click_track k = { 0 };
    sel_press(&s, (struct sel_pt){ 100, 7 }, sel_click(&k, 1000, (struct sel_pt){ 100, 7 }));
    CHECK(!s.on);
    CHECK(!sel_drag(&s, (struct sel_pt){ 100, 7 }));   /* the same cell: still nothing */
    CHECK(sel_drag(&s, (struct sel_pt){ 101, 4 }));
    CHECK(s.on);
    struct sel_pt a, b;
    CHECK(sel_range(&s, &text, &a, &b));
    CHECK(a.line == 100 && a.col == 7 && b.line == 101 && b.col == 4);
    CHECK(sel_has(&a, &b, 100, 19) && sel_has(&a, &b, 101, 0) && !sel_has(&a, &b, 101, 5));
    CHECK(!sel_has(&a, &b, 100, 6));
    CHECK(copied(&s, "ls -l /data\n\xc3\xa9usic"));
    /* backwards: the same range */
    sel_press(&s, (struct sel_pt){ 101, 4 }, 1);
    CHECK(sel_drag(&s, (struct sel_pt){ 100, 7 }));
    CHECK(copied(&s, "ls -l /data\n\xc3\xa9usic"));
    return true;
}

bool t_consel_word_line(void)
{
    text_init_test();
    struct selection s = { 0 };
    sel_press(&s, (struct sel_pt){ 100, 13 }, 2);   /* in "/data" */
    CHECK(s.on);
    CHECK(copied(&s, "/data"));
    sel_press(&s, (struct sel_pt){ 100, 10 }, 2);   /* "-l" is a word */
    CHECK(copied(&s, "-l"));
    sel_press(&s, (struct sel_pt){ 100, 1 }, 2);    /* "jam:/>": ':' and '/' are word's, '>' not */
    CHECK(copied(&s, "jam:/"));
    sel_press(&s, (struct sel_pt){ 102, 3 }, 2);    /* "(" a run of its own */
    CHECK(copied(&s, "("));
    sel_press(&s, (struct sel_pt){ 102, 1 }, 2);    /* blanks: their run, copied as nothing */
    CHECK(copied(&s, ""));
    sel_press(&s, (struct sel_pt){ 101, 9 }, 2);    /* "notes.txt" */
    CHECK(copied(&s, "notes.txt"));
    CHECK(sel_drag(&s, (struct sel_pt){ 102, 7 }));   /* by words: to the end of "b" */
    CHECK(copied(&s, "notes.txt\na  (x) b"));
    CHECK(sel_drag(&s, (struct sel_pt){ 101, 1 }));   /* backwards: from the start of "music" */
    CHECK(copied(&s, "\xc3\xa9usic  notes.txt"));
    sel_press(&s, (struct sel_pt){ 104, 5 }, 3);    /* a line */
    CHECK(copied(&s, "after the gap"));
    CHECK(sel_drag(&s, (struct sel_pt){ 106, 0 }));
    CHECK(copied(&s, "after the gap\n\nx"));
    return true;
}

bool t_consel_scrollback(void)
{
    text_init_test();
    struct selection s = { 0 };
    sel_press(&s, (struct sel_pt){ 102, 5 }, 1);
    CHECK(sel_drag(&s, (struct sel_pt){ 104, 4 }));
    CHECK(copied(&s, ") b\n\nafter"));   /* 103 is gone: an empty line, its newline kept */
    char small[5];
    CHECK_EQ(sel_copy(&s, &text, small, sizeof(small)), 5);   /* cut at the buffer */
    CHECK(!memcmp(small, ") b\n\n", 5));
    sel_press(&s, (struct sel_pt){ 99, 0 }, 1);   /* from before what is kept */
    CHECK(sel_drag(&s, (struct sel_pt){ 100, 2 }));
    CHECK(copied(&s, "\njam"));
    /* clicks */
    struct click_track k = { 0 };
    struct sel_pt p = { 100, 3 }, q = { 100, 4 };
    CHECK_EQ(sel_click(&k, 1000, p), 1);
    CHECK_EQ(sel_click(&k, 1300, p), 2);
    CHECK_EQ(sel_click(&k, 1700, p), 3);
    CHECK_EQ(sel_click(&k, 1800, p), 1);    /* a fourth starts again */
    CHECK_EQ(sel_click(&k, 1900, q), 1);    /* another cell */
    CHECK_EQ(sel_click(&k, 2500, q), 1);    /* too late */
    CHECK_EQ(sel_click(&k, 2500 + 500, q), 2);   /* 500 ms is still a double click */
    return true;
}

static struct input_key_event key_of(uint16_t usage, uint8_t state, uint8_t mods)
{
    return (struct input_key_event){ usage, state, mods, 0 };
}

/* The keys pasting text types: their characters, at most max. */
static unsigned typed(const char *text, uint32_t *out, unsigned max)
{
    size_t at = 0;
    unsigned n = 0;
    struct input_key_event ev;
    while (n < max && paste_key(text, strlen(text), &at, &ev)) {
        if (ev.usage || ev.state != INPUT_KEY_DOWN || ev.mods)
            return 0;
        out[n++] = ev.codepoint;
    }
    return n;
}

bool t_consel_keys(void)
{
    static const struct { uint16_t u; uint8_t mods; enum clip_key want; } k[] = {
        { 0x06, INPUT_MOD_LGUI, CLIP_COPY },
        { 0x06, INPUT_MOD_RGUI | INPUT_MOD_LSHIFT, CLIP_COPY },
        { 0x06, INPUT_MOD_LCTRL | INPUT_MOD_LSHIFT, CLIP_COPY },
        { 0x19, INPUT_MOD_LGUI, CLIP_PASTE },
        { 0x19, INPUT_MOD_RCTRL | INPUT_MOD_RSHIFT, CLIP_PASTE },
        { 0x06, INPUT_MOD_LCTRL, CLIP_NONE },                    /* Ctrl+C: the interrupt */
        { 0x19, INPUT_MOD_LCTRL, CLIP_NONE },
        { 0x06, INPUT_MOD_LGUI | INPUT_MOD_LCTRL, CLIP_NONE },
        { 0x06, INPUT_MOD_LCTRL | INPUT_MOD_LSHIFT | INPUT_MOD_LALT, CLIP_NONE },
        { 0x06, 0, CLIP_NONE },
        { 0x04, INPUT_MOD_LGUI, CLIP_NONE },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        struct input_key_event ev = key_of(k[i].u, INPUT_KEY_DOWN, k[i].mods);
        if (clip_key_of(&ev) != k[i].want)
            FAIL("key %u: usage 0x%x mods 0x%x", i, k[i].u, k[i].mods);
    }
    struct input_key_event ev = key_of(0x04, INPUT_KEY_DOWN, 0);
    CHECK(key_clears_selection(&ev));
    ev = key_of(0x04, INPUT_KEY_UP, 0);
    CHECK(!key_clears_selection(&ev));
    ev = key_of(0xe1, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT);   /* Shift alone */
    CHECK(!key_clears_selection(&ev));
    ev = key_of(0x4b, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT);   /* the scrollback */
    CHECK(!key_clears_selection(&ev));
    uint32_t cp[16];
    CHECK_EQ(typed("a\r\nb\rc\nd\te", cp, 16), 9);
    static const uint32_t want[] = { 'a', '\n', 'b', '\n', 'c', '\n', 'd', '\t', 'e' };
    CHECK(!memcmp(cp, want, sizeof(want)));
    /* an ESC (a bracket's end among them), other controls, DEL, C1, bad UTF-8 */
    CHECK_EQ(typed("x\x1b[201~y\x03\x7f\xc2\x9b\xff\xc3\xa9", cp, 16), 8);
    static const uint32_t want2[] = { 'x', '[', '2', '0', '1', '~', 'y', 0xe9 };
    CHECK(!memcmp(cp, want2, sizeof(want2)));
    return true;
}

bool t_consel_tint(void)
{
    static uint32_t buf[16 * 20];
    struct cell_look l;
    cell_look_bitmap(&l);
    uint32_t bg = cell_palette[C_BLACK], fg = cell_palette[C_WHITE];
    uint32_t want = 0;
    for (unsigned sh = 0; sh < 24; sh += 8) {   /* 45% blackcurrant over bg, rounded */
        uint32_t b = bg >> sh & 0xff, t = SEL_TINT >> sh & 0xff;
        want |= ((b * (255 - SEL_TINT_A) + t * SEL_TINT_A + 127) / 255) << sh;
    }
    CHECK_EQ(cell_selected_bg(bg), want);
    CHECK(SEL_TINT_A * 100 / 255 == 45);
    struct cell c = { 'I', ATTR(C_WHITE, C_BLACK), 0 };
    cell_paint(buf, 16, 16, 20, &l, 0, 0, c, CELL_SELECTED);
    unsigned fgs = 0, bgs = 0;
    for (unsigned i = 0; i < 16 * 16; i++) {
        uint32_t p = buf[(i / 16) * 16 + i % 16];
        if (i % 16 >= GW)
            continue;
        fgs += p == fg;
        bgs += p == want;
        CHECK(p == fg || p == want);
    }
    CHECK(fgs > 0 && bgs > 0);
    cell_paint(buf, 16, 16, 20, &l, 0, 0, c, CELL_SELECTED | CELL_CURSOR);   /* the cursor wins */
    for (unsigned y = 0; y < GH; y++)
        for (unsigned x = 0; x < GW; x++)
            CHECK(buf[y * 16 + x] == fg || buf[y * 16 + x] == bg);
    return true;
}
