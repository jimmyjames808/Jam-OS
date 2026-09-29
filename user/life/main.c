/* life: Conway's Game of Life on a huge torus, computed by every CPU.
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
 * The screen is a window onto the world in the console's alternate screen:
 * each text cell holds two square "pixels" (an upper half block in one
 * colour over the other). Zoomed in, a pixel is a cell: newborn white,
 * living green, just died blue. Zoomed out, a pixel is a block of cells
 * coloured by how many are alive, so the whole world fits on the screen.
 *
 * Keys: arrows move, + / - zoom, space pause, n one step (paused), f / s
 * faster / slower, r a new random soup, c clear, g a Gosper glider gun and
 * p an R-pentomino at the centre of the view, q or Esc quits. */
#include "../fun/fun.h"

#define BAND 8   /* rows per work item */

static uint32_t W, WW;        /* world size (cells) and words per row */
static uint64_t *cur, *old;   /* this generation and the one before */
static uint64_t gen;
static uint64_t pop_by[FUN_MAX_THREADS];

/* ---- the engine ------------------------------------------------------------------ */

static inline uint64_t cell_word(const uint64_t *g, uint32_t y, uint32_t w)
{
    return g[(uint64_t)y * WW + w];
}

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

static uint64_t population(const uint64_t *g)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < (uint64_t)W * WW; i++)
        n += popcount64(g[i]);
    return n;
}

/* A pattern in rows of '.' and 'O', its top-left corner at (x, y). */
static void stamp(const char *const *rows, uint32_t nrows, uint32_t x, uint32_t y)
{
    for (uint32_t j = 0; j < nrows; j++)
        for (uint32_t i = 0; rows[j][i]; i++)
            if (rows[j][i] == 'O') {
                set(cur, x + i, y + j, true);
                set(old, x + i, y + j, false);
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
            uint64_t a = rng_next(&s), b = rng_next(&s), c = rng_next(&s);
            cur[(uint64_t)y * WW + w] = a & (b | c);   /* 37.5% alive */
            old[(uint64_t)y * WW + w] = 0;
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
    return cur && old;
}

static void world_clear(void)
{
    memset(cur, 0, (uint64_t)W * WW * 8);
    memset(old, 0, (uint64_t)W * WW * 8);
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
    stamp(glider, 3, 10, 10);
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

    /* The R-pentomino settles at generation 1103 with 116 cells. */
    world_alloc(1024);
    stamp(rpent, 3, 512, 512);
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

    /* Every CPU together computes exactly what one CPU does alone. */
    world_alloc(512);
    soup_seed = 12345;
    soup();
    uint64_t *copy = big_alloc((uint64_t)W * WW * 8);
    memcpy(copy, cur, (uint64_t)W * WW * 8);
    for (int i = 0; i < 64; i++)
        step(true);
    uint64_t *par = big_alloc((uint64_t)W * WW * 8);
    memcpy(par, cur, (uint64_t)W * WW * 8);
    memcpy(cur, copy, (uint64_t)W * WW * 8);
    for (int i = 0; i < 64; i++)
        step(false);
    check(!memcmp(par, cur, (uint64_t)W * WW * 8) && population(cur) > 1000,
          "random soup 512x512, 64 generations: all CPUs == one CPU");

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
    uint64_t x10 = all ? one * 10 / all : 0;
    say("life: selftest: 4096x4096: %lu ms/gen on 1 CPU, %lu.%lu ms/gen on %u threads (%lu.%lux)\n",
        (unsigned long)(one / 4000000), (unsigned long)(all / 4000000),
        (unsigned long)(all / 400000 % 10), n, (unsigned long)(x10 / 10),
        (unsigned long)(x10 % 10));

    say("life: selftest %s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ---- the view ---------------------------------------------------------------------- */

static struct term T;
static int zoom;               /* < 0: 2^-zoom pixels per cell; >= 0: 2^zoom cells per pixel */
static uint32_t vx, vy;        /* the world cell at the view's centre */
static uint32_t PW, PH;        /* the view in pixels (2 per text row) */
#define TOP 1                  /* text rows above the view */

/* Alive cells in [x, x + n) of row y (wrapping), n <= W. */
static uint32_t count_run(const uint64_t *g, uint32_t y, uint32_t x, uint32_t n)
{
    const uint64_t *row = g + (uint64_t)(y & (W - 1)) * WW;
    uint32_t total = 0;
    x &= W - 1;
    while (n) {
        uint32_t bit = x % 64, take = 64 - bit < n ? 64 - bit : n;
        uint64_t word = row[x / 64] >> bit;
        if (take < 64)
            word &= (1ull << take) - 1;
        total += popcount64(word);
        n -= take;
        x = (x + take) & (W - 1);
    }
    return total;
}

static uint8_t density_colour(uint32_t alive, uint64_t cells)
{
    if (!alive)
        return C_BLACK;
    uint64_t pm = (uint64_t)alive * 1000 / cells;   /* per mille */
    static const struct { uint16_t below; uint8_t c; } ramp[] = {
        { 15, C_BLUE }, { 30, C_BBLUE }, { 50, C_CYAN }, { 80, C_BCYAN }, { 150, C_GREEN },
        { 300, C_BGREEN }, { 500, C_BYELLOW },
    };
    for (unsigned i = 0; i < sizeof(ramp) / sizeof(ramp[0]); i++)
        if (pm < ramp[i].below)
            return ramp[i].c;
    return C_WHITE;
}

static uint8_t pixel(uint32_t px, uint32_t py)
{
    int32_t cx = (int32_t)px - (int32_t)PW / 2, cy = (int32_t)py - (int32_t)PH / 2;
    if (zoom <= 0) {
        int s = -zoom;
        /* floor division by 2^s */
        uint32_t x = vx + (uint32_t)(cx >> s), y = vy + (uint32_t)(cy >> s);
        bool c = get(cur, x, y), o = get(old, x, y);
        return c ? (o ? C_BGREEN : C_WHITE) : o ? C_BLUE : C_BLACK;
    }
    uint32_t z = 1u << zoom;
    uint32_t x = vx + (uint32_t)(cx * (int32_t)z), y = vy + (uint32_t)(cy * (int32_t)z);
    uint32_t alive = 0;
    for (uint32_t j = 0; j < z; j++)
        alive += count_run(cur, y + j, x, z);
    return density_colour(alive, (uint64_t)z * z);
}

static void render_row(uint32_t row, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    for (uint32_t x = 0; x < PW; x++) {
        uint8_t top = pixel(x, 2 * row), bot = pixel(x, 2 * row + 1);
        if (top == bot)
            term_put(&T, (int)x, (int)(TOP + row), ' ', top, top);
        else
            term_put(&T, (int)x, (int)(TOP + row), G_UPPER, top, bot);
    }
}

static int max_zoom(void)
{
    int z = 0;
    while (((PW << z) < W || (PH << z) < W) && z < 12)   /* until the whole world shows */
        z++;
    return z;
}

/* ---- the game ---------------------------------------------------------------------- */

static const uint32_t speeds[] = { 1, 2, 4, 8, 16, 32, 64, 0 };   /* gens per frame; 0: max */
#define FRAME_NS 33000000ull

static void hud(uint64_t pop, uint32_t gps, uint64_t gen_us, uint32_t speed, bool paused)
{
    char a[32], b[32];
    term_fill(&T, 0, 0, (int)T.cols, 1, ' ', C_WHITE, C_BLUE);
    int x = term_text(&T, 1, 0, "JAM OS LIFE", C_BYELLOW, C_BLUE);
    x = term_textf(&T, x + 2, 0, C_WHITE, C_BLUE, "%ux%u torus = %s cells", W, W,
                   commas(a, sizeof(a), (uint64_t)W * W));
    x = term_textf(&T, x + 3, 0, C_BCYAN, C_BLUE, "gen %s", commas(b, sizeof(b), gen));
    x = term_textf(&T, x + 3, 0, C_BGREEN, C_BLUE, "alive %s", commas(a, sizeof(a), pop));
    x = term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "%u gen/s", gps);
    x = term_textf(&T, x + 3, 0, C_BMAGENTA, C_BLUE, "%u CPUs: %lu.%02lu ms/gen", pool_threads(),
                   (unsigned long)(gen_us / 1000), (unsigned long)(gen_us % 1000 / 10));
    if (zoom <= 0)
        x = term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "zoom %ux", 1u << -zoom);
    else
        x = term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "zoom 1/%u", 1u << zoom);
    if (paused)
        term_text(&T, x + 3, 0, "PAUSED", C_BYELLOW, C_RED);
    else if (speed)
        term_textf(&T, x + 3, 0, C_WHITE, C_BLUE, "x%u", speed);
    else
        term_text(&T, x + 3, 0, "MAX", C_BYELLOW, C_BLUE);
    int y = (int)T.rows - 1;
    term_fill(&T, 0, y, (int)T.cols, 1, ' ', C_GREY, C_BLACK);
    term_text(&T, 1, y,
              "arrows move   + - zoom   space pause   n step   f s faster/slower   "
              "r new soup   c clear   g glider gun   p R-pentomino   q quit",
              C_GREY, C_BLACK);
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
    status_t st = term_open(&T);
    if (st != OK) {
        say("life: no console screen (%s)\n", status_str(st));
        return 1;
    }
    PW = T.cols;
    PH = (T.rows - 2) * 2;
    soup();
    vx = vy = W / 2;
    zoom = 0;
    uint32_t si = 2;          /* x4 */
    bool paused = false, quit = false;
    uint64_t pop = population(cur), gens_total = 0, compute_ns = 0;
    uint64_t win_t = now_ns(), win_gen = 0, gen_us = 0;
    uint32_t gps = 0;
    while (!quit) {
        uint64_t t0 = now_ns();
        if (!paused) {
            uint32_t n = speeds[si];
            for (uint32_t i = 0; n ? i < n : now_ns() - t0 < 25000000ull; i++) {
                uint64_t s0 = now_ns();
                pop = step(pool_threads() > 1);
                uint64_t d = now_ns() - s0;
                compute_ns += d;
                gens_total++;
                gen_us = gen_us ? (gen_us * 7 + d / 1000) / 8 : d / 1000;
            }
        }
        if (now_ns() - win_t >= 1000000000ull) {
            gps = (uint32_t)((gen - win_gen) * 1000000000ull / (now_ns() - win_t));
            win_t = now_ns();
            win_gen = gen;
        }
        pool_run(render_row, NULL, PH / 2);
        hud(pop, paused ? 0 : gps, gen_us, speeds[si], paused);
        term_flush(&T);
        /* Keys until the frame is up. */
        int k;
        while ((k = term_key(&T, t0 + FRAME_NS)) != KEY_NONE) {
            uint32_t span = zoom <= 0 ? PW >> -zoom : PW << zoom;
            uint32_t vspan = zoom <= 0 ? PH >> -zoom : PH << zoom;
            switch (k) {
            case KEY_QUIT: case 'q': case 'Q': quit = true; break;
            case KEY_LEFT:  vx = (vx - span / 4) & (W - 1); break;
            case KEY_RIGHT: vx = (vx + span / 4) & (W - 1); break;
            case KEY_UP:    vy = (vy - vspan / 4) & (W - 1); break;
            case KEY_DOWN:  vy = (vy + vspan / 4) & (W - 1); break;
            case '+': case '=': if (zoom > -3) zoom--; break;
            case '-': case '_': if (zoom < max_zoom()) zoom++; break;
            case ' ': paused = !paused; break;
            case 'n': if (paused) pop = step(pool_threads() > 1); break;
            case 'f': if (si + 1 < sizeof(speeds) / sizeof(speeds[0])) si++; break;
            case 's': if (si) si--; break;
            case 'r': soup(); pop = population(cur); break;
            case 'c': world_clear(); pop = 0; break;
            case 'g': stamp(gun, 9, vx - 18, vy - 4); pop = population(cur); break;
            case 'p': stamp(rpent, 3, vx - 1, vy - 1); pop = population(cur); break;
            }
            if (quit)
                break;
        }
    }
    term_close(&T);
    char a[32], b[32];
    uint64_t avg_us = gens_total ? compute_ns / gens_total / 1000 : 0;
    say("life: %s generations of a %ux%u world (%s cells) on %u threads, %lu.%02lu ms/gen\n",
        commas(a, sizeof(a), gens_total), W, W, commas(b, sizeof(b), (uint64_t)W * W),
        pool_threads(), (unsigned long)(avg_us / 1000), (unsigned long)(avg_us % 1000 / 10));
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return selftest();
    return play(argc, argv);
}
