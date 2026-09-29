/* life: Conway's Game of Life on a huge torus, computed and drawn by every CPU.
 *
 *   run life [size=8192] [threads=N]      play (size: a power of two >= 64)
 *   run life --selftest                   check the engine, exit 0 if right
 *
 * The world is size x size cells (8192 x 8192 = 67 million by default), one
 * bit a cell: a row is size / 64 words, and one generation computes 64
 * cells at a time with bitwise adders (each word's eight neighbour vectors
 * are the rows above, at and below, shifted one bit left and right with
 * the neighbouring words' edge bits carried in). Every generation is split
 * into bands of rows that the pool's threads (one per CPU) take one at a
 * time, so all CPUs share each generation.
 *
 * Ages: three more bit planes hold, per cell, a counter of the generations
 * since it last changed (0..7, saturating), kept with the same bitwise
 * tricks in the same pass (a few more operations per 64 cells). Newborn
 * cells are drawn white-hot and cool through yellow and green to blue as
 * they stay alive; cells that just died leave a fading purple trail.
 *
 * The screen (borrowed from the console, full resolution): zoomed in, each
 * cell is a crisp square of 2..32 pixels (with a dark gap from 4 up), or
 * one pixel; zoomed out, each pixel averages a block of 2x2 .. 16x16 cells
 * (coloured by how many are alive and how young they are), down to the
 * whole world at once (the torus repeats, dimmed). The view glides to where
 * the arrows send it. Every CPU draws its share of the rows. A map of the
 * whole world (bottom right) shows where the view is.
 *
 * Keys: arrows move, + / - zoom, space pause, n one step (paused), f / s
 * faster / slower, r a new random soup, c clear, g a Gosper glider gun and
 * p an R-pentomino at the centre of the view, a ages on / off, m the map,
 * h the help line, q or Esc quits. */
#include "../fun/fun.h"

#define BAND 8   /* rows per work item */

static uint32_t W, WW;        /* world size (cells) and words per row */
static uint64_t *cur, *old;   /* this generation and the one before */
static uint64_t *age[3];      /* the age counter's bits (see above) */
static bool ages = true;
static uint64_t gen;
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

/* One generation on the pool (threads > 1) or on this thread alone.
 * Returns the population. */
static uint64_t step(bool parallel)
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

static inline bool get(const uint64_t *g, uint32_t x, uint32_t y)
{
    x &= W - 1;
    y &= W - 1;
    return g[(uint64_t)y * WW + x / 64] >> (x % 64) & 1;
}

static inline void set(uint64_t *g, uint32_t x, uint32_t y, bool on)
{
    x &= W - 1;
    y &= W - 1;
    uint64_t *p = &g[(uint64_t)y * WW + x / 64], m = 1ull << (x % 64);
    *p = on ? *p | m : *p & ~m;
}

static inline uint32_t age_of(uint32_t x, uint32_t y)
{
    return (uint32_t)get(age[0], x, y) | (uint32_t)get(age[1], x, y) << 1 |
           (uint32_t)get(age[2], x, y) << 2;
}

static uint64_t population(const uint64_t *g)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < (uint64_t)W * WW; i++)
        n += popcount64(g[i]);
    return n;
}

/* A pattern in rows of '.' and 'O', its top-left corner at (x, y): newborn
 * cells, in a cleared space (margin cells all round) so it gets a start. */
static void stamp(const char *const *rows, uint32_t nrows, uint32_t x, uint32_t y, uint32_t margin)
{
    uint32_t w = (uint32_t)strlen(rows[0]);
    for (uint32_t j = 0; j < nrows + 2 * margin; j++)
        for (uint32_t i = 0; i < w + 2 * margin; i++) {
            set(cur, x + i - margin, y + j - margin, false);
            set(old, x + i - margin, y + j - margin, false);
            for (int k = 0; k < 3; k++)
                set(age[k], x + i - margin, y + j - margin, true);
        }
    for (uint32_t j = 0; j < nrows; j++)
        for (uint32_t i = 0; rows[j][i]; i++)
            if (rows[j][i] == 'O') {
                set(cur, x + i, y + j, true);
                set(old, x + i, y + j, false);
                for (int k = 0; k < 3; k++)
                    set(age[k], x + i, y + j, false);
            }
}

static const char *const glider[] = { ".O.", "..O", "OOO" };
static const char *const rpent[] = { ".OO", "OO.", ".O." };
static const char *const gun[] = {
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

static uint64_t soup_seed = 0x5eed11fe;

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

static void soup(void)
{
    soup_seed = rng_next(&soup_seed);
    pool_run(soup_item, NULL, (W + BAND - 1) / BAND);
    gen = 0;
}

static bool world_alloc(uint32_t size)
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

static void world_clear(void)
{
    uint64_t bytes = (uint64_t)W * WW * 8;
    memset(cur, 0, bytes);
    memset(old, 0, bytes);
    for (int k = 0; k < 3; k++)
        memset(age[k], 0xff, bytes);
    gen = 0;
}

/* ---- the self-test ---------------------------------------------------------------- */

static int failures;

static void check(bool ok, const char *what)
{
    say("life: selftest: %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static int selftest(void)
{
    uint32_t n = pool_start(0);
    say("life: selftest on %u threads (%u CPUs by CPUID)\n", n, fun_cpu_count());
    char what[96];

    /* A glider moves one cell down and right every 4 generations. */
    world_alloc(256);
    world_clear();
    stamp(glider, 3, 10, 10, 0);
    uint64_t pop = 0;
    for (int i = 0; i < 4; i++)
        pop = step(true);
    bool same = pop == 5;
    for (uint32_t j = 0; j < 3; j++)
        for (uint32_t i = 0; i < 3; i++)
            same &= get(cur, 11 + i, 11 + j) == (glider[j][i] == 'O');
    check(same, "glider: 4 generations = moved by (+1, +1), 5 cells");
    /* ... and wraps round the torus: after 4 * 256 generations it is back. */
    for (int i = 0; i < 4 * 256 - 4; i++)
        step(true);
    same = population(cur) == 5;
    for (uint32_t j = 0; j < 3; j++)
        for (uint32_t i = 0; i < 3; i++)
            same &= get(cur, 10 + i, 10 + j) == (glider[j][i] == 'O');
    check(same, "glider: 1024 generations on a 256-torus = back home");

    /* A blinker at the left/right word seam (x = 63, 64, 65). */
    world_clear();
    set(cur, 63, 50, true), set(cur, 64, 50, true), set(cur, 65, 50, true);
    step(true);
    bool ok = get(cur, 64, 49) && get(cur, 64, 50) && get(cur, 64, 51) && population(cur) == 3;
    step(true);
    ok &= get(cur, 63, 50) && get(cur, 64, 50) && get(cur, 65, 50) && population(cur) == 3;
    check(ok, "blinker across a word boundary: period 2");
    /* Ages: its middle never changes (7 for good after 7 generations); its
     * ends change every generation (0); a cell far away stays 7. */
    for (int i = 0; i < 8; i++)
        step(true);
    ok = age_of(64, 50) == 7 && age_of(63, 50) == 0 && age_of(64, 49) == 0 &&
         age_of(200, 200) == 7 && get(cur, 63, 50);
    step(true);
    ok &= age_of(64, 50) == 7 && age_of(63, 50) == 0 && !get(cur, 63, 50) && get(cur, 64, 49);
    check(ok, "ages: a blinker's middle is 7, its ends are always 0");
    world_clear();
    stamp(glider, 3, 100, 100, 0);
    for (int i = 0; i < 3; i++)
        step(true);
    ok = true;
    for (uint32_t y = 90; y < 120; y++)
        for (uint32_t x = 90; x < 120; x++)
            if (get(cur, x, y))
                ok &= age_of(x, y) <= 3;   /* a glider has no cell older than its period */
    check(ok && population(cur) == 5, "ages: a glider's cells are all young");

    /* The R-pentomino settles at generation 1103 with 116 cells. */
    world_alloc(1024);
    world_clear();
    stamp(rpent, 3, 512, 512, 0);
    /* It settles into 116 cells (6 of them gliders flying off) at generation
     * 1103: find the last generation whose population differs from gen 1500's. */
    uint64_t t0 = now_ns(), last_change = 0, last_pop = 0;
    while (gen < 1500) {
        pop = step(true);
        if (pop != last_pop)
            last_change = gen;
        last_pop = pop;
    }
    uint64_t ms = (now_ns() - t0) / 1000000;
    snprintf(what, sizeof(what), "R-pentomino: 116 cells for good from gen %lu (1103-ish)",
             (unsigned long)last_change);
    check(pop == 116 && last_change >= 1100 && last_change <= 1106, what);
    say("life: selftest: 1500 generations of 1024x1024 in %lu ms\n", (unsigned long)ms);

    /* Every CPU together computes exactly what one CPU does alone (ages too). */
    world_alloc(512);
    soup_seed = 12345;
    soup();
    uint64_t bytes = (uint64_t)W * WW * 8, words = bytes / 8;
    uint64_t *copy = big_alloc(bytes * 4);
    memcpy(copy, cur, bytes);
    for (int k = 0; k < 3; k++)
        memcpy(copy + (k + 1) * words, age[k], bytes);
    for (int i = 0; i < 64; i++)
        step(true);
    uint64_t *par = big_alloc(bytes * 4);
    memcpy(par, cur, bytes);
    for (int k = 0; k < 3; k++)
        memcpy(par + (k + 1) * words, age[k], bytes);
    memcpy(cur, copy, bytes);
    memset(old, 0, bytes);
    for (int k = 0; k < 3; k++)
        memcpy(age[k], copy + (k + 1) * words, bytes);
    for (int i = 0; i < 64; i++)
        step(false);
    ok = !memcmp(par, cur, bytes) && population(cur) > 1000;
    for (int k = 0; k < 3; k++)
        ok &= !memcmp(par + (k + 1) * words, age[k], bytes);
    check(ok, "random soup 512x512, 64 generations: all CPUs == one CPU");

    /* How much faster all CPUs are on a big world (information only). */
    world_alloc(4096);
    soup();
    t0 = now_ns();
    for (int i = 0; i < 4; i++)
        step(false);
    uint64_t one = now_ns() - t0;
    t0 = now_ns();
    for (int i = 0; i < 4; i++)
        step(true);
    uint64_t all = now_ns() - t0;
    ages = false;
    t0 = now_ns();
    for (int i = 0; i < 4; i++)
        step(true);
    uint64_t noage = now_ns() - t0;
    ages = true;
    uint64_t x10 = all ? one * 10 / all : 0;
    say("life: selftest: 4096x4096: %lu ms/gen on 1 CPU, %lu.%lu ms/gen on %u threads (%lu.%lux); "
        "%lu.%lu ms/gen without ages\n",
        (unsigned long)(one / 4000000), (unsigned long)(all / 4000000),
        (unsigned long)(all / 400000 % 10), n, (unsigned long)(x10 / 10),
        (unsigned long)(x10 % 10), (unsigned long)(noage / 4000000),
        (unsigned long)(noage / 400000 % 10));

    say("life: selftest %s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ---- drawing ---------------------------------------------------------------------- */

#define BG       0x05070d
#define GRID     0x0b0f1c

/* Colours by (alive, age): hot to cool; the dead fade out. */
static uint32_t col_alive[8], col_dead[8];
static uint32_t col_born, col_live, col_died;   /* without ages: from the last generation */
static uint32_t ramp[257];                      /* density views */

static int zl;                 /* zoom level: >= 0: 2^zl pixels a cell; < 0: 2^-zl cells a pixel */
static int zpx;                /* pixels per cell (zoomed in), else 1 */
static int kcells;             /* cells per pixel (zoomed out), else 1 */
static double vx, vy;          /* the view's centre (cells, not wrapped) */
static double tx, ty;          /* where it is gliding to */
static bool show_map = true, show_help = true;

static void make_colours(void)
{
    static const uint32_t a[8] = { 0xffffff, 0xfff4b8, 0xffd36a, 0xb8f060,
                                   0x5ee08a, 0x3cc4c0, 0x3c96e6, 0x4a70e8 };
    static const uint32_t d[8] = { 0x4a2868, 0x36204e, 0x261838, 0x1a1128,
                                   0x110c1c, 0x0a0914, BG, BG };
    for (int i = 0; i < 8; i++) {
        col_alive[i] = a[i];
        col_dead[i] = d[i];
    }
    col_born = 0xffffff;
    col_live = 0x5ee08a;
    col_died = 0x3a2050;
    static const struct { int at; uint32_t c; } stops[] = {
        { 0, BG }, { 24, 0x14205a }, { 70, 0x2050b0 }, { 120, 0x2aa0d8 }, { 170, 0x60e070 },
        { 215, 0xffe070 }, { 256, 0xffffff },
    };
    for (int i = 0, k = 0; i <= 256; i++) {
        while (i > stops[k + 1].at)
            k++;
        ramp[i] = mixc(stops[k].c, stops[k + 1].c,
                       (uint32_t)((i - stops[k].at) * 256 / (stops[k + 1].at - stops[k].at)));
    }
}

static inline uint32_t cell_colour(uint32_t y, uint32_t x)
{
    uint64_t i = (uint64_t)y * WW + x / 64;
    uint32_t b = x % 64;
    bool alive = cur[i] >> b & 1;
    if (!ages) {
        bool was = old[i] >> b & 1;
        return alive ? (was ? col_live : col_born) : was ? col_died : BG;
    }
    uint32_t a = (uint32_t)(age[0][i] >> b & 1) | (uint32_t)(age[1][i] >> b & 1) << 1 |
                 (uint32_t)(age[2][i] >> b & 1) << 2;
    return alive ? col_alive[a] : col_dead[a];
}

static inline uint32_t density_colour(uint32_t alive, uint32_t young, uint32_t cells)
{
    if (!alive)
        return BG;
    /* sqrt spreads the low densities (a settled soup is mostly ~0.03 .. 0.1) */
    uint32_t t = (uint32_t)(sqrtf_((float)alive / (float)cells * 1.2f) * 256);
    uint32_t c = ramp[t > 256 ? 256 : t < 24 ? 24 : t];
    return young ? mixc(c, 0xffd080, young * 100 / alive) : c;
}

/* The screen's top-left in world pixels (cells * zoom, or cells / zoom
 * zoomed out), and the world's copy the view centre is in (zoomed out so
 * far that the torus repeats on the screen; INT64_MIN: it doesn't). */
static int64_t org_x, org_y, home_x, home_y;

static void render_band(uint32_t band, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    const struct surf *s = &scr.s;
    int y0 = (int)band * 16, y1 = y0 + 16 < s->h ? y0 + 16 : s->h;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = s->px + (uint64_t)y * s->stride;
        if (kcells == 1) {
            int z = zpx, zs = __builtin_ctz((unsigned)z), gap = z >= 4 ? (z >= 16 ? 2 : 1) : 0;
            int64_t wy = org_y + y;
            uint32_t cy = (uint32_t)((wy >> zs) & (W - 1));
            if ((int)(wy & (z - 1)) >= z - gap) {
                for (int x = 0; x < s->w; x++)
                    row[x] = GRID;
                continue;
            }
            int x = 0;
            while (x < s->w) {
                int64_t wx = org_x + x;
                int inx = (int)(wx & (z - 1)), run = z - inx;
                if (run > s->w - x)
                    run = s->w - x;
                uint32_t c = cell_colour(cy, (uint32_t)((wx >> zs) & (W - 1)));
                int body = z - gap - inx;
                for (int i = 0; i < run; i++)
                    row[x + i] = i < body ? c : GRID;
                x += run;
            }
            continue;
        }
        /* Zoomed out: each pixel is k x k cells (k <= 16: in one word). */
        uint32_t k = (uint32_t)kcells;
        int64_t wy = org_y + y;
        uint32_t cy = (uint32_t)((wy * k) & (W - 1));
        bool other_y = home_y != INT64_MIN && (wy * k - home_y < 0 || wy * k - home_y >= W);
        uint64_t mask = (1ull << k) - 1;
        for (int x = 0; x < s->w; x++) {
            int64_t wx = org_x + x;
            uint32_t cx = (uint32_t)((wx * k) & (W - 1));
            uint32_t alive = 0, young = 0, word = cx / 64, sh = cx % 64;
            for (uint32_t j = 0; j < k; j++) {
                uint64_t i = (uint64_t)(cy + j) * WW + word;
                uint64_t bits = cur[i] >> sh & mask;
                alive += popcount64(bits);
                if (ages && bits)
                    young += popcount64(bits & ~((age[1][i] | age[2][i]) >> sh));
            }
            uint32_t c = density_colour(alive, young, k * k);
            bool other = other_y ||
                         (home_x != INT64_MIN && (wx * k - home_x < 0 || wx * k - home_x >= W));
            row[x] = other ? scalec(c, 80) : c;
        }
    }
}

/* The map: the whole world, each pixel a block of cells. */
static uint32_t *map_px;
static int map_n;           /* map_n x map_n pixels */
static uint64_t map_at;

static void map_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    uint32_t k = W / (uint32_t)map_n, dj = k >= 16 ? 2 : 1;   /* cells per map pixel */
    for (int x = 0; x < map_n; x++) {
        uint32_t alive = 0, cx = (uint32_t)x * k;
        for (uint32_t j = 0; j < k; j += dj) {   /* every other row when big */
            const uint64_t *r = cur + (uint64_t)(y * k + j) * WW;
            if (k >= 64) {
                for (uint32_t w = cx / 64; w < (cx + k) / 64; w++)
                    alive += popcount64(r[w]);
            } else {
                alive += popcount64(r[cx / 64] >> (cx % 64) & ((1ull << k) - 1));
            }
        }
        map_px[y * (uint32_t)map_n + (uint32_t)x] =
            alive ? density_colour(alive, 0, k * k / dj) : 0x080a14;
    }
}

static int map_x0, map_y0;

static void map_dot(int x, int y)
{
    x = ((x % map_n) + map_n) % map_n;
    y = ((y % map_n) + map_n) % map_n;
    scr.s.px[(uint64_t)(map_y0 + y) * scr.s.stride + map_x0 + x] = 0xffffff;
}

static void draw_map(void)
{
    const struct surf *s = &scr.s;
    int u = scr.ui, m = 12 * u;
    map_x0 = s->w - map_n - m;
    map_y0 = s->h - map_n - m - (show_help ? TEXT_H(u) + 16 * u : 0);
    panel(s, map_x0 - 5 * u, map_y0 - 5 * u, map_n + 10 * u, map_n + 10 * u, 6 * u, 0x000000, 180);
    struct surf mp = { map_px, map_n, map_n, map_n };
    blit(s, map_x0, map_y0, &mp, 0, 0, map_n, map_n);
    /* the view: a rectangle (wrapping round the edges) */
    double per = (double)W / map_n;
    double vw = zl >= 0 ? (double)s->w / zpx : (double)s->w * kcells;
    double vh = zl >= 0 ? (double)s->h / zpx : (double)s->h * kcells;
    if (vw >= W && vh >= W)
        return;
    int rw = (int)(vw / per), rh = (int)(vh / per);
    rw = rw > map_n ? map_n : rw < 3 ? 3 : rw;
    rh = rh > map_n ? map_n : rh < 3 ? 3 : rh;
    int rx = (int)floord((vx - vw / 2) / per), ry = (int)floord((vy - vh / 2) / per);
    for (int i = 0; i < rw; i++) {
        map_dot(rx + i, ry);
        map_dot(rx + i, ry + rh - 1);
    }
    for (int j = 0; j < rh; j++) {
        map_dot(rx, ry + j);
        map_dot(rx + rw - 1, ry + j);
    }
}

static int max_out(void)   /* the most zoomed-out level: the whole world in view */
{
    int z = 0;
    while ((W >> z) > (uint32_t)scr.h && z < 4)
        z++;
    return -z;
}

static void set_zoom(void)
{
    zpx = zl >= 0 ? 1 << zl : 1;
    kcells = zl < 0 ? 1 << -zl : 1;
}

static void render(void)
{
    const struct surf *s = &scr.s;
    if (zl >= 0) {
        org_x = (int64_t)floord(vx * zpx) - s->w / 2;
        org_y = (int64_t)floord(vy * zpx) - s->h / 2;
    } else {
        org_x = (int64_t)floord(vx / kcells) - s->w / 2;
        org_y = (int64_t)floord(vy / kcells) - s->h / 2;
    }
    home_x = home_y = INT64_MIN;
    if (zl < 0 && (int64_t)(W / (uint32_t)kcells) < s->w) {   /* the torus repeats: dim the copies */
        home_x = (int64_t)floord(vx) - W / 2;
        if ((int64_t)(W / (uint32_t)kcells) < s->h)
            home_y = (int64_t)floord(vy) - W / 2;
    }
    pool_run(render_band, NULL, (uint32_t)(s->h + 15) / 16);
}

/* ---- the game ---------------------------------------------------------------------- */

/* Generations a second; 0: as many as fit (MAX). */
static const uint32_t speeds[] = { 5, 15, 30, 60, 120, 240, 480, 960, 0 };
#define NSPEEDS (sizeof(speeds) / sizeof(speeds[0]))

static void hud(uint64_t pop, uint32_t gps, uint64_t gen_us, uint32_t speed, bool paused,
                uint32_t fps10)
{
    const struct surf *s = &scr.s;
    int u = scr.ui, pad = 10 * u;
    char a[32], b[32], line[160], z[24], sp[16], stats[256];
    snprintf(line, sizeof(line), "%u x %u torus:  \a%s\a cells", W, W,
             commas(a, sizeof(a), (uint64_t)W * W));
    if (zl >= 0)
        snprintf(z, sizeof(z), "%u px a cell", 1u << zl);
    else
        snprintf(z, sizeof(z), "%ux%u cells a px", 1u << -zl, 1u << -zl);
    if (paused)
        snprintf(sp, sizeof(sp), "paused");
    else if (speed)
        snprintf(sp, sizeof(sp), "%u/s", speed);
    else
        snprintf(sp, sizeof(sp), "MAX");
    snprintf(stats, sizeof(stats),
             "gen \a%s\a    alive \a%s\a    \a%u\a gen/s    \a%lu.%02lu ms\a a gen on \a%u\a CPUs"
             "    zoom \a%s\a    speed \a%s\a    \a%u.%u\a fps",
             commas(a, sizeof(a), gen), commas(b, sizeof(b), pop), gps,
             (unsigned long)(gen_us / 1000), (unsigned long)(gen_us % 1000 / 10), pool_threads(), z,
             sp, fps10 / 10, fps10 % 10);
    int w1 = text_width(3 * u, "LIFE") + pad + text_width(u, line), w2 = text_width(u, stats);
    int pw = (w1 > w2 ? w1 : w2) + 2 * pad, ph = TEXT_H(3 * u) + TEXT_H(u) + 3 * pad;
    panel(s, pad, pad, pw, ph, 8 * u, 0x000000, 170);
    int x = text_shadow(s, 2 * pad, 2 * pad, 3 * u, 0xa8f070, "LIFE");
    text2(s, x + pad, 2 * pad + TEXT_H(3 * u) - TEXT_H(u) - 3 * u, u, 0x8890b0, 0xe8ecff, false, line);
    text2(s, 2 * pad, 2 * pad + TEXT_H(3 * u) + pad, u, 0x8890b0, 0xffffff, false, stats);
    if (paused)
        text_shadow(s, (s->w - text_width(3 * u, "PAUSED")) / 2, 3 * pad, 3 * u, 0xffe070, "PAUSED");
    if (show_help) {
        const char *help = "arrows move    + - zoom    space pause    n step    f s faster / slower"
                           "    r new soup    c clear    g glider gun    p R-pentomino    a ages"
                           "    m map    h help    q quit";
        int hw = text_width(u, help) + 2 * pad, hh = TEXT_H(u) + pad;
        panel(s, (s->w - hw) / 2, s->h - hh - pad, hw, hh, 6 * u, 0x000000, 160);
        text(s, (s->w - hw) / 2 + pad, s->h - hh - pad + pad / 2, u, 0xb0b8d8, help);
    }
}

static int play(int argc, char **argv)
{
    uint32_t size = (uint32_t)arg_num(argc, argv, "size", 8192);
    if (size < 64 || size > 32768 || (size & (size - 1))) {
        say("life: size must be a power of two, 64 .. 32768\n");
        return 2;
    }
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    if (!world_alloc(size)) {
        say("life: not enough memory for %ux%u\n", size, size);
        return 1;
    }
    status_t st = gfx_open();
    if (st != OK) {
        say("life: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    make_colours();
    map_n = 256 * scr.ui;
    while ((uint32_t)map_n > W || map_n > scr.h / 4)
        map_n /= 2;
    map_px = big_alloc((uint64_t)map_n * map_n * 4);
    soup();
    vx = tx = W / 2.0;
    vy = ty = W / 2.0;
    zl = scr.ui > 1 ? 2 : 1;
    set_zoom();
    uint32_t si = 3;   /* 60 generations a second */
    bool paused = false, quit = false, dirty = true;
    uint64_t pop = population(cur), gens_total = 0, compute_ns = 0;
    uint64_t win_t = now_ns(), win_gen = 0, gen_us = 0, last = now_ns();
    uint32_t gps = 0;
    double owed = 0;   /* generations due (fractions carry over) */
    struct fps fps = { 0 };
    const uint64_t frame_ns = 16666667;
    while (!quit) {
        uint64_t t0 = now_ns();
        double dt = (double)(t0 - last) / 1e9;
        last = t0;
        if (dt > 0.25)
            dt = 0.25;
        if (!paused) {
            uint32_t n = ~0u;
            if (speeds[si]) {
                owed += speeds[si] * dt;
                n = (uint32_t)owed;
                owed -= n;
            }
            for (uint32_t i = 0; i < n; i++) {
                uint64_t s0 = now_ns();
                pop = step(pool_threads() > 1);
                uint64_t d = now_ns() - s0;
                compute_ns += d;
                gens_total++;
                gen_us = gen_us ? (gen_us * 7 + d / 1000) / 8 : d / 1000;
                dirty = true;
                if (now_ns() - t0 > frame_ns * 6 / 10) {   /* keep the frame rate */
                    owed = 0;
                    break;
                }
            }
        }
        if (now_ns() - win_t >= 1000000000ull) {
            gps = (uint32_t)((gen - win_gen) * 1000000000ull / (now_ns() - win_t));
            win_t = now_ns();
            win_gen = gen;
        }
        /* The glide: most of the way in a few frames. */
        if (vx != tx || vy != ty) {
            double f = 1 - exp2d(-dt * 16);
            vx += (tx - vx) * f;
            vy += (ty - vy) * f;
            double px = zl >= 0 ? 1.0 / zpx : kcells;   /* a pixel, in cells */
            if ((tx - vx) * (tx - vx) + (ty - vy) * (ty - vy) < px * px / 16) {
                vx = tx;
                vy = ty;
            }
            dirty = true;
        }
        if (dirty) {
            render();
            if (show_map) {
                if (t0 - map_at > 250000000ull || !map_at) {
                    pool_run(map_row, NULL, (uint32_t)map_n);
                    map_at = t0;
                }
                draw_map();
            }
            fps_frame(&fps);
            hud(pop, paused ? 0 : gps, gen_us, speeds[si], paused, fps.x10);
            gfx_present();
            dirty = false;
        }
        /* Keys until the next frame (or until a key when nothing moves). */
        bool moving = !paused || vx != tx || vy != ty;
        int k = gfx_key(moving ? t0 + frame_ns : DEADLINE_NEVER);
        for (; k != KEY_NONE && !quit; k = gfx_key(0)) {
            double spanx = zl >= 0 ? (double)scr.w / zpx : (double)scr.w * kcells;
            double spany = zl >= 0 ? (double)scr.h / zpx : (double)scr.h * kcells;
            uint32_t cx = (uint32_t)(int64_t)floord(vx), cy = (uint32_t)(int64_t)floord(vy);
            dirty = true;
            switch (k) {
            case KEY_QUIT: case 'q': case 'Q': quit = true; break;
            case KEY_LEFT:  tx -= spanx / 4; break;
            case KEY_RIGHT: tx += spanx / 4; break;
            case KEY_UP:    ty -= spany / 4; break;
            case KEY_DOWN:  ty += spany / 4; break;
            case '+': case '=': case KEY_PGUP: if (zl < 5) zl++; break;
            case '-': case '_': case KEY_PGDN: if (zl > max_out()) zl--; break;
            case ' ': paused = !paused; break;
            case 'n': if (paused) pop = step(pool_threads() > 1); break;
            case 'f': if (si + 1 < NSPEEDS) si++; break;
            case 's': if (si) si--; break;
            case 'r': soup(); pop = population(cur); map_at = 0; break;
            case 'c': world_clear(); pop = 0; map_at = 0; break;
            case 'g': stamp(gun, 9, cx - 18, cy - 4, 24); pop = population(cur); break;
            case 'p': stamp(rpent, 3, cx - 1, cy - 1, 24); pop = population(cur); break;
            case 'a':
                ages = !ages;
                if (ages)   /* everything starts old: what is born from now on is new */
                    for (int j = 0; j < 3; j++)
                        memset(age[j], 0xff, (uint64_t)W * WW * 8);
                break;
            case 'm': show_map = !show_map; break;
            case 'h': show_help = !show_help; break;
            default: dirty = false;
            }
            set_zoom();
        }
        /* Keep the view's centre near the world (it wraps). */
        double wrap = floord(vx / W) * W;
        vx -= wrap;
        tx -= wrap;
        wrap = floord(vy / W) * W;
        vy -= wrap;
        ty -= wrap;
    }
    gfx_close();
    char a[32], b[32];
    uint64_t avg_us = gens_total ? compute_ns / gens_total / 1000 : 0;
    say("life: %s generations of a %ux%u world (%s cells) on %u threads, %lu.%02lu ms/gen, "
        "%u.%u fps\n",
        commas(a, sizeof(a), gens_total), W, W, commas(b, sizeof(b), (uint64_t)W * W),
        pool_threads(), (unsigned long)(avg_us / 1000), (unsigned long)(avg_us % 1000 / 10),
        fps.x10 / 10, fps.x10 % 10);
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return selftest();
    return play(argc, argv);
}
