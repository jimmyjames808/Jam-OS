/* splash: the video's decoder and its place on the screen (splash_int.h).
 *
 * pl_mpeg decodes the file's MPEG-1 video frame by frame (audio left to
 * sound.c's decoder of its own): each frame is Y at full size and Cb, Cr at
 * half size both ways (4:2:0). draw.c turns a frame into the screen's back
 * buffer; this file decides how. The file is made at the PC's 2560x1440
 * (tools/mksplash.sh), so there it is drawn pixel for pixel. Otherwise:
 *   - a screen at least twice as big: the largest integer scale that fits
 *     (each pixel repeated; DRAW_UP);
 *   - a smaller screen: scaled down to fit, keeping its shape: by an
 *     integer n, an n x n box average (2560x1440 on 1280x800: n = 2;
 *     DRAW_BOX), else bilinear (2560x1440 on 1920x1080; DRAW_BILINEAR).
 * The picture is centred; the rest of the screen is the background the
 * frame has (SPLASH_BG), drawn by main.c. */
#include "splash_int.h"

static plm_t *plm;
static struct video_layout lay;

void video_layout_for(int w, int h, struct video_layout *out)
{
    struct video_layout lay = { .w = w, .h = h, .n = 1 };
    if (w <= scr.w && h <= scr.h) {
        int k = scr.w / w < scr.h / h ? scr.w / w : scr.h / h;
        lay.mode = DRAW_UP;
        lay.n = k > 8 ? 8 : k;
        lay.ow = w * lay.n;
        lay.oh = h * lay.n;
    } else {
        /* Down: the tighter of the two ratios decides (in 1/65536). */
        uint64_t sx = ((uint64_t)scr.w << 16) / (uint64_t)w;
        uint64_t sy = ((uint64_t)scr.h << 16) / (uint64_t)h;
        uint64_t s = sx < sy ? sx : sy;
        lay.ow = (int)(((uint64_t)w * s) >> 16);
        lay.oh = (int)(((uint64_t)h * s) >> 16);
        int n = w / (lay.ow ? lay.ow : 1);
        if (n >= 2 && w % n == 0 && h % n == 0 && w / n <= scr.w && h / n <= scr.h &&
            w / n >= lay.ow - 1 && h / n >= lay.oh - 1) {
            lay.mode = DRAW_BOX;
            lay.n = n;
            lay.ow = w / n;
            lay.oh = h / n;
        } else {
            lay.mode = DRAW_BILINEAR;
        }
    }
    lay.x = (scr.w - lay.ow) / 2;
    lay.y = (scr.h - lay.oh) / 2;
    *out = lay;
}

status_t video_open(const uint8_t *mpg, size_t len)
{
    /* pl_mpeg reads the bytes only; its type isn't const. */
    plm = plm_create_with_memory((uint8_t *)(uintptr_t)mpg, len, 0);
    if (!plm)
        return ERR_NO_MEMORY;
    plm_set_audio_enabled(plm, 0);
    int w = plm_get_width(plm), h = plm_get_height(plm);
    if (!plm_has_headers(plm) || w < 16 || h < 16 || w > 4096 || h > 4096 || w % 2 || h % 2) {
        video_close();
        return ERR_NOT_SUPPORTED;
    }
    video_layout_for(w, h, &lay);
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

double video_seconds(void)
{
    return plm ? plm_get_duration(plm) : 0;
}

const struct video_layout *video_layout(void)
{
    return &lay;
}

const char *video_mode(char *buf, size_t n)
{
    if (lay.mode == DRAW_UP && lay.n == 1)
        snprintf(buf, n, "1:1");
    else if (lay.mode == DRAW_UP)
        snprintf(buf, n, "%dx", lay.n);
    else if (lay.mode == DRAW_BOX)
        snprintf(buf, n, "1/%d (box)", lay.n);
    else
        snprintf(buf, n, "%dx%d (bilinear)", lay.ow, lay.oh);
    return buf;
}

void video_draw(const plm_frame_t *f)
{
    draw_frame(f, &lay);
}
