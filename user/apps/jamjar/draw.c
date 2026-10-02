/* jamjar: the whole frame, back to front: the background, the panels and
 * the stereo bars (faded out as the big view comes), the big view's
 * sunburst (growing from its centre as it comes), the cover and the names
 * beside it, then the overlays: the roulette and the help. The frame is
 * drawn whole each time into scr.s; gfx_present sends only the pixels
 * that changed. */
#include "jamjar.h"

/* The text centred in r across (cut short with "..." if it is wider). */
static void line_in(const struct rect *r, int scale, uint32_t c, const char *str)
{
    int w = text_width(scale, str);
    if (w <= r->w)
        text(&scr.s, r->x + (r->w - w) / 2, r->y, scale, c, str);
    else
        text_clip(&scr.s, &(struct rect){ r->x, r->y, r->w, TEXT_H(scale) }, scale, c, str);
}

/* Beside the sunburst: the album's cover, and the title, the artist and
 * the album under it; the mark at the top right. */
static void big_title(const struct app *a)
{
    const struct layout *lo = &a->lo;
    int u = lo->u;
    art_mark(&scr.s, lo->w - 56 * u, 16 * u, 40 * u);
    const char *slash = strrchr(a->snap.path, '/');
    if (!a->snap.path[0] || !slash)
        return;
    const struct rect *ar = &lo->big_art;
    /* Its rounded corners over the background's colour at its middle. */
    uint32_t bg = mixc(C_BG, C_BG2, (uint32_t)((ar->y + ar->h / 2) * 256 / lo->h));
    art_cover(&scr.s, ar, album_hash(a->snap.path, (size_t)(slash - a->snap.path)), a->snap.path,
              bg);
    const struct track_names *n = &a->now;
    struct rect r = lo->big_text;
    line_in(&r, 2 * u, C_CREAM, n->title);
    r.y += TEXT_H(2 * u) + 12 * u;
    line_in(&r, u, C_GOLD, n->artist);
    r.y += TEXT_H(u) + 6 * u;
    line_in(&r, u, C_DIM, n->album);
}

/* The background, a band of rows per pool item: the screen's biggest
 * fill. Over what is drawn already at alpha `fade` (0..255) when it is
 * less than 255: the panels fading out as the big view comes. */
#define BANDS 32

struct bg {
    const struct layout *lo;
    uint32_t fade;
};

static void bg_band(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const struct bg *b = arg;
    int w = b->lo->w, h = b->lo->h;
    int y0 = h * (int)item / BANDS, y1 = h * ((int)item + 1) / BANDS;
    for (int y = y0; y < y1; y++) {
        uint32_t c = mixc(C_BG, C_BG2, (uint32_t)(y * 256 / h));
        if (b->fade >= 255)
            fill(&scr.s, 0, y, w, 1, c);
        else
            fill_pm(&scr.s, 0, y, w, 1, argb_pm(c, b->fade));
    }
}

static void background(const struct layout *lo, uint32_t fade)
{
    struct bg b = { lo, fade };
    if (pool_threads() > 1) {
        pool_run(bg_band, &b, BANDS);
        return;
    }
    for (uint32_t i = 0; i < BANDS; i++)
        bg_band(i, 0, &b);
}

void draw_frame(struct app *a, uint64_t t)
{
    const struct layout *lo = &a->lo;
    float k = a->full_t * a->full_t * (3 - 2 * a->full_t);   /* smoothstep */
    background(lo, 255);
    if (a->full_t < 1.0f) {
        draw_top(a);
        draw_library(a);
        draw_now(a, t);
        bars_draw(&a->bars, &scr.s, &lo->jam);
        if (a->full_t > 0.0f)   /* fading out: the background over them */
            background(lo, (uint32_t)(k * 255.0f));
    }
    burst_draw(&a->bars, &scr.s, lo->burst_x, lo->burst_y, lo->burst_r, k);
    if (a->full_t > 0.6f)
        big_title(a);
    roulette_draw(&a->roul, &a->lib, lo, &scr.s);
    if (a->help)
        draw_help(a);
}
