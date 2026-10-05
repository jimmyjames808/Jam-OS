/* Boxes, regions and damage lists for the compositor (comp.h): pure code,
 * no protocol, so utest links it and checks it directly.
 *
 * A region is a list of disjoint boxes. Adding a box first cuts it out of
 * every box already there, then appends it; subtracting cuts it out of
 * each box (a box cut by another leaves at most four pieces: above,
 * below, left and right of the cut). Results are not merged into fewer
 * boxes: the lists are small (a window's input or opaque region is
 * usually one box) and capped at COMP_REGION_RECTS_MAX, past which a
 * change is refused and the region stays as it was.
 *
 * Coordinates are clamped to +-COMP_COORD_MAX when a box is made, so no
 * arithmetic on two of them can overflow an int32_t. */
#include "comp.h"

static int32_t clamp(int64_t v)
{
    return v < -COMP_COORD_MAX ? -COMP_COORD_MAX : v > COMP_COORD_MAX ? COMP_COORD_MAX : (int32_t)v;
}

struct comp_box box_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0)
        return (struct comp_box){ 0, 0, 0, 0 };
    return (struct comp_box){ clamp(x), clamp(y), clamp((int64_t)x + w), clamp((int64_t)y + h) };
}

bool box_empty(struct comp_box b)
{
    return b.x2 <= b.x1 || b.y2 <= b.y1;
}

struct comp_box box_intersect(struct comp_box a, struct comp_box b)
{
    struct comp_box r = {
        a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1,
        a.x2 < b.x2 ? a.x2 : b.x2, a.y2 < b.y2 ? a.y2 : b.y2,
    };
    return box_empty(r) ? (struct comp_box){ 0, 0, 0, 0 } : r;
}

struct comp_box box_bounds(struct comp_box a, struct comp_box b)
{
    if (box_empty(a))
        return b;
    if (box_empty(b))
        return a;
    return (struct comp_box){
        a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
        a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2,
    };
}

struct comp_box box_translate(struct comp_box b, int32_t dx, int32_t dy)
{
    if (box_empty(b))
        return b;
    struct comp_box r = { clamp((int64_t)b.x1 + dx), clamp((int64_t)b.y1 + dy),
                          clamp((int64_t)b.x2 + dx), clamp((int64_t)b.y2 + dy) };
    return box_empty(r) ? (struct comp_box){ 0, 0, 0, 0 } : r;
}

bool box_contains(struct comp_box b, int32_t x, int32_t y)
{
    return x >= b.x1 && x < b.x2 && y >= b.y1 && y < b.y2;
}

/* ---- regions ---------------------------------------------------------------------- */

void region_init(struct comp_region *g, uint32_t *charge)
{
    g->b = NULL;
    g->n = g->cap = 0;
    g->charge = charge;
}

void region_fini(struct comp_region *g)
{
    region_clear(g);
    free(g->b);
    g->b = NULL;
    g->cap = 0;
}

void region_clear(struct comp_region *g)
{
    if (g->charge)
        *g->charge -= g->n;
    g->n = 0;
}

/* May g hold n boxes (its own cap and its client's)? */
static bool fits(const struct comp_region *g, uint32_t n)
{
    if (n > COMP_REGION_RECTS_MAX)
        return false;
    return !g->charge || n <= g->n || *g->charge + (n - g->n) <= COMP_CLIENT_RECTS_MAX;
}

/* g's boxes become the n boxes of v (the array is taken: g frees it). */
static void take(struct comp_region *g, struct comp_box *v, uint32_t n, uint32_t cap)
{
    if (g->charge)
        *g->charge = *g->charge - g->n + n;
    free(g->b);
    g->b = v;
    g->n = n;
    g->cap = cap;
}

/* The pieces of a that are not in cut (at most four) into out; how many. */
static unsigned cut_out(struct comp_box a, struct comp_box cut, struct comp_box out[4])
{
    struct comp_box i = box_intersect(a, cut);
    if (box_empty(i)) {
        out[0] = a;
        return 1;
    }
    unsigned n = 0;
    if (a.y1 < i.y1)
        out[n++] = (struct comp_box){ a.x1, a.y1, a.x2, i.y1 };   /* above */
    if (i.y2 < a.y2)
        out[n++] = (struct comp_box){ a.x1, i.y2, a.x2, a.y2 };   /* below */
    if (a.x1 < i.x1)
        out[n++] = (struct comp_box){ a.x1, i.y1, i.x1, i.y2 };   /* left */
    if (i.x2 < a.x2)
        out[n++] = (struct comp_box){ i.x2, i.y1, a.x2, i.y2 };   /* right */
    return n;
}

/* g's boxes with cut taken out, and `extra` more free slots after them:
 * the new array and its count, or an error with g unchanged. */
static status_t cut_all(const struct comp_region *g, struct comp_box cut, unsigned extra,
                        struct comp_box **out, uint32_t *n_out)
{
    uint32_t cap = g->n * 4 + extra;
    struct comp_box *v = malloc((cap ? cap : 1) * sizeof(*v));
    if (!v)
        return ERR_NO_MEMORY;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->n; i++)
        n += cut_out(g->b[i], cut, v + n);
    if (!fits(g, n + extra)) {
        free(v);
        return ERR_NO_RESOURCES;
    }
    *out = v;
    *n_out = n;
    return OK;
}

status_t region_add(struct comp_region *g, struct comp_box box)
{
    if (box_empty(box))
        return OK;
    struct comp_box *v;
    uint32_t n;
    status_t st = cut_all(g, box, 1, &v, &n);
    if (st != OK)
        return st;
    v[n++] = box;
    take(g, v, n, g->n * 4 + 1);
    return OK;
}

status_t region_subtract(struct comp_region *g, struct comp_box box)
{
    if (box_empty(box) || !g->n)
        return OK;
    struct comp_box *v;
    uint32_t n;
    status_t st = cut_all(g, box, 0, &v, &n);
    if (st == OK)
        take(g, v, n, g->n * 4);
    return st;
}

status_t region_copy(struct comp_region *dst, const struct comp_region *src)
{
    if (!fits(dst, src->n))
        return ERR_NO_RESOURCES;
    struct comp_box *v = malloc((src->n ? src->n : 1) * sizeof(*v));
    if (!v)
        return ERR_NO_MEMORY;
    memcpy(v, src->b, src->n * sizeof(*v));
    take(dst, v, src->n, src->n);
    return OK;
}

bool region_contains(const struct comp_region *g, int32_t x, int32_t y)
{
    for (uint32_t i = 0; i < g->n; i++)
        if (box_contains(g->b[i], x, y))
            return true;
    return false;
}

struct comp_box region_extents(const struct comp_region *g)
{
    struct comp_box r = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < g->n; i++)
        r = box_bounds(r, g->b[i]);
    return r;
}

/* ---- damage ----------------------------------------------------------------------- */

void damage_init(struct comp_damage *d, struct comp_box *boxes, uint32_t max)
{
    d->n = 0;
    d->max = max;
    d->b = boxes;
}

void damage_clear(struct comp_damage *d)
{
    d->n = 0;
}

void damage_add(struct comp_damage *d, struct comp_box box)
{
    if (box_empty(box))
        return;
    for (uint32_t i = 0; i < d->n; i++) {
        struct comp_box in = box_intersect(d->b[i], box);
        if (in.x1 == box.x1 && in.y1 == box.y1 && in.x2 == box.x2 && in.y2 == box.y2)
            return;   /* already damaged */
    }
    if (d->n < d->max) {
        d->b[d->n++] = box;
        return;
    }
    struct comp_box all = box;
    for (uint32_t i = 0; i < d->n; i++)
        all = box_bounds(all, d->b[i]);
    d->b[0] = all;
    d->n = 1;
}

bool damage_empty(const struct comp_damage *d)
{
    return d->n == 0;
}
