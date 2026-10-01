/* splash: `run splash --alpha`, libfun's alpha blending on the screen.
 *
 * The logo drawn by hand, as it stands at the end of the animation: seven
 * drupelets as anti-aliased discs (disc_aa) in a hexagon round the middle
 * one, a glow behind the gold one (discs of growing radius at low alpha,
 * blended over each other), a translucent panel (fill_pm) and an
 * anti-aliased underline (line_aa). It stays until a key, or DEMO_HOLD.
 * tools/splash-test.sh screenshots it: the discs' colours at their
 * centres, and partly covered pixels round their edges. */
#include "splash_int.h"

#define DEMO_HOLD (4 * NS_PER_S)

/* The logo in the video's 1280x720 frame: centre, the drupelets' radius
 * and spacing, and their colours (the middle one first, then clockwise
 * from the top). */
#define LOGO_X  456
#define LOGO_Y  360
#define LOGO_R  44
#define LOGO_D  91
#define GOLD    0xd99f31u

static const uint32_t colours[7] = { 0x8e1b3b, 0xa9234a, 0xc8274d, GOLD, 0xc8274d, 0xa9234a,
                                     0xc8274d };

int splash_alpha_demo(void)
{
    status_t st = gfx_open_on(SPLASH_BG);
    if (st != OK) {
        printf("splash: no screen (%s)\n", status_str(st));
        return 1;
    }
    float k = (float)(scr.w / 1280 < scr.h / 720 ? scr.w / 1280 : scr.h / 720);
    k = k < 1 ? 1 : k;
    float ox = ((float)scr.w - 1280 * k) / 2, oy = ((float)scr.h - 720 * k) / 2;
    float cx = ox + LOGO_X * k, cy = oy + LOGO_Y * k, r = LOGO_R * k, d = LOGO_D * k;
    /* Hexagon: the neighbours at 0, 60, ... 300 degrees from straight up. */
    float px[7] = { cx }, py[7] = { cy };
    for (int i = 0; i < 6; i++) {
        px[i + 1] = cx + d * (float)sind(i * 1.0471975511965976);
        py[i + 1] = cy - d * (float)cosd(i * 1.0471975511965976);
    }
    fill_pm(&scr.s, (int)(ox + 280 * k), (int)(oy + 200 * k), (int)(720 * k), (int)(320 * k),
            argb_pm(0xffffff, 18));
    for (int g = 8; g >= 1; g--)
        disc_aa(&scr.s, px[3], py[3], r + (float)g * 5 * k, GOLD, 14);
    for (int i = 0; i < 7; i++)
        disc_aa(&scr.s, px[i], py[i], r, colours[i], 255);
    line_aa(&scr.s, cx - 1.5f * d, cy + 1.75f * d, cx + 5.5f * d, cy + 1.5f * d, 3 * k, GOLD, 200);
    gfx_present();
    printf("splash: alpha demo drawn at %dx%d (scale %d)\n", scr.w, scr.h, (int)k);
    (void)gfx_key(now() + DEMO_HOLD);
    gfx_close();
    return 0;
}
