/* jamjar: the stereo bars across the bottom of the screen, and the
 * smoothing the sunburst (burst.c) shares.
 *
 * Each channel has BARS bars, one per band of the player's `stereo` (64,
 * log-spaced from 40 Hz to 16 kHz, low on the left), each band's byte
 * already in dB (the player maps -76..-16 dB to 0..255 with a tilt of
 * 3 dB an octave). Here, per channel:
 *   - a little smoothing across neighbours (a quarter from each side), so
 *     one band alone doesn't flicker;
 *   - per-band normalisation: each band's long average (about 6 s, both
 *     channels together) is pulled toward the average of all bands by a
 *     gain kept within GAIN_MIN..GAIN_MAX, so bands that are always quiet
 *     (the top octave) still move and a loud bass doesn't sit at the top.
 *     One gain a band for both channels keeps their balance: a quieter
 *     channel stays quieter;
 *   - a fast attack (an ease with ATTACK_S) and a steady fall (FALL a
 *     second), so the bars are calm, never jittery;
 *   - with nothing heard (paused, stopped) every target is 0 and the bars
 *     fall the same way, smoothly.
 * The picture: one line across the middle of the strip; the left
 * channel's bars grow up from it, crimson at the foot to gold at 80 % of
 * the full height, the right's grow down from it, a deep crimson at the
 * line to crimson at full depth; a small gap at the line, rounded ends,
 * anti-aliased edges. The bars are drawn across the thread pool (each bar
 * is its own columns, so no two workers touch one pixel). */
#include "jamjar.h"

#define ATTACK_S    0.035f   /* seconds: the rise's time constant */
#define FALL        1.25f    /* heights a second */
#define NORM_S      6.0f     /* seconds: the per-band average's time constant */
#define GAIN_MIN    0.85f
#define GAIN_MAX    1.45f
#define SPIN_MIN    0.15f    /* the sunburst's turn, radians a second, in silence ... */
#define SPIN_BUSY   0.6f     /* ... and more with every bar at the top */
#define SPIN_EASE_S 0.8f     /* seconds: how fast its speed follows */
#define C_DOWN0     0x7a1e47u   /* the right channel's bars: at the line ... */
#define C_DOWN1     0xc8264au   /* ... and at full depth */

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

/* Each channel's targets with neighbours blended in (x), and the shared
 * per-band average and gain moved on. */
static void targets(struct bars *b, const uint8_t *const in[NCH], bool live, float dt,
                  float x[NCH][BARS])
{
    float slow = ease(dt, NORM_S), mean = 0;
    for (int c = 0; c < NCH; c++)
        for (int i = 0; i < BARS; i++) {
            float l = in[c][i > 0 ? i - 1 : 0], r = in[c][i < BARS - 1 ? i + 1 : i];
            x[c][i] = live ? (0.25f * l + 0.5f * (float)in[c][i] + 0.25f * r) / 255.0f : 0.0f;
        }
    for (int i = 0; i < BARS; i++) {
        if (live)
            b->avg[i] += ((x[0][i] + x[1][i]) * 0.5f - b->avg[i]) * slow;
        mean += b->avg[i] / BARS;
    }
    for (int i = 0; i < BARS; i++) {
        float g = (mean + 0.08f) / (b->avg[i] + 0.08f);
        g = g < GAIN_MIN ? GAIN_MIN : g > GAIN_MAX ? GAIN_MAX : g;
        b->gain[i] += (g - b->gain[i]) * slow;
    }
}

void bars_step(struct bars *b, const uint8_t left[BARS], const uint8_t right[BARS], bool live,
               float dt)
{
    dt = dt > 0.1f ? 0.1f : dt < 0 ? 0 : dt;
    const uint8_t *const in[NCH] = { left, right };
    float x[NCH][BARS], up = ease(dt, ATTACK_S);
    targets(b, in, live, dt, x);
    for (int c = 0; c < NCH; c++)
        for (int i = 0; i < BARS; i++) {
            float t = x[c][i] * b->gain[i], v = b->v[c][i];
            t = t > 1.0f ? 1.0f : t;
            v = t > v ? v + (t - v) * up : v - FALL * dt;
            v = v < t && t <= b->v[c][i] ? t : v;   /* a fall stops at the target */
            b->v[c][i] = v < 0 ? 0 : v;
        }
    float goal = live ? SPIN_MIN + bars_energy(b) * SPIN_BUSY : 0.0f;
    b->speed += (goal - b->speed) * ease(dt, SPIN_EASE_S);
    b->speed = b->speed < 0.002f && !live ? 0.0f : b->speed;
    b->spin += b->speed * dt;
    b->spin -= b->spin > 6.2831853f ? 6.2831853f : 0.0f;
}

bool bars_busy(const struct bars *b)
{
    for (int c = 0; c < NCH; c++)
        for (int i = 0; i < BARS; i++)
            if (b->v[c][i] > 0.001f)
                return true;
    return b->speed > 0.0f;
}

float bars_bass(const struct bars *b)
{
    float s = 0;
    for (int i = 2; i <= 5; i++)
        s += b->v[CH_LEFT][i] + b->v[CH_RIGHT][i];
    return s / 8.0f;
}

float bars_energy(const struct bars *b)
{
    float s = 0;
    for (int i = 0; i < BARS; i++)
        s += b->v[CH_LEFT][i] + b->v[CH_RIGHT][i];
    return s / (2.0f * BARS);
}

/* ---- drawing ------------------------------------------------------------------------ */

#define GRAD_MAX 1024   /* rows of a gradient: a half strip is never taller */

/* Where things go in a rect: the line between the channels, the bars'
 * full height on each side, a bar's width and pitch, and the left edge. */
struct geom {
    const struct surf *dst;
    const struct bars *b;
    float x0, pitch, bw;        /* the first bar's left edge, bar to bar, a bar's width */
    int   mid, line;            /* the line between the channels (y), its thickness */
    int   gap, full;            /* the gap each side of it; a bar's full height */
    uint32_t grad[NCH][GRAD_MAX];   /* the colour by distance from the line */
};

static void geometry(struct geom *g, const struct rect *r, int u)
{
    float margin = 16.0f * u, w = (float)r->w - 2 * margin;
    g->pitch = w / BARS;
    g->bw = g->pitch * 0.74f;
    g->x0 = (float)r->x + margin + (g->pitch - g->bw) / 2;
    g->mid = r->y + r->h / 2;
    g->line = u;
    g->gap = 3 * u;
    g->full = r->h / 2 - g->gap - 4 * u;
    g->full = g->full > GRAD_MAX ? GRAD_MAX : g->full < 1 ? 1 : g->full;
}

static void gradients(struct geom *g)
{
    for (int y = 0; y < g->full; y++) {
        /* Up: crimson at the foot to gold at 80 % of the full height. */
        uint32_t t = (uint32_t)(y * 320 / g->full);
        t = t > 256 ? 256 : t;
        g->grad[CH_LEFT][y] =
            t < 128 ? mixc(C_BERRY1, C_BERRY2, t * 2) : mixc(C_BERRY2, C_GOLD, (t - 128) * 2);
        g->grad[CH_RIGHT][y] = mixc(C_DOWN0, C_DOWN1, (uint32_t)(y * 256 / g->full));
    }
}

void bars_where(const struct rect *r, int i, int *x0, int *x1, int *mid)
{
    static struct geom g;
    geometry(&g, r, scr.ui > 0 ? scr.ui : 1);
    float l = g.x0 + (float)i * g.pitch;
    *x0 = (int)l;
    *x1 = (int)(l + g.bw) + 1;
    *mid = g.mid;
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

/* Bar i of channel c: from the line (less the gap) out by its height, its
 * far end rounded (a radius of half its width, or its height if less). */
static void bar(const struct geom *g, int c, int i)
{
    float l = g->x0 + (float)i * g->pitch, rr = l + g->bw;
    float h = g->b->v[c][i] * (float)g->full;
    if (h < 0.05f)
        return;
    float rad = g->bw / 2 < h ? g->bw / 2 : h;
    bool up = c == CH_LEFT;
    float foot = up ? (float)(g->mid - g->gap) : (float)(g->mid + g->line + g->gap);
    float tip = up ? foot - h : foot + h;
    int y0 = up ? (int)floord(tip) : (int)foot, y1 = up ? (int)foot : (int)tip + 1;
    for (int y = y0; y < y1; y++) {
        /* How much of this row is in the bar, and how far its centre is from the tip. */
        float top = (float)y > (up ? tip : foot) ? (float)y : (up ? tip : foot);
        float bot = (float)(y + 1) < (up ? foot : tip) ? (float)(y + 1) : (up ? foot : tip);
        if (bot <= top)
            continue;
        float d = up ? (float)y + 0.5f - tip : tip - ((float)y + 0.5f), inset = 0.0f;
        if (d < rad) {
            float e = rad - (d > 0 ? d : 0), hw = rad * rad - e * e;
            inset = rad - (hw > 0 ? sqrtf_(hw) : 0);
        }
        int from = up ? (int)foot - 1 - y : y - (int)foot;
        uint32_t col = g->grad[c][from < 0 ? 0 : from >= g->full ? g->full - 1 : from];
        span(g->dst, y, l + inset, rr - inset, col, (uint32_t)((bot - top) * 256.0f));
    }
}

static void bar_job(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const struct geom *g = arg;
    for (int i = (int)item * 4; i < (int)item * 4 + 4 && i < BARS; i++) {
        bar(g, CH_LEFT, i);
        bar(g, CH_RIGHT, i);
    }
}

void bars_draw(const struct bars *b, const struct surf *dst, const struct rect *r)
{
    static struct geom g;
    int u = scr.ui > 0 ? scr.ui : 1;
    g.dst = dst;
    g.b = b;
    geometry(&g, r, u);
    gradients(&g);
    fill(dst, r->x + 16 * u, g.mid, r->w - 32 * u, g.line, C_LINE);   /* the line */
    text(dst, r->x + 4 * u, g.mid - g.gap - TEXT_H(u), u, C_FAINT, "L");
    text(dst, r->x + 4 * u, g.mid + g.line + g.gap, u, C_FAINT, "R");
    if (pool_threads() > 1) {
        pool_run(bar_job, &g, (BARS + 3) / 4);
        return;
    }
    for (uint32_t i = 0; i < (BARS + 3) / 4; i++)
        bar_job(i, 0, &g);
}
