/* jamcover: one cover, from the picture's bytes to the pixels jamjar keeps.
 * The size is read first (stbi_info) and refused past JAMCOVER_MAX_SIDE or
 * JAMCOVER_MAX_PIXELS before anything is decoded; then stb_image decodes
 * it into its arena as RGBA bytes, which become premultiplied 0xAARRGGBB
 * in place, and the middle square is scaled (libfun's scale_pm) to the
 * sizes asked for. */
#include "jamcover_int.h"

bool cover_size_ok(int w, int h)
{
    return w >= 1 && h >= 1 && w <= (int)JAMCOVER_MAX_SIDE && h <= (int)JAMCOVER_MAX_SIDE &&
           (uint64_t)w * (uint64_t)h <= JAMCOVER_MAX_PIXELS;
}

/* stb_image's RGBA bytes, in place, as premultiplied 0xAARRGGBB. */
static void premultiply(uint8_t *p, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++, p += 4) {
        uint32_t a = p[3], r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
        uint32_t v = a << 24 | r << 16 | g << 8 | b;
        memcpy(p, &v, 4);
    }
}

status_t cover_decode(const uint8_t *pic, size_t n, uint32_t *small, uint32_t *large, int *w,
                      int *h)
{
    int pw = 0, ph = 0;
    if (!stbi_size(pic, n, &pw, &ph))
        return ERR_NOT_SUPPORTED;
    if (!cover_size_ok(pw, ph))
        return ERR_OUT_OF_RANGE;
    uint8_t *rgba = stbi_rgba(pic, n, &pw, &ph);
    if (!rgba || !cover_size_ok(pw, ph)) {
        stbi_arena_reset();
        return ERR_INVALID_ARGS;
    }
    premultiply(rgba, (size_t)pw * (size_t)ph);
    const uint32_t *px = (const uint32_t *)rgba;
    int side = pw < ph ? pw : ph, ox = (pw - side) / 2, oy = (ph - side) / 2;
    const uint32_t *sq = px + (size_t)oy * pw + ox;
    scale_pm(&(struct picture){ sq, side, side, pw }, small, JAMCOVER_SMALL, JAMCOVER_SMALL);
    if (large)
        scale_pm(&(struct picture){ sq, side, side, pw }, large, JAMCOVER_LARGE, JAMCOVER_LARGE);
    stbi_arena_reset();
    *w = pw;
    *h = ph;
    return OK;
}
