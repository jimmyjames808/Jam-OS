/* jamjar: the panels of the frame: the top bar, the library's three
 * columns and the help card. Now playing is in nowplaying.c, the jam in
 * bars.c, and draw.c puts them together. Everything here reads the app
 * and draws; nothing changes state. */
#include "jamjar.h"

/* ---- the top bar ------------------------------------------------------------------- */

static void search_box(const struct app *a)
{
    const struct layout *lo = &a->lo;
    const struct rect *r = &lo->search;
    int u = lo->u;
    if (r->w < 60 * u)
        return;
    panel(&scr.s, r->x, r->y, r->w, r->h, r->h / 2, a->searching ? C_BERRY1 : C_LINE, 256);
    panel(&scr.s, r->x + u, r->y + u, r->w - 2 * u, r->h - 2 * u, r->h / 2 - u, C_PANEL, 256);
    float cx = (float)(r->x + 18 * u), cy = (float)(r->y + r->h / 2 - u);
    ring_aa(&scr.s, cx, cy, 5.5f * u, 1.6f * u, C_DIM, 255);   /* a magnifier */
    line_aa(&scr.s, cx + 4.0f * u, cy + 4.0f * u, cx + 8.5f * u, cy + 8.5f * u, 2.0f * u, C_DIM,
            255);
    int tx = r->x + 34 * u, ty = r->y + (r->h - TEXT_H(u)) / 2, tw = r->w - 48 * u;
    if (!a->view.query[0] && !a->searching) {
        text(&scr.s, tx, ty, u, C_FAINT, "search   /");
        return;
    }
    int end = text_clip(&scr.s, tx, ty, u, C_CREAM, tw, a->view.query);
    if (a->searching && (now() / (500 * NS_PER_MS)) % 2 == 0)
        fill(&scr.s, end + u, ty, 2 * u, TEXT_H(u), C_GOLD);   /* the caret */
}

/* The right of the bar: a toast, or the library's and the timer's state. */
static void status_line(const struct app *a)
{
    const struct layout *lo = &a->lo;
    int u = lo->u, y = lo->status.y + (lo->status.h - TEXT_H(u)) / 2;
    int right = lo->status.x + lo->status.w;
    char buf[160];
    if (a->toast[0] && now() - a->toast_at < TOAST_NS) {
        int w = text_width(u, a->toast);
        text(&scr.s, right - w, y, u, C_GOLD, a->toast);
        return;
    }
    const struct library *l = &a->lib;
    if (!l->ready)
        snprintf(buf, sizeof(buf), "reading %s: %u files", l->root, l->nfiles);
    else
        snprintf(buf, sizeof(buf), "%u artists, %u albums, %u tracks", l->nartists, l->nalbums,
                 l->ntracks);
    int x = right - text_width(u, buf);
    if (a->snap.sleep_s) {
        char z[32];
        snprintf(z, sizeof(z), "sleep %u:%02u", a->snap.sleep_s / 60, a->snap.sleep_s % 60);
        int zw = text_width(u, z);
        text(&scr.s, right - zw, y, u, C_GOLD, z);
        x -= zw + 20 * u;
    }
    if (x > lo->status.x)
        text(&scr.s, x, y, u, C_FAINT, buf);
}

void draw_top(struct app *a)
{
    const struct layout *lo = &a->lo;
    int u = lo->u;
    art_mark(&scr.s, lo->mark.x, lo->mark.y, lo->mark.w);
    int ty = lo->mark.y + (lo->mark.h - TEXT_H(2 * u)) / 2;
    int x = text(&scr.s, lo->mark.x + lo->mark.w + 12 * u, ty, 2 * u, C_CREAM, "jam");
    text(&scr.s, x, ty, 2 * u, C_GOLD, "jar");
    search_box(a);
    status_line(a);
}

/* ---- the library -------------------------------------------------------------------- */

/* What a row of column c says, and whether it is what is playing. */
static const char *row_text(const struct app *a, int c, uint32_t item, char *buf, size_t cap,
                            const char **right, bool *playing)
{
    const struct library *l = &a->lib;
    int64_t t = a->now_track;
    *right = NULL;
    *playing = false;
    if (c == COL_ARTIST && item == ROW_ALL) {
        snprintf(buf, cap, "%u", l->ntracks);
        *right = buf;
        return "All artists";
    }
    if (c == COL_ARTIST) {
        snprintf(buf, cap, "%u", l->artist[item].tracks);
        *right = buf;
        *playing = t >= 0 && l->album[l->track[t].album].artist == item;
        return l->artist[item].name;
    }
    if (c == COL_ALBUM) {
        *right = l->album[item].year[0] ? l->album[item].year : NULL;
        *playing = t >= 0 && l->track[t].album == item;
        return l->album[item].name;
    }
    *playing = t == (int64_t)item;
    return l->track[item].name;
}

static void row_back(const struct app *a, int c, int row, const struct rect *rr)
{
    const struct view *v = &a->view;
    int u = a->lo.u;
    if (row == v->sel[c]) {
        bool focus = c == v->col;
        panel(&scr.s, rr->x, rr->y, rr->w, rr->h, 6 * u, focus ? C_BERRY1 : C_ROW, 256);
        if (focus)
            fill(&scr.s, rr->x, rr->y + 5 * u, 3 * u, rr->h - 10 * u, C_GOLD);
    } else if (c == a->hover_col && row == a->hover_row) {
        panel(&scr.s, rr->x, rr->y, rr->w, rr->h, 6 * u, C_ROW, 140);
    }
}

static void one_row(const struct app *a, int c, int row, const struct rect *rr)
{
    int u = a->lo.u, x = rr->x + 10 * u, y = rr->y + (rr->h - TEXT_H(u)) / 2;
    uint32_t item = a->view.row[c][row];
    char buf[24];
    const char *right;
    bool playing;
    const char *name = row_text(a, c, item, buf, sizeof(buf), &right, &playing);
    row_back(a, c, row, rr);
    if (c == COL_ALBUM) {
        int sz = rr->h - 6 * u;
        uint32_t under = row == a->view.sel[c] ? (c == a->view.col ? C_BERRY1 : C_ROW) : C_PANEL;
        const struct lib_album *al = &a->lib.album[item];
        art_cover(&scr.s, rr->x + 4 * u, rr->y + 3 * u, sz, al->hash, a->lib.track[al->first].path,
                  under);
        x = rr->x + sz + 12 * u;
    }
    int rw = right ? text_width(u, right) + 12 * u : 0;
    if (playing) {
        disc_aa(&scr.s, (float)(rr->x + rr->w - rw - 9 * u), (float)(rr->y + rr->h / 2),
                3.5f * u, C_GOLD, 255);
        rw += 16 * u;
    }
    uint32_t col = row == a->view.sel[c] ? C_CREAM : playing ? C_GOLD : C_DIM;
    text_clip(&scr.s, x, y, u, col, rr->x + rr->w - rw - x - 4 * u, name);
    if (right)
        text(&scr.s, rr->x + rr->w - text_width(u, right) - 8 * u, y, u, C_FAINT, right);
}

static void column(const struct app *a, int c)
{
    static const char *const title[NCOLS] = { "ARTISTS", "ALBUMS", "TRACKS" };
    const struct layout *lo = &a->lo;
    const struct view *v = &a->view;
    int u = lo->u;
    char head[48];
    snprintf(head, sizeof(head), "%s  %u", title[c],
             c == COL_ARTIST ? v->nrows[c] - 1 : v->nrows[c]);
    text(&scr.s, lo->head[c].x + 10 * u, lo->head[c].y + (lo->head[c].h - TEXT_H(u)) / 2, u,
         c == v->col ? C_GOLD : C_FAINT, head);
    const struct rect *l = &lo->list[c];
    int n = (int)v->nrows[c], top = v->top[c];
    for (int i = 0; i < lo->rows && top + i < n; i++) {
        struct rect rr = { l->x, l->y + i * lo->row_h, l->w - 6 * u, lo->row_h - 2 * u };
        one_row(a, c, top + i, &rr);
    }
    if (n > lo->rows) {   /* where the rows shown are in the column */
        int h = l->h * lo->rows / n, y = l->y + (l->h - h) * top / (n - lo->rows);
        panel(&scr.s, l->x + l->w - 3 * u, y, 3 * u, h, u, C_LINE, 256);
    }
}

void draw_library(struct app *a)
{
    const struct layout *lo = &a->lo;
    int u = lo->u;
    panel(&scr.s, lo->lib.x, lo->lib.y, lo->lib.w, lo->lib.h, 14 * u, C_PANEL, 256);
    if (!a->lib.ready || !a->view_ok || !a->lib.ntracks) {
        char msg[300];
        if (!a->lib.ready)
            snprintf(msg, sizeof(msg), "reading %s ...", a->lib.root);
        else if (a->lib.err)
            snprintf(msg, sizeof(msg), "%s: %s", a->lib.root, status_str(a->lib.err));
        else
            snprintf(msg, sizeof(msg), "no .mp3 or .wav files in %s", a->lib.root);
        struct rect r = { lo->lib.x, lo->lib.y, lo->lib.w, lo->lib.h };
        text_in(&scr.s, &r, u, C_DIM, msg);
        return;
    }
    for (int c = 0; c < NCOLS; c++)
        column(a, c);
}

/* ---- help ---------------------------------------------------------------------------- */

static const char *const help_keys[][2] = {
    { "Space", "play / pause (stopped: play the selection)" },
    { "Enter", "play the selection" },
    { "n  p", "next track, previous (. and , too)" },
    { "x", "stop" },
    { "+  -", "volume up, down" },
    { "arrows  Tab", "move in the lists, change column" },
    { "/", "search (Esc clears it)" },
    { "a", "play everything" },
    { "s", "shuffle or in order" },
    { "r", "jam roulette: spin for an album" },
    { "z", "sleep timer: 15, 30, 60, 90 min, off" },
    { "f", "the big view: the cover and the bars, full screen" },
    { "l", "show the track playing" },
    { "?  h", "this help" },
    { "q  Esc", "quit (the music plays on)" },
    { "mouse", "click selects, double-click plays, wheel scrolls" },
    { "", "drag the volume; click the cover to spin" },
};

void draw_help(const struct app *a)
{
    const struct layout *lo = &a->lo;
    int u = lo->u, n = (int)(sizeof(help_keys) / sizeof(help_keys[0]));
    int lh = TEXT_H(u) + 8 * u, w = 560 * u, h = n * lh + 90 * u;
    w = w > lo->w - 40 * u ? lo->w - 40 * u : w;
    struct rect r = { (lo->w - w) / 2, (lo->h - h) / 2, w, h };
    fill_pm(&scr.s, 0, 0, lo->w, lo->h, argb_pm(0x0a0709, 150));
    panel(&scr.s, r.x - 2 * u, r.y - 2 * u, r.w + 4 * u, r.h + 4 * u, 18 * u, C_BERRY1, 256);
    panel(&scr.s, r.x, r.y, r.w, r.h, 16 * u, C_PANEL, 256);
    text(&scr.s, r.x + 28 * u, r.y + 22 * u, 2 * u, C_GOLD, "keys");
    for (int i = 0; i < n; i++) {
        int y = r.y + 70 * u + i * lh;
        text(&scr.s, r.x + 28 * u, y, u, C_CREAM, help_keys[i][0]);
        text_clip(&scr.s, r.x + 170 * u, y, u, C_DIM, r.w - 190 * u, help_keys[i][1]);
    }
}
