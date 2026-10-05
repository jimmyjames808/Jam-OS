/* fontcheck: fontpreview's --check (tools/fontpreview.c says what it
 * checks), meant to run built with ASan and UBSan (build/host/fontcheck,
 * from `make check`): any read or write outside the font's block, the
 * surface or the string is a report and a failed run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fun.h>
#include "fontpreview.h"

#define BORDER 64           /* untouched pixels around the surface in its buffer */
#define SW     200
#define SH     60
#define GUARD  0x5a5a5au    /* the border's and the outside-the-clip colour */
#define ROUNDS 4000

static uint64_t seed = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(uint32_t n)
{
    return (uint32_t)(rng_next(&seed) >> 33) % n;
}

/* A random string: mostly Latin-1 text, with control bytes, stray
 * continuation bytes, cut sequences and other scripts among it. */
static void random_text(char *buf, size_t n)
{
    static const char *bits[] = { "a", "W", "AV", " ", "é", "…", "—", "ÿ", "\xe2\x80",
                                  "\x80", "\xff", "\x01", "\t", "日本", "\xf0\x9f\x98\x80",
                                  "\xc3", "To", "  ", "ij", "€" };
    size_t len = 0, want = rnd((uint32_t)n);
    buf[0] = 0;
    while (len < want) {
        const char *b = bits[rnd(sizeof(bits) / sizeof(bits[0]))];
        size_t bl = strlen(b);
        if (len + bl >= n)
            break;
        memcpy(buf + len, b, bl + 1);
        len += bl;
    }
}

/* The border round the surface, and the surface outside the clip, are
 * all GUARD. */
static bool untouched(const uint32_t *buf, int bw, const struct rect *clip)
{
    for (int y = 0; y < SH + 2 * BORDER; y++) {
        for (int x = 0; x < bw; x++) {
            int sx = x - BORDER, sy = y - BORDER;
            bool in = sx >= 0 && sy >= 0 && sx < SW && sy < SH && rect_has(clip, sx, sy);
            if (!in && buf[y * bw + x] != GUARD)
                return false;
        }
    }
    return true;
}

static bool check_draw(const struct font *f)
{
    int bw = SW + 2 * BORDER;
    static uint32_t buf[(SW + 2 * BORDER) * (SH + 2 * BORDER)];
    char text[64];
    for (int i = 0; i < ROUNDS; i++) {
        for (size_t p = 0; p < sizeof(buf) / sizeof(buf[0]); p++)
            buf[p] = GUARD;
        struct surf s = { buf + BORDER * bw + BORDER, SW, SH, bw };
        struct rect clip = { (int)rnd(SW + 80) - 40, (int)rnd(SH + 40) - 20, (int)rnd(SW + 40),
                             (int)rnd(SH + 20) };
        random_text(text, sizeof(text));
        int x = (int)rnd(SW + 300) - 200, y = (int)rnd(SH + 80) - 40;
        if (rnd(2))
            font_draw(&s, &clip, f, x, y, 0xffffff, text);
        else
            font_draw_in(&s, &clip, f, 0xffffff, rnd(2) ? FONT_CENTRE : FONT_LEFT, text);
        if (!untouched(buf, bw, &clip)) {
            fprintf(stderr, "fontcheck: drawn outside clip {%d,%d,%d,%d} at %d,%d\n", clip.x, clip.y,
                    clip.w, clip.h, x, y);
            return false;
        }
    }
    return true;
}

/* buf is a start of text followed by "…", or text itself. */
static bool is_cut_of(const char *buf, const char *text, size_t *start)
{
    size_t n = strlen(buf);
    if (!strcmp(buf, text)) {
        *start = n;
        return true;
    }
    if (n < 3 || strcmp(buf + n - 3, "…"))
        return n == 0;
    *start = n - 3;
    return !strncmp(buf, text, n - 3);
}

static bool check_ellipsize(const struct font *f)
{
    char text[64], out[80], longer[80];
    for (int i = 0; i < ROUNDS; i++) {
        random_text(text, sizeof(text));
        int max_w = (int)rnd(300) - 10;
        size_t n = rnd(sizeof(out)), start = 0;
        size_t len = font_ellipsize(f, text, max_w, out, n);
        if (!n)
            continue;
        bool ok = len < n && len == strlen(out) && font_width(f, out) <= (max_w > 0 ? max_w : 0) &&
                  is_cut_of(out, text, &start);
        /* Nothing at all: not even "…" fits. */
        if (ok && !len && text[0])
            ok = font_width(f, "…") > max_w || n < 4;
        /* The longest: one code point more (or the whole text) doesn't fit. */
        if (ok && len && start < strlen(text)) {
            const char *p = text + start;
            (void)utf8_next(&p);
            size_t more = (size_t)(p - text);
            memcpy(longer, text, more);
            bool whole = more == strlen(text);
            strcpy(longer + more, whole ? "" : "…");
            bool space = longer[more - 1] == ' ' || (more >= 2 && !memcmp(longer + more - 2, "\xc2\xa0", 2));
            ok = space || font_width(f, longer) > max_w || strlen(longer) >= n;
        }
        if (!ok) {
            fprintf(stderr, "fontcheck: font_ellipsize(\"%s\", %d, %zu) gave \"%s\"\n", text, max_w,
                    n, out);
            return false;
        }
    }
    return true;
}

int font_check(void)
{
    struct font *f[2];
    if (font_open(FONT_REGULAR, 13, &f[0]) != OK || font_open(FONT_MEDIUM, 26, &f[1]) != OK) {
        fprintf(stderr, "fontcheck: font_open failed\n");
        return 2;
    }
    struct font *g;
    bool ok = font_open(FONT_REGULAR, FONT_PX_MIN - 1, &g) == ERR_OUT_OF_RANGE &&
              font_open(FONT_REGULAR, FONT_PX_MAX + 1, &g) == ERR_OUT_OF_RANGE &&
              font_open((enum font_weight)7, 13, &g) == ERR_OUT_OF_RANGE;
    for (int i = 0; i < 2 && ok; i++)
        ok = check_draw(f[i]) && check_ellipsize(f[i]);
    font_close(f[0]);
    font_close(f[1]);
    printf("fontcheck: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
