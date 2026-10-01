/* jamjar: the whole frame, back to front: the background, the panels
 * (faded out as the full jar comes), the jam (its top rising from the
 * layout's place to most of the screen in the full jar), the big title
 * over the full jar, then the overlays: the roulette and the help. The
 * frame is drawn whole each time into scr.s; gfx_present sends only the
 * pixels that changed. */
#include "jamjar.h"

struct rect app_jam(const struct app *a)
{
    struct rect r = a->lo.jam;
    float t = a->full_t, k = t * t * (3 - 2 * t);   /* smoothstep */
    int top = r.y + (int)((float)(a->lo.h * 30 / 100 - r.y) * k);
    r.h += r.y - top;
    r.y = top;
    return r;
}

/* Over the full jar: the album's label, and the title, the artist and the
 * album, big, in the middle. */
static void big_title(const struct app *a)
{
    const struct layout *lo = &a->lo;
    int u = lo->u;
    art_mark(&scr.s, lo->w - 56 * u, 16 * u, 40 * u);
    const char *slash = strrchr(a->snap.path, '/');
    if (!a->snap.path[0] || !slash)
        return;
    int art = lo->h * 16 / 100, y = lo->h * 5 / 100;
    art_draw(&scr.s, (lo->w - art) / 2, y, art,
             album_hash(a->snap.path, (size_t)(slash - a->snap.path)), C_BG);
    y += art + 18 * u;
    const struct track_names *n = &a->now;
    int ts = text_width(3 * u, n->title) > lo->w - 80 * u ? 2 * u : 3 * u;
    struct rect r = { 40 * u, y, lo->w - 80 * u, TEXT_H(ts) };
    text_in(&scr.s, &r, ts, C_CREAM, n->title);
    r.y += TEXT_H(ts) + 12 * u;
    r.h = TEXT_H(u);
    text_in(&scr.s, &r, u, C_GOLD, n->artist);
    r.y += TEXT_H(u) + 6 * u;
    text_in(&scr.s, &r, u, C_DIM, n->album);
}

/* The background, a band of rows per pool item: the screen's biggest fill. */
#define BANDS 32

static void bg_band(uint32_t item, uint32_t worker, void *arg)
{
    (void)worker;
    const struct layout *lo = arg;
    int y0 = lo->h * (int)item / BANDS, y1 = lo->h * ((int)item + 1) / BANDS;
    for (int y = y0; y < y1; y++)
        fill(&scr.s, 0, y, lo->w, 1, mixc(C_BG, C_BG2, (uint32_t)(y * 256 / lo->h)));
}

static void background(const struct layout *lo)
{
    if (pool_threads() > 1) {
        pool_run(bg_band, (void *)lo, BANDS);
        return;
    }
    for (uint32_t i = 0; i < BANDS; i++)
        bg_band(i, 0, (void *)lo);
}

void draw_frame(struct app *a, uint64_t t)
{
    const struct layout *lo = &a->lo;
    background(lo);
    if (a->full_t < 1.0f) {
        draw_top(a);
        draw_library(a);
        draw_now(a, t);
        if (a->full_t > 0.0f)   /* fading out: the background over them */
            fill_pm(&scr.s, 0, 0, lo->w, lo->jam.y,
                    argb_pm(C_BG, (uint32_t)(a->full_t * 255.0f)));
    }
    struct rect jr = app_jam(a);
    simmer_draw(&a->sim, &scr.s, &jr);
    if (a->full_t > 0.6f)
        big_title(a);
    roulette_draw(&a->roul, &a->lib, lo, &scr.s);
    if (a->help)
        draw_help(a);
}
