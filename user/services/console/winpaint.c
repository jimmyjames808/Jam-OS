/* console: drawing the cell grid into the terminal window's buffers
 * (window mode, window.c; console.h).
 *
 * The cells are drawn in the window's look (cells.h: JetBrains Mono, or
 * the 8x16 bitmap with `terminal.font = bitmap`), WIN_PAD pixels in from
 * every edge: the padding, and whatever is left past the last whole cell
 * on the right and at the bottom, is the terminal's background.
 *
 * The window has two buffers (libjwl's): the compositor shows one while
 * the next frame is drawn into the other. Each buffer has a shadow grid
 * here, what each of its cells shows, so a frame draws only the cells
 * that buffer doesn't show yet. What the compositor is told changed (the
 * damage) is measured against the buffer it shows now, not the one drawn
 * into: whole rows of cells (a cell high, the window's width), merged
 * into runs, at most DAMAGE_RUNS of them (more: one run from the first to
 * the last); a buffer new to us (its padding just filled) is damaged
 * whole.
 *
 * A buffer's shadow is forgotten (every cell drawn again) when the buffer
 * is new (a new size, a new pool, or its pixels moved), after a reconnect
 * and when the grid's size or the look changes. */
#include "console.h"

#define DAMAGE_RUNS 16

/* One of the window's buffers, as we last drew it. */
struct slot_shadow {
    struct cell *cells;     /* rows * cols: what each cell shows */
    uint8_t     *inverse;   /* rows * cols: drawn inverted (the cursor) */
    uint32_t    *px;        /* the buffer it is of (NULL: none yet) */
    int32_t      w, h;      /* its size */
    bool         valid;     /* cells and inverse say what it shows */
};
static struct slot_shadow slots[2];
static int shown = -1;               /* the slot presented last (-1: none) */

/* The frame being drawn: its buffer, and the rows that differ from the
 * buffer the compositor shows. */
static struct jwl_frame frame;
static bool row_damaged[MAX_ROWS];
static bool all_damaged;             /* a new buffer: damage all of it */

static bool alloc_slot(struct slot_shadow *s)
{
    size_t n = (size_t)rows * cols;
    struct cell *c = malloc(n * sizeof(struct cell));
    uint8_t *inv = malloc(n);
    if (!c || !inv) {
        free(c);
        free(inv);
        return false;
    }
    free(s->cells);
    free(s->inverse);
    s->cells = c;
    s->inverse = inv;
    s->valid = false;
    s->px = NULL;   /* the padding drawn again too (a new grid: maybe a new look) */
    return true;
}

bool paint_regrid(void)
{
    for (unsigned i = 0; i < 2; i++)
        if (!alloc_slot(&slots[i]))
            return false;
    return true;
}

void paint_forget(void)
{
    slots[0].valid = slots[1].valid = false;
    slots[0].px = slots[1].px = NULL;
}

/* grid_walk's callback: cell (x, y) of the frame should show c. */
static void show_cell(uint32_t x, uint32_t y, struct cell c, uint8_t inv)
{
    size_t i = (size_t)y * cols + x;
    struct slot_shadow *s = &slots[frame.slot], *o = shown >= 0 ? &slots[shown] : NULL;
    if (!s->valid || !cell_same(s->cells[i], c) || s->inverse[i] != inv) {
        cell_paint(frame.px, frame.stride / 4, frame.width, frame.height, &look,
                   WIN_PAD + (int32_t)x * look.w, WIN_PAD + (int32_t)y * look.h, c, inv);
        s->cells[i] = c;
        s->inverse[i] = inv;
    }
    if (!o || o == s || !o->valid || !cell_same(o->cells[i], c) || o->inverse[i] != inv)
        row_damaged[y] = true;
}

/* A fresh buffer: everything but the cells (the padding, and what is
 * past the last whole cell) in the background. */
static void fill_margins(void)
{
    uint32_t stride = (uint32_t)frame.stride / 4, bg = cell_palette[C_BLACK];
    int32_t gx1 = WIN_PAD + (int32_t)cols * look.w, gy1 = WIN_PAD + (int32_t)rows * look.h;
    for (int32_t y = 0; y < frame.height; y++) {
        uint32_t *p = frame.px + (size_t)y * stride;
        bool grid_row = y >= WIN_PAD && y < gy1;
        for (int32_t x = 0; x < frame.width; x++) {
            if (grid_row && x == WIN_PAD) {
                x = gx1 - 1;   /* the cells are drawn by show_cell */
                continue;
            }
            p[x] = bg;
        }
    }
}

/* The damaged rows as runs of whole cell rows, the window's width. */
static unsigned damage_runs(struct jwl_rect *r)
{
    if (all_damaged) {
        r[0] = (struct jwl_rect){ 0, 0, frame.width, frame.height };
        return 1;
    }
    unsigned n = 0;
    uint32_t first = rows, last = 0;
    bool over = false;
    for (uint32_t y = 0; y < rows; y++) {
        if (!row_damaged[y])
            continue;
        first = first == rows ? y : first;
        last = y;
        int32_t top = WIN_PAD + (int32_t)y * look.h;
        if (n && r[n - 1].y + r[n - 1].h == top)
            r[n - 1].h += look.h;
        else if (n < DAMAGE_RUNS)
            r[n++] = (struct jwl_rect){ 0, top, frame.width, look.h };
        else
            over = true;
    }
    if (over) {   /* too many: one run over all of them */
        r[0] = (struct jwl_rect){ 0, WIN_PAD + (int32_t)first * look.h, frame.width,
                                  (int32_t)(last + 1 - first) * look.h };
        n = 1;
    }
    return n;
}

/* The buffer from jwl_window_begin: forget its shadow if it isn't the
 * buffer the shadow is of. */
static void check_slot(void)
{
    struct slot_shadow *s = &slots[frame.slot];
    if (s->px == frame.px && s->w == frame.width && s->h == frame.height && s->valid)
        return;
    s->px = frame.px;
    s->w = frame.width;
    s->h = frame.height;
    s->valid = false;
    fill_margins();
    all_damaged = true;
}

void window_render(void)
{
    struct jwl_window *w = window_now();
    if (!w || !slots[0].cells) {
        dirty = false;   /* nothing shows it: drawn in full when a window comes */
        return;
    }
    status_t st = jwl_window_begin(w, &frame);
    if (st == ERR_SHOULD_WAIT)
        return;   /* both buffers shown or in a paint: stay dirty, try again */
    dirty = false;
    if (st != OK)
        return;
    if (frame.width < (int32_t)cols * look.w + 2 * WIN_PAD ||
        frame.height < (int32_t)rows * look.h + 2 * WIN_PAD)
        return;   /* a size the grid doesn't fit yet: its configure comes */
    all_damaged = false;
    check_slot();
    memset(row_damaged, 0, sizeof(row_damaged));
    grid_walk(show_cell);
    slots[frame.slot].valid = true;
    struct jwl_rect r[DAMAGE_RUNS];
    unsigned n = damage_runs(r);
    if (!n)
        return;   /* what is shown is what we have: nothing to present */
    st = jwl_window_present(w, r, n, false);
    if (st == OK)
        shown = (int)frame.slot;
}
