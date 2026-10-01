/* jamjar: the sunburst of the big view (`f`).
 *
 * A translucent gold disc in the middle whose radius and brightness pulse
 * with the bass, and RAYS rays round it, one per bar of bars.c: the left
 * channel's 64 bands round the left half (the bass at the bottom, the
 * highs at the top), the right channel's mirrored round the right half,
 * so bass meets bass at the bottom and the highs meet at the top. Each
 * ray is a line with round ends from just outside the disc outward, its
 * length the bar's height, coloured from crimson at its foot to gold at
 * its tip; the whole picture turns (bars.c keeps the angle: slowly, and
 * faster the busier the music is). All sizes are shares of a unit U made
 * from how far the picture may reach, so the furthest a ray's tip can get
 * (the disc at its biggest, a ray at full length, its round end and the
 * anti-aliased edge) is that reach.
 *
 * Drawing: the rays are computed once a frame; the rows of the picture are
 * cut into bands, one pool item each, and each worker draws the disc and
 * every ray clipped to its own rows, so no two workers touch one pixel. A
 * ray is drawn row by row over only the pixels near it: on each row, the
 * span where the ray (a capsule: its segment widened by half its width
 * plus a pixel) crosses, then each pixel's coverage from its distance to
 * the segment. */
#include "jamjar.h"

#define RAYS        (2 * BARS)
#define DISC        0.12f    /* of U: the disc's radius in silence ... */
#define DISC_BASS   0.05f    /* ... and more with the bass at the top */
#define GAP         0.015f   /* of U: from the disc to the rays' feet */
#define SPAN        0.30f    /* of U: a ray at full length */
#define WIDTH_SHARE 0.7f     /* a ray's width: of its share of the disc's rim */
#define REACH_U     (DISC + DISC_BASS + GAP + SPAN + 0.003f)   /* the tip's round end too */
#define C_RAY0      0xc8264au   /* a ray's foot ... */
#define C_RAY1      0xd9a032u   /* ... and its tip */
#define ROW_BANDS   48       /* pool items: bands of rows */
#define TAU         6.2831853f

struct ray {
    float x0, y0;            /* its foot */
    float dx, dy, len;       /* the unit direction out, and the length */
    float half;              /* half its width */
};

static struct {
    const struct surf *dst;
    struct ray ray[RAYS];
    float cx, cy, r0;        /* the disc */
    uint32_t disc_a;         /* ... its alpha */
    int   top, rows;         /* the rows the picture can touch */
} B;

/* The rays and the disc for this frame. */
static void place(const struct bars *b, int cx, int cy, int reach, float grow)
{
    int u = scr.ui > 0 ? scr.ui : 1;
    float unit = ((float)reach - 2.0f - 0.75f * (float)u) / REACH_U * grow;
    float bass = bars_bass(b);
    B.cx = (float)cx;
    B.cy = (float)cy;
    B.r0 = unit * (DISC + DISC_BASS * bass);
    B.disc_a = (uint32_t)((0.15f + 0.5f * bass) * 255.0f * grow);
    float half = TAU * B.r0 / RAYS * WIDTH_SHARE / 2;
    half = half < 0.75f * (float)u ? 0.75f * (float)u : half;
    float foot = B.r0 + GAP * unit;
    for (int i = 0; i < RAYS; i++) {
        /* i 0..63: the left channel's bands 0..63, from the bottom round
         * the left; 64..127: the right's, 63..0, from the top round the
         * right. y grows down, so an angle of pi/2 points down. */
        bool left = i < BARS;
        float v = left ? b->v[CH_LEFT][i] : b->v[CH_RIGHT][RAYS - 1 - i];
        float a = b->spin + TAU / 4 + ((float)i + 0.5f) * (TAU / RAYS);
        struct ray *r = &B.ray[i];
        r->dx = (float)cosd((double)a);
        r->dy = (float)sind((double)a);
        r->x0 = B.cx + r->dx * foot;
        r->y0 = B.cy + r->dy * foot;
        r->len = v * SPAN * unit;
        r->half = half;
    }
    B.top = cy - reach - 1;
    B.rows = 2 * reach + 3;
}

/* Where the row at height y (a pixel centre's) crosses ray r's capsule
 * of radius rho: [*lo, *hi]; false if it doesn't. The capsule is the two
 * end discs and the band between: the union of their spans on the row
 * (it is convex, so that is one span). */
static bool row_span(const struct ray *r, float y, float rho, float *lo, float *hi)
{
    float l = 1e9f, h = -1e9f;
    float ex[2] = { r->x0, r->x0 + r->dx * r->len }, ey[2] = { r->y0, r->y0 + r->dy * r->len };
    for (int k = 0; k < 2; k++) {
        float d = y - ey[k];
        if (d * d >= rho * rho)
            continue;
        float s = sqrtf_(rho * rho - d * d);
        l = ex[k] - s < l ? ex[k] - s : l;
        h = ex[k] + s > h ? ex[k] + s : h;
    }
    /* The band: along = (x - x0) dx + (y - y0) dy in [0, len] and
     * across = -(x - x0) dy + (y - y0) dx in [-rho, rho], both linear in x. */
    float a0 = (y - r->y0) * r->dy, c0 = (y - r->y0) * r->dx, bl = -1e9f, bh = 1e9f;
    if (r->dx > 1e-6f || r->dx < -1e-6f) {
        float p = -a0 / r->dx, q = (r->len - a0) / r->dx;
        bl = p < q ? p : q;
        bh = p < q ? q : p;
    } else if (a0 < 0 || a0 > r->len) {
        bh = bl - 1;
    }
    if (r->dy > 1e-6f || r->dy < -1e-6f) {
        float p = (c0 - rho) / r->dy, q = (c0 + rho) / r->dy;
        bl = (p < q ? p : q) > bl ? (p < q ? p : q) : bl;
        bh = (p < q ? q : p) < bh ? (p < q ? q : p) : bh;
    } else if (c0 < -rho || c0 > rho) {
        bh = bl - 1;
    }
    if (bh >= bl) {
        l = r->x0 + bl < l ? r->x0 + bl : l;
        h = r->x0 + bh > h ? r->x0 + bh : h;
    }
    *lo = l;
    *hi = h;
    return h >= l;
}

/* Ray r on the rows of s, whose row 0 is the screen's row `top`. */
static void ray_draw(const struct surf *s, int top, const struct ray *r)
{
    float rho = r->half + 1.0f;
    float ya = r->y0 < r->y0 + r->dy * r->len ? r->y0 : r->y0 + r->dy * r->len;
    float yb = r->y0 + r->y0 + r->dy * r->len - ya;
    int y0 = (int)floord(ya - rho) - top, y1 = (int)(yb + rho) + 1 - top;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > s->h ? s->h : y1;
    float inv = r->len > 0.0f ? 256.0f / r->len : 0.0f;
    for (int y = y0; y < y1; y++) {
        float cy = (float)(y + top) + 0.5f, lo, hi;
        if (!row_span(r, cy, rho, &lo, &hi))
            continue;
        int xa = (int)floord(lo), xb = (int)hi + 1;
        xa = xa < 0 ? 0 : xa;
        xb = xb > s->w ? s->w : xb;
        uint32_t *row = s->px + (uint64_t)y * s->stride;
        for (int x = xa; x < xb; x++) {
            float wx = (float)x + 0.5f - r->x0, wy = cy - r->y0;
            float t = wx * r->dx + wy * r->dy;
            t = t < 0.0f ? 0.0f : t > r->len ? r->len : t;
            float ox = wx - t * r->dx, oy = wy - t * r->dy;
            float c = r->half - sqrtf_(ox * ox + oy * oy) + 0.5f;
            if (c <= 0.0f)
                continue;
            uint32_t k = c >= 1.0f ? 256 : (uint32_t)(c * 256.0f);
            row[x] = mixc(row[x], mixc(C_RAY0, C_RAY1, (uint32_t)(t * inv)), k);
        }
    }
}

static void band_job(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    (void)arg;
    int y0 = B.top + B.rows * (int)item / ROW_BANDS;
    int y1 = B.top + B.rows * ((int)item + 1) / ROW_BANDS;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > B.dst->h ? B.dst->h : y1;
    if (y1 <= y0)
        return;
    struct surf s = { B.dst->px + (uint64_t)y0 * B.dst->stride, B.dst->w, y1 - y0,
                      B.dst->stride };
    if (B.r0 > 0.5f)
        disc_aa(&s, B.cx, B.cy - (float)y0, B.r0, C_GOLD, B.disc_a);
    for (int i = 0; i < RAYS; i++)
        ray_draw(&s, y0, &B.ray[i]);
}

void burst_draw(const struct bars *b, const struct surf *dst, int cx, int cy, int reach,
                float grow)
{
    if (grow <= 0.0f || reach < 8)
        return;
    B.dst = dst;
    place(b, cx, cy, reach, grow > 1.0f ? 1.0f : grow);
    if (pool_threads() > 1) {
        pool_run(band_job, NULL, ROW_BANDS);
        return;
    }
    for (uint32_t i = 0; i < ROW_BANDS; i++)
        band_job(i, 0, NULL);
}
