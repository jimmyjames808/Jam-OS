/* jamjar: jam roulette (`r`, or a click on the big label). A reel of the
 * library's jar labels in a random order spins across the middle of the
 * screen, slows down (a cubic ease-out over SPIN), the pointer above it
 * kicking at every label that passes, lands on one album, shows its name,
 * and after LINGER the app plays that album. The reel is ROUL_TILES albums
 * picked at random (never the same twice in a row), and it lands near its
 * end, so every spin passes dozens of labels, never on the album playing
 * (if there is another). */
#include "jamjar.h"

#define SPIN   (4200 * NS_PER_MS)
#define LINGER (1300 * NS_PER_MS)
#define ROUL_BG 0x120d10   /* the reel's band */

bool roulette_start(struct roulette *r, const struct library *l, uint64_t seed, int64_t avoid,
                    uint64_t t)
{
    memset(r, 0, sizeof(*r));
    if (!l->ready || !l->nalbums)
        return false;
    uint64_t s = seed | 1;
    uint32_t n = 0;
    for (; n < ROUL_TILES; n++) {   /* random albums, never one twice in a row */
        uint32_t k = (uint32_t)(rng_next(&s) % l->nalbums);
        if (n && k == r->tile[n - 1] && l->nalbums > 1)
            k = (k + 1 + (uint32_t)(rng_next(&s) % (l->nalbums - 1))) % l->nalbums;
        r->tile[n] = k;
    }
    r->ntiles = n;
    r->target = n - 5 - (uint32_t)(rng_next(&s) % 4);
    /* Never the album playing, when there is another: its neighbour on the
     * reel is another album (never one twice in a row). */
    if (avoid >= 0 && r->tile[r->target] == (uint64_t)avoid && l->nalbums > 1)
        r->target++;
    r->t0 = t;
    r->last_tick = 0;
    r->on = true;
    return true;
}

int64_t roulette_step(struct roulette *r, uint64_t t)
{
    if (!r->on)
        return -1;
    float e = t > r->t0 ? (float)(t - r->t0) / (float)SPIN : 0.0f;
    if (e >= 1.0f) {
        e = 1.0f;
        if (!r->landed) {
            r->landed = true;
            r->landed_at = t;
        }
    }
    float k = 1.0f - e;
    r->pos = (float)r->target * (1.0f - k * k * k);
    int under = (int)(r->pos + 0.5f);
    if (under != r->last_tick) {
        r->last_tick = under;
        r->kick = 1.0f;
    }
    r->kick *= 0.82f;
    if (r->landed && t - r->landed_at >= LINGER) {
        r->on = false;
        return r->tile[r->target];
    }
    return -1;
}

void roulette_draw(const struct roulette *r, const struct library *l, const struct layout *lo,
                   const struct surf *s)
{
    if (!r->on)
        return;
    int u = lo->u, size = lo->h * 15 / 100, big = size * 13 / 10;
    float gap = (float)size * 1.22f;
    int cx = lo->w / 2, cy = lo->h * 46 / 100;
    fill_pm(s, 0, 0, lo->w, lo->h, argb_pm(0x0a0709, 200));
    /* The reel's own band, one solid colour, so the labels' corners (drawn
     * over it) match what is around them. */
    int band = big + 40 * u;
    fill(s, 0, cy - band / 2, lo->w, band, ROUL_BG);
    fill(s, 0, cy - band / 2, lo->w, u, C_BERRY0);
    fill(s, 0, cy + band / 2 - u, lo->w, u, C_BERRY0);
    int tw = text_width(2 * u, "jam roulette") + 48 * u, ty = cy - big / 2 - 100 * u;
    panel(s, &(struct rect){ (lo->w - tw) / 2, ty, tw, 52 * u }, 26 * u, ROUL_BG, 256);
    text_in(s, &(struct rect){ 0, ty, lo->w, 52 * u }, 2 * u, C_GOLD, "jam roulette");
    for (uint32_t i = 0; i < r->ntiles; i++) {
        float d = (float)i - r->pos;
        int x = cx + (int)(d * gap);
        if (x + big < 0 || x - big > lo->w)
            continue;
        bool centre = d > -0.5f && d <= 0.5f;
        int sz = centre ? big : size;
        const struct lib_album *al = &l->album[r->tile[i]];
        const struct rect at = { x - sz / 2, cy - sz / 2, sz, sz };
        art_cover(s, &at, al->hash, l->track[al->first].path, ROUL_BG);
        float far = d < 0 ? -d : d;
        if (!centre)   /* the further from the pointer, the darker (corners kept round) */
            panel(s, &at, sz / 7, ROUL_BG, (uint32_t)(far > 4 ? 200 : 60 + far * 35));
    }
    /* The pointer: a gold triangle over the middle, kicked by each label. */
    float py = (float)(cy - big / 2 - 14 * u) - r->kick * 8.0f * (float)u;
    float tri[6] = { (float)cx - 12.0f * u, py - 16.0f * u, (float)cx + 12.0f * u,
                     py - 16.0f * u, (float)cx, py };
    poly_aa(s, tri, 3, C_GOLD, 255);
    if (!r->landed)
        return;
    const struct lib_album *a = &l->album[r->tile[r->target]];
    struct rect t = { 0, cy + big / 2 + 24 * u, lo->w, 40 * u };
    text_in(s, &t, 2 * u, C_CREAM, a->name);
    t.y += 44 * u;
    text_in(s, &t, u, C_DIM, l->artist[a->artist].name);
}
