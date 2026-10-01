/* jamcover: the self-test (`jamcover --selftest`, `run jamcover
 * --selftest` from the shell): stb_image on a 4x4 PNG with alpha and a
 * 16x16 JPEG (<testpics.h>), a cut-off PNG, a PNG whose header says
 * 5000x5000 (refused before it is decoded), bytes that are no picture,
 * and a whole cover made of the PNG at both sizes, premultiplied. That the
 * helper's crash or hang costs jamjar nothing is jamjar's self-test. */
#include <testpics.h>
#include "jamcover_int.h"

static void test_stbi(void)
{
    int w = 0, h = 0;
    uint8_t *px = stbi_rgba(test_png4, sizeof(test_png4), &w, &h);
    bool ok = px && w == 4 && h == 4 && px[0] == 255 && px[1] == 0 && px[3] == 255 &&
              px[12] == 0 && px[14] == 255 && px[15] == 128;
    stbi_arena_reset();
    fun_check(ok, "decode: a PNG with alpha");
    px = stbi_rgba(test_jpg16, sizeof(test_jpg16), &w, &h);
    ok = px && w == 16 && h == 16;
    for (int i = 0; ok && i < 16 * 16; i++) {
        const uint8_t *q = px + 4 * i;
        ok &= q[0] > 190 && q[0] < 210 && q[1] > 110 && q[1] < 130 && q[2] > 30 && q[2] < 50;
    }
    stbi_arena_reset();
    fun_check(ok, "  ... a JPEG");
}

/* The refusals, each through cover_decode as a request would go. */
static void test_refused(uint32_t *small)
{
    static uint8_t bad[sizeof(test_png4)];
    int w = 0, h = 0;
    memcpy(bad, test_png4, sizeof(bad));
    bool ok = cover_decode(bad, 50, small, NULL, &w, &h) == ERR_INVALID_ARGS;   /* cut in IDAT */
    bad[16] = bad[17] = 0;   /* IHDR: 5000 x 5000 (the CRC is not checked by stb) */
    bad[18] = 0x13;
    bad[19] = 0x88;
    bad[20] = bad[21] = 0;
    bad[22] = 0x13;
    bad[23] = 0x88;
    ok &= stbi_size(bad, sizeof(bad), &w, &h) && w == 5000 && !cover_size_ok(w, h);
    ok &= cover_decode(bad, sizeof(bad), small, NULL, &w, &h) == ERR_OUT_OF_RANGE;
    ok &= cover_size_ok(640, 640) && cover_size_ok(611, 640) && !cover_size_ok(0, 10);
    fun_check(ok, "  ... a cut-off PNG fails; one said to be 5000x5000 is refused unread");
    static const uint8_t junk[64] = "not a picture at all, just some words in a tag";
    fun_check(cover_decode(junk, sizeof(junk), small, NULL, &w, &h) == ERR_NOT_SUPPORTED,
              "  ... bytes that are no PNG or JPEG: not supported");
}

/* The PNG made bigger to both sizes: red on the left, blue at half alpha
 * (premultiplied: 0x80000080) on the right, both sizes. */
static void test_cover(uint32_t *small, uint32_t *large)
{
    int w = 0, h = 0;
    status_t st = cover_decode(test_png4, sizeof(test_png4), small, large, &w, &h);
    unsigned s = JAMCOVER_SMALL, l = JAMCOVER_LARGE;
    bool ok = st == OK && w == 4 && h == 4;
    ok &= small[0] == 0xffff0000u && small[s - 1] == 0x80000080u &&
          small[(s - 1) * s] == 0xffff0000u && small[s * s - 1] == 0x80000080u;
    ok &= large[0] == 0xffff0000u && large[l * l - 1] == 0x80000080u;
    fun_check(ok, "cover: the PNG at 256 and 512, premultiplied, red left, blue right");
}

int jamcover_selftest(void)
{
    fun_selftest_begin("jamcover", 72);
    uint32_t *small = big_alloc((uint64_t)JAMCOVER_SMALL * JAMCOVER_SMALL * 4);
    uint32_t *large = big_alloc((uint64_t)JAMCOVER_LARGE * JAMCOVER_LARGE * 4);
    if (!small || !large) {
        fun_check(false, "memory for the pixels");
        return fun_selftest_end();
    }
    test_stbi();
    test_refused(small);
    test_cover(small, large);
    return fun_selftest_end();
}
