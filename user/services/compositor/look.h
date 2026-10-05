/* look.h: the compositor's look, every colour and size of it in one place
 * (the owner's "style B1", docs/G1-PLAN.md "The look"): the windows'
 * decorations, their rounded corners and shadows, and the wallpaper.
 * title.c draws the title bars, shape.c the corners and shadows, mask.c
 * makes the anti-aliased shapes they use, wallpaper.c the background.
 *
 * Scale: 1x on every output. The numbers below are pixels as they are:
 * the PC's 2560x1440 monitor is of ordinary density (a 27-inch 1440p
 * screen is about 109 pixels an inch, what other desktops draw at 1x), and
 * the terminal's text, which the owner kept at 8x16 pixels, sits in these
 * windows at that size. A 2x for a dense (4K) screen would multiply them
 * here; not built (no such screen to try it on).
 *
 * What a window looks like follows from its decorations' sizes alone
 * (look_of), which the window manager sets (deco.c) and the scene damages
 * around (deco_set), so a change of look always repaints what it changes:
 *   - floating: a title bar of COMP_TITLE_H with the three circles on its
 *     left and the title centred, a DECO_OUTLINE outline on the other three
 *     sides, LOOK_RADIUS corners (the client's corner pixels clipped too)
 *     and a soft shadow, larger under the focused window;
 *   - maximised: the title bar only, square, no shadow (it fills the
 *     output: there is nothing for corners or a shadow to show);
 *   - tiled: a DECO_BORDER border all round in the focus's colour, its
 *     corners LOOK_TILE_RADIUS round with the border following the curve,
 *     no shadow;
 *   - plain (full screen, and windows with no decorations): the surface
 *     only, square. */
#pragma once

#include <splash.h>
#include "comp.h"

/* ---- floating windows ------------------------------------------------------------------ */

#define LOOK_RADIUS           10         /* a floating window's corners */
#define LOOK_BAR_FOCUSED      0x30363eu  /* the title bar, focused */
#define LOOK_BAR              0x262b31u  /* ... and not */
#define LOOK_OUTLINE_FOCUSED  0x4b535du  /* the 1-pixel outline round the window, focused */
#define LOOK_OUTLINE          0x363c44u  /* ... and not */
#define LOOK_TITLE_FOCUSED    0xe6e9ecu  /* the title's text, focused */
#define LOOK_TITLE            0x7f8892u  /* ... and not */
#define LOOK_TITLE_PX         13         /* the title's size, pixels to the em (Inter) */
#define LOOK_TEXT_PAD         8          /* between the circles and the title, and at the right */

/* The three circles, on the left of the title bar: close, minimise, full
 * screen, in that order (enum title_button). Each a disc LOOK_BTN_D across;
 * on an unfocused window all grey, unless the pointer is over them; with
 * the pointer over them, every circle shows its symbol in a dark shade of
 * its own colour. A press lands on the circle LOOK_BTN_HIT pixels round it
 * (the boxes touch: no press falls between two circles). */
#define LOOK_BTN_D       12
#define LOOK_BTN_LEFT    9                              /* the first's left, from the frame's */
#define LOOK_BTN_GAP     6                              /* between two circles */
#define LOOK_BTN_TOP     ((COMP_TITLE_H - LOOK_BTN_D) / 2)   /* from the bar's top */
#define LOOK_BTN_HIT     3
#define LOOK_CLOSE       0xd4537eu  /* raspberry */
#define LOOK_MINIMISE    0xef9f27u  /* apricot */
#define LOOK_FULLSCREEN  0x7f77ddu  /* blackcurrant */
#define LOOK_BTN_IDLE    0x4a5058u  /* an unfocused window's circles */
#define LOOK_CLOSE_INK   0x5a1f35u  /* the symbols: x, -, the full-screen arrows */
#define LOOK_MINIMISE_INK 0x6b420eu
#define LOOK_FULLSCREEN_INK 0x2d2958u
/* The circles' row, from the frame's left edge to the last one's right. */
#define LOOK_BTNS_W      (LOOK_BTN_LEFT + 3 * LOOK_BTN_D + 2 * LOOK_BTN_GAP)

/* ---- tiled windows --------------------------------------------------------------------- */

#define LOOK_TILE_RADIUS   6
#define LOOK_TILE_FOCUSED  0x7f77ddu   /* the border, focused: blackcurrant */
#define LOOK_TILE          0x363c44u

/* ---- shadows ---------------------------------------------------------------------------
 *
 * A floating window's shadow is its frame, moved down by DY, blurred and
 * darkened by at most ALPHA (of 255) where it is darkest: black over what
 * is below, outside the window's shape only. The blur is three box blurs
 * BOX pixels wide one after the other (a close match for a Gaussian of
 * sigma sqrt((BOX^2 - 1) / 4): 12.5 pixels for 25, which is a CSS blur of
 * 25; 5.5 for 11), made once into an edge profile (shape.c), so a shadow
 * costs a table look-up and a multiply a pixel, never a blur. The blur's
 * tails are cut at REACH pixels from the frame's edges, where they are too
 * faint to change a pixel (under half a level of 255). */
#define LOOK_SHADOW_F_BOX    25         /* focused */
#define LOOK_SHADOW_F_REACH  30
#define LOOK_SHADOW_F_DY     10
#define LOOK_SHADOW_F_ALPHA  128        /* 50% */
#define LOOK_SHADOW_BOX      11         /* not focused */
#define LOOK_SHADOW_REACH    13
#define LOOK_SHADOW_DY       4
#define LOOK_SHADOW_ALPHA    77         /* 30% */
#define LOOK_SHADOW_REACH_MAX LOOK_SHADOW_F_REACH
/* How far either shadow reaches past the frame on each side (the focused
 * one's is the larger every way): what a window's damage takes in. */
#define LOOK_SHADOW_SIDE     LOOK_SHADOW_F_REACH
#define LOOK_SHADOW_ABOVE    (LOOK_SHADOW_F_REACH - LOOK_SHADOW_F_DY)
#define LOOK_SHADOW_BELOW    (LOOK_SHADOW_F_REACH + LOOK_SHADOW_F_DY)

/* ---- the wallpaper ----------------------------------------------------------------------
 *
 * A dark base with three soft glows (wallpaper.c), made once at the
 * output's size. Each glow is a colour at a centre (thousandths of the
 * output's width and height) fading to nothing at a radius (thousandths
 * of the output's longer side) as (1 - d^2/r^2)^2; the glows add to the
 * base, and an 8x8 ordered dither takes the fractions, so the slow slopes
 * show no bands. */
#define LOOK_WALL_BASE 0x161b26u
struct look_glow {
    uint32_t rgb;
    int32_t  cx, cy;               /* thousandths of the width, of the height */
    int32_t  r;                    /* thousandths of the longer side */
};
#define LOOK_GLOWS 3
static const struct look_glow look_glows[LOOK_GLOWS] = {
    { 0x4a2450u, 150, 180, 620 },  /* blackcurrant, top left */
    { 0x5a2c18u, 880, 900, 520 },  /* apricot, bottom right */
    { 0x24203fu, 520, 540, 480 },  /* deep blackcurrant, the middle */
};
/* compctl.blank's screen: the splash's background (nothing drawn, not even
 * the wallpaper, between a reboot's request and the next boot's splash,
 * whose kernel starts the screen in that colour: a reboot looks like
 * switching the PC on, as compctl.idl says). */
#define LOOK_BLANK SPLASH_BG

/* ---- which look a window has ------------------------------------------------------------ */

enum look_kind {
    LOOK_PLAIN,                    /* no decorations: full screen, testwin's */
    LOOK_FLOATING,                 /* title bar and outline */
    LOOK_BAR_ONLY,                 /* maximised: the title bar alone */
    LOOK_TILED,                    /* a border all round, no title bar */
};

static inline enum look_kind look_of(const struct comp_window *w)
{
    if (w->deco_top >= COMP_TITLE_H)
        return w->deco_left > 0 ? LOOK_FLOATING : LOOK_BAR_ONLY;
    if (w->deco_top > 0 || w->deco_left > 0 || w->deco_right > 0 || w->deco_bottom > 0)
        return LOOK_TILED;
    return LOOK_PLAIN;
}
