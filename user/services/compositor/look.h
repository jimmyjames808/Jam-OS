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
/* compctl.blank's screen: black (nothing drawn, not even the wallpaper,
 * between a reboot's request and the next boot's splash). */
#define LOOK_BLANK 0x000000u

/* ---- the desktop: the strip along the top (strip.c, stripdraw.c) ------------------------
 *
 * A full-width strip LOOK_STRIP_H high, frosted: a blur of the wallpaper
 * under it (three box blurs LOOK_STRIP_BLUR wide, about CSS's blur(16px)),
 * a little more saturated, under a dark tint, with a faint line along its
 * bottom; made once for the output's size (frost.c), so it costs a copy a
 * pixel. On it three islands, slightly lighter (white at
 * LOOK_ISLAND_ALPHA), rounded: left "Jam OS", the screens' dots and "+";
 * centre the screen's windows as chips; right the layout, network and
 * volume icons and the clock. Windows never go under the strip: they
 * start LOOK_STRIP_GAP below it. Colours are the owner's prototype's
 * (rgba() values there are the alphas here, of 255). */
#define LOOK_STRIP_H          40
#define LOOK_STRIP_GAP        6          /* the strip's bottom to the windows' top */
#define LOOK_STRIP_BLUR       33         /* each box blur's width: sigma about 16 */
#define LOOK_STRIP_SAT        333        /* saturation, in 256ths (1.3) */
#define LOOK_STRIP_TINT       0x181b22u  /* rgba(24, 27, 34, .42) */
#define LOOK_STRIP_TINT_A     107
#define LOOK_STRIP_LINE_A     20         /* the bottom line: white at 8% */
#define LOOK_ISLAND_TOP       7
#define LOOK_ISLAND_H         26
#define LOOK_ISLAND_R         10
#define LOOK_ISLAND_EDGE      8          /* the outer islands from the output's sides */
#define LOOK_ISLAND_PAD       8          /* inside an island, at either end */
#define LOOK_ISLAND_GAP       6          /* between the things in an island */
#define LOOK_ISLAND_A         20         /* white at 8% */
#define LOOK_ISLAND_MIDDLE    520        /* the centre island is at most the output less this */
#define LOOK_STRIP_PX         12         /* the strip's text */
#define LOOK_BTN_H            20         /* a button on the strip ("Jam OS", a chip, the clock) */
#define LOOK_BTN_R            6
#define LOOK_BTN_PAD          7          /* text buttons' sides */
#define LOOK_JAM_INK          0xffb340u  /* "Jam OS" */
#define LOOK_JAM_OPEN         0xef9f27u  /* ... its button while the search box is open: */
#define LOOK_JAM_OPEN_A       56         /* apricot at 22% */
#define LOOK_INK              0xe3e8eeu  /* text */
#define LOOK_MUTED            0x8b98a6u  /* quieter text, labels */
#define LOOK_DIM              0x59616cu  /* "No windows", the search box's hint */
#define LOOK_DOT              0x8b939eu  /* a screen's dot; a minimised chip's text */
#define LOOK_DOT_D            7
#define LOOK_DOT_GAP          5
#define LOOK_DOT_CUR_W        18         /* the current screen's pill */
#define LOOK_DOT_CUR_R        4
#define LOOK_DOT_FULL_R       2          /* a full-screen screen's square */
#define LOOK_PLUS             9          /* the "+" across */
#define LOOK_CHIP_PAD         8
#define LOOK_CHIP_MAX         150        /* a chip's width at most */
#define LOOK_CHIP_MIN         40         /* ... and at least (past what fits: hidden) */
#define LOOK_CHIP_FOCUS_A     97         /* the focused chip: raspberry at 38% */
#define LOOK_CHIP_FOCUS_INK   0xffffffu
#define LOOK_CHIP_MIN_DOT     6          /* a minimised chip's apricot dot */
#define LOOK_ICON_W           22         /* the right island's icon buttons */
#define LOOK_ICON_R           5
#define LOOK_ICON_INK         0xc2cbd5u
#define LOOK_ICON_ON_A        115        /* an icon whose popover is open: blackcurrant at 45% */

/* ---- frosted glass: the search box, Alt+Tab, popovers, notifications --------------------
 *
 * A card over a blur of what is behind it, windows included (frost.c: made
 * while the card is open, of its box only, again when what is behind it
 * changes), tinted dark, a 1-pixel light outline, rounded, with the focused
 * window's shadow. Sections inside are split by inset dividers: 1 pixel,
 * white at 10%, stopping at the card's padding. */
#define LOOK_GLASS_TINT       0x1e222au  /* rgba(30, 34, 42, .76) */
#define LOOK_GLASS_TINT_A     194
#define LOOK_GLASS_LINE_A     26         /* the outline and the dividers: white at 10% */
#define LOOK_GLASS_R          12
#define LOOK_GLASS_BLUR       37         /* each box blur's width: sigma about 18 */
#define LOOK_ROW_H            36         /* a row of a list: a letter tile and a name */
#define LOOK_ROW_R            8
#define LOOK_ROW_PAD          10
#define LOOK_ROW_SEL_A        92         /* the selected row: blackcurrant at 36% */
#define LOOK_TILE_D           24         /* a letter tile */
#define LOOK_TILE_R           7
#define LOOK_MENU_PX          13         /* rows' text */
#define LOOK_LABEL_PX         11         /* small labels: "Screen 2", "Output" */
/* The search box: centred, its top at LOOK_SEARCH_TOP thousandths of the
 * output's height, LOOK_SEARCH_W wide (less on a narrow output). */
#define LOOK_SEARCH_W         560
#define LOOK_SEARCH_TOP       150
#define LOOK_SEARCH_PAD       8
#define LOOK_SEARCH_IN_H      36         /* the line typed into */
#define LOOK_SEARCH_PX        16
#define LOOK_SEARCH_LIST      420        /* the list's height at most (and 52% of the output) */
#define LOOK_ALTTAB_W         270
#define LOOK_ALTTAB_PAD       6
#define LOOK_ALTTAB_ROWS      16         /* rows shown at most; the list scrolls past them */
#define LOOK_GROUP_H          22         /* a group's label ("Screen 2") */
/* Popovers: their top 2 pixels below the strip, their right edge on the
 * right edge of what opened them. */
#define LOOK_POP_W            220
#define LOOK_POP_PAD          12
#define LOOK_POP_GAP          2
#define LOOK_POP_DIV          9          /* space above and below a divider */
#define LOOK_POP_LINE         20         /* a line of a popover's text */
#define LOOK_POP_BIG_PX       17         /* the clock's time */
#define LOOK_SLIDER_ON        0xd4537eu  /* the volume slider's filled part: raspberry */
#define LOOK_SLIDER_OFF       0x4a5058u
#define LOOK_LIVE             0x97c459u  /* "Connected": leaf green */
#define LOOK_TODAY            0xef9f27u  /* the calendar's today, apricot, */
#define LOOK_TODAY_INK        0x3a1f00u  /* ... its number dark */
/* Notifications: cards stacked down the top right, under the strip. */
#define LOOK_NOTE_W           300
#define LOOK_NOTE_RIGHT       10
#define LOOK_NOTE_TOP         (LOOK_STRIP_H + 8)
#define LOOK_NOTE_GAP         8
#define LOOK_NOTE_PAD_X       12
#define LOOK_NOTE_PAD_Y       10
#define LOOK_NOTE_TILE        28
#define LOOK_NOTE_TILE_R      8
#define LOOK_NOTE_BTN_H       22
#define LOOK_NOTE_BTN_A       23         /* a button: white at 9% */
#define LOOK_NOTE_PRI         0xef9f27u  /* the first button: apricot at 25%, */
#define LOOK_NOTE_PRI_A       64
#define LOOK_NOTE_PRI_INK     0xffb340u  /* ... its text */

/* ---- animations (anim.c): the owner's timings, ease-out ---------------------------------- */
#define LOOK_ANIM_OPEN_MS     150        /* grow from 92% and fade in; closing the reverse */
#define LOOK_ANIM_OPEN_FROM   920        /* thousandths of the size */
#define LOOK_ANIM_MIN_MS      260        /* shrink into the chip, and out of it */
#define LOOK_ANIM_MIN_TO      80         /* thousandths of the size, in the chip */
#define LOOK_ANIM_MIN_ALPHA   38         /* ... and its alpha there (15%) */
#define LOOK_ANIM_SLIDE_MS    260        /* screens sliding sideways */
#define LOOK_NOTE_IN_MS       200        /* a notification comes in, */
#define LOOK_NOTE_OUT_MS      180        /* ... and fades out */
#define LOOK_NOTE_SHOW_MS     5000       /* ... after this, unless it has buttons */
#define LOOK_ALTTAB_SHOW_MS   120        /* Alt held this long before the list shows */

/* The jam colours of letter tiles and window chips. */
#define LOOK_JAM_RASPBERRY    LOOK_CLOSE
#define LOOK_JAM_APRICOT      LOOK_MINIMISE
#define LOOK_JAM_BLACKCURRANT LOOK_FULLSCREEN

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
