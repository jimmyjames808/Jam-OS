/* console: drawing the cell grid into the terminal window's buffers
 * (window mode, window.c; console.h).
 *
 * The window has two buffers (libjwl's): the compositor shows one while
 * the next frame is drawn into the other. Each buffer has a shadow grid
 * here, what each of its cells shows, so a frame draws only the cells
 * that buffer doesn't show yet. What the compositor is told changed (the
 * damage) is measured against the buffer it shows now, not the one drawn
 * into: whole rows of cells (GH pixels high, the window's width), merged
 * into runs, at most DAMAGE_RUNS of them (more: one run from the first to
 * the last).
 *
 * A buffer's shadow is forgotten (every cell drawn again) when the buffer
 * is new (a new size, a new pool, or its pixels moved), after a reconnect
 * and when the grid's size changes. The window's pixels past the last
 * whole cell (a size that isn't a multiple of 8x16) are the background. */
#include "console.h"

#define DAMAGE_RUNS 16

/* The palette as xrgb8888 (screen.c's colours). */
static const uint32_t rgb[16] = {
    0x101018, 0xcc4444, 0x44aa44, 0xccaa33, 0x4466cc, 0xaa44aa, 0x44aaaa, 0xb0b0b0,
    0x707070, 0xff6666, 0x66dd66, 0xffdd55, 0x6699ff, 0xdd77dd, 0x66dddd, 0xf0f0f0,
};

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
}

static void draw_cell(uint32_t x, uint32_t y, struct cell c, bool inverse)
{
    uint32_t fg = rgb[c.attr & 15], bg = rgb[c.attr >> 4];
    if (inverse) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    uint8_t block[GH];
    const uint8_t *g = cell_bits(c, block);
    uint32_t stride = (uint32_t)frame.stride / 4;
    uint32_t *row = frame.px + (size_t)y * GH * stride + (size_t)x * GW;
    for (int i = 0; i < GH; i++, row += stride) {
        uint8_t bits = g[i];
        for (int j = 0; j < GW; j++)
            row[j] = (bits & (0x80 >> j)) ? fg : bg;
    }
}

/* grid_walk's callback: cell (x, y) of the frame should show c. */
static void show_cell(uint32_t x, uint32_t y, struct cell c, uint8_t inv)
{
    size_t i = (size_t)y * cols + x;
    struct slot_shadow *s = &slots[frame.slot], *o = shown >= 0 ? &slots[shown] : NULL;
    if (!s->valid || s->cells[i].ch != c.ch || s->cells[i].attr != c.attr ||
        s->inverse[i] != inv) {
        draw_cell(x, y, c, inv);
        s->cells[i] = c;
        s->inverse[i] = inv;
    }
    if (!o || o == s || !o->valid || o->cells[i].ch != c.ch || o->cells[i].attr != c.attr ||
        o->inverse[i] != inv)
        row_damaged[y] = true;
}

/* A fresh buffer: the margins past the last whole cell in the background. */
static void fill_margins(void)
{
    uint32_t stride = (uint32_t)frame.stride / 4, bg = rgb[C_BLACK];
    uint32_t gw = cols * GW, gh = rows * GH;
    for (uint32_t y = 0; y < (uint32_t)frame.height; y++) {
        uint32_t *p = frame.px + (size_t)y * stride;
        for (uint32_t x = y < gh ? gw : 0; x < (uint32_t)frame.width; x++)
            p[x] = bg;
    }
}

/* The damaged rows as runs of whole cell rows, the window's width. */
static unsigned damage_runs(struct jwl_rect *r)
{
    unsigned n = 0;
    uint32_t first = rows, last = 0;
    bool over = false;
    for (uint32_t y = 0; y < rows; y++) {
        if (!row_damaged[y])
            continue;
        first = first == rows ? y : first;
        last = y;
        if (n && r[n - 1].y + r[n - 1].h == (int32_t)(y * GH))
            r[n - 1].h += GH;
        else if (n < DAMAGE_RUNS)
            r[n++] = (struct jwl_rect){ 0, (int32_t)(y * GH), frame.width, GH };
        else
            over = true;
    }
    if (over) {   /* too many: one run over all of them */
        r[0] = (struct jwl_rect){ 0, (int32_t)(first * GH), frame.width,
                                  (int32_t)((last + 1 - first) * GH) };
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
    if (frame.width < (int32_t)(cols * GW) || frame.height < (int32_t)(rows * GH))
        return;   /* a size the grid doesn't fit yet: its configure comes */
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
