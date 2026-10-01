/* jamjar: where everything goes (jamjar.h, struct layout). From the top:
 * the bar (the mark and "jamjar", the search box, the status on the
 * right); then the library on the left (three columns: artists, albums,
 * tracks) and now playing on the right (the album's jar label, the title,
 * the progress bar, the buttons, the volume and the play order); and the
 * stereo bars across the bottom, a fifth of the height. The big view (f)
 * has its own places: a column at the left for the cover and the names,
 * and the sunburst's circle in the middle of the rest. Sizes are in units of the
 * UI scale (1 up to 1080 lines, 2 above), the big parts in shares of the
 * screen, so 1280x720 and 2560x1440 look the same. Pure arithmetic: the
 * self-test checks it for screens QEMU doesn't have. */
#include "jamjar.h"

#define MARGIN   16
#define GAP      12
#define BAR_H    44
#define PAD      12    /* inside the library */
#define PAD_NOW  20    /* inside now playing */
#define HEAD_H   24
#define ROW_H    26
#define COL_GAP  10
#define BTN_SIZE  44
#define PLAY_SIZE 60
#define LIB_SHARE 56   /* percent of the width */

static void place_top(struct layout *l)
{
    int u = l->u, m = MARGIN * u;
    l->top = (struct rect){ m, m, l->w - 2 * m, BAR_H * u };
    l->mark = (struct rect){ m, m + 2 * u, 40 * u, 40 * u };
    int sx = l->mark.x + l->mark.w + 12 * u + text_width(2 * u, "jamjar") + 28 * u;
    int sw = 380 * u, room = l->top.x + l->top.w - 260 * u - sx;
    l->search = (struct rect){ sx, m + 6 * u, sw < room ? sw : room, 32 * u };
    int stx = l->search.x + l->search.w + 16 * u;
    l->status = (struct rect){ stx, m + 6 * u, l->top.x + l->top.w - stx, 32 * u };
}

static void place_library(struct layout *l)
{
    int u = l->u, pad = PAD * u, cg = COL_GAP * u;
    static const int share[NCOLS] = { 30, 34, 36 };
    int iw = l->lib.w - 2 * pad - (NCOLS - 1) * cg, x = l->lib.x + pad;
    for (int c = 0; c < NCOLS; c++) {
        int cw = c == NCOLS - 1 ? l->lib.x + l->lib.w - pad - x : iw * share[c] / 100;
        l->head[c] = (struct rect){ x, l->lib.y + pad, cw, HEAD_H * u };
        int ly = l->head[c].y + l->head[c].h + 6 * u;
        l->list[c] = (struct rect){ x, ly, cw, l->lib.y + l->lib.h - pad - ly };
        x += cw + cg;
    }
    l->row_h = ROW_H * u;
    l->rows = l->list[0].h / l->row_h;
}

static void place_now(struct layout *l)
{
    int u = l->u, pad = PAD_NOW * u;
    struct rect *n = &l->now;
    int a = n->w * 40 / 100, ah = n->h * 46 / 100;
    a = a < ah ? a : ah;
    l->art = (struct rect){ n->x + pad, n->y + pad, a, a };
    int tx = l->art.x + a + 18 * u;
    l->title = (struct rect){ tx, l->art.y + 4 * u, n->x + n->w - pad - tx, a - 8 * u };
    l->progress = (struct rect){ n->x + pad, l->art.y + a + 28 * u, n->w - 2 * pad, 10 * u };
    int by = l->progress.y + l->progress.h + 34 * u, cx = n->x + n->w / 2;
    int p = PLAY_SIZE * u, b = BTN_SIZE * u, sy = by + (p - b) / 2;
    l->btn[BTN_PLAY] = (struct rect){ cx - p / 2, by, p, p };
    l->btn[BTN_PREV] = (struct rect){ cx - p / 2 - 20 * u - b, sy, b, b };
    l->btn[BTN_NEXT] = (struct rect){ cx + p / 2 + 20 * u, sy, b, b };
    l->btn[BTN_STOP] = (struct rect){ l->btn[BTN_PREV].x - 32 * u - b, sy, b, b };
    int vy = by + p + 26 * u, vw = n->w * 42 / 100;
    l->vol = (struct rect){ n->x + pad + 40 * u, vy, vw, 12 * u };
    int mw = 150 * u;
    l->mode = (struct rect){ n->x + n->w - pad - mw, vy - 9 * u, mw, 30 * u };
    int iy = l->mode.y + l->mode.h + 16 * u, ih = n->y + n->h - pad - iy;
    if (ih >= TEXT_H(u) + 16 * u)   /* else there is no room for it */
        l->info = (struct rect){ n->x + pad, iy, n->w - 2 * pad, ih };
}

/* The big view: the column takes BIG_COL % of the width; the sunburst is
 * as big as the height allows, centred in what is left; the cover and the
 * names (title, artist, album) are centred in the column, one block. */
#define BIG_COL    30
#define BIG_MARGIN 32
#define BIG_ART    36   /* percent of the height: the cover at most */

static void place_big(struct layout *l)
{
    int u = l->u, m = BIG_MARGIN * u, col = l->w * BIG_COL / 100;
    int r = (l->h - 2 * m) / 2, rw = (l->w - col) / 2 - m;
    l->burst_r = r < rw ? r : rw;
    l->burst_x = col + (l->w - col) / 2;
    l->burst_y = l->h / 2;
    int inner = col - 2 * m, art = l->h * BIG_ART / 100;
    art = art < inner ? art : inner;
    int text_h = TEXT_H(2 * u) + 12 * u + TEXT_H(u) + 6 * u + TEXT_H(u);
    int y = (l->h - art - 24 * u - text_h) / 2;
    l->big_art = (struct rect){ m + (inner - art) / 2, y, art, art };
    l->big_text = (struct rect){ m, y + art + 24 * u, inner, text_h };
}

void layout_make(struct layout *l, int w, int h, int ui)
{
    memset(l, 0, sizeof(*l));
    int u = l->u = ui < 1 ? 1 : ui;
    l->w = w;
    l->h = h;
    l->ts = u;
    l->tb = 2 * u;
    place_top(l);
    int jam_h = h * 21 / 100;
    jam_h = jam_h < 90 * u ? 90 * u : jam_h;
    l->jam = (struct rect){ 0, h - jam_h, w, jam_h };
    int m = MARGIN * u, gap = GAP * u, y0 = l->top.y + l->top.h + gap, y1 = l->jam.y - 8 * u;
    int lw = (w - 2 * m - gap) * LIB_SHARE / 100;
    l->lib = (struct rect){ m, y0, lw, y1 - y0 };
    l->now = (struct rect){ m + lw + gap, y0, w - m - (m + lw + gap), y1 - y0 };
    place_library(l);
    place_now(l);
    place_big(l);
}
