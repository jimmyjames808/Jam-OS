/* A window's shape (paint.h, look.h): its rounded corners, and its shadow.
 * Both are drawn tile by tile on the painting workers, each touching only
 * the pixels of its own small boxes: the r by r corner squares, and the
 * shadow's ring round the frame.
 *
 * Corners. Before a window is drawn into a tile, the pixels below its
 * corner squares are kept (shape_save); after, each pixel there becomes
 * what was below, the window's pixel and the edge's colour, by the corner
 * mask's coverages (mask.c):
 *     out = below * (255 - cov) + edge * ring + window * (cov - ring)   (/ 255)
 * so the window, the client's own pixels included, is cut round and its
 * outline or border follows the curve. A window with round corners
 * doesn't hide what is below its corner squares: paint.c asks
 * shape_corner_meets before it skips anything under it.
 *
 * Shadows. A floating window's shadow is the Gaussian-like blur of its
 * frame moved down (look.h). Blurring a box is the same as blurring each
 * axis apart, so the shadow's darkness at (x, y) is
 *     alpha * fx(x) * fy(y),   fx(x) = T(x - x1) - T(x - x2)
 * for the moved frame [x1, x2), where T is the blurred edge: the running
 * sum of the blur's kernel (three boxes of width BOX convolved), made once
 * here, 16-bit fixed point, cut to 0 and to 1 REACH pixels out. Pixels the
 * window covers aren't drawn (all of the frame but its corner squares), so
 * a shadow costs only its ring. Black over what is below, by that alpha:
 * each channel times (255 - a) / 255, rounded. */
#include "paint.h"

#define T_ONE 65536u   /* the edge profile's 1 */

/* One shadow's numbers and its edge profile. */
struct shadow {
    int32_t  reach, dy;
    uint32_t alpha;
    uint32_t t[2 * LOOK_SHADOW_REACH_MAX];   /* T(d) for d in [-reach, reach), T_ONE = 1 */
};

static struct shadow shadows[2];   /* [0] not focused, [1] focused */

/* ---- corners -------------------------------------------------------------------------- */

const struct corner_mask *shape_corners(const struct comp_window *w)
{
    enum look_kind k = look_of(w);
    const struct corner_mask *m = k == LOOK_FLOATING ? &mask_float
                                  : k == LOOK_TILED  ? &mask_tile
                                                     : NULL;
    struct comp_box f = window_frame(w);
    if (!m || f.x2 - f.x1 < 2 * m->r || f.y2 - f.y1 < 2 * m->r)
        return NULL;
    return m;
}

/* Corner k's square of frame f (0 top left, 1 top right, 2 bottom left, 3
 * bottom right). */
static struct comp_box corner_box(struct comp_box f, int32_t r, int k)
{
    int32_t x = k & 1 ? f.x2 - r : f.x1, y = k & 2 ? f.y2 - r : f.y1;
    return (struct comp_box){ x, y, x + r, y + r };
}

bool shape_corner_meets(const struct comp_window *w, struct comp_box b)
{
    const struct corner_mask *m = shape_corners(w);
    if (!m)
        return false;
    struct comp_box f = window_frame(w);
    for (int k = 0; k < 4; k++)
        if (!box_empty(box_intersect(corner_box(f, m->r, k), b)))
            return true;
    return false;
}

void shape_save(const struct comp_window *w, const struct tile_buf *t, uint32_t *save)
{
    const struct corner_mask *m = shape_corners(w);
    if (!m)
        return;
    struct comp_box f = window_frame(w);
    for (int k = 0; k < 4; k++) {
        struct comp_box c = corner_box(f, m->r, k), in = box_intersect(c, t->b);
        uint32_t *to = save + k * m->r * m->r;
        for (int32_t y = in.y1; y < in.y2; y++)
            memcpy(to + (y - c.y1) * m->r + (in.x1 - c.x1), tile_row(t, y) + (in.x1 - t->b.x1),
                   (size_t)(in.x2 - in.x1) * 4);
    }
}

/* Corner k (box c) where it meets t: below from save, edge colour e. */
static void clip_corner(const struct corner_mask *m, const struct tile_buf *t, struct comp_box c,
                        int k, const uint32_t *save, uint32_t e)
{
    struct comp_box in = box_intersect(c, t->b);
    for (int32_t y = in.y1; y < in.y2; y++) {
        int32_t j = k & 2 ? c.y2 - 1 - y : y - c.y1;
        uint32_t *row = tile_row(t, y) - t->b.x1;
        const uint32_t *below = save + (y - c.y1) * m->r - c.x1;
        for (int32_t x = in.x1; x < in.x2; x++) {
            int32_t i = k & 1 ? c.x2 - 1 - x : x - c.x1;
            uint32_t cov = m->cov[j * m->r + i], ring = m->ring[j * m->r + i];
            if (cov == 255 && ring == 0)
                continue;   /* all window */
            uint32_t p = row[x], b = below[x], out = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t v = (b >> sh & 0xff) * (255 - cov) + (e >> sh & 0xff) * ring +
                             (p >> sh & 0xff) * (cov - ring) + 128;
                out |= ((v + (v >> 8)) >> 8) << sh;
            }
            row[x] = out;
        }
    }
}

void shape_clip(const struct comp_window *w, const struct tile_buf *t, const uint32_t *save)
{
    const struct corner_mask *m = shape_corners(w);
    if (!m)
        return;
    struct comp_box f = window_frame(w);
    uint32_t e = title_edge_colour(w);
    for (int k = 0; k < 4; k++)
        clip_corner(m, t, corner_box(f, m->r, k), k, save + k * m->r * m->r, e);
}

/* ---- shadows -------------------------------------------------------------------------- */

/* The edge profile of a blur of three boxes `box` wide: the kernel (its
 * 3 * box - 2 weights sum to box^3), and its running sum at each offset d
 * from the edge, the pixel's centre taken (d = 0: the first pixel inside). */
static void make_shadow(struct shadow *sh, int32_t box, int32_t reach, int32_t dy, uint32_t alpha)
{
    static uint64_t k1[3 * LOOK_SHADOW_REACH_MAX * 2], k2[sizeof(k1) / sizeof(k1[0])];
    int32_t n = 3 * box - 2, half = n / 2;
    memset(k1, 0, sizeof(k1));
    for (int32_t i = 0; i < box; i++)
        k1[i] = 1;
    for (int pass = 0; pass < 2; pass++) {   /* two more boxes, convolved in */
        memset(k2, 0, sizeof(k2));
        for (int32_t i = 0; i < n; i++)
            for (int32_t j = 0; j < box && i - j >= 0; j++)
                k2[i] += k1[i - j];
        memcpy(k1, k2, sizeof(k1));
    }
    uint64_t total = (uint64_t)box * (uint64_t)box * (uint64_t)box, sum = 0;
    *sh = (struct shadow){ .reach = reach, .dy = dy, .alpha = alpha };
    for (int32_t i = 0; i < half + reach; i++) {
        sum += k1[i];
        if (i - half >= -reach)
            sh->t[i - half + reach] = (uint32_t)((sum * T_ONE + total / 2) / total);
    }
}

void shape_init(void)
{
    make_shadow(&shadows[0], LOOK_SHADOW_BOX, LOOK_SHADOW_REACH, LOOK_SHADOW_DY,
                LOOK_SHADOW_ALPHA);
    make_shadow(&shadows[1], LOOK_SHADOW_F_BOX, LOOK_SHADOW_F_REACH, LOOK_SHADOW_F_DY,
                LOOK_SHADOW_F_ALPHA);
}

/* T(d): the edge profile, 0 before -reach and T_ONE from reach. */
static inline uint32_t edge(const struct shadow *sh, int32_t d)
{
    if (d < -sh->reach)
        return 0;
    return d >= sh->reach ? T_ONE : sh->t[d + sh->reach];
}

/* Pixels x1..x2 of row (in the tile) darkened by the shadow, its row
 * factor ry (alpha * fy) and the moved frame's sides fx1, fx2. */
static void shadow_span(const struct shadow *sh, uint32_t *row, int32_t x1, int32_t x2,
                        uint64_t ry, struct comp_box f)
{
    for (int32_t x = x1; x < x2; x++) {
        uint64_t fx = edge(sh, x - f.x1) - edge(sh, x - f.x2);
        uint32_t a = (uint32_t)((ry * fx + (1ull << 31)) >> 32);
        if (!a)
            continue;
        uint32_t p = row[x], out = 0;
        for (int s = 0; s < 24; s += 8) {
            uint32_t v = (p >> s & 0xff) * (255 - a) + 128;
            out |= ((v + (v >> 8)) >> 8) << s;
        }
        row[x] = out;
    }
}

void shadow_draw(const struct comp_window *w, const struct tile_buf *t)
{
    if (look_of(w) != LOOK_FLOATING)
        return;
    const struct shadow *sh = &shadows[(w->flags & COMP_WIN_FOCUSED) != 0];
    struct comp_box f = window_frame(w), m = box_translate(f, 0, sh->dy);
    struct comp_box all = { m.x1 - sh->reach, m.y1 - sh->reach, m.x2 + sh->reach,
                            m.y2 + sh->reach };
    struct comp_box in = box_intersect(all, t->b);
    const struct corner_mask *cm = shape_corners(w);
    int32_t r = cm ? cm->r : 0;
    for (int32_t y = in.y1; y < in.y2; y++) {
        uint64_t ry = (uint64_t)sh->alpha * (edge(sh, y - m.y1) - edge(sh, y - m.y2));
        if (!ry)
            continue;
        uint32_t *row = tile_row(t, y) - t->b.x1;
        /* The window hides [hx1, hx2) of this row: none outside its frame's
         * rows, less its corner squares in their rows. */
        int32_t cut = y < f.y1 + r || y >= f.y2 - r ? r : 0;
        int32_t hx1 = f.x1 + cut, hx2 = f.x2 - cut;
        if (y < f.y1 || y >= f.y2 || hx1 >= hx2) {
            shadow_span(sh, row, in.x1, in.x2, ry, m);
            continue;
        }
        shadow_span(sh, row, in.x1, hx1 < in.x2 ? hx1 : in.x2, ry, m);
        shadow_span(sh, row, hx2 > in.x1 ? hx2 : in.x1, in.x2, ry, m);
    }
}
