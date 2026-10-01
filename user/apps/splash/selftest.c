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
        uint32_t rgb = (uint32_t)rng_next(&rng) & 0xffffff;
        uint32_t src = argb_pm(rgb, (uint32_t)rng_next(&rng) & 0xff);
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

/* A frame made here, 64x32: grey (Y 100, no colour) but for a brighter
 * block (Y 200) with odd edges and, nearly on it, a red one (Cr 240) on
 * the chroma grid, so each mode has flat areas and edges to show. */
static uint8_t fy[64 * 32], fcb[32 * 16], fcr[32 * 16];

static plm_frame_t fake_frame(void)
{
    plm_frame_t f = { 0 };
    f.width = f.y.width = 64;
    f.height = f.y.height = 32;
    f.cb.width = f.cr.width = 32;
    f.cb.height = f.cr.height = 16;
    f.y.data = fy;
    f.cb.data = fcb;
    f.cr.data = fcr;
    memset(fy, 100, sizeof(fy));
    memset(fcb, 128, sizeof(fcb));
    memset(fcr, 128, sizeof(fcr));
    for (int y = 9; y < 23; y++)        /* odd edges: a 2:1 box straddles them */
        for (int x = 17; x < 47; x++)
            fy[y * 64 + x] = 200;
    for (int y = 4; y < 12; y++)        /* a red chroma block: luma 16..47 x 8..23 */
        for (int x = 8; x < 24; x++)
            fcr[y * 32 + x] = 240;
    return f;
}

/* Draw the fake frame on a screen of sw x sh; pixel (x, y) of it. */
static uint32_t fake_draw(int sw, int sh, int x, int y, char *mode, size_t n)
{
    static uint32_t px[256 * 128];
    plm_frame_t f = fake_frame();
    scr.w = scr.s.w = scr.s.stride = sw;
    scr.h = scr.s.h = sh;
    scr.s.px = px;
    memset(px, 0, sizeof(px));
    struct video_layout l;
    video_layout_for(64, 32, &l);   /* video.c's rules, without a file */
    draw_frame(&f, &l);
    if (mode)
        snprintf(mode, n, "%d/%d", l.mode, l.n);
    return px[(l.y + y) * sw + l.x + x];
}

static void check_draw(void)
{
    char m[16];
    plm_frame_t f = fake_frame();
    uint32_t grey = video_pixel(&f, 0, 0), red = video_pixel(&f, 20, 10);
    uint32_t a = fake_draw(64, 32, 0, 0, m, sizeof(m));
    uint32_t inside = fake_draw(64, 32, 20, 10, NULL, 0);
    uint32_t edge = fake_draw(64, 32, 16, 10, NULL, 0);   /* left of a chroma edge */
    fun_check(!strcmp(m, "0/1") && a == grey && inside == red,
              "1:1: flat areas exact (chroma interpolated only at edges)");
    fun_check(color_diff(edge, red) > 8 && color_diff(edge, video_pixel(&f, 15, 10)) > 8,
              "1:1: a chroma edge is blended, not a 2x2 step");
    uint32_t up = fake_draw(128, 64, 41, 21, m, sizeof(m));
    fun_check(!strcmp(m, "0/2") && up == red, "2x on a screen twice as big");
    uint32_t box = fake_draw(32, 16, 4, 2, m, sizeof(m));       /* source 8..9 x 4..5 */
    uint32_t boxin = fake_draw(32, 16, 10, 5, NULL, 0);
    fun_check(!strcmp(m, "1/2") && box == grey && boxin == red, "1/2 box: flat areas exact");
    uint32_t boxedge = fake_draw(32, 16, 8, 6, NULL, 0);   /* Y 100 and 200 averaged */
    fun_check(color_diff(boxedge, grey) > 20 && color_diff(boxedge, video_pixel(&f, 20, 12)) > 20,
              "1/2 box: an edge pixel is the average");
    uint32_t bil = fake_draw(48, 24, 2, 2, m, sizeof(m));
    uint32_t bilin = fake_draw(48, 24, 22, 11, NULL, 0);
    fun_check(!strcmp(m, "2/1") && bil == grey && bilin == red, "3/4 bilinear: flat areas exact");
}

static void check_video(const uint8_t *mpg, size_t len)
{
    if (!mpg) {
        fun_check(false, "splash.mpg is in bootfs");
        return;
    }
    /* Laid out 1:1 on a screen of the file's own size, drawn as on the PC. */
    scr.w = 4096;
    scr.h = 4096;
    status_t st = video_open(mpg, len);
    fun_check(st == OK, "the video opens (an MPEG-1 program stream)");
    if (st != OK)
        return;
    const struct video_layout *l = video_layout();
    video_close();
    scr.w = scr.s.w = scr.s.stride = l->w;
    scr.h = scr.s.h = l->h;
    scr.s.px = big_alloc((uint64_t)l->w * l->h * 4);
    if (!scr.s.px || video_open(mpg, len) != OK) {
        fun_check(false, "memory for a frame");
        return;
    }
    unsigned frames = 0;
    uint32_t corner = 0;
    uint64_t dec = 0, draw = 0, worst = 0, t = now();
    plm_frame_t *f;
    while ((f = video_next())) {
        uint64_t t1 = now();
        video_draw(f);
        uint64_t t2 = now();
        dec += t1 - t;
        draw += t2 - t1;
        worst = t1 - t > worst ? t1 - t : worst;
        if (!frames)
            corner = video_pixel(f, 8, 8);
        frames++;
        t = now();
    }
    double fps = video_fps(), secs = video_seconds();
    video_close();
    unsigned us_dec = frames ? (unsigned)(dec / 1000 / frames) : 0;
    unsigned us_draw = frames ? (unsigned)(draw / 1000 / frames) : 0;
    say("splash: selftest: %ux%u, %u frames: decoded in %u.%u ms a frame (worst %lu ms), "
        "drawn 1:1 in %u.%u ms (%u threads)\n", (unsigned)l->w, (unsigned)l->h, frames,
        us_dec / 1000, us_dec / 100 % 10, (unsigned long)(worst / NS_PER_MS), us_draw / 1000,
        us_draw / 100 % 10, pool_threads());
    unsigned want = (unsigned)(secs * fps + 0.5);
    fun_check(fps >= 23.9 && fps <= 60.1 && frames + 1 >= want && frames <= want + 1,
              "every frame of the file's length at its rate");
    fun_check(color_diff(corner, SPLASH_BG) <= 6, "the first frame's corner is #1E1A1D");
    int16_t *pcm = NULL;
    size_t n = 0;
    st = sound_decode(mpg, len, &pcm, &n);
    say("splash: selftest: sound %s, %lu frames (%lu ms; the video %lu ms)\n", status_str(st),
        (unsigned long)n, (unsigned long)(n * 1000 / SPLASH_RATE),
        (unsigned long)(secs * 1000));
    fun_check(st == OK && n + SPLASH_RATE / 4 >= (size_t)(secs * SPLASH_RATE) &&
              n <= (size_t)(secs * SPLASH_RATE) + SPLASH_RATE / 4,
              "the sound: 48 kHz stereo, as long as the video");
}

int splash_selftest(const uint8_t *mpg, size_t len)
{
    fun_selftest_begin("splash", 62);
    check_blend();
    check_shapes();
    check_draw();
    check_video(mpg, len);
    return fun_selftest_end();
}
