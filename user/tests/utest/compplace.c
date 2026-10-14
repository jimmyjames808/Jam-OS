/* utest: where an xdg_positioner puts a popup (user/services/compositor
 * xdgplace.c, linked in: arithmetic only), each case placed exactly.
 *
 * t_popup_place: a 60x40 popup against the anchor rectangle (10, 10) 20x20
 * of its parent: every anchor point (the corners, the edges' middles, the
 * middle) with the gravity down and right; every gravity from the middle
 * (an odd size rounds the half down); the offset added. Then against
 * bounds of 0..100 each way (the room on the output, in the parent's
 * coordinates): no adjustment leaves it outside; flip_x and flip_y mirror
 * the anchor and the gravity when that fits, and undo a flip that doesn't;
 * slide_x and slide_y move it in from either side the gravity's way first
 * (none counting as right, or down), and a popup wider than the bounds
 * ends against the edge the protocol's two steps leave it at; resize_x and
 * resize_y cut it to the bounds, and leave one with nothing inside as it
 * was; the protocol's order: a flip that fits wins over a slide, a slide
 * over a resize; every adjustment at once in a corner. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl/xdg_shell.h>
#include "utest.h"
#include "xdg.h"

#define A_NONE   JWL_XDG_POSITIONER_ANCHOR_NONE
#define A_T      JWL_XDG_POSITIONER_ANCHOR_TOP
#define A_B      JWL_XDG_POSITIONER_ANCHOR_BOTTOM
#define A_L      JWL_XDG_POSITIONER_ANCHOR_LEFT
#define A_R      JWL_XDG_POSITIONER_ANCHOR_RIGHT
#define A_TL     JWL_XDG_POSITIONER_ANCHOR_TOP_LEFT
#define A_BL     JWL_XDG_POSITIONER_ANCHOR_BOTTOM_LEFT
#define A_TR     JWL_XDG_POSITIONER_ANCHOR_TOP_RIGHT
#define A_BR     JWL_XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT
#define FLIP_X   JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
#define FLIP_Y   JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y
#define SLIDE_X  JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
#define SLIDE_Y  JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
#define RESIZE_X JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X
#define RESIZE_Y JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y

static const struct comp_box anywhere = { -100000, -100000, 100000, 100000 };
static const struct comp_box room = { 0, 0, 100, 100 };

/* A w x h popup against the anchor rectangle (ax, ay) aw x ah. */
static struct xdg_rules rules(int32_t w, int32_t h, int32_t ax, int32_t ay, int32_t aw,
                              int32_t ah)
{
    return (struct xdg_rules){ .w = w, .h = h, .ax = ax, .ay = ay, .aw = aw, .ah = ah,
                               .size_set = true, .rect_set = true };
}

/* r with anchor a, gravity g and the adjustments placed in bounds b is
 * the box (x1, y1) .. (x2, y2). */
static bool placed(struct xdg_rules r, uint32_t a, uint32_t g, uint32_t adjust, struct comp_box b,
                   int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    r.anchor = a;
    r.gravity = g;
    r.adjust = adjust;
    struct comp_box p = xdg_place(&r, b);
    if (p.x1 != x1 || p.y1 != y1 || p.x2 != x2 || p.y2 != y2)
        FAIL("anchor %u gravity %u adjust %u: placed at %d,%d..%d,%d, want %d,%d..%d,%d", a, g,
             adjust, p.x1, p.y1, p.x2, p.y2, x1, y1, x2, y2);
    return true;
}

/* Every anchor point, the gravity down and right: the popup's top left there. */
static bool anchors(void)
{
    struct xdg_rules r = rules(60, 40, 10, 10, 20, 20);
    static const struct { uint32_t a; int32_t x, y; } at[] = {
        { A_TL, 10, 10 }, { A_T, 20, 10 },  { A_TR, 30, 10 }, { A_L, 10, 20 }, { A_NONE, 20, 20 },
        { A_R, 30, 20 },  { A_BL, 10, 30 }, { A_B, 20, 30 },  { A_BR, 30, 30 },
    };
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++)
        CHECK(placed(r, at[i].a, A_BR, 0, anywhere, at[i].x, at[i].y, at[i].x + 60, at[i].y + 40));
    return true;
}

/* Every gravity from the middle of the rectangle (20, 20). */
static bool gravities(void)
{
    struct xdg_rules r = rules(60, 40, 10, 10, 20, 20);
    static const struct { uint32_t g; int32_t x, y; } at[] = {
        { A_TL, -40, -20 }, { A_T, -10, -20 }, { A_TR, 20, -20 }, { A_L, -40, 0 },
        { A_NONE, -10, 0 }, { A_R, 20, 0 },    { A_BL, -40, 20 }, { A_B, -10, 20 },
        { A_BR, 20, 20 },
    };
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++)
        CHECK(placed(r, A_NONE, at[i].g, 0, anywhere, at[i].x, at[i].y, at[i].x + 60,
                     at[i].y + 40));
    /* an odd size: its half rounds down */
    CHECK(placed(rules(61, 41, 10, 10, 20, 20), A_NONE, A_NONE, 0, anywhere, -10, 0, 51, 41));
    /* a rectangle of no size: a point */
    CHECK(placed(rules(60, 40, 7, 9, 0, 0), A_BR, A_BR, 0, anywhere, 7, 9, 67, 49));
    /* the offset moves the result */
    r.dx = 5;
    r.dy = -3;
    CHECK(placed(r, A_BR, A_BR, 0, anywhere, 35, 27, 95, 67));
    return true;
}

/* flip_x and flip_y, at the right and bottom edges of 0..100. */
static bool flips(void)
{
    /* from the right edge of (80, 80) 10x10, 30x10 to the right: past 100 */
    struct xdg_rules r = rules(30, 10, 80, 80, 10, 10);
    CHECK(placed(r, A_R, A_R, 0, room, 90, 80, 120, 90));             /* none: stays out */
    CHECK(placed(r, A_R, A_R, FLIP_X, room, 50, 80, 80, 90));         /* to the left of it */
    CHECK(placed(r, A_BR, A_BR, FLIP_X, room, 50, 90, 80, 100));      /* corner: x only */
    struct xdg_rules d = rules(10, 30, 80, 80, 10, 10);
    CHECK(placed(d, A_B, A_B, FLIP_Y, room, 85 - 5, 50, 85 + 5, 80));  /* above it */
    CHECK(placed(d, A_BR, A_BR, FLIP_Y, room, 90, 50, 100, 80));      /* corner: y only */
    /* a flip that doesn't fit either is undone */
    struct xdg_rules wide = rules(90, 10, 40, 10, 10, 10);
    CHECK(placed(wide, A_R, A_R, FLIP_X, room, 50, 10, 140, 20));
    /* and a slide comes after it */
    CHECK(placed(wide, A_R, A_R, FLIP_X | SLIDE_X, room, 10, 10, 100, 20));
    /* a flip that fits wins over a slide */
    CHECK(placed(r, A_R, A_R, FLIP_X | SLIDE_X, room, 50, 80, 80, 90));
    return true;
}

/* slide_x and slide_y from either side, and wider than the bounds. */
static bool slides(void)
{
    struct xdg_rules r = rules(30, 30, 80, 80, 10, 10);
    CHECK(placed(r, A_R, A_R, SLIDE_X, room, 70, 70, 100, 100));      /* in from the right */
    CHECK(placed(r, A_B, A_B, SLIDE_Y, room, 70, 70, 100, 100));      /* in from below */
    struct xdg_rules l = rules(30, 30, 10, 10, 10, 10);
    CHECK(placed(l, A_L, A_L, SLIDE_X, room, 0, 0, 30, 30));          /* in from the left */
    CHECK(placed(l, A_T, A_T, SLIDE_Y, room, 0, 0, 30, 30));          /* in from above */
    /* centred on the anchor (no gravity): its left edge out, slid right */
    CHECK(placed(rules(60, 10, 0, 0, 10, 10), A_NONE, A_NONE, SLIDE_X, room, 0, 0, 60, 10));
    /* wider than the bounds: the gravity right, from x 50: back until its
     * left edge meets 0 */
    struct xdg_rules w = rules(150, 10, 40, 10, 10, 10);
    CHECK(placed(w, A_R, A_R, SLIDE_X, room, 0, 10, 150, 20));
    /* the gravity left, its right edge at 40: on until it meets 100 */
    CHECK(placed(w, A_L, A_L, SLIDE_X, room, -50, 10, 100, 20));
    /* and a slide wins over a resize */
    CHECK(placed(r, A_R, A_R, SLIDE_X | RESIZE_X, room, 70, 70, 100, 100));
    return true;
}

/* resize_x and resize_y. */
static bool resizes(void)
{
    struct xdg_rules r = rules(30, 30, 80, 80, 10, 10);
    CHECK(placed(r, A_R, A_R, RESIZE_X, room, 90, 70, 100, 100));     /* 10 wide is left */
    CHECK(placed(r, A_BR, A_BR, RESIZE_X | RESIZE_Y, room, 90, 90, 100, 100));
    struct xdg_rules l = rules(30, 30, 10, 10, 10, 10);
    CHECK(placed(l, A_TL, A_TL, RESIZE_X | RESIZE_Y, room, 0, 0, 10, 10));
    /* nothing of it inside: left as it was */
    struct xdg_rules out = rules(20, 20, 110, 10, 10, 10);
    CHECK(placed(out, A_R, A_R, RESIZE_X, room, 120, 5, 140, 25));
    /* wider than the bounds, after the slide: cut to them */
    CHECK(placed(rules(150, 10, 40, 10, 10, 10), A_R, A_R, SLIDE_X | RESIZE_X, room, 0, 10, 100,
                 20));
    return true;
}

/* Everything at once, a menu opening at the bottom right corner of the
 * room: flipped both ways, so it opens up and left of the anchor. */
static bool corner(void)
{
    struct xdg_rules r = rules(40, 30, 90, 90, 5, 5);
    CHECK(placed(r, A_BR, A_BR, 63, room, 50, 60, 90, 90));
    /* a menu too tall to flip: flipped across, slid up, then cut */
    struct xdg_rules tall = rules(40, 150, 90, 20, 5, 5);
    CHECK(placed(tall, A_BR, A_BR, 63, room, 50, 0, 90, 100));
    return true;
}

bool t_popup_place(void)
{
    return anchors() && gravities() && flips() && slides() && resizes() && corner();
}
