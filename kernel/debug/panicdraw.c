/* The panic screen's drawing (<jam/panicscreen.h>): the busy ring, the
 * baked Inter text, the 8x16 details panel and where each goes. Every
 * function draws on a struct panic_canvas (the framebuffer, or a test's
 * buffer) and touches nothing else: no lock, no allocation, no state.
 *
 * The ring is docs/design/cursors.svg's busy cursor, drawn at
 * PANIC_RING_PX for its 24 units (5/3 pixel a unit): a ring of radius 8
 * stroked 4.6 wide in the outline colour, 2.4 wide in white over it, and a
 * raspberry arc 2.4 wide and 14 long (round ends) over that, which turns.
 * The kernel has no floating point, so each pixel of its box is worked out
 * with integers in 1/256 pixels: the distance of the pixel's centre from
 * the ring's centre (an integer square root) gives how much of it each
 * stroke covers, as a one-pixel ramp across the stroke's edge (as libfun's
 * anti-aliased shapes do); the arc's ends are where the arc's two end
 * points are nearer than the ring. Each pixel is the strokes blended in
 * turn over PANIC_BG, so the box is written, never read.
 *
 * Text is laid out as libfun's fontdraw.c does it (the pen in 1/256
 * pixels, kerning, each glyph at the nearest quarter pixel) and blended
 * with the same integer arithmetic as libfun's alpha.c, over what the
 * canvas has: the result is libfun's to the pixel (ktest
 * panicscreen_text_matches_libfun). */
#include <stdint.h>
#include <jam/panicscreen.h>

extern const uint8_t font_8x16[128][16];   /* kernel/dev/font_8x16.c */

/* cursors.svg's measures in 1/256 pixels: u100 is a hundredth of a unit. */
#define UNITS(u100) ((int32_t)((u100) * 256 * PANIC_RING_PX / 24 / 100))
#define RING_R      UNITS(800)    /* the ring's radius */
#define RING_DARK   UNITS(230)    /* half the outline stroke's width (4.6) */
#define RING_LIGHT  UNITS(120)    /* half the white stroke's (2.4) */
#define ARC_HALF    UNITS(120)    /* half the arc's (2.4) */
/* The arc's length, 14 units on a radius of 8: 1.75 radians, in 65536ths
 * of a turn (1.75 / (2 pi) * 65536). */
#define ARC_TURN    18253u
#define RING_INK    0x262a35u     /* the outline */
#define RING_FILL   0xf6f3f8u     /* the white */
#define RING_ARC    0xd4537eu     /* raspberry */

/* ---- pixels -------------------------------------------------------------------------- */

static void put(const struct panic_canvas *c, int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || (uint32_t)x >= c->w || (uint32_t)y >= c->h)
        return;
    c->px[(uint64_t)y * c->stride + (uint32_t)x] = (rgb >> 16 & 0xff) << c->rs |
                                                    (rgb >> 8 & 0xff) << c->gs |
                                                    (rgb & 0xff) << c->bs;
}

static uint32_t get(const struct panic_canvas *c, int x, int y)
{
    uint32_t p = c->px[(uint64_t)y * c->stride + (uint32_t)x];
    return (p >> c->rs & 0xff) << 16 | (p >> c->gs & 0xff) << 8 | (p >> c->bs & 0xff);
}

/* x / 255 rounded, for 0 <= x <= 255 * 255 (libfun's alpha.c). */
static uint32_t div255(uint32_t x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* rgb at coverage a over the opaque pixel dst: libfun's
 * px_over(dst, argb_pm(rgb, a)). */
static uint32_t over(uint32_t dst, uint32_t rgb, uint32_t a)
{
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        uint32_t v = div255((rgb >> sh & 0xff) * a) + div255((dst >> sh & 0xff) * (255 - a));
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}

void panic_fill(const struct panic_canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            put(c, i, j, rgb);
}

/* ---- the ring ------------------------------------------------------------------------ */

/* sin of i/64 of a quarter turn, times 1 << 14, rounded. */
static const int16_t quarter_sin[65] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590,
    3981, 4370, 4756, 5139, 5520, 5897, 6270, 6639, 7005, 7366,
    7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702,
    11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395,
    13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286,
    15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261,
    16305, 16340, 16364, 16379, 16384,
};

int32_t panic_sin(uint32_t turn)
{
    turn &= 0xffff;
    uint32_t in = turn & 0x3fff, q = turn >> 14;
    if (q & 1)
        in = 0x4000 - in;   /* the second and fourth quarters run backwards */
    uint32_t i = in >> 8, frac = in & 0xff;
    int32_t v = quarter_sin[i];
    if (frac)
        v += ((quarter_sin[i + 1] - v) * (int32_t)frac + 128) >> 8;
    return q & 2 ? -v : v;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    for (; b; b >>= 2) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
    }
    return r;
}

/* How much of a pixel a stroke covers whose edge is `room` (1/256 pixels)
 * from the pixel's centre, inside positive: 0..255 over one pixel. */
static uint32_t cover(int32_t room)
{
    int32_t v = room + 128;
    v = v < 0 ? 0 : v > 256 ? 256 : v;
    return ((uint32_t)v * 255 + 128) >> 8;
}

static int32_t absi(int32_t v)
{
    return v < 0 ? -v : v;
}

/* The arc, for one frame: its ends' directions (1 << 14 long) and points. */
struct arc {
    int32_t u0x, u0y, u1x, u1y;   /* the start's and the end's direction */
    int32_t e0x, e0y, e1x, e1y;   /* their points on the ring, 1/256 pixels */
};

static void arc_at(struct arc *a, uint32_t turn)
{
    a->u0x = panic_sin(turn + 0x4000);
    a->u0y = panic_sin(turn);
    a->u1x = panic_sin(turn + ARC_TURN + 0x4000);
    a->u1y = panic_sin(turn + ARC_TURN);
    /* Divided, not shifted: rounding towards 0 either way keeps a quarter
     * turn's frame exactly the last one turned (ktest panicscreen_ring). */
    a->e0x = RING_R * a->u0x / 16384;
    a->e0y = RING_R * a->u0y / 16384;
    a->e1x = RING_R * a->u1x / 16384;
    a->e1y = RING_R * a->u1y / 16384;
}

/* The distance from (x, y) (1/256 pixels from the centre, d from it) to
 * the arc's centre line: across the ring inside its sector (shorter than
 * half a turn: on the clockwise side of the start, the other side of the
 * end, y pointing down), else to the nearer end. */
static int32_t arc_distance(const struct arc *a, int32_t x, int32_t y, int32_t d)
{
    int64_t after_start = (int64_t)a->u0x * y - (int64_t)a->u0y * x;
    int64_t before_end = (int64_t)x * a->u1y - (int64_t)y * a->u1x;
    if (after_start >= 0 && before_end >= 0)
        return absi(d - RING_R);
    int32_t dx0 = x - a->e0x, dy0 = y - a->e0y, dx1 = x - a->e1x, dy1 = y - a->e1y;
    uint32_t d0 = isqrt((uint32_t)(dx0 * dx0 + dy0 * dy0));
    uint32_t d1 = isqrt((uint32_t)(dx1 * dx1 + dy1 * dy1));
    return (int32_t)(d0 < d1 ? d0 : d1);
}

static uint32_t ring_pixel(const struct arc *a, int32_t x, int32_t y)
{
    int32_t d = (int32_t)isqrt((uint32_t)(x * x + y * y)), off = absi(d - RING_R);
    uint32_t dark = cover(RING_DARK - off);
    if (!dark)
        return PANIC_BG;
    uint32_t px = over(PANIC_BG, RING_INK, dark);
    px = over(px, RING_FILL, cover(RING_LIGHT - off));
    return over(px, RING_ARC, cover(ARC_HALF - arc_distance(a, x, y, d)));
}

void panic_ring(const struct panic_canvas *c, int cx, int cy, uint32_t turn)
{
    struct arc a;
    arc_at(&a, turn);
    const int half = PANIC_RING_PX / 2;
    for (int j = -half; j < half; j++)
        for (int i = -half; i < half; i++)
            put(c, cx + i, cy + j, ring_pixel(&a, i * 256 + 128, j * 256 + 128));
}

/* ---- text ---------------------------------------------------------------------------- */

static int slot(const struct panic_font *f, char ch)
{
    unsigned char u = (unsigned char)ch;
    return u < 128 ? f->slot_of[u] : -1;
}

/* The kerning between slots l and r: kern[] is sorted by (l, r). */
static int32_t kern(const struct panic_font *f, int l, int r)
{
    uint32_t key = (uint32_t)(l << 8 | r), lo = 0, hi = f->nkern;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, k = (uint32_t)(f->kern[mid].l << 8 | f->kern[mid].r);
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < f->nkern && (uint32_t)(f->kern[lo].l << 8 | f->kern[lo].r) == key)
        return f->kern[lo].d;
    return 0;
}

/* A walk through s's glyphs, the pen in 1/256 pixels (fontdraw.c's run). */
struct walk {
    const char *p;
    int prev;      /* the last slot (-1: none yet) */
    int64_t pen;
};

/* The next glyph's slot (-1 at the end) and *at its pen. */
static int walk_next(const struct panic_font *f, struct walk *w, int64_t *at)
{
    for (; *w->p; w->p++) {
        int s = slot(f, *w->p);
        if (s < 0)
            continue;   /* not baked: no room */
        w->p++;
        if (w->prev >= 0)
            w->pen += kern(f, w->prev, s);
        *at = w->pen;
        w->pen += f->adv[s];
        w->prev = s;
        return s;
    }
    return -1;
}

int panic_text_width(const struct panic_font *f, const char *s)
{
    struct walk w = { s, -1, 0 };
    int64_t at;
    while (walk_next(f, &w, &at) >= 0)
        ;
    return (int)((w.pen + 128) >> 8);
}

static void glyph(const struct panic_canvas *c, const struct panic_font *f,
                  const struct panic_glyph *g, int x, int y, uint32_t rgb)
{
    for (int j = 0; j < g->h; j++) {
        int py = y + g->y + j;
        if (py < 0 || (uint32_t)py >= c->h)
            continue;
        for (int i = 0; i < g->w; i++) {
            int px = x + g->x + i;
            uint32_t a = f->cov[g->off + (uint32_t)(j * g->w + i)];
            if (px < 0 || (uint32_t)px >= c->w || !a)
                continue;
            put(c, px, py, a == 255 ? rgb : over(get(c, px, py), rgb, a));
        }
    }
}

void panic_text(const struct panic_canvas *c, const struct panic_font *f, int x, int y,
                uint32_t rgb, const char *s)
{
    struct walk w = { s, -1, 0 };
    int64_t at;
    int sl;
    while ((sl = walk_next(f, &w, &at)) >= 0) {
        int64_t q = (at * PANIC_PHASES + 128) >> 8;   /* the pen in quarter pixels */
        glyph(c, f, &f->g[sl][q & (PANIC_PHASES - 1)], x + (int)(q >> 2), y, rgb);
    }
}

void panic_mono(const struct panic_canvas *c, int x, int y, uint32_t fg, uint32_t bg,
                const char *s, int n)
{
    for (int k = 0; k < n && s[k]; k++, x += 8) {
        unsigned char ch = (unsigned char)s[k];
        const uint8_t *rows = font_8x16[ch < 0x80 ? ch : '?'];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                put(c, x + i, y + j, rows[j] & (0x80 >> i) ? fg : bg);
    }
}

/* ---- where ---------------------------------------------------------------------------- */

void panic_layout(uint32_t w, uint32_t h, struct panic_layout *o)
{
    /* The ring a little above the middle, the lines under it; the panel,
     * when it comes, below them without moving them: on a short screen
     * the ring goes up as far as the panel needs. */
    const int below = PANIC_RING_PX / 2 + 28 + panic_font_title.cap_h + 26 + 20 + 32;
    o->panel_h = PANIC_PANEL_LINES * 16 + 24;
    o->ring_x = (int)w / 2;
    o->ring_y = (int)(h * 2 / 5);
    int fit = (int)h - 16 - o->panel_h - below;
    if (o->ring_y > fit)
        o->ring_y = fit > PANIC_RING_PX ? fit : PANIC_RING_PX;
    o->title_y = o->ring_y + PANIC_RING_PX / 2 + 28 + panic_font_title.cap_h;
    o->small_y = o->title_y + 26;
    o->code_y = o->small_y + 20;
    o->panel_y = o->code_y + 32;
    o->panel_w = (int)w - 32 < PANIC_PANEL_W ? (int)w - 32 : PANIC_PANEL_W;
    o->panel_w = o->panel_w < 64 ? 64 : o->panel_w;
    o->panel_cols = (o->panel_w - 32) / 8;
    o->panel_x = ((int)w - o->panel_w) / 2;
}
