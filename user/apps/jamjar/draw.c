/* jamjar: the frame, back to front: the background, the panels and the
 * stereo bars (faded out as the big view comes), the big view's sunburst
 * (growing from its centre as it comes), the cover and the names beside
 * it, then the overlays: the roulette and the help.
 *
 * Most frames differ from the last in little: the bars, the progress bar
 * and its times (in the big view, the sunburst) move by themselves 60
 * times a second, the rest of the picture (the lists, the text, the
 * covers, the panels) only when something is asked for or arrives. So a
 * frame is drawn in two parts: the rest (draw_static) and the moving
 * parts over it (draw_dynamic, in the rectangles dyn_rects gives). The
 * picture below the moving parts is kept (redraw.under). A frame whose
 * rest is what the last frame's was (the same `key`: draw_key hashes
 * every input of the rest; read it before adding anything to the rest
 * that reads state it doesn't) puts that back, draws the moving parts
 * again and says (a->dmg) that only their rectangles changed. Any other
 * frame (a new layout, hover, a scroll, a new track, a cover arriving,
 * the help, the roulette, the big view coming or going, ...) is drawn
 * whole into scr.s and presented whole (gfx_present compares all of it).
 * The self-test (selftest.c) draws sequences of such frames both ways and
 * compares the pixels. */
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

/* ---- the parts ---------------------------------------------------------------------- */

enum { MODE_WHOLE, MODE_PANELS, MODE_BIG };

/* How this frame is drawn: the panels (the usual view), the big view
 * settled, or whole each time (a transition, an overlay over everything). */
static int frame_mode(const struct app *a)
{
    if (a->help || a->roul.on)
        return MODE_WHOLE;
    if (a->full_t <= 0.0f)
        return MODE_PANELS;
    if (a->full_t < 1.0f)
        return MODE_WHOLE;
    /* The sunburst is drawn after the cover and the names here, before
     * them in a transition: the same picture if it keeps clear of them
     * (and of the mark at the top right). */
    const struct layout *lo = &a->lo;
    int u = lo->u, r = lo->burst_r + 4 * u + 2;
    const struct rect mark = { lo->w - 56 * u, 16 * u, 40 * u, 40 * u };
    const struct rect *clear[] = { &lo->big_art, &lo->big_text, &mark };
    for (unsigned i = 0; i < sizeof(clear) / sizeof(clear[0]); i++) {
        const struct rect *q = clear[i];
        int dx = lo->burst_x < q->x ? q->x - lo->burst_x
                 : lo->burst_x > q->x + q->w ? lo->burst_x - (q->x + q->w) : 0;
        int dy = lo->burst_y < q->y ? q->y - lo->burst_y
                 : lo->burst_y > q->y + q->h ? lo->burst_y - (q->y + q->h) : 0;
        if ((int64_t)dx * dx + (int64_t)dy * dy <= (int64_t)r * r)
            return MODE_WHOLE;
    }
    return MODE_BIG;
}

/* The rectangles of the moving parts of mode, clipped to the screen. */
static unsigned dyn_rects(const struct app *a, int mode, struct rect out[DYN_MAX])
{
    const struct layout *lo = &a->lo;
    struct rect all[DYN_MAX];
    unsigned k = 0, n = 0;
    if (mode == MODE_PANELS) {
        all[k++] = lo->jam;
        all[k++] = progress_area(lo);
    } else {
        int r = lo->burst_r + 4 * lo->u + 2;
        all[k++] = (struct rect){ lo->burst_x - r, lo->burst_y - r, 2 * r, 2 * r };
    }
    for (unsigned i = 0; i < k; i++) {
        struct rect q = all[i];
        int x1 = q.x + q.w > lo->w ? lo->w : q.x + q.w;
        int y1 = q.y + q.h > lo->h ? lo->h : q.y + q.h;
        q.x = q.x < 0 ? 0 : q.x;
        q.y = q.y < 0 ? 0 : q.y;
        q.w = x1 - q.x;
        q.h = y1 - q.y;
        if (q.w > 0 && q.h > 0)
            out[n++] = q;
    }
    return n;
}

/* Everything but the moving parts. */
static void draw_static(struct app *a, int mode)
{
    background(&a->lo, 255);
    if (mode == MODE_PANELS) {
        draw_top(a);
        draw_library(a);
        draw_now(a);
    } else {
        big_title(a);
    }
}

/* The moving parts over it; serial: on this thread alone (a frame of
 * little work: waking every CPU for it costs more than it does). */
static void draw_dynamic(struct app *a, uint64_t t, int mode, bool serial)
{
    const struct layout *lo = &a->lo;
    if (mode == MODE_PANELS) {
        draw_progress(a, t);
        if (serial)
            bars_draw_serial(&a->bars, &scr.s, &lo->jam);
        else
            bars_draw(&a->bars, &scr.s, &lo->jam);
    } else {
        burst_draw(&a->bars, &scr.s, lo->burst_x, lo->burst_y, lo->burst_r, 1.0f);
    }
}

/* ---- what the rest of the frame is drawn from ----------------------------------------- */

static uint64_t hb(uint64_t h, const void *p, size_t n)   /* FNV-1a over n bytes */
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static uint64_t hs(uint64_t h, const char *s)
{
    return hb(h, s, strlen(s) + 1);
}

#define HV(h, v) ((h) = hb((h), &(v), sizeof(v)))

/* A hash of everything draw_static(mode) reads that can change from one
 * frame to the next, and nothing that only the moving parts read (the
 * bars, the elapsed time, the track's length). Text is hashed as its
 * bytes. Time enters only as the few things drawn by it: the search box's
 * caret, a toast's expiry, a player gone quiet. */
static uint64_t draw_key(const struct app *a, int mode)
{
    const struct layout *lo = &a->lo;
    const struct snap *s = &a->snap;
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)mode;
    h = hb(h, lo, sizeof(*lo));
    uint32_t cg = cover_gen();
    HV(h, cg);
    h = hs(h, s->path);
    h = hs(h, a->now.title);
    h = hs(h, a->now.artist);
    h = hs(h, a->now.album);
    h = hs(h, a->now.year);
    if (mode != MODE_PANELS)
        return h;
    uint64_t t = now();
    uint32_t flags = (uint32_t)a->searching | (uint32_t)toast_shown(a) << 1 |
                     (uint32_t)a->ordered << 2 | (uint32_t)a->drag_vol << 3 |
                     (uint32_t)a->lib.ready << 4 | (uint32_t)a->view_ok << 5 |
                     (uint32_t)s->link << 6 | (uint32_t)snap_stale(s, t) << 7 |
                     (uint32_t)(a->searching && t / (500 * NS_PER_MS) % 2 == 0) << 8 |
                     (uint32_t)rect_has(&lo->vol, a->mx, a->my) << 9 |
                     (uint32_t)rect_has(&lo->mode, a->mx, a->my) << 10;
    for (int b = 0; b < NBTNS; b++)
        flags |= (uint32_t)rect_has(&lo->btn[b], a->mx, a->my) << (11 + b);
    HV(h, flags);
    int32_t vol = a->drag_vol ? a->vol_shown : s->volume;
    HV(h, vol);
    HV(h, s->playing);
    HV(h, s->sleep_s);
    HV(h, s->tracks);
    HV(h, a->hover_col);
    HV(h, a->hover_row);
    HV(h, a->now_track);
    HV(h, a->lib.nfiles);
    HV(h, a->lib.err);
    HV(h, a->lib.ntracks);
    HV(h, a->lib.nalbums);
    HV(h, a->lib.nartists);
    h = hs(h, s->folder);
    h = hs(h, s->note);
    h = hs(h, a->view.query);
    if (toast_shown(a))
        h = hs(h, a->toast);
    if (a->lib.ready && a->view_ok) {
        const struct view *v = &a->view;
        HV(h, v->col);
        for (int c = 0; c < NCOLS; c++) {
            HV(h, v->sel[c]);
            HV(h, v->top[c]);
            HV(h, v->nrows[c]);
            for (int i = 0; i < lo->rows && v->top[c] + i < (int)v->nrows[c]; i++)
                HV(h, v->row[c][v->top[c] + i]);
        }
    }
    return h;
}

/* ---- a frame ----------------------------------------------------------------------- */

void draw_forget(struct app *a)
{
    a->rd.valid = false;
}

/* Every part in the order a transition needs: the panels fading out as
 * the big view comes, the sunburst growing, the overlays. */
static void draw_whole(struct app *a, uint64_t t)
{
    const struct layout *lo = &a->lo;
    float k = a->full_t * a->full_t * (3 - 2 * a->full_t);   /* smoothstep */
    background(lo, 255);
    if (a->full_t < 1.0f) {
        draw_top(a);
        draw_library(a);
        draw_now(a);
        draw_progress(a, t);
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

/* Room for under[i] to be w x h; false if there is no memory. */
static bool under_room(struct redraw *d, unsigned i, int w, int h)
{
    size_t need = (size_t)w * (size_t)h;
    if (d->cap[i] < need) {
        free(d->under[i].px);
        d->under[i].px = malloc(need * sizeof(uint32_t));
        d->cap[i] = d->under[i].px ? need : 0;
        if (!d->under[i].px)
            return false;
    }
    d->under[i].w = w;
    d->under[i].h = h;
    d->under[i].stride = w;
    return true;
}

void draw_frame(struct app *a, uint64_t t)
{
    struct redraw *d = &a->rd;
    int mode = d->off ? MODE_WHOLE : frame_mode(a);
    a->dmg_all = true;
    a->ndmg = 0;
    if (mode == MODE_WHOLE) {
        d->valid = false;
        d->whole++;
        draw_whole(a, t);
        return;
    }
    struct rect dyn[DYN_MAX];
    unsigned n = dyn_rects(a, mode, dyn);
    uint64_t key = draw_key(a, mode);
    if (d->valid && d->mode == mode && d->key == key && d->n == n) {
        /* The rest is as it was: put back what is under the moving parts
         * and draw them again; only their rectangles changed. */
        for (unsigned i = 0; i < n; i++)
            blit(&scr.s, d->dyn[i].x, d->dyn[i].y, &d->under[i],
                 &(struct rect){ 0, 0, d->dyn[i].w, d->dyn[i].h });
        draw_dynamic(a, t, mode, true);
        for (unsigned i = 0; i < n; i++)
            a->dmg[i] = d->dyn[i];
        a->ndmg = n;
        a->dmg_all = false;
        d->part++;
        return;
    }
    draw_static(a, mode);
    d->valid = n > 0;
    for (unsigned i = 0; i < n && d->valid; i++) {   /* keep what is under the moving parts */
        d->valid = under_room(d, i, dyn[i].w, dyn[i].h);
        if (d->valid)
            blit(&d->under[i], 0, 0, &scr.s, &dyn[i]);
        d->dyn[i] = dyn[i];
    }
    d->mode = mode;
    d->key = key;
    d->n = n;
    d->whole++;
    draw_dynamic(a, t, mode, false);
}
