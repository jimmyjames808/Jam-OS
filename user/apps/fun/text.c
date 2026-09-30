/* libfun: proportional text from the 8x16 console font, and the FPS counter (fun.h). */
#include <font.h>
#include "fun.h"

/* ---- text ---------------------------------------------------------------------------- */

/* Per glyph: the first ink column and the advance at scale 1; and the
 * glyph at twice the size smoothed by scale2x (EPX), for scales >= 2. */
static struct { int8_t left; uint8_t adv; } gm[128];
static uint16_t big_glyph[128][32];
static bool text_ready;

static inline int gbit(uint8_t c, int x, int y)
{
    if (x < 0 || x > 7 || y < 0 || y > 15)
        return 0;
    return font_8x16[c][y] >> (7 - x) & 1;
}

/* The columns glyph c's ink spans: *lo..*hi (8, -1 for a blank glyph). */
static void ink_span(int c, int *lo, int *hi)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (gbit((uint8_t)c, x, y)) {
                *lo = x < *lo ? x : *lo;
                *hi = x > *hi ? x : *hi;
            }
}

/* scale2x: each pixel P becomes 4, a corner taking a neighbour's value
 * where two neighbours agree (rounds the diagonals). Into big_glyph[c]. */
static void scale2x(int c)
{
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++) {
            int p = gbit((uint8_t)c, x, y), a = gbit((uint8_t)c, x, y - 1);
            int b = gbit((uint8_t)c, x + 1, y), l = gbit((uint8_t)c, x - 1, y);
            int d = gbit((uint8_t)c, x, y + 1);
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
    for (int c = 32; c < 127; c++) {
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

static void glyph(const struct surf *s, int x, int y, int scale, uint32_t c, uint8_t ch)
{
    int left = gm[ch].left, w = (gm[ch].adv - 1) * scale;
    for (int j = 0; j < 16 * scale; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= s->h)
            continue;
        uint32_t *row = s->px + (uint64_t)yy * s->stride;
        if (scale == 1) {
            uint8_t bits = (uint8_t)(left >= 0 ? font_8x16[ch][j] << left
                                               : font_8x16[ch][j] >> -left);
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
        for (const char *p = str; *p; p++) {
            uint8_t ch = (uint8_t)*p;
            if (ch == '\a' || ch < 32 || ch > 126)
                continue;
            glyph(s, xx + o, y + o, scale, 0x000000, ch);
            xx += gm[ch].adv * scale;
        }
    }
    bool use_alt = false;
    for (; *str; str++) {
        uint8_t ch = (uint8_t)*str;
        if (ch == '\a') {
            use_alt = !use_alt;
            continue;
        }
        if (ch < 32 || ch > 126)
            ch = '?';
        glyph(s, x, y, scale, use_alt ? alt : c, ch);
        x += gm[ch].adv * scale;
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
    for (; *str; str++) {
        uint8_t ch = (uint8_t)*str;
        if (ch == '\a')
            continue;
        if (ch < 32 || ch > 126)
            ch = '?';
        w += gm[ch].adv;
    }
    return w * (scale < 1 ? 1 : scale);
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
