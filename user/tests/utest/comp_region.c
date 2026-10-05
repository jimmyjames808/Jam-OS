/* utest: the compositor's boxes, regions and damage lists
 * (user/services/compositor/region.c, linked in): clamping, adding and
 * subtracting keep the boxes disjoint and the pixels right (checked pixel
 * by pixel against a bitmap), the caps refuse and leave a region as it
 * was, the client's box count follows every change, and damage merges
 * into its bounds past its maximum. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "comp.h"
#include "utest.h"

#define SIDE 32   /* the bitmap the region is checked against */

/* A random box inside the bitmap (and a little past its edges). */
static struct comp_box any_box(uint32_t *seed)
{
    uint32_t r = *seed = *seed * 1103515245u + 12345u;
    int32_t x = (int32_t)(r >> 8 & 31) - 2, y = (int32_t)(r >> 16 & 31) - 2;
    return box_make(x, y, (int32_t)(r >> 24 & 15), (int32_t)(r >> 4 & 15));
}

/* The region holds exactly the bitmap's pixels, and its boxes don't overlap. */
static bool same_pixels(const struct comp_region *g, const bool bits[SIDE][SIDE])
{
    for (int32_t y = -4; y < SIDE + 4; y++)
        for (int32_t x = -4; x < SIDE + 4; x++) {
            bool in = x >= 0 && y >= 0 && x < SIDE && y < SIDE && bits[y][x];
            unsigned hits = 0;
            for (uint32_t i = 0; i < g->n; i++)
                hits += box_contains(g->b[i], x, y);
            if (hits != (in ? 1u : 0u))
                FAIL("pixel %d,%d in %u boxes, want %d", x, y, hits, in);
        }
    return true;
}

static void paint(bool bits[SIDE][SIDE], struct comp_box b, bool on)
{
    for (int32_t y = b.y1 < 0 ? 0 : b.y1; y < b.y2 && y < SIDE; y++)
        for (int32_t x = b.x1 < 0 ? 0 : b.x1; x < b.x2 && x < SIDE; x++)
            bits[y][x] = on;
}

static bool random_changes(void)
{
    static bool bits[SIDE][SIDE];
    uint32_t seed = 7, charge = 0;
    struct comp_region g;
    region_init(&g, &charge);
    memset(bits, 0, sizeof(bits));
    for (unsigned i = 0; i < 400; i++) {
        /* inside the bitmap, which only knows its own pixels */
        struct comp_box b = box_intersect(any_box(&seed), (struct comp_box){ 0, 0, SIDE, SIDE });
        bool add = (seed >> 3 & 3) != 0;
        status_t st = add ? region_add(&g, b) : region_subtract(&g, b);
        if (st == ERR_NO_RESOURCES)
            continue;   /* over the cap: unchanged, as same_pixels checks next */
        CHECK_ST(st, OK);
        paint(bits, b, add);
        CHECK_EQ(charge, g.n);
        if (!same_pixels(&g, bits))
            FAIL("after change %u", i);
    }
    region_fini(&g);
    CHECK_EQ(charge, 0);
    return true;
}

static bool caps(void)
{
    uint32_t charge = 0;
    struct comp_region g, h;
    region_init(&g, &charge);
    region_init(&h, &charge);
    for (int32_t i = 0; i < 256; i++)
        CHECK_ST(region_add(&g, box_make(i * 2, 0, 1, 1)), OK);
    CHECK_ST(region_add(&g, box_make(0, 2, 1, 1)), ERR_NO_RESOURCES);
    CHECK_EQ(g.n, 256);
    CHECK(!region_contains(&g, 0, 2));
    /* a cut that would split boxes past the cap: refused, nothing changed */
    CHECK_ST(region_subtract(&g, box_make(0, 0, 3, 1)), OK);   /* removes two whole boxes */
    CHECK_EQ(g.n, 254);
    CHECK_ST(region_copy(&h, &g), OK);
    CHECK_EQ(charge, 508);
    /* the client's budget: 4096 in all */
    charge = COMP_CLIENT_RECTS_MAX - 1;
    region_clear(&h);   /* its 254 back */
    CHECK_EQ(charge, COMP_CLIENT_RECTS_MAX - 255);
    charge = COMP_CLIENT_RECTS_MAX;
    CHECK_ST(region_add(&h, box_make(0, 0, 1, 1)), ERR_NO_RESOURCES);
    CHECK_EQ(h.n, 0);
    charge = 254;   /* g's, as it really is */
    region_fini(&g);
    region_fini(&h);
    CHECK_EQ(charge, 0);
    return true;
}

static bool boxes_and_damage(void)
{
    struct comp_box b = box_make(0x7ffffff0, -0x7ffffff0, 0x7fffffff, 5);
    CHECK(b.x1 == COMP_COORD_MAX && b.x2 == COMP_COORD_MAX && box_empty(b));
    b = box_make(-10, -10, 0x7fffffff, 20);
    CHECK(b.x1 == -10 && b.x2 == COMP_COORD_MAX && b.y2 == 10);
    CHECK(box_empty(box_make(1, 1, 0, 5)) && box_empty(box_make(1, 1, 5, -1)));
    struct comp_box t = box_translate(box_make(0, 0, 4, 4), 0x7fffffff, 0);
    CHECK(box_empty(t));   /* pushed past the clamp: nothing left */
    struct comp_box boxes[4];
    struct comp_damage d;
    damage_init(&d, boxes, 4);
    damage_add(&d, box_make(0, 0, 10, 10));
    damage_add(&d, box_make(2, 2, 3, 3));   /* inside the first: nothing new */
    CHECK_EQ(d.n, 1);
    for (int32_t i = 1; i < 4; i++)
        damage_add(&d, box_make(i * 20, 0, 5, 5));
    CHECK_EQ(d.n, 4);
    damage_add(&d, box_make(0, 50, 1, 1));   /* a fifth: all merge */
    CHECK_EQ(d.n, 1);
    CHECK(d.b[0].x1 == 0 && d.b[0].y1 == 0 && d.b[0].x2 == 65 && d.b[0].y2 == 51);
    damage_clear(&d);
    CHECK(damage_empty(&d));
    return true;
}

bool t_comp_regions(void)
{
    return boxes_and_damage() && caps() && random_changes();
}
