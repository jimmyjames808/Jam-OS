/* splash: the video (splash_int.h).
 *
 * pl_mpeg decodes the file's MPEG-1 video frame by frame (audio left to
 * sound.c's decoder of its own): each frame is Y at full size and Cb, Cr at
 * half size both ways (4:2:0). video_draw converts it to 0xRRGGBB into the
 * screen's back buffer, every pixel repeated `scale` times both ways
 * (nearest neighbour: cheap and sharp), in bands of 16 source rows on the
 * thread pool. The scale is the largest integer at which the frame fits
 * the screen (2 for 1280x720 on 2560x1440; at least 1: a smaller screen
 * shows the middle of it), and the frame is centred; the rest of the
 * screen is the background the frame has (SPLASH_BG), drawn by main.c. */
#include "splash_int.h"

#define BAND 16   /* source rows a pool item converts (a macroblock row) */

static plm_t *plm;
static struct rect area;   /* the scaled frame on the screen (may be bigger than it) */
static int scale;

status_t video_open(const uint8_t *mpg, size_t len)
{
    /* pl_mpeg reads the bytes only; its type isn't const. */
    plm = plm_create_with_memory((uint8_t *)(uintptr_t)mpg, len, 0);
    if (!plm)
        return ERR_NO_MEMORY;
    plm_set_audio_enabled(plm, 0);
    int w = plm_get_width(plm), h = plm_get_height(plm);
    if (!plm_has_headers(plm) || w < 16 || h < 16 || w > 4096 || h > 4096) {
        video_close();
        return ERR_NOT_SUPPORTED;
    }
    scale = scr.w / w < scr.h / h ? scr.w / w : scr.h / h;
    scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    area = (struct rect){ (scr.w - w * scale) / 2, (scr.h - h * scale) / 2, w * scale, h * scale };
    return OK;
}

void video_close(void)
{
    if (plm)
        plm_destroy(plm);
    plm = NULL;
}

plm_frame_t *video_next(void)
{
    return plm ? plm_decode_video(plm) : NULL;
}

double video_fps(void)
{
    double fps = plm ? plm_get_framerate(plm) : 0;
    return fps > 1 && fps < 241 ? fps : 30;
}

struct rect video_area(void)
{
    return area;
}

int video_scale(void)
{
    return scale;
}

static inline uint32_t clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
}

/* Studio-range BT.601 YCbCr (what MPEG-1 holds) to RGB, in 16.16 fixed
 * point: R = 1.164 (Y - 16) + 1.596 Cr', G = ... - 0.392 Cb' - 0.813 Cr',
 * B = ... + 2.017 Cb' (Cb' = Cb - 128, Cr' = Cr - 128). */
static inline uint32_t ycc(int y, int cb, int cr)
{
    int l = (y - 16) * 76309;
    cb -= 128;
    cr -= 128;
    return clamp255((l + cr * 104597 + 32768) >> 16) << 16 |
           clamp255((l - cb * 25675 - cr * 53279 + 32768) >> 16) << 8 |
           clamp255((l + cb * 132201 + 32768) >> 16);
}

uint32_t video_pixel(const plm_frame_t *f, int x, int y)
{
    int c = (y / 2) * (int)f->cb.width + x / 2;
    return ycc(f->y.data[y * (int)f->y.width + x], f->cb.data[c], f->cr.data[c]);
}

/* Source row y of f into the back buffer: its first copy, pixels repeated
 * scale times, clipped to the screen. */
static void draw_row(const plm_frame_t *f, int y, uint32_t *dst, int x0, int x1)
{
    const uint8_t *py = f->y.data + (uint64_t)y * f->y.width;
    const uint8_t *pb = f->cb.data + (uint64_t)(y / 2) * f->cb.width;
    const uint8_t *pr = f->cr.data + (uint64_t)(y / 2) * f->cr.width;
    for (int x = x0; x < x1; x++) {
        uint32_t c = ycc(py[x], pb[x / 2], pr[x / 2]);
        int sx = area.x + x * scale;
        for (int k = 0; k < scale; k++)
            dst[sx + k] = c;
    }
}

static void draw_band(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const plm_frame_t *f = arg;
    int w = (int)f->width;
    /* The source columns that land on the screen (all of them unless the
     * screen is smaller than the frame). */
    int x0 = area.x < 0 ? (-area.x + scale - 1) / scale : 0;
    int x1 = area.x + w * scale > scr.w ? (scr.w - area.x) / scale : w;
    for (int y = (int)item * BAND; y < (int)(item + 1) * BAND && y < (int)f->height; y++) {
        int sy = area.y + y * scale;
        if (sy < 0 || sy + scale > scr.h)
            continue;
        uint32_t *first = scr.s.px + (uint64_t)sy * scr.s.stride;
        draw_row(f, y, first, x0, x1);
        for (int k = 1; k < scale; k++)
            memcpy(first + (uint64_t)k * scr.s.stride + area.x + x0 * scale,
                   first + area.x + x0 * scale, (size_t)(x1 - x0) * scale * 4);
    }
}

void video_draw(const plm_frame_t *f)
{
    pool_run(draw_band, (void *)(uintptr_t)f, (f->height + BAND - 1) / BAND);
}
