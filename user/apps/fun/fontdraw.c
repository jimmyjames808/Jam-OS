/* libfun: the smooth text, measured, cut and drawn (fun.h "smooth text";
 * the fonts are baked by font.c).
 *
 * Everything here reads a struct font and writes only the surface's
 * pixels and its own locals: no allocation, no static state, so any
 * number of threads draw with one font at once (the compositor's
 * painting workers, each into its tile).
 *
 * One walk serves measuring, cutting and drawing (struct run): code point
 * by code point, the pen in 1/256 pixels, moved by the kerning between
 * each glyph and the one before it, then by the glyph's advance. A glyph
 * is drawn at the pen rounded to the nearest 1/FONT_PHASES pixel: the
 * whole pixel moves the glyph, the phase picks which of its baked copies.
 * A width is the pen at the end, rounded to whole pixels; since drawing
 * walks the same way, text drawn at x ends where x + font_width says. */
#include "internal.h"

/* U+2026 in UTF-8: what font_ellipsize writes. */
#define ELLIPSIS     "\xe2\x80\xa6"
#define ELLIPSIS_LEN 3

/* A walk through a string's glyphs. */
struct run {
    const char *p;     /* the next code point */
    const char *end;   /* where the string's part stops (a cut: before its end) */
    bool        ell;   /* the ellipsis comes after that part */
    int         prev;  /* the last slot (-1: none yet), for kerning */
    int64_t     pen;   /* 1/256 pixels from the start */
};

static struct run run_of(const char *str, const char *end, bool ell)
{
    return (struct run){ str, end, ell, -1, 0 };
}

/* The kerning between slots l and r: a binary search of the pairs. */
static int32_t kern_of(const struct font *f, int l, int r)
{
    uint32_t key = (uint32_t)(l * FONT_SLOTS + r), lo = 0, hi = f->nkern;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (f->kern[mid].pair < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < f->nkern && f->kern[lo].pair == key ? f->kern[lo].d : 0;
}

/* The next glyph's slot (-1 at the end), *at its pen (1/256 pixels), and
 * the pen past it. */
static int run_next(const struct font *f, struct run *r, int64_t *at)
{
    int slot;
    if (r->p < r->end && *r->p) {
        uint32_t cp = utf8_next(&r->p);
        slot = font_slot(cp);
    } else if (r->ell) {
        r->ell = false;
        slot = FONT_ELLIPSIS;
    } else {
        return -1;
    }
    if (r->prev >= 0)
        r->pen += kern_of(f, r->prev, slot);
    *at = r->pen;
    r->pen += f->adv[slot];
    r->prev = slot;
    return slot;
}

/* A pen in 1/256 pixels as whole pixels, rounded, kept within int. */
static int pen_px(int64_t pen)
{
    int64_t px = (pen + 128) >> 8;
    return px > INT32_MAX ? INT32_MAX : px < INT32_MIN ? INT32_MIN : (int)px;
}

int font_width(const struct font *f, const char *str)
{
    struct run r = run_of(str, str + strlen(str), false);
    int64_t at;
    while (run_next(f, &r, &at) >= 0)
        ;
    return pen_px(r.pen);
}

/* The part of str to draw in max_w pixels, at most max_bytes of it with
 * the ellipsis's bytes: *end and *ell for run_of, and the width. Every
 * start of str that ends at a code point other than a space is tried
 * with the ellipsis after it, while the pen alone is still inside
 * max_w (the ellipsis's advance and kerning only add); the longest that
 * fits wins. */
static int cut(const struct font *f, const char *str, int max_w, size_t max_bytes,
               const char **end, bool *ell)
{
    size_t len = strlen(str);
    int w = font_width(f, str);
    if (w <= max_w && len <= max_bytes) {
        *end = str + len;
        *ell = false;
        return w;
    }
    int64_t limit = (int64_t)max_w * 256 + 127;   /* a pen that rounds to max_w at most */
    int64_t tail = f->adv[FONT_ELLIPSIS], best_w = -1, at = 0;
    const char *best = str;
    struct run r = run_of(str, str + len, false);
    int slot = -1;
    do {
        bool space = slot == font_slot(' ') || slot == font_slot(0xa0);
        if (!space && (size_t)(r.p - str) + ELLIPSIS_LEN <= max_bytes) {
            int64_t k = slot >= 0 ? kern_of(f, slot, FONT_ELLIPSIS) : 0;
            if (r.pen + k + tail <= limit) {
                best = r.p;
                best_w = r.pen + k + tail;
            }
        }
        slot = run_next(f, &r, &at);
    } while (slot >= 0 && at <= limit);
    *end = best;
    *ell = best_w >= 0;
    return best_w >= 0 ? pen_px(best_w) : 0;
}

size_t font_ellipsize(const struct font *f, const char *str, int max_w, char *buf, size_t n)
{
    if (!n)
        return 0;
    const char *end;
    bool ell;
    cut(f, str, max_w, n - 1, &end, &ell);
    size_t len = (size_t)(end - str);
    memcpy(buf, str, len);
    if (ell) {
        memcpy(buf + len, ELLIPSIS, ELLIPSIS_LEN);
        len += ELLIPSIS_LEN;
    }
    buf[len] = 0;
    return len;
}

/* A clip box: x1 <= x < x2, y1 <= y < y2. */
struct clip {
    int64_t x1, y1, x2, y2;
};

/* r (NULL: all of s) within s. */
static struct clip clip_of(const struct surf *s, const struct rect *r)
{
    struct clip c = { 0, 0, s->w, s->h };
    if (r) {
        c.x1 = r->x > 0 ? r->x : 0;
        c.y1 = r->y > 0 ? r->y : 0;
        int64_t x2 = (int64_t)r->x + r->w, y2 = (int64_t)r->y + r->h;
        c.x2 = x2 < c.x2 ? x2 : c.x2;
        c.y2 = y2 < c.y2 ? y2 : c.y2;
    }
    return c;
}

/* One glyph's coverage in rgb, its pen at x on baseline y, inside c. */
static void draw_glyph(const struct surf *s, const struct clip *c, const struct font *f,
                       const struct font_glyph *g, int64_t x, int64_t y, uint32_t rgb)
{
    int64_t gx = x + g->x, gy = y + g->y;
    int64_t x1 = gx > c->x1 ? gx : c->x1, x2 = gx + g->w < c->x2 ? gx + g->w : c->x2;
    int64_t y1 = gy > c->y1 ? gy : c->y1, y2 = gy + g->h < c->y2 ? gy + g->h : c->y2;
    if (x1 >= x2 || y1 >= y2)
        return;
    for (int64_t yy = y1; yy < y2; yy++) {
        const uint8_t *cov = f->cov + g->off + (yy - gy) * g->w + (x1 - gx);
        uint32_t *d = s->px + yy * s->stride + x1;
        for (int64_t i = 0; i < x2 - x1; i++) {
            if (cov[i] == 255)
                d[i] = rgb;
            else if (cov[i])
                d[i] = px_over(d[i], argb_pm(rgb, cov[i]));
        }
    }
}

/* The run's glyphs from pen position x (whole pixels) on baseline y,
 * inside c; the x after them. Every glyph is walked (the pen must reach
 * the end for the x after), but only those that meet c draw. */
static int draw_run(const struct surf *s, const struct clip *c, const struct font *f,
                    struct run *r, int64_t x, int64_t y, uint32_t rgb)
{
    rgb &= 0xffffff;
    int64_t at;
    int slot;
    while ((slot = run_next(f, r, &at)) >= 0) {
        int64_t q = (at * FONT_PHASES + 128) >> 8;   /* the pen in 1/FONT_PHASES pixels */
        draw_glyph(s, c, f, &f->g[slot][q & (FONT_PHASES - 1)], x + (q >> 2), y, rgb);
    }
    return pen_px((int64_t)x * 256 + r->pen);
}
_Static_assert(FONT_PHASES == 4, "draw_run's q >> 2 and q & 3 are for four phases");

int font_draw(const struct surf *s, const struct rect *clip, const struct font *f, int x, int y,
              uint32_t rgb, const char *str)
{
    struct clip c = clip_of(s, clip);
    struct run r = run_of(str, str + strlen(str), false);
    return draw_run(s, &c, f, &r, x, y, rgb);
}

int font_draw_in(const struct surf *s, const struct rect *r, const struct font *f, uint32_t rgb,
                 enum font_align align, const char *str)
{
    const char *end;
    bool ell;
    int w = cut(f, str, r->w, SIZE_MAX, &end, &ell);
    if (end == str && !ell)
        return 0;
    int64_t x = r->x, y = r->y + ((int64_t)r->h + f->m.cap_h) / 2;
    if (align == FONT_CENTRE)
        x += ((int64_t)r->w - w) / 2;
    struct clip c = clip_of(s, r);
    struct run run = run_of(str, end, ell);
    draw_run(s, &c, f, &run, x, y, rgb);
    return w;
}
