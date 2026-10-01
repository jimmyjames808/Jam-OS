/* libfun: proportional text from the 8x16 console font, and the FPS counter (fun.h).
 *
 * Text is UTF-8. The glyphs: ASCII from the console's font (<font.h>),
 * U+00A0 .. U+017F (Latin-1 and Latin Extended-A: "Fünf", "JAŸ-Z", "¥")
 * from the same font's table here (font_latin.c, `make font`), and one
 * fallback box for any other code point and for each malformed byte, so a
 * name in another script shows one box per character, never one per byte.
 * Control characters draw as '?'. */
#include <font.h>
#include "internal.h"

/* ---- text ---------------------------------------------------------------------------- */

#define LATIN_FIRST 0xa0u
#define LATIN_N     224u                 /* U+00A0 .. U+017F */
#define G_LATIN     128u                 /* glyph number of U+00A0 */
#define G_BOX       (G_LATIN + LATIN_N)  /* the fallback */
#define NGLYPHS     (G_BOX + 1)

/* Per glyph: the first ink column and the advance at scale 1; and the
 * glyph at twice the size smoothed by scale2x (EPX), for scales >= 2. */
static struct { int8_t left; uint8_t adv; } gm[NGLYPHS];
static uint16_t big_glyph[NGLYPHS][32];
static bool text_ready;

/* The fallback: a box the height of a capital. */
static const uint8_t box_rows[16] = { 0, 0, 0, 0x7c, 0x44, 0x44, 0x44, 0x44, 0x44,
                                      0x44, 0x44, 0x7c, 0, 0, 0, 0 };

static const uint8_t *rows_of(unsigned g)
{
    if (g < G_LATIN)
        return font_8x16[g];
    if (g < G_BOX)
        return font_latin[g - G_LATIN];
    return box_rows;
}

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

/* The glyph for code point cp. */
static unsigned glyph_of(uint32_t cp)
{
    if (cp >= 32 && cp < 127)
        return cp;
    if (cp == 0xa0)
        return ' ';
    if (cp >= LATIN_FIRST && cp < LATIN_FIRST + LATIN_N)
        return G_LATIN + cp - LATIN_FIRST;
    if (cp < 0xa0)   /* C0 and C1 controls, DEL */
        return '?';
    return G_BOX;
}

static inline int gbit(unsigned g, int x, int y)
{
    if (x < 0 || x > 7 || y < 0 || y > 15)
        return 0;
    return rows_of(g)[y] >> (7 - x) & 1;
}

/* The columns glyph g's ink spans: *lo..*hi (8, -1 for a blank glyph). */
static void ink_span(unsigned g, int *lo, int *hi)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (gbit(g, x, y)) {
                *lo = x < *lo ? x : *lo;
                *hi = x > *hi ? x : *hi;
            }
}

/* scale2x: each pixel P becomes 4, a corner taking a neighbour's value
 * where two neighbours agree (rounds the diagonals). Into big_glyph[c]. */
static void scale2x(unsigned c)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++) {
            int p = gbit(c, x, y), a = gbit(c, x, y - 1);
            int b = gbit(c, x + 1, y), l = gbit(c, x - 1, y);
            int d = gbit(c, x, y + 1);
            int e0 = p, e1 = p, e2 = p, e3 = p;
            if (l == a && l != d && a != b)
                e0 = a;
            if (a == b && a != l && b != d)
                e1 = b;
            if (d == l && d != b && l != a)
                e2 = l;
            if (b == d && b != a && d != l)
                e3 = d;
            big_glyph[c][2 * y] |= (uint16_t)(e0 << (15 - 2 * x) | e1 << (14 - 2 * x));
            big_glyph[c][2 * y + 1] |= (uint16_t)(e2 << (15 - 2 * x) | e3 << (14 - 2 * x));
        }
}

static void text_init(void)
{
    int digit_w = 0;
    for (unsigned c = 32; c < NGLYPHS; c++) {
        if (c == 127)
            c = G_LATIN;
        int lo = 8, hi = -1;
        ink_span(c, &lo, &hi);
        if (hi < 0) {   /* space */
            gm[c].left = 0;
            gm[c].adv = 4;
        } else {
            gm[c].left = (int8_t)lo;
            gm[c].adv = (uint8_t)(hi - lo + 2);
        }
        if (c >= '0' && c <= '9' && hi - lo + 1 > digit_w)
            digit_w = hi - lo + 1;
        scale2x(c);
    }
    /* Digits: all as wide as the widest, centred (numbers don't wobble). */
    for (int c = '0'; c <= '9'; c++) {
        int ink = gm[c].adv - 1;
        gm[c].left = (int8_t)(gm[c].left - (digit_w - ink) / 2);
        gm[c].adv = (uint8_t)(digit_w + 1);
    }
    text_ready = true;
}

/* A row of a scale-1 glyph: bits (MSB leftmost) at x on row (width w). */
static void glyph_row1(uint32_t *row, int x, int w, uint8_t bits, uint32_t c)
{
    for (int i = 0; bits && i < 8; i++, bits <<= 1)
        if ((bits & 0x80) && x + i >= 0 && x + i < w)
            row[x + i] = c;
}

static void glyph(const struct surf *s, int x, int y, int scale, uint32_t c, unsigned ch)
{
    int left = gm[ch].left, w = (gm[ch].adv - 1) * scale;
    for (int j = 0; j < 16 * scale; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= s->h)
            continue;
        uint32_t *row = s->px + (uint64_t)yy * s->stride;
        if (scale == 1) {
            uint8_t bits = (uint8_t)(left >= 0 ? rows_of(ch)[j] << left
                                               : rows_of(ch)[j] >> -left);
            glyph_row1(row, x, s->w, bits, c);
            continue;
        }
        uint16_t bits = big_glyph[ch][j * 2 / scale];
        for (int i = 0; i < w; i++) {
            int sx = (2 * left) + i * 2 / scale;   /* column in the 16-wide glyph */
            if (sx >= 0 && sx < 16 && (bits >> (15 - sx) & 1) && x + i >= 0 && x + i < s->w)
                row[x + i] = c;
        }
    }
}

static int draw_text(const struct surf *s, int x, int y, int scale, uint32_t c, uint32_t alt,
                     bool shadow, const char *str)
{
    if (!text_ready)
        text_init();
    if (scale < 1)
        scale = 1;
    if (shadow) {   /* a darkened halo one step down-right */
        int o = scale > 1 ? scale / 2 + 1 : 1;
        int xx = x;
        for (const char *p = str; *p;) {
            uint32_t cp = utf8_next(&p);
            if (cp == '\a')
                continue;
            unsigned g = glyph_of(cp);
            glyph(s, xx + o, y + o, scale, 0x000000, g);
            xx += gm[g].adv * scale;
        }
    }
    bool use_alt = false;
    for (const char *p = str; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\a') {
            use_alt = !use_alt;
            continue;
        }
        unsigned g = glyph_of(cp);
        glyph(s, x, y, scale, use_alt ? alt : c, g);
        x += gm[g].adv * scale;
    }
    return x;
}

int text(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str)
{
    return draw_text(s, x, y, scale, c, c, false, str);
}

int text_shadow(const struct surf *s, int x, int y, int scale, uint32_t c, const char *str)
{
    return draw_text(s, x, y, scale, c, c, true, str);
}

int text2(const struct surf *s, int x, int y, int scale, uint32_t c, uint32_t alt, bool shadow,
          const char *str)
{
    return draw_text(s, x, y, scale, c, alt, shadow, str);
}

int textf(const struct surf *s, int x, int y, int scale, uint32_t c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return draw_text(s, x, y, scale, c, c, false, buf);
}

int text_width(int scale, const char *str)
{
    if (!text_ready)
        text_init();
    int w = 0;
    for (const char *p = str; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp != '\a')
            w += gm[glyph_of(cp)].adv;
    }
    return w * (scale < 1 ? 1 : scale);
}

int text_clip(const struct surf *s, int x, int y, int scale, uint32_t c, int max_w,
              const char *str)
{
    if (text_width(scale, str) <= max_w)
        return text(s, x, y, scale, c, str);
    /* As many whole characters as fit before "..."; not even the dots fit:
     * nothing (never past max_w). */
    int dots = text_width(scale, "..."), w = 0;
    if (dots > max_w)
        return x;
    const char *p = str, *end = str;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        int cw = cp == '\a' ? 0 : gm[glyph_of(cp)].adv * (scale < 1 ? 1 : scale);
        if (w + cw + dots > max_w)
            break;
        w += cw;
        end = p;
    }
    char buf[512];
    size_t n = (size_t)(end - str) < sizeof(buf) - 4 ? (size_t)(end - str) : sizeof(buf) - 4;
    memcpy(buf, str, n);
    memcpy(buf + n, "...", 4);
    return text(s, x, y, scale, c, buf);
}

void text_in(const struct surf *s, const struct rect *r, int scale, uint32_t c, const char *str)
{
    text(s, r->x + (r->w - text_width(scale, str)) / 2, r->y + (r->h - TEXT_H(scale)) / 2, scale,
         c, str);
}

void fps_frame(struct fps *f)
{
    uint64_t t = now();
    if (!f->t0)
        f->t0 = t;
    f->n++;
    if (t - f->t0 >= 500000000ull) {
        f->x10 = (uint32_t)((uint64_t)f->n * 10000000000ull / (t - f->t0));
        f->t0 = t;
        f->n = 0;
    }
}
