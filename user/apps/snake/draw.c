/* snake: the picture (snake.h).
 *
 * The rules move the snake a whole cell at a step; the picture slides it.
 * Between two steps the head is drawn on its way from the cell it left to
 * the cell it is in, and the tail's end on its way out of the cell it left
 * (game.left), by the fraction of the step that has passed. Everything
 * between is a band through the cells' centres, so the body bends round
 * corners in one piece. It is drawn twice, tail first: a wider dark band
 * as the outline, then the body, each segment its own shade.
 *
 * The background (the field's checks, the panels, the help) is drawn once
 * into `bg`; a frame copies the field and the numbers' panel from it and
 * draws the apple, the snake and the numbers over the copy. */
#include "snake.h"

#define EAT_NS 320000000ull   /* the ring where an apple was eaten */

static int B;                 /* a cell's size in pixels */
static int u;                 /* the UI scale (scr.ui) */
static struct rect field, stats, keys;
static struct surf bg;

/* ---- layout and the background ---- */

static const char *const help[][2] = {
    { "arrows", "turn" }, { "w a s d", "turn" }, { "p", "pause" }, { "Enter", "again" },
    { "q  Esc", "quit" },
};
#define NHELP ((int)(sizeof(help) / sizeof(help[0])))

static void layout(void)
{
    int W = scr.w, H = scr.h;
    u = scr.ui;
    int pw = 190 * u, gap = 20 * u, row = TEXT_H(u) + 3 * u + TEXT_H(2 * u) + 12 * u;
    B = H * 84 / 100 / FH;
    if (B * FW > W - 2 * (pw + 2 * gap))
        B = (W - 2 * (pw + 2 * gap)) / FW;
    field = (struct rect){ (W - B * FW) / 2, (H - B * FH) / 2, B * FW, B * FH };
    stats = (struct rect){ field.x - gap - pw, field.y + TEXT_H(3 * u) + gap, pw,
                           12 * u + 4 * row };
    keys = (struct rect){ field.x + field.w + gap, field.y, pw,
                          24 * u + NHELP * (TEXT_H(u) + 5 * u) - 5 * u };
}

static void draw_background(void)
{
    struct surf *s = &bg;
    vgrad(s, 0, 0, s->w, s->h, 0x182044, 0x05060c);
    struct rect rim = { field.x - 2, field.y - 2, field.w + 4, field.h + 4 };
    glow(s, &rim, 15, 0x3ad07a);
    fill(s, rim.x, rim.y, rim.w, rim.h, 0x4a9c6c);
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++)
            fill(s, field.x + x * B, field.y + y * B, B, B, (x + y) % 2 ? 0x0d1f18 : 0x132a20);
    text(s, stats.x + 2, field.y - TEXT_H(u) - 4 * u, u, 0x6b7bb0, "JAM OS");
    text_shadow(s, stats.x, field.y, 3 * u, 0x6fe070, "SNAKE");
    card(s, &stats, 8 * u);
    card(s, &keys, 8 * u);
    int kw = 0;
    for (int i = 0; i < NHELP; i++)
        kw = text_width(u, help[i][0]) > kw ? text_width(u, help[i][0]) : kw;
    for (int i = 0, y = keys.y + 12 * u; i < NHELP; i++, y += TEXT_H(u) + 5 * u) {
        text(s, keys.x + 12 * u, y, u, 0xe0e6ff, help[i][0]);
        text(s, keys.x + 12 * u + kw + 12 * u, y, u, 0x8290c0, help[i][1]);
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

/* ---- the snake ---- */

struct pt {
    int x, y;   /* screen pixels */
};

static struct pt centre(const struct game *g, int cell)
{
    return (struct pt){ field.x + cell % g->w * B + B / 2, field.y + cell / g->w * B + B / 2 };
}

/* From a towards b by f / 256. */
static struct pt slide(struct pt a, struct pt b, int f)
{
    return (struct pt){ a.x + (b.x - a.x) * f / 256, a.y + (b.y - a.y) * f / 256 };
}

/* Point i along the snake at fraction f (0..256) of the step: 0 the head
 * on its way into its cell, 1 .. len - 1 the centres of the cells behind
 * it, len the tail's end on its way out of the cell it left. */
static struct pt point(const struct game *g, int i, int f)
{
    if (i == 0)
        return slide(centre(g, game_cell(g, 1)), centre(g, game_cell(g, 0)), f);
    if (i == g->len)
        return slide(centre(g, g->left), centre(g, game_cell(g, g->len - 1)), f);
    return centre(g, game_cell(g, i));
}

/* A band d wide from a to b (one above or beside the other), round at both ends. */
static void band(struct pt a, struct pt b, int d, uint32_t c)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int w = a.x < b.x ? b.x - a.x : a.x - b.x, h = a.y < b.y ? b.y - a.y : a.y - b.y;
    fill(&scr.s, x0 - (h ? d / 2 : 0), y0 - (h ? 0 : d / 2), h ? d : w, h ? h : d, c);
    disc(&scr.s, a.x, a.y, d / 2, c, 256);
    disc(&scr.s, b.x, b.y, d / 2, c, 256);
}

/* The body's colour at segment i of len: bright at the head, deeper
 * towards the tail, every other segment a shade lighter. */
static uint32_t body_rgb(const struct game *g, int i)
{
    uint32_t c = mixc(0x86e86a, 0x1f8a52, (uint32_t)(i * 256 / g->len));
    if (g->over && !g->won)
        c = mixc(c, 0x8a3038, 150);
    return i % 2 ? mixc(c, 0xffffff, 26) : c;
}

static void draw_eyes(struct pt head, struct pt neck, int d)
{
    int fx = head.x > neck.x ? 1 : head.x < neck.x ? -1 : 0;
    int fy = head.y > neck.y ? 1 : head.y < neck.y ? -1 : 0;
    if (!fx && !fy)
        fx = 1;
    for (int side = -1; side <= 1; side += 2) {
        int ex = head.x + fx * d * 14 / 100 - fy * side * d * 24 / 100;
        int ey = head.y + fy * d * 14 / 100 + fx * side * d * 24 / 100;
        disc(&scr.s, ex, ey, d * 17 / 100 + 1, 0xffffff, 256);
        disc(&scr.s, ex + fx * d * 6 / 100, ey + fy * d * 6 / 100, d * 9 / 100 + 1, 0x101820, 256);
    }
}

static void draw_snake(const struct game *g, int f)
{
    int d = B * 76 / 100;
    for (int pass = 0; pass < 2; pass++)
        for (int i = g->len - 1; i >= 0; i--)
            band(point(g, i, f), point(g, i + 1, f), pass ? d : d + 2 * (B / 12 + 1),
                 pass ? body_rgb(g, i) : 0x07140d);
    struct pt head = point(g, 0, f);
    disc(&scr.s, head.x, head.y, d / 2 + B / 16, body_rgb(g, 0), 256);
    draw_eyes(head, centre(g, game_cell(g, 1)), d);
}

static void draw_apple(const struct game *g, uint64_t t)
{
    struct pt p = centre(g, g->food);
    int r = B * 34 / 100 + (int)(sind((double)(t % 900000000ull) / 9e8 * 6.283185) * B * 0.035);
    disc(&scr.s, p.x + B / 14, p.y + B / 12, r, 0x05100a, 120);   /* its shadow */
    disc(&scr.s, p.x, p.y, r, 0xe8382e, 256);
    disc(&scr.s, p.x - r / 3, p.y - r / 3, r / 3, 0xff9a8a, 200);
    fill(&scr.s, p.x - B / 28, p.y - r - B / 8, B / 14 + 1, B / 6, 0x6a4a20);
    disc(&scr.s, p.x + B / 8, p.y - r - B / 16, B / 9 + 1, 0x58c048, 256);
}

/* ---- the frame ---- */

static void restore(const struct rect *r)
{
    blit(&scr.s, r->x, r->y, &bg, r->x, r->y, r->w, r->h);
}

static void stat(int *y, const char *label, const char *value, uint32_t c)
{
    text(&scr.s, stats.x + 12 * u, *y, u, 0x8fa3d8, label);
    *y += TEXT_H(u) + 3 * u;
    text_shadow(&scr.s, stats.x + 12 * u, *y, 2 * u, c, value);
    *y += TEXT_H(2 * u) + 12 * u;
}

static void draw_stats(const struct game *g, uint32_t best)
{
    char a[32];
    int y = stats.y + 12 * u;
    stat(&y, "SCORE", commas(a, sizeof(a), g->score), 0xffe07a);
    stat(&y, "BEST", commas(a, sizeof(a), best), 0xb8c4f0);
    snprintf(a, sizeof(a), "%d", g->len);
    stat(&y, "LENGTH", a, 0xffffff);
    snprintf(a, sizeof(a), "%u", game_level(g));
    stat(&y, "SPEED", a, 0x7fe0ff);
}

/* PAUSED, GAME OVER or YOU WIN over a darkened field. */
static void draw_message(const struct game *g)
{
    char a[48], n[24];
    const struct surf *s = &scr.s;
    blend(s, field.x, field.y, field.w, field.h, 0x04050c, 110);
    const char *m1 = g->won ? "YOU WIN" : g->over ? "GAME OVER" : "PAUSED";
    struct rect r = { field.x, field.y + field.h / 2 - TEXT_H(4 * u) - 8 * u, field.w,
                      TEXT_H(4 * u) };
    text_shadow(s, r.x + (r.w - text_width(4 * u, m1)) / 2, r.y, 4 * u,
                g->won ? 0x7fe08a : g->over ? 0xff6a5a : 0xffffff, m1);
    r.y += r.h + 10 * u;
    r.h = TEXT_H(2 * u);
    if (g->over) {
        snprintf(a, sizeof(a), "score %s", commas(n, sizeof(n), g->score));
        text_in(s, &r, 2 * u, 0xffe07a, a);
        r.y += r.h + 8 * u;
    }
    r.h = TEXT_H(u);
    text_in(s, &r, u, 0xc8d0f0, g->over ? "Enter: play again" : "p: carry on");
}

void draw(const struct game *g, uint64_t t, uint32_t best)
{
    restore(&field);
    restore(&stats);
    /* How far through the step: the slide stands still while paused, and
     * is finished once the game is over. */
    uint64_t at = g->paused ? g->paused_at : t;
    uint64_t since = at > g->stepped_at ? at - g->stepped_at : 0;
    int f = g->over || since >= g->step_ns ? 256 : (int)(since * 256 / g->step_ns);
    if (!g->won)
        draw_apple(g, g->paused ? g->paused_at : t);
    if (g->ate_at && t - g->ate_at < EAT_NS) {
        struct pt p = centre(g, g->ate_cell);
        uint32_t k = (uint32_t)((t - g->ate_at) * 256 / EAT_NS);
        disc(&scr.s, p.x, p.y, B / 2 + (int)(B * k / 256), 0xffe07a, 110 - 110 * k / 256);
    }
    draw_snake(g, f);
    draw_stats(g, best);
    if (g->paused || g->over)
        draw_message(g);
}
