/* jamjar: the simmer, the jam across the bottom of the screen that moves
 * with the music.
 *
 * The surface is SIM_COLS points across the width, each the sum of two
 * heights (fractions of the jam's rectangle): a shape made from the
 * player's sixteen bands (the bass in the middle, the highs at both
 * edges, smoothed between bands, plus two slow travelling waves so it is
 * never still), and ripples: a row of springs, each pulled toward its
 * neighbours (the tension that makes waves travel) and back to flat, with
 * damping. A splash (a click, a bubble bursting, a drop falling back)
 * kicks the ripples' speeds, and the kick spreads out as waves.
 *
 * When a band jumps well above its own recent average (a beat, a snare)
 * a bubble starts deep in the jam at that band's place, rises, wobbles,
 * and bursts at the surface into a few drops that fly up and fall back.
 * Gold seeds hang in the jam and drift. All of it in float, in pixels of
 * the jam's rectangle at draw time, so the rectangle can grow (the full
 * jar) without the state knowing. */
#include "jamjar.h"

#define TENSION  6000.0f   /* ripples: neighbour pull (1/s^2, unit spacing): ~77 points/s */
#define SPRING   6.0f      /* ... back to flat */
#define DAMP     2.2f
#define SUBSTEP  (1.0f / 240.0f)
#define GRAVITY  2.4f      /* drops: rect heights per s^2 */

static float frand(uint64_t *r)
{
    return (float)(rng_next(r) >> 40) / (float)(1u << 24);
}

static float sinf_(float x)
{
    return (float)sind((double)x);
}

/* 1 - e^(-dt / tau): how far a smoothed value moves in dt. */
static float ease(float dt, float tau)
{
    return 1.0f - (float)exp2d(-(double)(dt / tau) * 1.4426950408889634);
}

void simmer_init(struct simmer *s, uint64_t seed)
{
    memset(s, 0, sizeof(*s));
    s->rng = seed | 1;
    for (int i = 0; i < SIM_SEEDS; i++)
        s->seed[i] = (struct seed){ frand(&s->rng), 0.15f + 0.7f * frand(&s->rng),
                                    0.6f + 0.6f * frand(&s->rng),
                                    (frand(&s->rng) - 0.5f) * 0.02f };
}

/* The band value at band coordinate f (0..15), Catmull-Rom between bands. */
static float band_at(const float *b, float f)
{
    int i = (int)f;
    float t = f - (float)i;
    float p0 = b[i > 0 ? i - 1 : 0], p1 = b[i], p2 = b[i < 15 ? i + 1 : 15];
    float p3 = b[i < 14 ? i + 2 : 15];
    float v = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 +
                                              t * (3 * (p1 - p2) + p3 - p0)));
    return v < 0 ? 0 : v > 1.2f ? 1.2f : v;
}

/* Where band i sits across the width: the bass in the middle. */
static float band_x(int i, bool right)
{
    float f = ((float)i + 0.5f) / 16.0f * 0.5f;
    return right ? 0.5f + f : 0.5f - f;
}

static void spawn_bubble(struct simmer *s, float x, float size)
{
    for (int i = 0; i < SIM_BUBBLES; i++) {
        struct bubble *b = &s->bub[i];
        if (b->live)
            continue;
        *b = (struct bubble){ x, 0.75f + 0.25f * frand(&s->rng), 0.4f + 0.6f * size,
                              0.25f + 0.35f * frand(&s->rng), 6.2832f * frand(&s->rng), true };
        return;
    }
}

static void spawn_drops(struct simmer *s, float x, float y, int n, float power)
{
    for (int i = 0; i < SIM_DROPS && n; i++) {
        struct drop *d = &s->drop[i];
        if (d->live)
            continue;
        float vx = (frand(&s->rng) - 0.5f) * 0.25f;
        float vy = -(0.5f + 0.7f * frand(&s->rng)) * power;
        vy = vy < -1.4f ? -1.4f : vy;   /* up to 0.4 of the jam's height */
        *d = (struct drop){ x, y, vx, vy, 0.5f + 0.7f * frand(&s->rng), true };
        n--;
    }
}

/* Push the surface at x (0..1) by `kick` (+: down), spread over a few points. */
static void push(struct simmer *s, float x, float kick)
{
    int c = (int)(x * (SIM_COLS - 1));
    for (int k = -4; k <= 4; k++) {
        int j = c + k;
        if (j >= 0 && j < SIM_COLS)
            s->v[j] -= kick * (1.0f - (float)(k < 0 ? -k : k) / 5.0f);
    }
}

static float surface(const struct simmer *s, float x);

void simmer_splash(struct simmer *s, float x, float strength)
{
    push(s, x, 2.5f * strength);
    spawn_drops(s, x, surface(s, x), 4 + (int)(6 * strength), 0.8f + strength);
}

/* The bands: smoothed, and a bubble for each that jumps. */
static void listen(struct simmer *s, const uint8_t bands[16], uint8_t level, float dt)
{
    float up = ease(dt, 0.035f), down = ease(dt, 0.22f), slow = ease(dt, 1.2f);
    for (int i = 0; i < 16; i++) {
        float x = (float)bands[i] / 255.0f;
        s->band[i] += (x - s->band[i]) * (x > s->band[i] ? up : down);
        if (x - s->avg[i] > 0.16f && x > 0.35f && frand(&s->rng) < 0.5f)
            spawn_bubble(s, band_x(i, rng_next(&s->rng) & 1), x);
        s->avg[i] += (x - s->avg[i]) * slow;
    }
    float lv = (float)level / 255.0f;
    s->level += (lv - s->level) * (lv > s->level ? up : down);
}

/* The shape from the bands, and the ripples on it. */
static void springs(struct simmer *s, float dt)
{
    float base = 0.20f + 0.12f * s->level;
    for (int k = 0; k < SIM_COLS; k++) {
        float x = (float)k / (SIM_COLS - 1), f = (x < 0.5f ? 0.5f - x : x - 0.5f) * 2.0f;
        float wave = 0.03f * sinf_(6.2832f * (x * 1.3f + s->t * 0.11f)) +
                     0.018f * sinf_(6.2832f * (x * 3.1f - s->t * 0.19f));
        s->rest[k] = base + 0.55f * band_at(s->band, f * 15.0f) + wave;
    }
    /* Semi-implicit Euler (speed first, then height with the new speed):
     * stable while sqrt(4 TENSION) * SUBSTEP < 2. */
    for (float left = dt; left > 0; left -= SUBSTEP) {
        float h = left < SUBSTEP ? left : SUBSTEP;
        for (int k = 0; k < SIM_COLS; k++) {
            float l = s->h[k > 0 ? k - 1 : 0], r = s->h[k < SIM_COLS - 1 ? k + 1 : k];
            float acc = TENSION * (l + r - 2 * s->h[k]) - SPRING * s->h[k] - DAMP * s->v[k];
            s->v[k] += acc * h;
        }
        for (int k = 0; k < SIM_COLS; k++)
            s->h[k] += s->v[k] * h;
    }
}

/* The surface's height (0..1 of the rectangle) at x (0..1). */
static float surface(const struct simmer *s, float x)
{
    float f = x * (SIM_COLS - 1);
    int i = (int)f;
    i = i < 0 ? 0 : i > SIM_COLS - 2 ? SIM_COLS - 2 : i;
    float t = f - (float)i;
    float a = s->rest[i] + s->h[i], b = s->rest[i + 1] + s->h[i + 1];
    float y = a + (b - a) * t;
    return y < 0.03f ? 0.03f : y > 0.97f ? 0.97f : y;
}

/* Bubbles rise and burst; drops fly and fall back; seeds drift. */
static void particles(struct simmer *s, float dt)
{
    for (int i = 0; i < SIM_BUBBLES; i++) {
        struct bubble *b = &s->bub[i];
        if (!b->live)
            continue;
        b->y -= b->vy * dt;
        b->ph += dt * 5.0f;
        b->x += sinf_(b->ph) * 0.0006f;
        if (b->y > 0)
            continue;
        b->live = false;   /* at the surface: it bursts */
        push(s, b->x, -0.6f * b->r);
        spawn_drops(s, b->x, surface(s, b->x), 2 + (int)(3 * b->r), 0.5f + 0.5f * b->r);
    }
    for (int i = 0; i < SIM_DROPS; i++) {
        struct drop *d = &s->drop[i];
        if (!d->live)
            continue;
        d->vy += GRAVITY * dt;
        d->x += d->vx * dt;
        d->y -= d->vy * dt;
        if (d->x < 0 || d->x > 1 || (d->vy > 0 && d->y < surface(s, d->x))) {
            d->live = false;
            if (d->x >= 0 && d->x <= 1)
                push(s, d->x, 0.15f * d->r);
        }
    }
    for (int i = 0; i < SIM_SEEDS; i++) {
        struct seed *e = &s->seed[i];
        e->x += e->drift * dt;
        e->x = e->x < 0 ? e->x + 1 : e->x > 1 ? e->x - 1 : e->x;
    }
}

void simmer_step(struct simmer *s, const uint8_t bands[16], uint8_t level, float dt)
{
    dt = dt > 0.1f ? 0.1f : dt < 0 ? 0 : dt;
    s->t += dt;
    listen(s, bands, level, dt);
    springs(s, dt);
    particles(s, dt);
}

bool simmer_busy(const struct simmer *s)
{
    for (int i = 0; i < SIM_BUBBLES; i++)
        if (s->bub[i].live)
            return true;
    for (int i = 0; i < SIM_DROPS; i++)
        if (s->drop[i].live)
            return true;
    float e = 0;
    for (int k = 0; k < SIM_COLS; k++)
        e += s->v[k] < 0 ? -s->v[k] : s->v[k];
    return e > 0.05f || s->level > 0.01f;
}

/* ---- drawing ------------------------------------------------------------------------ */

/* The jam's colour by depth below the surface, for a rectangle hgt high:
 * a lit rim, the berry, then darker toward the bottom. */
static void make_lut(struct simmer *s, int hgt, int u)
{
    int n = hgt < 1024 ? hgt : 1024;
    int rim = 3 * u, mid = n * 2 / 5 > rim + 1 ? n * 2 / 5 : rim + 1;
    for (int d = 0; d < n; d++) {
        uint32_t c;
        if (d < rim)
            c = mixc(C_ROSE, C_BERRY2, (uint32_t)(d * 256 / rim));
        else if (d < mid)
            c = mixc(C_BERRY2, C_BERRY0, (uint32_t)((d - rim) * 256 / (mid - rim)));
        else
            c = mixc(C_BERRY0, 0x2a0b16, (uint32_t)((d - mid) * 256 / (n - mid > 0 ? n - mid : 1)));
        s->lut[d] = c;
    }
    s->lut_h = n;
}

/* The body: column by column from the surface down, the top pixel at its
 * coverage, a soft haze above it. */
static void body(struct simmer *s, const struct surf *dst, const struct rect *r, int u)
{
    int haze = 26 * u;
    for (int x = 0; x < r->w; x++) {
        float sy = (float)r->h * (1.0f - surface(s, ((float)x + 0.5f) / (float)r->w));
        int top = (int)sy;
        float frac = 1.0f - (sy - (float)top);
        int px = r->x + x;
        if (px < 0 || px >= dst->w)
            continue;
        for (int y = top - haze > 0 ? top - haze : 0; y < top && r->y + y < dst->h; y++) {
            uint32_t a = (uint32_t)((y - (top - haze)) * 40 / haze);
            uint32_t *p = dst->px + (uint64_t)(r->y + y) * dst->stride + px;
            *p = mixc(*p, C_BERRY1, a);
        }
        for (int y = top; y < r->h && r->y + y < dst->h; y++) {
            if (r->y + y < 0)
                continue;
            int d = y - top < s->lut_h ? y - top : s->lut_h - 1;
            uint32_t *p = dst->px + (uint64_t)(r->y + y) * dst->stride + px;
            *p = y == top ? mixc(*p, s->lut[0], (uint32_t)(frac * 256)) : s->lut[d];
        }
    }
}

/* The gloss along the surface. */
static void gloss(const struct simmer *s, const struct surf *dst, const struct rect *r, int u)
{
    int step = 6 * u;
    float px = (float)r->x, py = (float)r->y + (float)r->h * (1.0f - surface(s, 0));
    for (int x = step; x <= r->w + step; x += step) {
        float xx = (float)(x > r->w ? r->w : x);
        float yy = (float)r->y + (float)r->h * (1.0f - surface(s, xx / (float)r->w));
        line_aa(dst, px, py + 1.5f * u, (float)r->x + xx, yy + 1.5f * u, 1.2f * u, C_CREAM, 70);
        px = (float)r->x + xx;
        py = yy;
    }
}

void simmer_draw(struct simmer *s, const struct surf *screen, const struct rect *rr)
{
    int u = scr.ui > 0 ? scr.ui : 1;
    /* Drawn on the screen from the jam's top down only: drops flying above
     * it are cut off rather than drawn over the panels. */
    int top = rr->y < 0 ? 0 : rr->y > screen->h ? screen->h : rr->y;
    struct surf below = { screen->px + (uint64_t)top * screen->stride, screen->w,
                          screen->h - top, screen->stride };
    const struct surf *dst = &below;
    struct rect jam = { rr->x, rr->y - top, rr->w, rr->h };
    const struct rect *r = &jam;
    if (r->h != s->hgt) {
        make_lut(s, r->h, u);
        s->hgt = r->h;
    }
    body(s, dst, r, u);
    float W = (float)r->w, H = (float)r->h;
    for (int i = 0; i < SIM_SEEDS; i++) {   /* the seeds, hanging in the jam */
        const struct seed *e = &s->seed[i];
        float sy = H * (1.0f - surface(s, e->x));
        float y = sy + e->d * (H - sy) + 3.0f * sinf_(s->t * 0.7f + (float)i) * u;
        float sx = (float)r->x + e->x * W, len = e->r * 2.6f * u;
        line_aa(dst, sx - len * 0.6f, (float)r->y + y - len * 0.25f, sx + len * 0.6f,
                (float)r->y + y + len * 0.25f, len * 0.9f, C_GOLD, (uint32_t)(230 - 120 * e->d));
    }
    for (int i = 0; i < SIM_BUBBLES; i++) {
        const struct bubble *b = &s->bub[i];
        if (!b->live)
            continue;
        float sy = H * (1.0f - surface(s, b->x)), rad = (2.5f + 7.0f * b->r) * u;
        float y = sy + b->y * (H - sy) + rad;
        float x = (float)r->x + b->x * W;
        disc_aa(dst, x, (float)r->y + y, rad, C_ROSE, 70);
        ring_aa(dst, x, (float)r->y + y, rad, 1.3f * u, C_ROSE, 190);
        disc_aa(dst, x - rad * 0.35f, (float)r->y + y - rad * 0.35f, rad * 0.25f, C_CREAM, 170);
    }
    gloss(s, dst, r, u);
    for (int i = 0; i < SIM_DROPS; i++) {
        const struct drop *d = &s->drop[i];
        if (d->live)
            disc_aa(dst, (float)r->x + d->x * W, (float)r->y + H * (1.0f - d->y),
                    (1.5f + 2.5f * d->r) * u, C_BERRY2, 235);
    }
}
