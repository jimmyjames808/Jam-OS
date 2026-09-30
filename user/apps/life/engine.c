/* life: the engine (life.h): the world, one bit a cell, and a generation
 * computed 64 cells at a time with bitwise adders, ages included (main.c
 * says how). */
#include "life.h"

uint32_t W, WW;
uint64_t *cur, *old;
uint64_t *age[3];
bool ages = true;
uint64_t gen;
static uint64_t pop_by[FUN_MAX_THREADS];

/* ---- the engine ------------------------------------------------------------------ */

/* One band of rows of the next generation: src -> dst. */
static void step_band(const uint64_t *src, uint64_t *dst, uint32_t band, uint32_t me)
{
    uint64_t pop = 0;
    uint32_t y0 = band * BAND, y1 = y0 + BAND < W ? y0 + BAND : W;
    for (uint32_t y = y0; y < y1; y++) {
        const uint64_t *ra = src + (uint64_t)((y - 1) & (W - 1)) * WW;
        const uint64_t *rb = src + (uint64_t)y * WW;
        const uint64_t *rc = src + (uint64_t)((y + 1) & (W - 1)) * WW;
        uint64_t *out = dst + (uint64_t)y * WW;
        uint64_t *a0 = age[0] + (uint64_t)y * WW, *a1 = age[1] + (uint64_t)y * WW;
        uint64_t *a2 = age[2] + (uint64_t)y * WW;
        for (uint32_t w = 0; w < WW; w++) {
            uint32_t wl = (w - 1) & (WW - 1), wr = (w + 1) & (WW - 1);
            uint64_t a = ra[w], b = rb[w], c = rc[w];
            uint64_t v[8] = {
                a << 1 | ra[wl] >> 63, a, a >> 1 | ra[wr] << 63,
                b << 1 | rb[wl] >> 63,    b >> 1 | rb[wr] << 63,
                c << 1 | rc[wl] >> 63, c, c >> 1 | rc[wr] << 63,
            };
            /* Count the neighbours per bit: s0 s1 the low bits, s2 "4 or more". */
            uint64_t s0 = 0, s1 = 0, s2 = 0;
            for (int k = 0; k < 8; k++) {
                uint64_t c0 = s0 & v[k];
                s0 ^= v[k];
                s2 |= s1 & c0;
                s1 ^= c0;
            }
            uint64_t n = s1 & ~s2 & (s0 | b);   /* 3, or 2 and alive */
            out[w] = n;
            pop += popcount64(n);
            if (ages) {
                /* +1 where not already 7, then 0 where the cell changed. */
                uint64_t x0 = a0[w], x1 = a1[w], x2 = a2[w];
                uint64_t inc = ~(x0 & x1 & x2), changed = n ^ b;
                uint64_t c1 = x0 & inc, c2 = x1 & c1;
                a0[w] = (x0 ^ inc) & ~changed;
                a1[w] = (x1 ^ c1) & ~changed;
                a2[w] = (x2 ^ c2) & ~changed;
            }
        }
    }
    pop_by[me] += pop;
}

struct step_job { const uint64_t *src; uint64_t *dst; };

static void step_item(uint32_t item, uint32_t me, void *arg)
{
    struct step_job *j = arg;
    step_band(j->src, j->dst, item, me);
}

uint64_t step(bool parallel)
{
    struct step_job j = { cur, old };
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        pop_by[i] = 0;
    uint32_t bands = (W + BAND - 1) / BAND;
    if (parallel) {
        pool_run(step_item, &j, bands);
    } else {
        for (uint32_t b = 0; b < bands; b++)
            step_band(j.src, j.dst, b, 0);
    }
    uint64_t *t = cur;
    cur = old;
    old = t;
    gen++;
    uint64_t pop = 0;
    for (uint32_t i = 0; i < FUN_MAX_THREADS; i++)
        pop += pop_by[i];
    return pop;
}

uint64_t population(const uint64_t *g)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < (uint64_t)W * WW; i++)
        n += popcount64(g[i]);
    return n;
}

/* Cell (x, y) dead, and old (its age bits all set). */
static void clear_cell(uint32_t x, uint32_t y)
{
    set(cur, x, y, false);
    set(old, x, y, false);
    for (int k = 0; k < 3; k++)
        set(age[k], x, y, true);
}

/* Cell (x, y) alive and new. */
static void birth_cell(uint32_t x, uint32_t y)
{
    set(cur, x, y, true);
    set(old, x, y, false);
    for (int k = 0; k < 3; k++)
        set(age[k], x, y, false);
}

void stamp(const char *const *rows, uint32_t nrows, uint32_t x, uint32_t y, uint32_t margin)
{
    uint32_t w = (uint32_t)strlen(rows[0]);
    for (uint32_t j = 0; j < nrows + 2 * margin; j++)
        for (uint32_t i = 0; i < w + 2 * margin; i++)
            clear_cell(x + i - margin, y + j - margin);
    for (uint32_t j = 0; j < nrows; j++)
        for (uint32_t i = 0; rows[j][i]; i++)
            if (rows[j][i] == 'O')
                birth_cell(x + i, y + j);
}

const char *const glider[3] = { ".O.", "..O", "OOO" };
const char *const rpent[3] = { ".OO", "OO.", ".O." };
const char *const gun[9] = {
    "........................O...........",
    "......................O.O...........",
    "............OO......OO............OO",
    "...........O...O....OO............OO",
    "OO........O.....O...OO..............",
    "OO........O...O.OO....O.O...........",
    "..........O.....O.......O...........",
    "...........O...O....................",
    "............OO......................",
};

uint64_t soup_seed = 0x5eed11fe;

static void soup_item(uint32_t item, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    uint64_t s = soup_seed ^ ((uint64_t)item * 0x9e3779b97f4a7c15ull) ^ 1;
    uint32_t y0 = item * BAND;
    for (uint32_t y = y0; y < y0 + BAND && y < W; y++)
        for (uint32_t w = 0; w < WW; w++) {
            uint64_t a = rng_next(&s), b = rng_next(&s), c = rng_next(&s), i = (uint64_t)y * WW + w;
            cur[i] = a & (b | c);   /* 37.5% alive */
            old[i] = 0;
            /* the living newborn (age 0), the dead long dead (age 7) */
            age[0][i] = age[1][i] = age[2][i] = ~cur[i];
        }
}

void soup(void)
{
    soup_seed = rng_next(&soup_seed);
    pool_run(soup_item, NULL, (W + BAND - 1) / BAND);
    gen = 0;
}

bool world_alloc(uint32_t size)
{
    W = size;
    WW = size / 64;
    uint64_t bytes = (uint64_t)W * WW * 8;
    cur = big_alloc(bytes);
    old = big_alloc(bytes);
    for (int k = 0; k < 3; k++)
        age[k] = big_alloc(bytes);
    gen = 0;
    return cur && old && age[0] && age[1] && age[2];
}

void world_clear(void)
{
    uint64_t bytes = (uint64_t)W * WW * 8;
    memset(cur, 0, bytes);
    memset(old, 0, bytes);
    for (int k = 0; k < 3; k++)
        memset(age[k], 0xff, bytes);
    gen = 0;
}
