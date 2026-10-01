/* splash: the self-test (`run splash --selftest`).
 *
 * libfun's alpha blending (alpha.c) against its definition: the SSE2 fill
 * against the per-pixel formula on random colours, the exact ends (alpha
 * 0 leaves the pixel, 255 replaces it), an image blit, and the
 * anti-aliased disc and line (full inside, untouched outside, partial on
 * the edge). Then the boot video in bootfs decoded whole, without the
 * screen: its size and rate, every frame, the background colour in the
 * corner of the first and last frames, the gold drupelet in the last (the
 * logo is assembled), the sound's length; with the decode speed, which is
 * what the PC's splash needs to keep up with (33 ms a frame). */
#include "splash_int.h"

#define NPX 37   /* an odd row: the vector path's pairs and its last pixel */

/* |a - b| per channel, the largest. */
static uint32_t color_diff(uint32_t a, uint32_t b)
{
    uint32_t m = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int d = (int)(a >> sh & 0xff) - (int)(b >> sh & 0xff);
        uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        m = ad > m ? ad : m;
    }
    return m;
}

static void check_blend(void)
{
    uint64_t rng = 0x5eed;
    uint32_t px[NPX], want[NPX];
    struct surf s = { px, NPX, 1, NPX };
    bool same = true;
    for (int round = 0; round < 200; round++) {
        uint32_t src = argb_pm((uint32_t)rng_next(&rng) & 0xffffff, (uint32_t)rng_next(&rng) & 0xff);
        for (int i = 0; i < NPX; i++) {
            px[i] = (uint32_t)rng_next(&rng) & 0xffffff;
            want[i] = px_over(px[i], src);
        }
        fill_pm(&s, 0, 0, NPX, 1, src);
        for (int i = 0; i < NPX; i++)
            same &= px[i] == want[i];
    }
    fun_check(same, "fill_pm (SSE2) == px_over on 200 random rows");
    fun_check(argb_pm(0xffffff, 128) == 0x80808080u, "argb_pm(white, 128) = 0x80808080");
    fun_check(px_over(0x000000, argb_pm(0xffffff, 128)) == 0x808080, "half white over black");
    fun_check(px_over(0x123456, 0) == 0x123456, "alpha 0 leaves the pixel");
    fun_check(px_over(0x123456, argb_pm(0xabcdef, 255)) == 0xabcdef, "alpha 255 replaces it");
    uint32_t img[4] = { 0, argb_pm(0xff0000, 255), argb_pm(0x00ff00, 128), 0 };
    struct surf src = { img, 4, 1, 4 };
    for (int i = 0; i < 4; i++)
        px[i] = 0x0000ff;
    blit_pm(&s, 0, 0, &src);
    fun_check(px[0] == 0x0000ff && px[1] == 0xff0000 && px[2] == 0x00807f && px[3] == 0x0000ff,
              "blit_pm: clear, opaque and half pixels");
}

static void check_shapes(void)
{
    static uint32_t px[64 * 64];
    struct surf s = { px, 64, 64, 64 };
    fill(&s, 0, 0, 64, 64, 0);
    disc_aa(&s, 32, 32, 10.3f, 0xffffff, 255);
    uint32_t edge = px[32 * 64 + 42] & 0xff;   /* centre 42.5: 0.2 past r, 0.3 covered */
    fun_check(px[32 * 64 + 32] == 0xffffff && px[32 * 64 + 50] == 0 && px[0] == 0,
              "disc_aa: full inside, nothing outside");
    fun_check(edge > 0 && edge < 255 && px[32 * 64 + 40] == 0xffffff,
              "disc_aa: the edge pixel partly covered");
    fill(&s, 0, 0, 64, 64, 0);
    line_aa(&s, 4, 10.5f, 60, 10.5f, 2.0f, 0xffffff, 255);
    uint32_t side = px[11 * 64 + 30] & 0xff, out = px[13 * 64 + 30];
    fun_check(px[10 * 64 + 30] == 0xffffff && out == 0 && side > 0,
              "line_aa: covered on the line, partly beside it, not off it");
}

static void check_video(const uint8_t *mpg, size_t len)
{
    if (!mpg) {
        fun_check(false, "splash.mpg is in bootfs");
        return;
    }
    scr.w = 1280;   /* video_open lays out for a screen: none here */
    scr.h = 720;
    fun_check(video_open(mpg, len) == OK, "the video opens (an MPEG-1 program stream)");
    uint64_t t = now();
    unsigned frames = 0;
    uint32_t first_corner = 0, last_corner = 0, gold = 0;
    plm_frame_t *f;
    bool size_ok = true;
    while ((f = video_next())) {
        size_ok &= f->width == 1280 && f->height == 720;
        if (!frames)
            first_corner = video_pixel(f, 8, 8);
        last_corner = video_pixel(f, 8, 8);
        gold = video_pixel(f, 535, 405);   /* the gold drupelet's middle, assembled */
        frames++;
    }
    uint64_t ms = (now() - t) / NS_PER_MS;
    video_close();
    say("splash: selftest: %u frames decoded in %lu ms (%lu.%lu ms a frame)\n", frames,
        (unsigned long)ms, (unsigned long)(frames ? ms / frames : 0),
        (unsigned long)(frames ? ms * 10 / frames % 10 : 0));
    fun_check(size_ok && video_fps() > 29.9 && video_fps() < 30.1, "1280x720 at 30 fps");
    fun_check(frames == 195, "195 frames (6.5 s)");
    fun_check(color_diff(first_corner, SPLASH_BG) <= 6 && color_diff(last_corner, SPLASH_BG) <= 6,
              "the background is #1E1A1D");
    fun_check(color_diff(gold, 0xd9a032) <= 24, "the gold drupelet is there at the end");
    int16_t *pcm = NULL;
    size_t n = 0;
    status_t st = sound_decode(mpg, len, &pcm, &n);
    say("splash: selftest: sound %s, %lu frames (%lu ms)\n", status_str(st), (unsigned long)n,
        (unsigned long)(n * 1000 / SPLASH_RATE));
    fun_check(st == OK && n >= 6 * SPLASH_RATE && n <= 7 * SPLASH_RATE,
              "the sound: 48 kHz stereo, 6.5 s");
}

int splash_selftest(const uint8_t *mpg, size_t len)
{
    fun_selftest_begin("splash", 62);
    check_blend();
    check_shapes();
    check_video(mpg, len);
    return fun_selftest_end();
}
