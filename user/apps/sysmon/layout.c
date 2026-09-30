/* sysmon: where everything goes (sysmon.h). From the top: a title line,
 * four cards side by side (CPU, memory, context switches, uptime), the
 * CPU tiles in rows, and the processes in what is left.
 *
 * The tiles: as many in a row as fit at TILE_W, each group of CPUs (P
 * threads, E threads) starting a row of its own under a small heading
 * when there is more than one group. Their height is what the screen has
 * left after the process table's five rows, between TILE_H_MIN and a
 * maximum, so 28 CPUs fit on 1280x800 and 4 don't leave a screen of
 * empty space. Pure arithmetic: the self-test checks it for screens and
 * CPU counts QEMU doesn't have. */
#include "sysmon.h"

#define MARGIN     16   /* all x the UI scale */
#define GAP        10
#define TILE_GAP   8
#define TILE_W     144
#define TILE_H_MIN 36
#define MIN_ROWS   5    /* process rows the tiles must leave room for */

static uint32_t rows_for(uint32_t n, uint32_t cols)
{
    return (n + cols - 1) / cols;
}

/* The cards' row; returns the y below it. */
static int place_top(struct layout *l, int w)
{
    int u = l->u, m = MARGIN * u, gap = GAP * u;
    l->title = (struct rect){ m, m, w - 2 * m, 36 * u };
    int cy = l->title.y + l->title.h + gap, cw = (w - 2 * m - (NCARDS - 1) * gap) / NCARDS;
    for (int i = 0; i < NCARDS; i++)
        l->card[i] = (struct rect){ m + i * (cw + gap), cy, cw, 96 * u };
    return cy + 96 * u + gap;
}

/* The tiles of one group from y down, `cols` a row; returns the y below them. */
static int place_group(struct layout *l, uint32_t first, uint32_t n, uint32_t cols, int y,
                       const struct rect *tile)
{
    int tg = TILE_GAP * l->u;
    for (uint32_t i = 0; i < n; i++)
        l->cpu[first + i] = (struct rect){ tile->x + (int)(i % cols) * (tile->w + tg),
                                           y + (int)(i / cols) * (tile->h + tg), tile->w, tile->h };
    return y + (int)rows_for(n, cols) * (tile->h + tg);
}

void layout_make(struct layout *l, int w, int h, int ui, const uint32_t group[NGROUPS])
{
    memset(l, 0, sizeof(*l));
    int u = l->u = ui, m = MARGIN * u, tg = TILE_GAP * u;
    int y = place_top(l, w);
    l->row_h = TEXT_H(u) + 6 * u;

    uint32_t cols = (uint32_t)((w - 2 * m + tg) / ((TILE_W + TILE_GAP) * u));
    uint32_t widest = 1, rows = 0, groups = 0;
    for (int g = 0; g < NGROUPS; g++) {
        widest = group[g] > widest ? group[g] : widest;
        groups += group[g] > 0;
    }
    cols = cols < 1 ? 1 : cols > widest ? widest : cols;   /* few CPUs: wider tiles */
    for (int g = 0; g < NGROUPS; g++)
        rows += rows_for(group[g], cols);
    rows = rows ? rows : 1;

    int label_h = groups > 1 ? TEXT_H(u) + 4 * u : 0;
    int table_min = (MIN_ROWS + 1) * l->row_h + 16 * u;
    int room = h - m - y - GAP * u - table_min - (int)groups * label_h;
    int tile_h = (room - (int)rows * tg) / (int)rows, tile_max = (rows <= 2 ? 110 : 64) * u;
    tile_h = tile_h > tile_max ? tile_max : tile_h < TILE_H_MIN * u ? TILE_H_MIN * u : tile_h;
    struct rect tile = { m, 0, (w - 2 * m - ((int)cols - 1) * tg) / (int)cols, tile_h };

    uint32_t first = 0;
    for (int g = 0; g < NGROUPS; g++) {
        if (!group[g])
            continue;
        if (label_h) {
            l->label[g] = (struct rect){ m, y, w - 2 * m, TEXT_H(u) };
            y += label_h;
        }
        y = place_group(l, first, group[g], cols, y, &tile);
        first += group[g];
    }
    y += GAP * u - tg;
    int fit = (h - m - y - 16 * u) / l->row_h - 1;
    l->rows = fit > TOP_MAX ? TOP_MAX : fit < 0 ? 0 : fit;
    l->table = (struct rect){ m, y, w - 2 * m, (l->rows + 1) * l->row_h + 16 * u };
}
