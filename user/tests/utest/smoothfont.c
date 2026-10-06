/* utest: libfun's smooth text (<fun.h> "smooth text": font.c, fontdraw.c)
 * with the built-in Inter faces. Every expectation is computed here: the
 * glyphs' outlines and advances are numbers from the font's tables
 * (third_party/inter, read with fontTools; each cited where it is used),
 * turned into pixels by the rules fun.h documents.
 *
 * font_open        sizes and weights refused out of range; the metrics
 *                  at 13 and 26 pixels; how long a bake takes here
 * font_measure     widths of known strings at both sizes and weights
 *                  (kerning included), "" is 0, font_draw returns x plus
 *                  the width
 * font_pixels      'I', "II" (the second at a half-pixel position) and
 *                  '-' (fractional rows) drawn white on black: each
 *                  pixel's coverage is the glyph's rectangle's share of
 *                  it, exact where it is 0 or 255, within 1 elsewhere
 * font_blend       coloured text over a pattern is, pixel for pixel,
 *                  px_over(pattern, argb_pm(colour, coverage)) with the
 *                  coverage from white on black: exact
 * font_clip        clip rectangles crossing each edge of the text, empty,
 *                  negative and huge ones: inside them the pixels of an
 *                  unclipped drawing, outside them nothing changed; text
 *                  running off each edge of a surface inside a bigger
 *                  buffer: the surface's part right, the rest untouched
 * font_ellipsis    font_ellipsize against a search of every cut, at every
 *                  width and buffer size; never a space before the "…";
 *                  font_draw_in draws exactly the cut text, centred
 * font_threads     four threads drawing with the same fonts at once get
 *                  the one thread's pixels; a write into a font's memory
 *                  kills the writer (`utest font-write`): it is read-only */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <os.h>
#include "utest.h"

#define PATTERN(x, y) ((uint32_t)((x) * 0x1f0b07 + (y) * 0x070d1b) & 0xffffff)

/* x / 255 rounded, as fun.h's blend has it (for 0 <= x <= 255 * 255). */
static uint32_t d255(uint32_t x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* rgb at coverage a over the opaque pixel d, written out per channel. */
static uint32_t over_expect(uint32_t d, uint32_t rgb, uint32_t a)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t v = d255((rgb >> sh & 0xff) * a) + d255((d >> sh & 0xff) * (255 - a));
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}

static void fill_pattern(const struct surf *s)
{
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            s->px[y * s->stride + x] = PATTERN(x, y);
}

static bool opened(enum font_weight w, int px, struct font **out)
{
    CHECK_ST(font_open(w, px, out), OK);
    return true;
}

bool t_font_open(void)
{
    struct font *f;
    CHECK_ST(font_open(FONT_REGULAR, FONT_PX_MIN - 1, &f), ERR_OUT_OF_RANGE);
    CHECK_ST(font_open(FONT_REGULAR, FONT_PX_MAX + 1, &f), ERR_OUT_OF_RANGE);
    CHECK_ST(font_open((enum font_weight)4, 13, &f), ERR_OUT_OF_RANGE);
    font_close(NULL);
    /* Inter: 2048 units to the em, ascender 1984, descender -494, no line
     * gap, capital height 1490 (hhea, 'H'). */
    static const struct { int px, asc, desc, line, cap; } m[] = {
        { 13, 13, 4, 16, 9 },    /* 12.59, 3.14, 15.73, 9.46 */
        { 26, 26, 7, 31, 19 },   /* 25.19, 6.27, 31.46, 18.92 */
    };
    for (unsigned i = 0; i < 2; i++) {
        for (int w = FONT_REGULAR; w <= FONT_MEDIUM; w++) {
            uint64_t t0 = now();
            if (!opened((enum font_weight)w, m[i].px, &f))
                return false;
            uint64_t us = (now() - t0) / NS_PER_US;
            const struct font_metrics *fm = font_metrics(f);
            CHECK_EQ(fm->px, m[i].px);
            CHECK_EQ(fm->ascent, m[i].asc);
            CHECK_EQ(fm->descent, m[i].desc);
            CHECK_EQ(fm->line_h, m[i].line);
            CHECK_EQ(fm->cap_h, m[i].cap);
            printf("utest: font: %s at %d px baked in %lu us\n", w ? "Medium" : "Regular", m[i].px,
                   (unsigned long)us);
            font_close(f);
        }
    }
    return true;
}

bool t_font_measure(void)
{
    /* Each glyph's advance and each pair's kerning in 1/256 pixels
     * (units * px * 256 / 2048, rounded), summed, rounded to pixels. From
     * hmtx and kern: "Terminal" 8305 units in Regular, 8444 in Medium;
     * "AV" 1413 + 1413 - 140 (Regular), 1452 + 1452 - 147 (Medium). */
    static const struct { int w, px; const char *s; int want; } k[] = {
        { FONT_REGULAR, 13, "Terminal", 53 },
        { FONT_REGULAR, 13, "AV", 17 },
        { FONT_REGULAR, 13, "A", 9 },
        { FONT_REGULAR, 13, "Wavy Tête", 64 },
        { FONT_REGULAR, 13, "jamjar — Jam OS’s music player", 196 },
        { FONT_REGULAR, 26, "Terminal", 105 },
        { FONT_REGULAR, 26, "AV", 34 },
        { FONT_MEDIUM, 13, "Terminal", 54 },
        { FONT_MEDIUM, 13, "AV", 18 },
        { FONT_MEDIUM, 13, "jamjar — Jam OS’s music player", 198 },
        { FONT_MEDIUM, 26, "Wavy Tête", 130 },
        { FONT_MEDIUM, 26, "…", 24 },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        struct font *f;
        if (!opened((enum font_weight)k[i].w, k[i].px, &f))
            return false;
        int got = font_width(f, k[i].s);
        if (got != k[i].want)
            FAIL("width of \"%s\" at %d px: %d, want %d", k[i].s, k[i].px, got, k[i].want);
        CHECK_EQ(font_width(f, ""), 0);
        static uint32_t px[64 * 32];
        struct surf s = { px, 64, 32, 64 };
        CHECK_EQ(font_draw(&s, NULL, f, -7, 20, 0xffffff, k[i].s), -7 + got);
        font_close(f);
    }
    return true;
}

/* A glyph's rectangle, in pixels from its pen (x right, y down from the
 * baseline). */
struct frect {
    double x0, y0, x1, y1;
};

/* The coverage (0..255) the rectangles give pixel i, j. */
static int coverage(const struct frect *r, int n, int i, int j)
{
    double sum = 0;
    for (int k = 0; k < n; k++) {
        double ox = (r[k].x1 < i + 1 ? r[k].x1 : i + 1) - (r[k].x0 > i ? r[k].x0 : i);
        double oy = (r[k].y1 < j + 1 ? r[k].y1 : j + 1) - (r[k].y0 > j ? r[k].y0 : j);
        if (ox > 0 && oy > 0)
            sum += ox * oy;
    }
    return (int)(sum * 255 + 0.5);
}

/* str drawn white on black with its pen at 4, 30, against rectangles r
 * (pen-relative) in a 48 x 40 surface. */
static bool glyph_pixels(const struct font *f, const char *str, const struct frect *r, int n)
{
    static uint32_t px[48 * 40];
    struct surf s = { px, 48, 40, 48 };
    memset(px, 0, sizeof(px));
    font_draw(&s, NULL, f, 4, 30, 0xffffff, str);
    for (int y = 0; y < s.h; y++) {
        for (int x = 0; x < s.w; x++) {
            uint32_t p = px[y * 48 + x], got = p & 0xff;
            int want = coverage(r, n, x - 4, y - 30);
            bool ok = p == got * 0x010101 &&
                      ((want == 0 || want == 255) ? (int)got == want
                                                  : (int)got >= want - 1 && (int)got <= want + 1);
            if (!ok)
                FAIL("\"%s\" at %d, %d: 0x%06x, want coverage %d", str, x - 4, y - 30, p, want);
        }
    }
    return true;
}

/* The pen's 1/256 pixels as fun.h places a glyph: whole pixels and the
 * nearest quarter. */
static double placed(int pen256)
{
    int q = (pen256 * 4 + 128) >> 8;
    return (q >> 2) + (q & 3) / 4.0;
}

bool t_font_pixels(void)
{
    struct font *f, *m;
    if (!opened(FONT_REGULAR, 13, &f) || !opened(FONT_MEDIUM, 26, &m))
        return false;
    double s = 13.0 / 2048;
    /* Regular 'I': the box 180..370 by 0..1490, advance 550; no I-I kerning. */
    struct frect i1 = { 180 * s, -1490 * s, 370 * s, 0 };
    double second = placed((550 * 13 * 256 + 1024) / 2048);   /* 893.75 -> 894: 3.5 px */
    CHECK(second == 3.5);
    struct frect ii[2] = { i1, { second + 180 * s, -1490 * s, second + 370 * s, 0 } };
    /* Regular '-': the box 144..798 by 553..719. */
    struct frect dash = { 144 * s, -719 * s, 798 * s, -553 * s };
    /* Medium 'I' at 26 px: the box 165..393 by 0..1490. */
    double t = 26.0 / 2048;
    struct frect im = { 165 * t, -1490 * t, 393 * t, 0 };
    bool ok = glyph_pixels(f, "I", &i1, 1) && glyph_pixels(f, "II", ii, 2) &&
              glyph_pixels(f, "-", &dash, 1) && glyph_pixels(m, "I", &im, 1);
    font_close(f);
    font_close(m);
    return ok;
}

/* b is coverage a (white on black) of ink over the pattern, pixel for
 * pixel; and enough of a is partial (the edges are smooth). */
static bool blend_matches(const uint32_t *a, const uint32_t *b, uint32_t ink)
{
    unsigned partial = 0;
    for (int i = 0; i < 160 * 40; i++) {
        int x = i % 160, y = i / 160;
        uint32_t c = a[i] & 0xff;
        CHECK_EQ(a[i], c * 0x010101);
        partial += c > 0 && c < 255;
        uint32_t want = c ? over_expect(PATTERN(x, y), ink, c) : PATTERN(x, y);
        if (b[i] != want)
            FAIL("pixel %d, %d: 0x%06x, want 0x%06x (coverage %u)", x, y, b[i], want, c);
    }
    CHECK(partial > 100);
    return true;
}

bool t_font_blend(void)
{
    static uint32_t a[160 * 40], b[160 * 40];
    struct surf sa = { a, 160, 40, 160 }, sb = { b, 160, 40, 160 };
    const uint32_t ink = 0xd4537e;
    for (int w = FONT_REGULAR; w <= FONT_MEDIUM; w++) {
        struct font *f;
        if (!opened((enum font_weight)w, 16, &f))
            return false;
        memset(a, 0, sizeof(a));
        fill_pattern(&sb);
        const char *str = "Wavy Tête, ½ “jam” …";
        font_draw(&sa, NULL, f, 3, 26, 0xffffff, str);
        font_draw(&sb, NULL, f, 3, 26, ink | 0xff000000u, str);   /* the top byte is ignored */
        font_close(f);
        if (!blend_matches(a, b, ink))
            return false;
    }
    return true;
}

/* ---- clipping ------------------------------------------------------------------------- */

#define CW        200
#define CH        48
#define CB        24                 /* the guard border round a surface in its buffer */
#define CBW       (CW + 2 * CB)
#define CBH       (CH + 2 * CB)
#define GUARDPX   0x00a5a5a5u
#define CLIP_TEXT "AVATAR, jamjar…"

/* One clip rectangle against the unclipped drawing ref. */
static bool clip_case(const struct font *f, const uint32_t *ref, const struct rect *clip)
{
    static uint32_t px[CW * CH];
    struct surf s = { px, CW, CH, CW };
    fill_pattern(&s);
    font_draw(&s, clip, f, 10, 30, 0xf0f0f0, CLIP_TEXT);
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            int64_t cx = clip->x, cy = clip->y;
            bool in = x >= cx && y >= cy && x < cx + clip->w && y < cy + clip->h;
            uint32_t want = in ? ref[y * CW + x] : PATTERN(x, y);
            if (px[y * CW + x] != want)
                FAIL("clip {%d,%d,%d,%d}: pixel %d, %d is 0x%06x, want 0x%06x", clip->x, clip->y,
                     clip->w, clip->h, x, y, px[y * CW + x], want);
        }
    }
    return true;
}

/* What edge_case's buffer must hold for text at x, y: the text drawn
 * unclipped over the pattern in a buffer as big, CB further in, with the
 * border round the surface put back to guard. */
static void edge_ref(const struct font *f, uint32_t *big, int x, int y)
{
    for (int by = 0; by < CBH; by++)
        for (int bx = 0; bx < CBW; bx++)
            big[by * CBW + bx] = PATTERN(bx - CB, by - CB);
    struct surf s = { big, CBW, CBH, CBW };
    font_draw(&s, NULL, f, x + CB, y + CB, 0xf0f0f0, CLIP_TEXT);
    for (int by = 0; by < CBH; by++) {
        for (int bx = 0; bx < CBW; bx++) {
            if (bx < CB || by < CB || bx >= CB + CW || by >= CB + CH)
                big[by * CBW + bx] = GUARDPX;
        }
    }
}

/* The text at x, y of a surface inside a guarded buffer, against big. */
static bool edge_case(const struct font *f, const uint32_t *big, int x, int y)
{
    static uint32_t buf[CBW * CBH];
    for (unsigned i = 0; i < sizeof(buf) / sizeof(buf[0]); i++)
        buf[i] = GUARDPX;
    struct surf s = { buf + CB * CBW + CB, CW, CH, CBW };
    fill_pattern(&s);
    font_draw(&s, NULL, f, x, y, 0xf0f0f0, CLIP_TEXT);
    for (int i = 0; i < CBW * CBH; i++) {
        if (buf[i] != big[i])
            FAIL("text at %d, %d: pixel %d, %d is 0x%06x, want 0x%06x", x, y, i % CBW - CB,
                 i / CBW - CB, buf[i], big[i]);
    }
    return true;
}

bool t_font_clip(void)
{
    struct font *f;
    if (!opened(FONT_MEDIUM, 20, &f))
        return false;
    static uint32_t ref[CW * CH], big[CBW * CBH];
    struct surf r = { ref, CW, CH, CW };
    fill_pattern(&r);
    int end = font_draw(&r, NULL, f, 10, 30, 0xf0f0f0, CLIP_TEXT);
    CHECK(end > 100 && end < CW);   /* the text is inside, with room round it */
    static const struct rect clips[] = {
        { 0, 0, CW, CH },           /* all */
        { 12, 0, CW, CH },          /* the left edge through the first glyph */
        { 0, 0, 60, CH },           /* the right edge through a glyph */
        { 0, 20, CW, CH },          /* the top through the capitals */
        { 0, 0, CW, 29 },           /* the bottom just above the baseline */
        { 0, 31, CW, 10 },          /* only the descenders' rows */
        { 33, 17, 7, 9 },           /* a small box inside */
        { -50, -50, 70, 70 },       /* from outside the surface */
        { 140, 40, 1000, 1000 },    /* to beyond it */
        { 20, 10, 0, 30 },          /* empty */
        { 20, 10, 30, -5 },         /* negative */
        { 200, 0, 50, CH },         /* right of the surface */
        { -0x40000000, -0x40000000, 0x7fffffff, 0x7fffffff },   /* huge */
    };
    bool ok = true;
    for (unsigned i = 0; ok && i < sizeof(clips) / sizeof(clips[0]); i++)
        ok = clip_case(f, ref, &clips[i]);
    /* Off the left, right, top and bottom edges, off a corner, and wholly
     * off each side. */
    static const int at[][2] = { { -40, 30 }, { 100, 30 }, { 10, 8 }, { 10, 50 }, { -60, 4 },
                                 { 120, 60 }, { -400, 30 }, { 10, -100 }, { 300, 30 },
                                 { 10, 200 } };
    for (unsigned i = 0; ok && i < sizeof(at) / sizeof(at[0]); i++) {
        edge_ref(f, big, at[i][0], at[i][1]);
        ok = edge_case(f, big, at[i][0], at[i][1]);
    }
    font_close(f);
    return ok;
}

/* ---- the ellipsis ---------------------------------------------------------------------- */

/* What font_ellipsize must give, by trying every cut: str if it fits in
 * max_w and n - 1 bytes; else the longest start ending at a code point
 * that isn't a space, with "…", that fits both; else "". */
static void cut_expect(const struct font *f, const char *str, int max_w, size_t n, char *out)
{
    size_t len = strlen(str);
    out[0] = 0;
    if (font_width(f, str) <= max_w && len < n) {
        memcpy(out, str, len + 1);
        return;
    }
    char cand[128];
    const char *p = str;
    for (;;) {
        size_t k = (size_t)(p - str);
        bool space = k && (str[k - 1] == ' ' || (k >= 2 && !memcmp(str + k - 2, "\xc2\xa0", 2)));
        if (!space && k + 3 < n) {
            memcpy(cand, str, k);
            memcpy(cand + k, "…", 4);
            if (font_width(f, cand) <= max_w)
                memcpy(out, cand, k + 4);
        }
        if (!*p)
            break;
        (void)utf8_next(&p);
    }
}

static bool ellipsis_cases(const struct font *f, const char *str)
{
    char got[128], want[128];
    int full = font_width(f, str);
    for (int w = -1; w <= full + 2; w++) {
        size_t len = font_ellipsize(f, str, w, got, sizeof(got));
        cut_expect(f, str, w, sizeof(got), want);
        if (strcmp(got, want) || len != strlen(got))
            FAIL("\"%s\" in %d px: \"%s\", want \"%s\"", str, w, got, want);
        CHECK(!strstr(got, " …"));
    }
    for (size_t n = 1; n <= strlen(str) + 5; n++) {
        size_t len = font_ellipsize(f, str, 10000, got, n);
        cut_expect(f, str, 10000, n, want);
        if (strcmp(got, want) || len >= n)
            FAIL("\"%s\" in %u bytes: \"%s\", want \"%s\"", str, (unsigned)n, got, want);
    }
    CHECK_EQ(font_ellipsize(f, str, 100, got, 0), 0);
    return true;
}

/* font_draw_in over r against font_draw of the cut text where fun.h says
 * it goes, clipped to r. */
static bool draw_in_case(const struct font *f, const char *str, const struct rect *r)
{
    static uint32_t a[120 * 40], b[120 * 40];
    struct surf sa = { a, 120, 40, 120 }, sb = { b, 120, 40, 120 };
    char cut[128];
    font_ellipsize(f, str, r->w, cut, sizeof(cut));
    int w = font_width(f, cut), base = r->y + (r->h + font_metrics(f)->cap_h) / 2;
    for (int align = FONT_LEFT; align <= FONT_CENTRE; align++) {
        fill_pattern(&sa);
        fill_pattern(&sb);
        CHECK_EQ(font_draw_in(&sa, r, f, 0xe6e9ec, (enum font_align)align, str), w);
        int x = align == FONT_CENTRE ? r->x + (r->w - w) / 2 : r->x;
        font_draw(&sb, r, f, x, base, 0xe6e9ec, cut);
        CHECK(!memcmp(a, b, sizeof(a)));
    }
    return true;
}

bool t_font_ellipsis(void)
{
    struct font *f;
    if (!opened(FONT_REGULAR, 13, &f))
        return false;
    static const char *strs[] = { "Terminal", "jamjar — Jam OS’s music player", "Größe café",
                                  "Jam OS is fun", "a\xff" "b\xe2\x80", "", "W" };
    bool ok = true;
    for (unsigned i = 0; ok && i < sizeof(strs) / sizeof(strs[0]); i++)
        ok = ellipsis_cases(f, strs[i]);
    /* A cut that would end in a space ends before it. */
    char got[64];
    font_ellipsize(f, "Jam OS is fun", font_width(f, "Jam OS i…") - 1, got, sizeof(got));
    if (ok && strcmp(got, "Jam OS…"))
        FAIL("\"Jam OS is fun\" cut to \"%s\", want \"Jam OS…\"", got);
    static const struct rect rs[] = { { 5, 3, 110, 28 }, { 5, 3, 60, 28 }, { -20, 10, 70, 16 },
                                      { 30, -10, 80, 28 }, { 0, 0, 8, 28 } };
    for (unsigned i = 0; ok && i < sizeof(rs) / sizeof(rs[0]); i++)
        ok = draw_in_case(f, "jamjar — Jam OS’s music player", &rs[i]);
    font_close(f);
    return ok;
}

/* ---- threads --------------------------------------------------------------------------- */

#define FT_THREADS 4
#define FT_ROUNDS  40
#define FT_W       200
#define FT_H       30

static const char *const ft_text[3] = { "jamjar — Jam OS’s music player", "Terminal",
                                        "sysmon: 8 CPUs, 2.0 GiB, too long to fit here at all" };
static struct font *ft_font[2];
static uint32_t ft_ref[2][3][FT_W * FT_H];
static uint32_t ft_bad;

/* Picture k in font i into s. */
static void ft_draw(const struct surf *s, int i, int k)
{
    fill_pattern(s);
    font_draw_in(s, &(struct rect){ 4, 0, FT_W - 8, FT_H }, ft_font[i], i ? 0xe6e9ec : 0x7f8892,
                 FONT_CENTRE, ft_text[k]);
}

static void ft_worker(void *arg)
{
    static uint32_t px[FT_THREADS][FT_W * FT_H];
    uint32_t *mine = px[(uintptr_t)arg];
    struct surf s = { mine, FT_W, FT_H, FT_W };
    for (int r = 0; r < FT_ROUNDS; r++) {
        for (int j = 0; j < 6; j++) {   /* each text in each font */
            ft_draw(&s, j / 3, j % 3);
            if (memcmp(mine, ft_ref[j / 3][j % 3], sizeof(ft_ref[0][0])))
                __atomic_add_fetch(&ft_bad, 1, __ATOMIC_RELAXED);
        }
    }
}

bool t_font_threads(void)
{
    if (!opened(FONT_REGULAR, 13, &ft_font[0]) || !opened(FONT_MEDIUM, 13, &ft_font[1]))
        return false;
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < 3; k++) {
            struct surf s = { ft_ref[i][k], FT_W, FT_H, FT_W };
            ft_draw(&s, i, k);
        }
    }
    static uint8_t stacks[FT_THREADS][16384] __attribute__((aligned(64)));
    handle_t th[FT_THREADS];
    __atomic_store_n(&ft_bad, 0, __ATOMIC_RELAXED);
    for (uintptr_t t = 0; t < FT_THREADS; t++)
        CHECK_ST(thread_spawn("font", ft_worker, (void *)t, stacks[t], sizeof(stacks[t]), &th[t]),
                 OK);
    if (!wait_threads(th, FT_THREADS))
        return false;
    font_close(ft_font[0]);
    font_close(ft_font[1]);
    CHECK_EQ(__atomic_load_n(&ft_bad, __ATOMIC_RELAXED), 0);
    struct process_info info;
    if (!run_child("font-write", 0, 0, &info))
        return false;
    CHECK(info.killed);
    return true;
}

/* "utest font-write": open a font and write into it. Its memory is
 * read-only, so the write faults and the kernel kills us. */
int font_write_child(void)
{
    struct font *f;
    if (font_open(FONT_REGULAR, 13, &f) != OK)
        return 2;
    *(volatile uint8_t *)f = 1;
    return 0;
}
