/* tetris: the picture (tetris.h). */
#include "tetris.h"

/* ---- drawing -------------------------------------------------------------------------- */

#define KEY_PX    0x010203       /* transparent pixels in the block sprites */
#define CLEAR_NS  260000000ull   /* the line-clear animation */
#define FLASH_NS  150000000ull   /* ... its flash; then the rows above slide down */
#define LOCK_FLASH_NS 140000000ull
#define TRAIL_NS  220000000ull

static int B;                    /* block size in pixels */
static int wx, wy;               /* the well's top-left on screen */
static int lx, rx, pw;           /* the side panels: left x, right x, width */
static int ls, ns, hs;           /* text scales: labels, numbers, headings */
static int hold_y, hold_h, stat_y, stat_h, next_y, next_h, help_y;
static struct surf bg;           /* everything that doesn't move, drawn once */
enum { SZ_WELL, SZ_BIG, SZ_SMALL, NSIZES };
static struct surf sprite[NSIZES][NPIECES];
static int sprite_px[NSIZES];

static const uint32_t piece_rgb[NPIECES] = {
    [I] = 0x2ec8f0, [O] = 0xf5cc1a, [T] = 0xb052d8, [S] = 0x4cd04a,
    [Z] = 0xf0443c, [J] = 0x4070ec, [L] = 0xf58a28,
};

/* A bevelled block b x b (its last row and column left transparent: the
 * gap between blocks). */
/* Pixel (x, y) of a block of colour c: the face n x n (n = b - 1), a
 * bevel e wide. */
static uint32_t block_px(int x, int y, int n, int e, uint32_t c)
{
    if (x == n || y == n)
        return KEY_PX;
    /* The face: a gentle top-to-bottom gradient. */
    uint32_t col = mixc(mixc(c, 0xffffff, 36), scalec(c, 200), (uint32_t)(y * 256 / n));
    int dt = y, dl = x, db = n - 1 - y, dr = n - 1 - x;
    int m = dt < dl ? dt : dl;
    m = db < m ? db : m;
    m = dr < m ? dr : m;
    if (m < e) {   /* the bevel: lit from the top left */
        if (m == dt && dt <= dr)
            col = mixc(c, 0xffffff, 120);
        else if (m == dl && dl <= db)
            col = mixc(c, 0xffffff, 64);
        else if (m == db)
            col = scalec(c, 120);
        else
            col = scalec(c, 160);
    } else if (y < e + (n - 2 * e) / 3 && x < n - e - 1) {
        /* a soft shine across the top of the face */
        uint32_t k = (uint32_t)(y - e) * 256 / (uint32_t)((n - 2 * e) / 3 + 1);
        col = mixc(col, 0xffffff, 40 - k * 40 / 256);
    }
    return col;
}

static struct surf make_block(int b, uint32_t c)
{
    struct surf s = surf_new(b, b);
    if (!s.px)
        return s;
    int n = b - 1, e = b >= 30 ? b / 8 : b >= 14 ? 3 : 2;
    for (int y = 0; y < b; y++)
        for (int x = 0; x < b; x++)
            s.px[y * b + x] = block_px(x, y, n, e, c);
    return s;
}

static inline double ease_out(double t)   /* 0..1 */
{
    if (t <= 0)
        return 0;
    if (t >= 1)
        return 1;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* ---- particles (line clears, hard drops) ---- */

#define MAX_PARTS 900
static struct part {
    float x, y, vx, vy;   /* position and velocity */
    float life, max;      /* seconds left; seconds it started with */
    uint32_t c;           /* colour */
    int size;             /* pixels square */
} parts[MAX_PARTS];
static int nparts;
static uint64_t rng_fx = 0x9a17c1e5;

static float frand(void) { return (float)(rng_next(&rng_fx) >> 40) / (float)(1 << 24); }

static void fx_spawn(float x, float y, float vx, float vy, float life, uint32_t c, int size)
{
    if (nparts == MAX_PARTS)
        return;
    parts[nparts++] = (struct part){ x, y, vx, vy, life, life, c, size };
}

void parts_step(float dt)
{
    if (dt > 0.1f)
        dt = 0.1f;
    int j = 0;
    for (int i = 0; i < nparts; i++) {
        struct part *p = &parts[i];
        p->life -= dt;
        if (p->life <= 0)
            continue;
        p->vy += (float)B * 30.0f * dt;   /* gravity */
        p->vx *= 1.0f - 1.5f * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        parts[j++] = *p;
    }
    nparts = j;
}

static void parts_draw(const struct surf *s)
{
    for (int i = 0; i < nparts; i++) {
        const struct part *p = &parts[i];
        uint32_t a = (uint32_t)(256 * p->life / p->max);
        int sz = p->size * (int)(64 + a * 3 / 4) / 256 + 1;
        blend(s, (int)p->x - sz / 2, (int)p->y - sz / 2, sz, sz, p->c, a);
    }
}

/* The effects the game's last moves call for (once per new event). */
static uint64_t seen_clear, seen_drop;

/* The burst of a line clear: particles from every cell of the rows the
 * last lock cleared, more for more rows. */
static void clear_burst(const struct game *g)
{
    for (int i = 0; i < g->ncleared; i++) {
        int row = g->cleared[i] - HIDDEN;
        for (int x = 0; x < BW; x++) {
            uint8_t cell = g->cleared_cells[i][x];
            uint32_t c = cell ? piece_rgb[cell - 1] : 0xffffff;
            for (int k = 0; k < 3 + g->ncleared; k++) {
                float a = frand() * 6.2832f, sp = (float)B * (4 + 10 * frand());
                fx_spawn((x + frand()) * B, (row + frand()) * B, sp * (float)cosd(a),
                      sp * (float)sind(a) - (float)B * 8, 0.5f + 0.5f * frand(),
                      mixc(c, 0xffffff, (uint32_t)(frand() * 160)), B / 5 + 2);
            }
        }
    }
}

void effects(const struct game *g)
{
    if (g->ncleared && g->cleared_at != seen_clear) {
        seen_clear = g->cleared_at;
        clear_burst(g);
    }
    if (g->dropped_at && g->dropped_at != seen_drop && g->drop_y1 > g->drop_y0) {
        seen_drop = g->dropped_at;
        for (int k = 0; k < 4; k++) {
            int cx = g->drop_x + shape[g->drop_type][g->drop_rot][k][0];
            int cy = g->drop_y1 + shape[g->drop_type][g->drop_rot][k][1] - HIDDEN;
            for (int j = 0; j < 3; j++)
                fx_spawn((cx + frand()) * B, (cy + 1) * B - 2, (frand() - 0.5f) * B * 8,
                      -frand() * B * 5, 0.25f + 0.2f * frand(),
                      mixc(piece_rgb[g->drop_type], 0xffffff, 140), B / 8 + 2);
        }
    }
}

/* ---- layout and the background ---- */

static const char *const help[][2] = {
    { "left right", "move" }, { "up  x", "rotate" }, { "z", "rotate back" },
    { "down", "soft drop" }, { "space", "hard drop" }, { "c", "hold" }, { "p", "pause" },
    { "q  Esc", "quit" },
};

static int help_kw(void)   /* the keys' column width */
{
    int kw = 0;
    for (unsigned i = 0; i < sizeof(help) / sizeof(help[0]); i++) {
        int w = text_width(ls, help[i][0]);
        kw = w > kw ? w : kw;
    }
    return kw;
}

static void layout(void)
{
    int W = scr.w, H = scr.h;
    B = (H * 86 / 100) / 20;
    if (B * 23 > W)
        B = W / 23;
    wx = (W - BW * B) / 2;
    wy = (H - 20 * B) / 2;
    pw = B * 5;
    lx = wx - B * 3 / 4 - pw;
    rx = wx + BW * B + B * 3 / 4;
    ls = B >= 44 ? 2 : 1;
    ns = B >= 44 ? 4 : 2;
    hs = B >= 44 ? 5 : 3;
    int vw = 0;   /* the side panels: as wide as the help needs */
    for (unsigned i = 0; i < sizeof(help) / sizeof(help[0]); i++) {
        int w = text_width(ls, help[i][1]);
        vw = w > vw ? w : vw;
    }
    if (2 * (B / 3) + help_kw() + 3 * ls * 4 + vw + B / 4 > pw) {
        pw = 2 * (B / 3) + help_kw() + 3 * ls * 4 + vw + B / 4;
        lx = wx - B * 3 / 4 - pw;
    }
    hold_y = wy + TEXT_H(hs) + B / 2;
    hold_h = 3 * B;
    stat_y = hold_y + hold_h + B / 2;
    /* four numbers and the level bar */
    stat_h = B / 3 + 4 * (TEXT_H(ls) + ls * 3 + TEXT_H(ns) + B / 3) + B / 4 + B / 3;
    next_y = wy;
    next_h = TEXT_H(ls) + B / 4 + 2 * B + B / 2 + 2 * (2 * B) + B / 4;
    help_y = next_y + next_h + B / 2;
    sprite_px[SZ_WELL] = B;
    sprite_px[SZ_BIG] = B * 4 / 5;
    sprite_px[SZ_SMALL] = B * 3 / 5;
    for (int z = 0; z < NSIZES; z++)
        for (int t = 0; t < NPIECES; t++)
            sprite[z][t] = make_block(sprite_px[z], piece_rgb[t]);
}

static void box(const struct surf *s, int x, int y, int w, int h, const char *label)
{
    int r = B / 4;
    panel(s, x - 2, y - 2, w + 4, h + 4, r + 2, 0x3b4c86, 170);
    panel(s, x, y, w, h, r, 0x0c1022, 240);
    if (label)
        text(s, x + B / 3, y + B / 5, ls, 0x8fa3d8, label);
}

static void draw_background(void)
{
    struct surf *s = &bg;
    vgrad(s, 0, 0, s->w, s->h, 0x182044, 0x05060c);
    /* faint stars */
    uint64_t r = 12345;
    for (int i = 0; i < s->w * s->h / 3000; i++) {
        int x = (int)(rng_next(&r) % (uint64_t)s->w), y = (int)(rng_next(&r) % (uint64_t)s->h);
        uint32_t a = 30 + (uint32_t)(rng_next(&r) % 90);
        int d = a > 100 && scr.ui > 1 ? 2 : 1;
        blend(s, x, y, d, d, 0xc8d4ff, a);
    }
    /* the well: a glowing rim, a dark floor with a faint grid */
    for (int g = 5; g >= 1; g--)
        panel(s, wx - 3 * g - 2, wy - 3 * g - 2, BW * B + 6 * g + 4, 20 * B + 6 * g + 4,
              B / 4 + 3 * g, 0x4a70ff, 22);
    fill(s, wx - 2, wy - 2, BW * B + 4, 20 * B + 4, 0x5a6cb0);
    vgrad(s, wx, wy, BW * B, 20 * B, 0x0b0f20, 0x070914);
    for (int x = 1; x < BW; x++)
        fill(s, wx + x * B - 1, wy, 1, 20 * B, 0x151b35);
    for (int y = 1; y < 20; y++)
        fill(s, wx, wy + y * B - 1, BW * B, 1, 0x151b35);
    /* the title: one letter per piece colour */
    static const char title[] = "TETRIS";
    static const int tc[] = { Z, L, O, S, I, T };
    int tx = lx;
    for (int i = 0; title[i]; i++) {
        char ch[2] = { title[i], 0 };
        tx = text_shadow(s, tx, wy, hs, piece_rgb[tc[i]], ch) + hs;
    }
    text(s, lx + 2, wy - TEXT_H(ls) - 4 * ls, ls, 0x6b7bb0, "JAM OS");
    box(s, lx, hold_y, pw, hold_h, "HOLD");
    box(s, lx, stat_y, pw, stat_h, NULL);
    box(s, rx, next_y, pw, next_h, "NEXT");
    int n = (int)(sizeof(help) / sizeof(help[0])), step = TEXT_H(ls) + ls * 5, kw = help_kw();
    int hh = 2 * (B / 3) + n * step - ls * 5;
    if (help_y + hh <= wy + 20 * B) {
        box(s, rx, help_y, pw, hh, NULL);
        int y = help_y + B / 3;
        for (int i = 0; i < n; i++, y += step) {
            text(s, rx + B / 3, y, ls, 0xe0e6ff, help[i][0]);
            text(s, rx + B / 3 + kw + 3 * ls * 4, y, ls, 0x8290c0, help[i][1]);
        }
    }
}

bool draw_setup(void)
{
    layout();
    bg = surf_new(scr.w, scr.h);
    if (!bg.px)
        return false;
    draw_background();
    blit(&scr.s, 0, 0, &bg, 0, 0, scr.w, scr.h);
    return true;
}

/* ---- the frame ---- */

static void restore(int x, int y, int w, int h)
{
    blit(&scr.s, x, y, &bg, x, y, w, h);
}

/* A piece picture centred in a box (x, y, w, h). */
static void piece_at(int x, int y, int w, int h, int type, int size, bool dim)
{
    int b = sprite_px[size];
    int minx = 4, maxx = 0, miny = 4, maxy = 0;
    for (int k = 0; k < 4; k++) {
        int cx = shape[type][0][k][0], cy = shape[type][0][k][1];
        minx = cx < minx ? cx : minx;
        maxx = cx > maxx ? cx : maxx;
        miny = cy < miny ? cy : miny;
        maxy = cy > maxy ? cy : maxy;
    }
    int px = x + (w - (maxx - minx + 1) * b) / 2, py = y + (h - (maxy - miny + 1) * b) / 2;
    for (int k = 0; k < 4; k++) {
        int cx = shape[type][0][k][0] - minx, cy = shape[type][0][k][1] - miny;
        blit_key(&scr.s, px + cx * b, py + cy * b, &sprite[size][type], KEY_PX);
        if (dim)
            blend(&scr.s, px + cx * b, py + cy * b, b - 1, b - 1, 0x101018, 170);
    }
}

static void stat(int *y, const char *label, const char *value, uint32_t c)
{
    text(&scr.s, lx + B / 3, *y, ls, 0x8fa3d8, label);
    *y += TEXT_H(ls) + ls * 3;
    text_shadow(&scr.s, lx + B / 3, *y, ns, c, value);
    *y += TEXT_H(ns) + B / 3;
}

/* The stack. During a line clear (`since` it: < CLEAR_NS) the rows above
 * the cleared ones are drawn where they were, then slide down into place,
 * and the cleared rows flash white-hot and shrink away. */
static void draw_stack(const struct game *g, const struct surf *well, uint64_t since)
{
    bool clearing = since < CLEAR_NS;
    int orig[BH];   /* each row's row before the clear */
    for (int r = 0; r < BH; r++)
        orig[r] = r;
    if (clearing) {
        int r = BH - 1;
        for (int o = BH - 1; o >= 0 && r >= 0; o--) {
            bool gone = false;
            for (int i = 0; i < g->ncleared; i++)
                gone |= g->cleared[i] == o;
            if (!gone)
                orig[r--] = o;
        }
        for (; r >= 0; r--)
            orig[r] = r - g->ncleared;
    }
    double slide = clearing ? ease_out(((double)since - FLASH_NS / 2) /
                                       (double)(CLEAR_NS - FLASH_NS / 2)) : 1;
    for (int r = 0; r < BH; r++) {
        int y = (int)((orig[r] + (r - orig[r]) * slide - HIDDEN) * B + 0.5);
        if (y <= -B)
            continue;
        for (int x = 0; x < BW; x++)
            if (g->board[r][x])
                blit_key(well, x * B, y, &sprite[SZ_WELL][g->board[r][x] - 1], KEY_PX);
    }
    if (!clearing || since >= FLASH_NS * 3 / 2)
        return;
    double t = (double)since / (double)(FLASH_NS * 3 / 2);
    for (int i = 0; i < g->ncleared; i++) {
        int y = (g->cleared[i] - HIDDEN) * B;
        if (y < 0)
            continue;
        int w = (int)(BW * B * (1 - ease_out((t - 0.35) / 0.65)));
        int x0 = (BW * B - w) / 2;
        for (int x = 0; x < BW; x++)
            if (g->cleared_cells[i][x])
                blit_key(well, x * B, y, &sprite[SZ_WELL][g->cleared_cells[i][x] - 1], KEY_PX);
        blend(well, 0, y, BW * B, B - 1, 0xffffff, (uint32_t)(256 * (t < 0.35 ? t / 0.35 : 1)));
        restore(wx, wy + y, x0, B - 1);
        restore(wx + x0 + w, wy + y, BW * B - x0 - w, B - 1);
    }
}

/* The last piece locked glows for a moment (not during a line clear), and
 * a hard drop leaves streaks from where the piece was to where it landed. */
static void draw_glows(const struct game *g, const struct surf *well, uint64_t now, bool clearing)
{
    if (g->pieces && now - g->locked_at < LOCK_FLASH_NS && !clearing) {
        uint32_t a = (uint32_t)(140 * (LOCK_FLASH_NS - (now - g->locked_at)) / LOCK_FLASH_NS);
        for (int k = 0; k < 4; k++) {
            int cx = g->locked_x + shape[g->locked_type][g->locked_rot][k][0];
            int cy = g->locked_y + shape[g->locked_type][g->locked_rot][k][1] - HIDDEN;
            blend(well, cx * B, cy * B, B - 1, B - 1, 0xffffff, a);
        }
    }
    if (g->dropped_at && now - g->dropped_at < TRAIL_NS) {
        double t = (double)(now - g->dropped_at) / TRAIL_NS;
        uint32_t c = piece_rgb[g->drop_type];
        for (int k = 0; k < 4; k++) {
            int cx = g->drop_x + shape[g->drop_type][g->drop_rot][k][0];
            int top = g->drop_y0 + shape[g->drop_type][g->drop_rot][k][1] - HIDDEN;
            int bot = g->drop_y1 + shape[g->drop_type][g->drop_rot][k][1] - HIDDEN;
            int y0 = top * B + (int)((bot - top) * B * ease_out(t * 1.3)), y1 = bot * B;
            for (int y = y0; y < y1; y++) {
                uint32_t a = (uint32_t)(130 * (1 - t) * (y - y0 + 1) / (y1 - y0 + 1));
                blend(well, cx * B + B / 5, y, B - 1 - 2 * (B / 5), 1, c, a);
            }
        }
    }
}

/* The falling piece, and its ghost where it will land. */
static void draw_piece(const struct game *g, const struct surf *well)
{
    int d = drop_distance(g);
    uint32_t c = piece_rgb[g->type];
    int t = B >= 30 ? 3 : 2;
    for (int k = 0; k < 4 && d; k++) {
        int cx = g->x + shape[g->type][g->rot][k][0];
        int cy = g->y + shape[g->type][g->rot][k][1] + d - HIDDEN;
        if (cy < 0)
            continue;
        blend(well, cx * B, cy * B, B - 1, B - 1, c, 50);
        struct surf cell = { well->px + (int64_t)cy * B * well->stride + cx * B, B - 1, B - 1,
                             well->stride };
        frame(&cell, 0, 0, B - 1, B - 1, t, mixc(c, 0xffffff, 40));
    }
    for (int k = 0; k < 4; k++) {
        int cx = g->x + shape[g->type][g->rot][k][0];
        int cy = g->y + shape[g->type][g->rot][k][1] - HIDDEN;
        blit_key(well, cx * B, cy * B, &sprite[SZ_WELL][g->type], KEY_PX);
    }
}

/* The side panels: hold, next, the numbers and the way to the next level. */
static void draw_side(const struct game *g, uint32_t best, int in_y)
{
    const struct surf *s = &scr.s;
    int iy = hold_y + in_y;
    if (g->hold >= 0)
        piece_at(lx, iy, pw, hold_h - in_y - B / 8, g->hold, SZ_BIG, g->held);
    iy = next_y + in_y;
    piece_at(rx, iy, pw, 2 * B + B / 2, g->next[0], SZ_BIG, false);
    iy += 2 * B + B / 2;
    for (int i = 1; i < 3; i++, iy += 2 * B)
        piece_at(rx, iy, pw, 2 * B, g->next[i], SZ_SMALL, false);

    char a[48];
    int y = stat_y + B / 3;
    stat(&y, "SCORE", commas(a, sizeof(a), g->score), 0xffe07a);
    snprintf(a, sizeof(a), "%u", g->level);
    stat(&y, "LEVEL", a, 0x7fe0ff);
    snprintf(a, sizeof(a), "%u", g->lines);
    stat(&y, "LINES", a, 0xffffff);
    stat(&y, "BEST", commas(a, sizeof(a), best), 0xb8c4f0);
    int bw = pw - 2 * (B / 3), bh = B / 4 > 4 ? B / 4 : 4;
    if (y + bh < stat_y + stat_h - B / 4) {
        panel(s, lx + B / 3, y, bw, bh, bh / 2, 0x242c50, 256);
        int fw = bw * (int)(g->lines % 10) / 10;
        if (fw >= bh)
            panel(s, lx + B / 3, y, fw, bh, bh / 2, 0x4ac8ff, 256);
    }
}

/* PAUSED, or GAME OVER with the score, over a darkened well. */
static void draw_message(const struct game *g, const struct surf *well)
{
    char a[48], n[24];
    blend(well, 0, 0, BW * B, 20 * B, 0x04050c, 190);
    const char *m1 = g->over ? "GAME OVER" : "PAUSED";
    int ts = hs, cy = 20 * B / 2 - TEXT_H(ts);
    while (ts > 1 && text_width(ts, m1) > BW * B - B)
        ts--;
    text_shadow(well, (BW * B - text_width(ts, m1)) / 2, cy, ts, g->over ? 0xff6a5a : 0xffffff,
                m1);
    cy += TEXT_H(ts) + B / 2;
    if (g->over) {
        snprintf(a, sizeof(a), "score %s", commas(n, sizeof(n), g->score));
        text_shadow(well, (BW * B - text_width(ls + 1, a)) / 2, cy, ls + 1, 0xffe07a, a);
        cy += TEXT_H(ls + 1) + B / 3;
    }
    const char *m2 = g->over ? "Enter: play again" : "p: carry on";
    text_shadow(well, (BW * B - text_width(ls, m2)) / 2, cy, ls, 0xc8d0f0, m2);
}

void draw(const struct game *g, uint64_t now, uint32_t best)
{
    struct surf *s = &scr.s;
    struct surf well = { s->px + (uint64_t)wy * s->stride + wx, BW * B, 20 * B, s->stride };
    int in_y = TEXT_H(ls) + B / 4;
    restore(wx, wy, BW * B, 20 * B);
    restore(lx, hold_y + in_y, pw, hold_h - in_y - B / 8);
    restore(lx, stat_y, pw, stat_h);
    restore(rx, next_y + in_y, pw, next_h - in_y - B / 8);

    uint64_t since = g->ncleared ? now - g->cleared_at : ~0ull;   /* the last line clear */
    draw_stack(g, &well, since);
    draw_glows(g, &well, now, since < CLEAR_NS);
    if (!g->over)
        draw_piece(g, &well);
    parts_draw(&well);
    draw_side(g, best, in_y);
    if (g->paused || g->over)
        draw_message(g, &well);
}

bool animating(const struct game *g, uint64_t now)
{
    return nparts || (g->ncleared && now - g->cleared_at < CLEAR_NS) ||
           (g->pieces && now - g->locked_at < LOCK_FLASH_NS) ||
           (g->dropped_at && now - g->dropped_at < TRAIL_NS);
}
