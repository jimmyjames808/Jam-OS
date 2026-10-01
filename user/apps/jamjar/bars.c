/* jamjar: the spectrum analyser across the bottom of the screen.
 *
 * BARS bars, one per band of the player's `spectrum` (64, log-spaced from
 * 40 Hz to 16 kHz, low on the left), each band's byte already in dB (the
 * player maps -76..-16 dB to 0..255 with a tilt of 3 dB an octave). Here:
 *   - a little smoothing across neighbours (a quarter from each side), so
 *     one band alone doesn't flicker;
 *   - per-band normalisation: each band's long average (about 6 s) is
 *     pulled toward the average of all bands by a gain kept within
 *     GAIN_MIN..GAIN_MAX, so bands that are always quiet (the top octave)
 *     still move and a loud bass doesn't sit at the top;
 *   - a fast attack (an ease with ATTACK_S) and a steady fall (FALL a
 *     second), so the bars are calm, never jittery;
 *   - a peak cap per bar that holds PEAK_HOLD_S, then falls, faster and
 *     faster;
 *   - with nothing heard (paused, stopped) every target is 0 and the bars
 *     and caps fall the same way, smoothly.
 * The picture: rounded bars with a gap, a vertical gradient from crimson at
 * the bottom to gold at full height, cream caps, anti-aliased edges, and a
 * faint reflection under the baseline. The bars are drawn across the
 * thread pool (each bar is its own columns, so no two workers touch one
 * pixel). */
#include "jamjar.h"

#define ATTACK_S    0.035f   /* seconds: the rise's time constant */
#define FALL        1.25f    /* heights a second */
#define PEAK_HOLD_S 0.5f
#define PEAK_FALL   0.9f     /* heights a second, at first (it speeds up) */
#define NORM_S      6.0f     /* seconds: the per-band average's time constant */
#define GAIN_MIN    0.85f
#define GAIN_MAX    1.45f

/* 1 - e^(-dt / tau). */
static float ease(float dt, float tau)
{
    return 1.0f - (float)exp2d(-(double)(dt / tau) * 1.4426950408889634);
}

void bars_init(struct bars *b)
{
    memset(b, 0, sizeof(*b));
    for (int i = 0; i < BARS; i++) {
        b->avg[i] = 0.35f;
        b->gain[i] = 1.0f;
    }
}

/* The targets: neighbours blended in, normalised. */
static void targets(struct bars *b, const uint8_t bands[BARS], bool live, float dt, float *x)
{
    float raw[BARS], mean = 0;
    for (int i = 0; i < BARS; i++)
        raw[i] = live ? (float)bands[i] / 255.0f : 0.0f;
    float slow = ease(dt, NORM_S);
    for (int i = 0; i < BARS; i++) {
        float l = raw[i > 0 ? i - 1 : 0], r = raw[i < BARS - 1 ? i + 1 : i];
        x[i] = 0.25f * l + 0.5f * raw[i] + 0.25f * r;
        if (live)
            b->avg[i] += (x[i] - b->avg[i]) * slow;
        mean += b->avg[i] / BARS;
    }
    for (int i = 0; i < BARS; i++) {
        float g = (mean + 0.08f) / (b->avg[i] + 0.08f);
        g = g < GAIN_MIN ? GAIN_MIN : g > GAIN_MAX ? GAIN_MAX : g;
        b->gain[i] += (g - b->gain[i]) * slow;
        x[i] *= b->gain[i];
        x[i] = x[i] > 1.0f ? 1.0f : x[i];
    }
}

void bars_step(struct bars *b, const uint8_t bands[BARS], bool live, float dt)
{
    dt = dt > 0.1f ? 0.1f : dt < 0 ? 0 : dt;
    float x[BARS], up = ease(dt, ATTACK_S);
    targets(b, bands, live, dt, x);
    for (int i = 0; i < BARS; i++) {
        float v = b->v[i];
        v = x[i] > v ? v + (x[i] - v) * up : v - FALL * dt;
        v = v < x[i] && x[i] <= b->v[i] ? x[i] : v;   /* a fall stops at the target */
        b->v[i] = v < 0 ? 0 : v;
        if (b->v[i] >= b->peak[i]) {
            b->peak[i] = b->v[i];
            b->hold[i] = PEAK_HOLD_S;
            b->pv[i] = 0;
        } else if ((b->hold[i] -= dt) <= 0) {
            b->pv[i] += 2.5f * dt;   /* it falls faster and faster */
            b->peak[i] -= (PEAK_FALL + b->pv[i]) * dt;
            b->peak[i] = b->peak[i] < b->v[i] ? b->v[i] : b->peak[i];
        }
    }
}

bool bars_busy(const struct bars *b)
{
    for (int i = 0; i < BARS; i++)
        if (b->v[i] > 0.001f || b->peak[i] > 0.001f)
            return true;
    return false;
}

/* ---- drawing ------------------------------------------------------------------------ */

/* Where things go in a rect: the baseline, the bars' full height, a bar's
 * width and pitch, and the left edge. */
struct geom {
    const struct surf *dst;
    const struct bars *b;
    float x0, pitch, bw;        /* the first bar's left edge, bar to bar, a bar's width */
    int   base, full, refl;     /* the baseline (y), the full height, the reflection's */
    int   u;
    uint32_t grad[1024];        /* the colour by height above the baseline */
};

static void geometry(struct geom *g, const struct rect *r, int u)
{
    float margin = 16.0f * u, w = (float)r->w - 2 * margin;
    g->pitch = w / BARS;
    g->bw = g->pitch * 0.74f;
    g->x0 = (float)r->x + margin + (g->pitch - g->bw) / 2;
    g->refl = r->h * 12 / 100;
    g->base = r->y + r->h - g->refl - 4 * u;
    g->full = g->base - r->y - 10 * u;
    g->full = g->full > 1024 ? 1024 : g->full < 1 ? 1 : g->full;
    g->u = u;
    for (int y = 0; y < g->full; y++) {
        /* Crimson at the foot to gold at 80 % of the full height. */
        uint32_t t = (uint32_t)(y * 320 / g->full);
        t = t > 256 ? 256 : t;
        g->grad[y] = t < 128 ? mixc(C_BERRY1, C_BERRY2, t * 2) : mixc(C_BERRY2, C_GOLD, (t - 128) * 2);
    }
}

/* One row of a bar: [l, r) in float pixels, colour c, coverage cov (0..256). */
static void span(const struct surf *s, int y, float l, float r, uint32_t c, uint32_t cov)
{
    if (y < 0 || y >= s->h || r <= l)
        return;
    uint32_t *row = s->px + (uint64_t)y * s->stride;
    for (int x = (int)l; x < (int)r + 1 && x < s->w; x++) {
        if (x < 0)
            continue;
        float a = ((float)(x + 1) < r ? (float)(x + 1) : r) - ((float)x > l ? (float)x : l);
        if (a <= 0)
            continue;
        uint32_t k = (uint32_t)(a * (float)cov);
        row[x] = k >= 255 ? c : mixc(row[x], c, k > 256 ? 256 : k);
    }
}

/* Bar i: the body (its top a half-disc), the reflection, the cap. */
static void bar(const struct geom *g, int i)
{
    const struct surf *s = g->dst;
    float l = g->x0 + (float)i * g->pitch, rr = l + g->bw, rad = g->bw / 2;
    float h = g->b->v[i] * (float)g->full, top = (float)g->base - h;
    for (int y = (int)top; y < g->base; y++) {
        float cy = (float)y + 0.5f, cov = 1.0f, inset = 0.0f;
        if (y == (int)top)
            cov = 1.0f - (top - (float)y);
        float d = top + rad - cy;   /* above the half-disc's centre */
        if (d > 0 && h > rad) {
            float hw = rad * rad - d * d;
            inset = rad - (hw > 0 ? sqrtf_(hw) : 0);
        }
        int hy = g->base - 1 - y;
        uint32_t c = g->grad[hy < 0 ? 0 : hy >= g->full ? g->full - 1 : hy];
        span(s, y, l + inset, rr - inset, c, (uint32_t)(cov * 256));
    }
    int rh = (int)(h * 0.45f) < g->refl ? (int)(h * 0.45f) : g->refl;
    for (int k = 0; k < rh; k++) {   /* the reflection: fading as it goes down */
        uint32_t a = (uint32_t)(60 * (rh - k) / (rh ? rh : 1));
        span(s, g->base + 2 * g->u + k, l, rr, g->grad[k < g->full ? k : g->full - 1], a);
    }
    float py = (float)g->base - g->b->peak[i] * (float)g->full - 5.0f * g->u;
    if (g->b->peak[i] > 0.004f)
        line_aa(s, l + 1.5f * g->u, py, rr - 1.5f * g->u, py, 3.0f * g->u, C_CREAM, 230);
}

static void bar_job(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const struct geom *g = arg;
    for (int i = (int)item * 4; i < (int)item * 4 + 4 && i < BARS; i++)
        bar(g, i);
}

void bars_draw(const struct bars *b, const struct surf *dst, const struct rect *r)
{
    static struct geom g;
    int u = scr.ui > 0 ? scr.ui : 1;
    g.dst = dst;
    g.b = b;
    geometry(&g, r, u);
    fill(dst, r->x + 16 * u, g.base + u - u, r->w - 32 * u, u, C_LINE);   /* the baseline */
    if (pool_threads() > 1) {
        pool_run(bar_job, &g, (BARS + 3) / 4);
        return;
    }
    for (int i = 0; i < BARS; i++)
        bar(&g, i);
}
