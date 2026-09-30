/* mines: the picture (mines.h).
 *
 * The board is always in the middle of the screen, its cells as big as the
 * screen allows for the level (so a pointer at the screen's centre is on
 * the board's centre at any resolution, which the mouse test relies on).
 * Above it the bar with the mines left, the face (the new-game button)
 * and the seconds; below it a button per level and one line of text: the
 * help, how the game ended, or that no mouse has been seen.
 *
 * What never changes for a level (the backdrop, the board's rim, the
 * bar's panel) is drawn once into `bg`; each frame copies the parts of it
 * that hold moving things and draws those over the copy. */
#include "mines.h"

#define TILE      0x5468a8   /* a closed cell */
#define TILE_DIM  0x2c3558   /* a closed cell pressed, or shown by a peek */
#define OPEN      0x171c30   /* an open cell */
#define INK       0x10131f   /* dark strokes on a closed cell */
#define STEEL     0xd8dcea   /* a mine on an open cell */

static const uint32_t number_rgb[9] = {
    0, 0x5aa2ff, 0x5ed06a, 0xff6257, 0xb58aff, 0xffa040, 0x3fd6d6, 0xe8ecf8, 0x9aa4c0,
};

static int C;                 /* a cell's size in pixels */
static int u;                 /* the UI scale (scr.ui) */
static int bw, bh;            /* the board in cells */
static struct rect board_r, hud_r, face_r, count_r, time_r, level_r[NLEVELS], line_r, stats_r;
static bool stats_fit;        /* the corner has room for the session's numbers */
static struct surf bg;

/* ---- layout and the background ---- */

static void layout(const struct level *lv)
{
    int W = scr.w, H = scr.h;
    u = scr.ui;
    bw = lv->w;
    bh = lv->h;
    int gap = 14 * u, hud_h = 60 * u, level_h = 34 * u, line_h = TEXT_H(u);
    /* As much room is kept above the board as below it, so it stays centred. */
    int above = gap + hud_h + gap, below = gap + level_h + gap + line_h + gap;
    int margin = above > below ? above : below;
    C = (H - 2 * margin) / bh;
    if (C * bw > W - 4 * gap)
        C = (W - 4 * gap) / bw;
    if (C > H / 13)
        C = H / 13;
    board_r = (struct rect){ (W - C * bw) / 2, (H - C * bh) / 2, C * bw, C * bh };

    int in = 8 * u, box_w = 124 * u, f = hud_h - 2 * in;
    int hud_w = board_r.w > 440 * u ? board_r.w : 440 * u;
    hud_r = (struct rect){ (W - hud_w) / 2, board_r.y - gap - hud_h, hud_w, hud_h };
    count_r = (struct rect){ hud_r.x + in, hud_r.y + in, box_w, f };
    time_r = (struct rect){ hud_r.x + hud_w - in - box_w, hud_r.y + in, box_w, f };
    face_r = (struct rect){ (W - f) / 2, hud_r.y + in, f, f };

    int lw = 170 * u, lgap = 10 * u, total = NLEVELS * lw + (NLEVELS - 1) * lgap;
    for (int i = 0; i < NLEVELS; i++)
        level_r[i] = (struct rect){ (W - total) / 2 + i * (lw + lgap),
                                    board_r.y + board_r.h + gap, lw, level_h };
    line_r = (struct rect){ 0, level_r[0].y + level_h + gap, W, line_h };
    stats_r = (struct rect){ W - 250 * u, 12 * u, 238 * u, 2 * TEXT_H(u) + 4 * u };
    stats_fit = hud_r.x + hud_r.w < stats_r.x || hud_r.y > stats_r.y + stats_r.h;
}

static void draw_background(void)
{
    struct surf *s = &bg;
    vgrad(s, 0, 0, s->w, s->h, 0x1a2340, 0x06070d);
    /* the board: a glowing rim round a dark floor */
    struct rect rim = { board_r.x - 2, board_r.y - 2, board_r.w + 3, board_r.h + 3 };
    glow(s, &rim, 15, 0x4a70ff);
    fill(s, rim.x, rim.y, rim.w, rim.h, 0x5a6cb0);
    fill(s, board_r.x, board_r.y, board_r.w - 1, board_r.h - 1, 0x0a0c16);
    card(s, &hud_r, 10 * u);
    if (stats_fit && hud_r.x > 24 * u + text_width(2 * u, "MINESWEEPER")) {
        text(s, 24 * u, 12 * u, u, 0x6b7bb0, "JAM OS");
        text_shadow(s, 24 * u, 12 * u + TEXT_H(u), 2 * u, 0xe0e6ff, "MINESWEEPER");
    }
}

bool draw_setup(const struct level *lv)
{
    layout(lv);
    if (!bg.px)
        bg = surf_new(scr.w, scr.h);
    if (!bg.px)
        return false;
    draw_background();
    blit(&scr.s, 0, 0, &bg, 0, 0, scr.w, scr.h);
    return true;
}

enum hit draw_hit(int x, int y, int *cx, int *cy)
{
    if (rect_has(&board_r, x, y)) {
        *cx = (x - board_r.x) / C;
        *cy = (y - board_r.y) / C;
        return *cx < bw && *cy < bh ? HIT_CELL : HIT_NONE;
    }
    if (rect_has(&face_r, x, y))
        return HIT_NEW;
    for (int i = 0; i < NLEVELS; i++)
        if (rect_has(&level_r[i], x, y)) {
            *cx = i;
            return HIT_LEVEL;
        }
    return HIT_NONE;
}

/* ---- the little pictures ---- */

/* A stroke t thick from (x0, y0) to (x1, y1), as squares along it. */
static void stroke(int x0, int y0, int x1, int y1, int t, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    for (int i = 0; i <= n; i++)
        fill(&scr.s, x0 + (n ? dx * i / n : 0) - t / 2, y0 + (n ? dy * i / n : 0) - t / 2, t, t, c);
}

static void draw_mine(const struct rect *r, uint32_t c)
{
    int cx = r->x + r->w / 2, cy = r->y + r->h / 2;
    int t = r->w / 12 > 2 ? r->w / 12 : 2, arm = r->w * 38 / 100, d = arm * 72 / 100;
    stroke(cx - arm, cy, cx + arm, cy, t, c);
    stroke(cx, cy - arm, cx, cy + arm, t, c);
    stroke(cx - d, cy - d, cx + d, cy + d, t, c);
    stroke(cx - d, cy + d, cx + d, cy - d, t, c);
    int rad = r->w * 27 / 100;
    disc(&scr.s, cx, cy, rad, c, 256);
    disc(&scr.s, cx - rad / 3, cy - rad / 3, rad / 4 + 1, c == STEEL ? 0xffffff : 0x8a6060, 230);
}

static void draw_flag(const struct rect *r)
{
    int w = r->w, t = w / 12 > 2 ? w / 12 : 2;
    int pole = r->x + w * 56 / 100, top = r->y + w * 20 / 100, th = w * 34 / 100;
    for (int j = 0; j < th; j++) {   /* a pennant: widest in the middle of its height */
        int k = j < th / 2 ? j : th - 1 - j;
        int len = w * 34 / 100 * (2 * k + 2) / th;
        fill(&scr.s, pole - len, top + j, len, 1, 0xf2483c);
    }
    fill(&scr.s, pole, top, t, w * 56 / 100, INK);
    fill(&scr.s, r->x + w * 30 / 100, r->y + w * 76 / 100, w * 46 / 100, t, INK);
}

/* The face's mouth: a curve through three heights (left/right ends at
 * `ends`, the middle at `mid`, below the face's centre). */
static void mouth(int cx, int cy, int half, int ends, int mid, int t)
{
    for (int i = -half; i <= half; i++) {
        int y = mid + (ends - mid) * i * i / (half * half);
        fill(&scr.s, cx + i - t / 2, cy + y - t / 2, t, t, INK);
    }
}

/* The new-game button: a face that shows how the game stands. */
static void draw_face(const struct board *b, const struct view *v)
{
    const struct surf *s = &scr.s;
    bevel(s, &face_r, 2 * u, 0x3a4678);
    int cx = face_r.x + face_r.w / 2, cy = face_r.y + face_r.h / 2, R = face_r.w * 36 / 100;
    int ex = R * 40 / 100, ey = R * 28 / 100, er = R / 7 + 1, t = R / 8 + 1;
    disc(s, cx, cy, R + u, 0x6a4c08, 256);
    disc(s, cx, cy, R, b->state == LOST ? 0xe8a040 : 0xffd23c, 256);
    if (b->state == LOST) {
        for (int k = -1; k <= 1; k += 2) {
            stroke(cx + k * ex - er, cy - ey - er, cx + k * ex + er, cy - ey + er, t, INK);
            stroke(cx + k * ex - er, cy - ey + er, cx + k * ex + er, cy - ey - er, t, INK);
        }
        mouth(cx, cy, R / 2, R * 55 / 100, R * 30 / 100, t);
    } else if (b->state == WON) {   /* sunglasses */
        fill(s, cx - R * 80 / 100, cy - ey - t / 2, R * 160 / 100, t, INK);
        disc(s, cx - ex, cy - ey + er / 2, er * 2, INK, 256);
        disc(s, cx + ex, cy - ey + er / 2, er * 2, INK, 256);
        mouth(cx, cy, R / 2, R * 25 / 100, R * 60 / 100, t);
    } else {
        disc(s, cx - ex, cy - ey, er, INK, 256);
        disc(s, cx + ex, cy - ey, er, INK, 256);
        if (v->pressing && v->hover_x >= 0)
            disc(s, cx, cy + R * 45 / 100, R / 5 + 1, INK, 256);   /* "o" */
        else
            mouth(cx, cy, R / 2, R * 30 / 100, R * 55 / 100, t);
    }
}

/* A counter box of the bar: a label and a three-digit number. */
static void counter(const struct rect *r, const char *label, int value, uint32_t c)
{
    const struct surf *s = &scr.s;
    char a[16];
    panel(s, r->x, r->y, r->w, r->h, 6 * u, 0x05060c, 256);
    text(s, r->x + 8 * u, r->y + (r->h - TEXT_H(u)) / 2, u, 0x6b7bb0, label);
    snprintf(a, sizeof(a), "%03d", value < -99 ? -99 : value > 999 ? 999 : value);
    text(s, r->x + r->w - 8 * u - text_width(2 * u, a), r->y + (r->h - TEXT_H(2 * u)) / 2, 2 * u, c,
         a);
}

/* ---- the cells ---- */

static void draw_open(const struct board *b, const struct rect *r, int x, int y)
{
    bool boom = x == b->boom_x && y == b->boom_y;
    fill(&scr.s, r->x, r->y, r->w, r->h, boom ? 0xc83030 : OPEN);
    if (b->cell[y][x] & CELL_MINE) {
        draw_mine(r, boom ? 0x1a1020 : STEEL);
    } else if (b->near[y][x]) {
        char a[2] = { (char)('0' + b->near[y][x]), 0 };
        int k = (C + 4) / 20 > 1 ? (C + 4) / 20 : 1;
        text_in(&scr.s, r, k, number_rgb[b->near[y][x]], a);
    }
}

static void draw_closed(const struct board *b, const struct view *v, const struct rect *r, int x,
                        int y)
{
    uint8_t c = b->cell[y][x];
    bool flag = c & CELL_FLAG, hover = x == v->hover_x && y == v->hover_y;
    bool live = b->state == PLAYING;
    int e = C / 10 > 2 ? C / 10 : 2;
    if (live && !flag && ((hover && v->pressing) || board_peeks(b, v->peek_x, v->peek_y, x, y)))
        bevel(&scr.s, r, -1, TILE_DIM);
    else
        bevel(&scr.s, r, e, live && hover ? mixc(TILE, 0xffffff, 48) : TILE);
    if (flag || (b->state == WON && (c & CELL_MINE)))
        draw_flag(r);
    if (b->state == LOST && flag && !(c & CELL_MINE)) {   /* a flag that was wrong */
        int t = C / 10 > 2 ? C / 10 : 2, m = C / 5;
        stroke(r->x + m, r->y + m, r->x + r->w - m, r->y + r->h - m, t, INK);
        stroke(r->x + m, r->y + r->h - m, r->x + r->w - m, r->y + m, t, INK);
    }
}

static void draw_cells(const struct board *b, const struct view *v)
{
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            struct rect r = { board_r.x + x * C, board_r.y + y * C, C - 1, C - 1 };
            uint8_t c = b->cell[y][x];
            bool shown_mine = b->state == LOST && (c & CELL_MINE) && !(c & CELL_FLAG);
            if ((c & CELL_OPEN) || shown_mine)
                draw_open(b, &r, x, y);
            else
                draw_closed(b, v, &r, x, y);
        }
}

/* ---- the frame ---- */

static void restore(const struct rect *r)
{
    blit(&scr.s, r->x, r->y, &bg, r->x, r->y, r->w, r->h);
}

static void draw_levels(const struct view *v)
{
    for (int i = 0; i < NLEVELS; i++) {
        char a[48];
        bool on = i == v->level;
        snprintf(a, sizeof(a), "%s %dx%d", levels[i].name, levels[i].w, levels[i].h);
        bevel(&scr.s, &level_r[i], on ? -2 * u : 2 * u, on ? 0x2a3a78 : 0x3a4678);
        text_in(&scr.s, &level_r[i], u, on ? 0xffffff : 0xb8c4f0, a);
    }
}

/* The line under the level buttons: what matters most right now. */
static void draw_line(const struct board *b, const struct view *v, uint64_t t)
{
    char a[160];
    uint32_t c = 0x8290c0;
    if (v->no_mouse) {
        snprintf(a, sizeof(a), "No mouse has moved yet: this game is played with a mouse. "
                               "q: quit");
        c = 0xffc04a;
    } else if (b->state == LOST) {
        snprintf(a, sizeof(a), "Boom! Click the face, or press n, for a new game");
        c = 0xff7a6a;
    } else if (b->state == WON) {
        snprintf(a, sizeof(a), "Cleared in %u s! Click the face, or press n, to play again",
                 board_seconds(b, t));
        c = 0x7fe08a;
    } else {
        snprintf(a, sizeof(a), "left click: reveal    right click: flag, or open around a "
                               "number    n: new game    1-4: level    q: quit");
    }
    int k = u;
    while (k > 1 && text_width(k, a) > scr.w - 40)
        k--;
    text_in(&scr.s, &line_r, k, c, a);
}

static void draw_stats(const struct view *v)
{
    char a[64];
    if (!stats_fit)
        return;
    restore(&stats_r);
    snprintf(a, sizeof(a), "won %u of %u", v->wins, v->games);
    text(&scr.s, stats_r.x + stats_r.w - text_width(u, a), stats_r.y, u, 0x8fa3d8, a);
    if (v->best[v->level])
        snprintf(a, sizeof(a), "best %u s", v->best[v->level]);
    else
        snprintf(a, sizeof(a), "no win yet");
    text(&scr.s, stats_r.x + stats_r.w - text_width(u, a), stats_r.y + TEXT_H(u) + 4 * u, u,
         0x6b7bb0, a);
}

void draw(const struct board *b, const struct view *v, uint64_t t)
{
    struct rect under = { 0, level_r[0].y, scr.w, line_r.y + line_r.h - level_r[0].y };
    restore(&hud_r);
    restore(&under);
    counter(&count_r, "MINES", board_mines_left(b), 0xff5a4a);
    counter(&time_r, "TIME", (int)board_seconds(b, t), 0xffe07a);
    draw_face(b, v);
    draw_cells(b, v);
    draw_levels(v);
    draw_line(b, v, t);
    draw_stats(v);
}
