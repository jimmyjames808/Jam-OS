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
#include "life.h"

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

/* Screen row y (w pixels at row) zoomed in: each cell zpx x zpx pixels,
 * with grid lines between them from 4 pixels a cell. */
static void row_zoomed_in(uint32_t *row, int y, int w)
{
    int z = zpx, zs = __builtin_ctz((unsigned)z), gap = z >= 4 ? (z >= 16 ? 2 : 1) : 0;
    int64_t wy = org_y + y;
    uint32_t cy = (uint32_t)((wy >> zs) & (W - 1));
    if ((int)(wy & (z - 1)) >= z - gap) {
        for (int x = 0; x < w; x++)
            row[x] = GRID;
        return;
    }
    int x = 0;
    while (x < w) {
        int64_t wx = org_x + x;
        int inx = (int)(wx & (z - 1)), run = z - inx;
        if (run > w - x)
            run = w - x;
        uint32_t c = cell_colour(cy, (uint32_t)((wx >> zs) & (W - 1)));
        int body = z - gap - inx;
        for (int i = 0; i < run; i++)
            row[x + i] = i < body ? c : GRID;
        x += run;
    }
}

/* The live and young cells of the k x k block at column cx (in one word:
 * k <= 16) from row cy down, into *alive and *young. */
static void block_count(uint32_t cx, uint32_t cy, uint32_t k, uint32_t *alive, uint32_t *young)
{
    uint64_t mask = (1ull << k) - 1;
    uint32_t word = cx / 64, sh = cx % 64;
    for (uint32_t j = 0; j < k; j++) {
        uint64_t i = (uint64_t)(cy + j) * WW + word;
        uint64_t bits = cur[i] >> sh & mask;
        *alive += popcount64(bits);
        if (ages && bits)
            *young += popcount64(bits & ~((age[1][i] | age[2][i]) >> sh));
    }
}

/* Screen row y zoomed out: each pixel is k x k cells (k <= 16: in one
 * word), coloured by how many live; another copy of the torus than the
 * one the view centre is in is dimmed. */
static void row_zoomed_out(uint32_t *row, int y, int w)
{
    uint32_t k = (uint32_t)kcells;
    int64_t wy = org_y + y;
    uint32_t cy = (uint32_t)((wy * k) & (W - 1));
    bool other_y = home_y != INT64_MIN && (wy * k - home_y < 0 || wy * k - home_y >= W);
    for (int x = 0; x < w; x++) {
        int64_t wx = org_x + x;
        uint32_t cx = (uint32_t)((wx * k) & (W - 1));
        uint32_t alive = 0, young = 0;
        block_count(cx, cy, k, &alive, &young);
        uint32_t c = density_colour(alive, young, k * k);
        bool other = other_y ||
                     (home_x != INT64_MIN && (wx * k - home_x < 0 || wx * k - home_x >= W));
        row[x] = other ? scalec(c, 80) : c;
    }
}

static void render_band(uint32_t band, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    const struct surf *s = &scr.s;
    int y0 = (int)band * 16, y1 = y0 + 16 < s->h ? y0 + 16 : s->h;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = s->px + (uint64_t)y * s->stride;
        if (kcells == 1)
            row_zoomed_in(row, y, s->w);
        else
            row_zoomed_out(row, y, s->w);
    }
}

/* The map: the whole world, each pixel a block of cells. */
static uint32_t *map_px;
static int map_n;           /* map_n x map_n pixels */
static uint64_t map_at;

/* The live cells of map row y's k x k block at column cx (every dj-th
 * row of it). */
static uint32_t map_block_alive(uint32_t y, uint32_t cx, uint32_t k, uint32_t dj)
{
    uint32_t alive = 0;
    for (uint32_t j = 0; j < k; j += dj) {   /* every other row when big */
        const uint64_t *r = cur + (uint64_t)(y * k + j) * WW;
        if (k >= 64) {
            for (uint32_t w = cx / 64; w < (cx + k) / 64; w++)
                alive += popcount64(r[w]);
        } else {
            alive += popcount64(r[cx / 64] >> (cx % 64) & ((1ull << k) - 1));
        }
    }
    return alive;
}

static void map_row(uint32_t y, uint32_t me, void *arg)
{
    (void)me;
    (void)arg;
    uint32_t k = W / (uint32_t)map_n, dj = k >= 16 ? 2 : 1;   /* cells per map pixel */
    for (int x = 0; x < map_n; x++) {
        uint32_t cx = (uint32_t)x * k;
        uint32_t alive = map_block_alive(y, cx, k, dj);
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

#define FRAME_NS 16666667ull

/* The game between frames. */
static struct {
    uint32_t si;                  /* speeds[si] */
    bool     paused, quit, dirty; /* dirty: draw a new frame */
    uint64_t pop;                 /* the population now */
    double   owed;                /* generations due (fractions carry over) */
    uint64_t gens, compute_ns;    /* generations computed, and their time (the summary) */
    uint64_t gen_us;              /* one generation's time, smoothed (the HUD) */
    uint64_t win_t, win_gen;      /* generations a second, counted over a second */
    uint32_t gps;                 /* the last second's generations */
    struct fps fps;               /* frames a second (the HUD) */
} game = { .si = 3, .dirty = true };   /* 60 generations a second */

/* The world, the screen, the colours, the map and the view. Returns 0, or
 * the exit code when something is missing. */
static int start(int argc, char **argv)
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
    game.pop = population(cur);
    return 0;
}

/* The generations due since the last frame at the chosen speed, but no
 * more than fit in 60% of a frame (t0: when the frame began). */
static void advance(uint64_t t0, double dt)
{
    if (!game.paused) {
        uint32_t n = ~0u;
        if (speeds[game.si]) {
            game.owed += speeds[game.si] * dt;
            n = (uint32_t)game.owed;
            game.owed -= n;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint64_t s0 = now();
            game.pop = step(pool_threads() > 1);
            uint64_t d = now() - s0;
            game.compute_ns += d;
            game.gens++;
            game.gen_us = game.gen_us ? (game.gen_us * 7 + d / 1000) / 8 : d / 1000;
            game.dirty = true;
            if (now() - t0 > FRAME_NS * 6 / 10) {   /* keep the frame rate */
                game.owed = 0;
                break;
            }
        }
    }
    if (now() - game.win_t >= NS_PER_S) {
        game.gps = (uint32_t)((gen - game.win_gen) * NS_PER_S / (now() - game.win_t));
        game.win_t = now();
        game.win_gen = gen;
    }
}

/* The view glides to where the arrows sent it: most of the way in a few frames. */
static void glide(double dt)
{
    if (vx == tx && vy == ty)
        return;
    double f = 1 - exp2d(-dt * 16);
    vx += (tx - vx) * f;
    vy += (ty - vy) * f;
    double px = zl >= 0 ? 1.0 / zpx : kcells;   /* a pixel, in cells */
    if ((tx - vx) * (tx - vx) + (ty - vy) * (ty - vy) < px * px / 16) {
        vx = tx;
        vy = ty;
    }
    game.dirty = true;
}

static void redraw(uint64_t t0)
{
    render();
    if (show_map) {
        if (t0 - map_at > 250000000ull || !map_at) {
            pool_run(map_row, NULL, (uint32_t)map_n);
            map_at = t0;
        }
        draw_map();
    }
    fps_frame(&game.fps);
    hud(game.pop, game.paused ? 0 : game.gps, game.gen_us, speeds[game.si], game.paused,
        game.fps.x10);
    gfx_present();
    game.dirty = false;
}

static void handle_key(int k)
{
    double spanx = zl >= 0 ? (double)scr.w / zpx : (double)scr.w * kcells;
    double spany = zl >= 0 ? (double)scr.h / zpx : (double)scr.h * kcells;
    uint32_t cx = (uint32_t)(int64_t)floord(vx), cy = (uint32_t)(int64_t)floord(vy);
    game.dirty = true;
    switch (k) {
    case KEY_QUIT: case 'q': case 'Q': game.quit = true; break;
    case KEY_LEFT:  tx -= spanx / 4; break;
    case KEY_RIGHT: tx += spanx / 4; break;
    case KEY_UP:    ty -= spany / 4; break;
    case KEY_DOWN:  ty += spany / 4; break;
    case '+': case '=': case KEY_PGUP: if (zl < 5) zl++; break;
    case '-': case '_': case KEY_PGDN: if (zl > max_out()) zl--; break;
    case ' ': game.paused = !game.paused; break;
    case 'n': if (game.paused) game.pop = step(pool_threads() > 1); break;
    case 'f': if (game.si + 1 < NSPEEDS) game.si++; break;
    case 's': if (game.si) game.si--; break;
    case 'r': soup(); game.pop = population(cur); map_at = 0; break;
    case 'c': world_clear(); game.pop = 0; map_at = 0; break;
    case 'g': stamp(gun, 9, cx - 18, cy - 4, 24); game.pop = population(cur); break;
    case 'p': stamp(rpent, 3, cx - 1, cy - 1, 24); game.pop = population(cur); break;
    case 'a':
        ages = !ages;
        if (ages)   /* everything starts old: what is born from now on is new */
            for (int j = 0; j < 3; j++)
                memset(age[j], 0xff, (uint64_t)W * WW * 8);
        break;
    case 'm': show_map = !show_map; break;
    case 'h': show_help = !show_help; break;
    default: game.dirty = false;
    }
    set_zoom();
}

/* Keep the view's centre near the world (it wraps). */
static void wrap_view(void)
{
    double wrap = floord(vx / W) * W;
    vx -= wrap;
    tx -= wrap;
    wrap = floord(vy / W) * W;
    vy -= wrap;
    ty -= wrap;
}

static int play(int argc, char **argv)
{
    int r = start(argc, argv);
    if (r)
        return r;
    uint64_t last = now();
    game.win_t = last;
    while (!game.quit) {
        uint64_t t0 = now();
        double dt = (double)(t0 - last) / 1e9;
        last = t0;
        if (dt > 0.25)
            dt = 0.25;
        advance(t0, dt);
        glide(dt);
        if (game.dirty)
            redraw(t0);
        /* Keys until the next frame (or until a key when nothing moves). */
        bool moving = !game.paused || vx != tx || vy != ty;
        int k = gfx_key(moving ? t0 + FRAME_NS : DEADLINE_NEVER);
        for (; k != KEY_NONE && !game.quit; k = gfx_key(0))
            handle_key(k);
        wrap_view();
    }
    gfx_close();
    char a[32], b[32];
    uint64_t avg_us = game.gens ? game.compute_ns / game.gens / 1000 : 0;
    say("life: %s generations of a %ux%u world (%s cells) on %u threads, %lu.%02lu ms/gen, "
        "%u.%u fps\n",
        commas(a, sizeof(a), game.gens), W, W, commas(b, sizeof(b), (uint64_t)W * W),
        pool_threads(), (unsigned long)(avg_us / 1000), (unsigned long)(avg_us % 1000 / 10),
        game.fps.x10 / 10, game.fps.x10 % 10);
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return life_selftest();
    return play(argc, argv);
}
