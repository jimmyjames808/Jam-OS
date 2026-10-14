/* Where an xdg_positioner puts a popup (xdg.h's xdg_place): arithmetic
 * only, no protocol, so utest checks every case exactly (utest/compplace.c).
 *
 * The protocol's rules (xdg-shell.xml, xdg_positioner), in the parent's
 * window geometry's coordinates:
 *   - the anchor point is on the anchor rectangle: a corner for a corner
 *     anchor, the middle of an edge for an edge, the middle for none;
 *   - the popup's window geometry goes from that point the way the gravity
 *     says (to the right of it for right, centred on it on an axis with
 *     none), then moved by the offset;
 *   - if it is not wholly inside the bounds (the room it may use on the
 *     output) the adjustments are tried, each axis on its own, in the
 *     protocol's order: flip (the anchor and the gravity mirrored on that
 *     axis, kept only if the flipped box fits there), then slide (moved
 *     along the axis, first the gravity's way then back, as
 *     constraint_adjustment's slide_x describes), then resize (cut to the
 *     bounds, if anything is left of it).
 * An axis with no adjustment that applies is left where the rules put it,
 * outside the bounds or not: the protocol's "none".
 *
 * Every number in the rules is held to +-XDG_PLACE_MAX (xdg.c) and the
 * bounds to the output's size plus a window's place, so no sum here
 * overflows. */
#include <jwl/xdg_shell.h>
#include "xdg.h"

_Static_assert((int)JWL_XDG_POSITIONER_GRAVITY_TOP == (int)JWL_XDG_POSITIONER_ANCHOR_TOP &&
               (int)JWL_XDG_POSITIONER_GRAVITY_BOTTOM == (int)JWL_XDG_POSITIONER_ANCHOR_BOTTOM &&
               (int)JWL_XDG_POSITIONER_GRAVITY_LEFT == (int)JWL_XDG_POSITIONER_ANCHOR_LEFT &&
               (int)JWL_XDG_POSITIONER_GRAVITY_RIGHT == (int)JWL_XDG_POSITIONER_ANCHOR_RIGHT &&
               (int)JWL_XDG_POSITIONER_GRAVITY_TOP_LEFT ==
                   (int)JWL_XDG_POSITIONER_ANCHOR_TOP_LEFT &&
               (int)JWL_XDG_POSITIONER_GRAVITY_BOTTOM_LEFT ==
                   (int)JWL_XDG_POSITIONER_ANCHOR_BOTTOM_LEFT &&
               (int)JWL_XDG_POSITIONER_GRAVITY_TOP_RIGHT ==
                   (int)JWL_XDG_POSITIONER_ANCHOR_TOP_RIGHT &&
               (int)JWL_XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT ==
                   (int)JWL_XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
               "anchors and gravities name the same edges");

#define A_TOP          JWL_XDG_POSITIONER_ANCHOR_TOP
#define A_BOTTOM       JWL_XDG_POSITIONER_ANCHOR_BOTTOM
#define A_LEFT         JWL_XDG_POSITIONER_ANCHOR_LEFT
#define A_RIGHT        JWL_XDG_POSITIONER_ANCHOR_RIGHT
#define A_TOP_LEFT     JWL_XDG_POSITIONER_ANCHOR_TOP_LEFT
#define A_BOTTOM_LEFT  JWL_XDG_POSITIONER_ANCHOR_BOTTOM_LEFT
#define A_TOP_RIGHT    JWL_XDG_POSITIONER_ANCHOR_TOP_RIGHT
#define A_BOTTOM_RIGHT JWL_XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT

/* One axis of a box: lo <= v < hi. */
struct span {
    int32_t lo, hi;
};

/* Which way an anchor or gravity e leans on an axis: -1 (left, or top),
 * +1 (right, or bottom), 0 (neither: the middle). */
static int lean(uint32_t e, bool y)
{
    if (y)
        return e == A_TOP || e == A_TOP_LEFT || e == A_TOP_RIGHT          ? -1
               : e == A_BOTTOM || e == A_BOTTOM_LEFT || e == A_BOTTOM_RIGHT ? 1
                                                                            : 0;
    return e == A_LEFT || e == A_TOP_LEFT || e == A_BOTTOM_LEFT     ? -1
           : e == A_RIGHT || e == A_TOP_RIGHT || e == A_BOTTOM_RIGHT ? 1
                                                                    : 0;
}

/* e mirrored on an axis (flip_x, flip_y): left and right swapped, or top
 * and bottom. */
static uint32_t mirror(uint32_t e, bool y)
{
    static const uint32_t mx[9] = { 0, A_TOP, A_BOTTOM, A_RIGHT, A_LEFT, A_TOP_RIGHT,
                                    A_BOTTOM_RIGHT, A_TOP_LEFT, A_BOTTOM_LEFT };
    static const uint32_t my[9] = { 0, A_BOTTOM, A_TOP, A_LEFT, A_RIGHT, A_BOTTOM_LEFT,
                                    A_TOP_LEFT, A_BOTTOM_RIGHT, A_TOP_RIGHT };
    return e < 9 ? (y ? my : mx)[e] : e;
}

/* One axis of where the rules put the popup for anchor a and gravity g,
 * before any adjustment. */
static struct span raw(const struct xdg_rules *r, uint32_t a, uint32_t g, bool y)
{
    int32_t start = y ? r->ay : r->ax, len = y ? r->ah : r->aw, side = y ? r->h : r->w;
    int32_t off = y ? r->dy : r->dx;
    int la = lean(a, y), lg = lean(g, y);
    int32_t at = la < 0 ? start : la > 0 ? start + len : start + len / 2;
    int32_t lo = lg < 0 ? at - side : lg > 0 ? at : at - side / 2;
    return (struct span){ lo + off, lo + off + side };
}

static bool fits(struct span s, struct span b)
{
    return s.lo >= b.lo && s.hi <= b.hi;
}

static int32_t least(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

/* slide_x or slide_y: first towards the gravity (lg; none counts as
 * right, or down) until the far edge is inside or the near one would leave
 * the bounds, then back the other way on the same terms. */
static struct span slide(struct span s, struct span b, int lg)
{
    for (int pass = 0; pass < 2; pass++) {
        bool forward = (lg >= 0) == (pass == 0);   /* towards +: right, or down */
        int32_t d = 0;
        if (forward && s.lo < b.lo && s.hi <= b.hi)
            d = least(b.lo - s.lo, b.hi - s.hi);
        else if (!forward && s.hi > b.hi && s.lo >= b.lo)
            d = -least(s.hi - b.hi, s.lo - b.lo);
        s.lo += d;
        s.hi += d;
    }
    return s;
}

/* One axis placed and adjusted. */
static struct span place_axis(const struct xdg_rules *r, struct span b, bool y)
{
    uint32_t flip = y ? JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y
                      : JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X;
    uint32_t sl = y ? JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
                    : JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;
    uint32_t rs = y ? JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y
                    : JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X;
    struct span s = raw(r, r->anchor, r->gravity, y);
    if (fits(s, b))
        return s;
    if (r->adjust & flip) {
        struct span f = raw(r, mirror(r->anchor, y), mirror(r->gravity, y), y);
        if (fits(f, b))
            return f;   /* a flip that doesn't fit is undone: the place before it */
    }
    if (r->adjust & sl)
        s = slide(s, b, lean(r->gravity, y));
    if ((r->adjust & rs) && !fits(s, b)) {
        struct span c = { s.lo > b.lo ? s.lo : b.lo, s.hi < b.hi ? s.hi : b.hi };
        if (c.hi > c.lo)
            s = c;   /* nothing left of it inside: left as it was */
    }
    return s;
}

struct comp_box xdg_place(const struct xdg_rules *r, struct comp_box bounds)
{
    struct span x = place_axis(r, (struct span){ bounds.x1, bounds.x2 }, false);
    struct span y = place_axis(r, (struct span){ bounds.y1, bounds.y2 }, true);
    return (struct comp_box){ x.lo, y.lo, x.hi, y.hi };
}
