/* jamjar: the self-test's redraw part (selftest.c runs it): a frame drawn
 * the way the app draws it (draw.c: the last picture kept, only the parts
 * that move drawn again over it) must equal one drawn whole, pixel for
 * pixel, whatever happened since the last frame, and everything that
 * differs from the last frame must lie in the rectangles the frame says it
 * changed (the ones gfx_present_rects is given). Each step draws a frame,
 * then draws it again whole into another buffer to compare. `want` says
 * which way the frame must have been drawn (1: only the moving parts, 0:
 * whole, -1: either): a change of anything the rest of the picture is
 * drawn from must make a whole frame, so every input of draw.c's key has a
 * step that changes it alone. */
#include "jamjar.h"

static uint32_t *frame_px, *prev_px, *ref_px;
static const char *rd_bad;      /* the first step that failed, and why */
static const char *rd_why;
static unsigned rd_steps, rd_partial;

static void rd_step(struct app *a, const char *what, int want, bool fresh)
{
    int w = scr.w, h = scr.h;
    size_t n = (size_t)w * (size_t)h;
    if (fresh)
        a->snap.at = now();   /* the player has just answered: not stale */
    uint64_t t = now(), phase = t / (500 * NS_PER_MS);
    memcpy(prev_px, frame_px, n * 4);
    draw_frame(a, t);
    bool dmg_all = a->dmg_all;
    unsigned ndmg = a->ndmg;
    struct rect dmg[DYN_MAX];
    memcpy(dmg, a->dmg, sizeof(dmg));
    /* the same frame whole, in the other buffer */
    struct redraw keep = a->rd;
    struct surf cur = scr.s;
    a->rd = (struct redraw){ .off = true };
    scr.s = (struct surf){ ref_px, w, h, w };
    draw_frame(a, t);
    scr.s = cur;
    a->rd = keep;
    rd_steps++;
    rd_partial += !dmg_all;
    if (now() / (500 * NS_PER_MS) != phase)
        return;   /* a caret blinked between the two draws: not comparable */
    const char *why = NULL;
    if (want >= 0 && (want == 1) == dmg_all)
        why = want ? "drawn whole, not only what moved" : "only what moved, not whole";
    size_t bad = 0, outside = 0;
    for (size_t i = 0; i < n; i++) {
        if (frame_px[i] != ref_px[i])
            bad++;
        if (!dmg_all && frame_px[i] != prev_px[i]) {
            int x = (int)(i % (size_t)w), y = (int)(i / (size_t)w);
            bool in = false;
            for (unsigned k = 0; k < ndmg; k++)
                in |= rect_has(&dmg[k], x, y);
            outside += !in;
        }
    }
    if (bad && !why)
        why = "differs from the whole redraw";
    if (outside && !why)
        why = "changed outside the rectangles it said";
    if (why && !rd_bad) {
        rd_bad = what;
        rd_why = why;
        say("jamjar: selftest: redraw: step '%s': %s (%lu pixels differ, %lu outside)\n", what,
            why, (unsigned long)bad, (unsigned long)outside);
    }
}

/* A step that must be drawn whole. */
static void rd_flip(struct app *a, const char *what)
{
    rd_step(a, what, 0, true);
}

/* The bars and the clock move on by n frames of 1/60 s, each a step. */
static void rd_moving(struct app *a, const char *what, int n, uint8_t level, int want)
{
    uint8_t bands[BARS];
    for (int f = 0; f < n; f++) {
        for (int i = 0; i < BARS; i++)
            bands[i] = (uint8_t)(level ? level - (uint8_t)((i * 3 + f * 7) % 90) : 0);
        bars_step(&a->bars, bands, bands, level != 0, 1.0f / 60);
        a->snap.elapsed_ms += 17;
        rd_step(a, what, want, true);
    }
}

/* ---- the sequence, a part at a time ----------------------------------------------------- */

/* The bars loud and falling to silence, the clock at its odd values. */
static void rd_moving_parts(struct app *a)
{
    rd_step(a, "the first frame", 0, true);
    rd_moving(a, "bars loud", 24, 250, 1);
    rd_moving(a, "bars falling to silence", 90, 0, 1);
    rd_moving(a, "bars quiet again", 6, 0, 1);
    rd_moving(a, "bars up again", 10, 120, 1);
    static const struct { uint64_t el, len; } clock[] = {
        { 0, 245000 }, { 999, 245000 }, { 59999, 245000 }, { 600000, 245000 },
        { 3599000, 3600000 }, { 5999000, 6000000 }, { 100000, 0 }, { 7000, 8000 },
        { 244000, 245000 },
    };
    for (unsigned i = 0; i < sizeof(clock) / sizeof(clock[0]); i++) {
        a->snap.elapsed_ms = clock[i].el;
        a->snap.length_ms = clock[i].len;
        rd_step(a, "the clock", 1, true);
    }
    a->snap.elapsed_ms = 83000;
    a->snap.length_ms = 245000;
    rd_step(a, "the clock back", 1, true);
}

/* The pointer: onto a button (whole), a pixel on (the same button: no change). */
static void rd_pointer(struct app *a)
{
    const struct rect *nb = &a->lo.btn[BTN_NEXT];
    a->mx = nb->x + nb->w / 2;
    a->my = nb->y + nb->h / 2;
    rd_flip(a, "the pointer on the next button");
    a->mx++;
    rd_step(a, "the pointer a pixel on", 1, true);
    a->mx = a->lo.vol.x + 3;
    a->my = a->lo.vol.y + a->lo.vol.h / 2;
    rd_flip(a, "the pointer on the volume");
    a->mx = a->lo.mode.x + 3;
    a->my = a->lo.mode.y + 3;
    rd_flip(a, "the pointer on the play order");
    a->mx = a->my = 1;
    rd_flip(a, "the pointer off them");
    a->hover_col = COL_TRACK;
    a->hover_row = 1;
    rd_flip(a, "a row under the pointer");
    rd_moving(a, "bars after that", 3, 150, 1);
    a->hover_row = 2;
    rd_flip(a, "the next row under it");
    a->hover_col = COL_ALBUM;
    rd_flip(a, "the same row of another column");
    a->hover_col = a->hover_row = -1;
    rd_flip(a, "the pointer off the rows");
}

/* The lists, the search box and the toast. */
static void rd_lists(struct app *a)
{
    view_select(&a->view, &a->lib, COL_ARTIST, 5);
    view_scroll(&a->view, a->lo.rows);
    rd_flip(a, "an artist selected");
    a->view.col = COL_TRACK;
    rd_flip(a, "the column moved");
    view_select(&a->view, &a->lib, COL_TRACK, (int)a->view.nrows[COL_TRACK] - 1);
    view_scroll(&a->view, 1);
    rd_flip(a, "a track selected and scrolled to");
    view_scroll(&a->view, a->lo.rows);
    rd_flip(a, "scrolled back");
    view_query(&a->view, &a->lib, "jay");
    rd_flip(a, "a search typed");
    rd_moving(a, "bars with a search", 2, 200, 1);
    view_query(&a->view, &a->lib, "");
    rd_flip(a, "the search cleared");
    a->searching = true;   /* the caret may blink: then the step is not compared */
    rd_flip(a, "typing into the search box");
    a->searching = false;
    rd_flip(a, "typing done");
    app_toast(a, "play %s (%s)", "4-44", "shuffle");
    rd_flip(a, "a toast");
    rd_moving(a, "bars under a toast", 2, 200, 1);
    app_toast(a, "another thing");
    rd_flip(a, "... and its text changed");
    a->toast_at = 0;
    rd_flip(a, "the toast gone");
    rd_moving(a, "bars after it", 2, 200, 1);
}

/* Each input of the lists on its own. */
static void rd_list_inputs(struct app *a)
{
    struct view *v = &a->view;
    snprintf(v->query, sizeof(v->query), "q");
    rd_flip(a, "the query's text alone");
    snprintf(v->query, sizeof(v->query), "%s", "");
    rd_flip(a, "... and gone");
    v->sel[COL_TRACK]++;
    rd_flip(a, "a selected row alone");
    v->sel[COL_TRACK]--;
    rd_flip(a, "... and back");
    v->col = (v->col + 1) % NCOLS;
    rd_flip(a, "the column in use alone");
    v->col = (v->col + NCOLS - 1) % NCOLS;
    rd_flip(a, "... and back");
    v->nrows[COL_ALBUM]--;
    rd_flip(a, "a column's count alone");
    v->nrows[COL_ALBUM]++;
    rd_flip(a, "... and back");
    if (v->nrows[COL_ALBUM] > 1) {
        uint32_t keep = v->row[COL_ALBUM][v->top[COL_ALBUM]];
        v->row[COL_ALBUM][v->top[COL_ALBUM]] = v->row[COL_ALBUM][v->top[COL_ALBUM] + 1];
        rd_flip(a, "a row's item alone");
        v->row[COL_ALBUM][v->top[COL_ALBUM]] = keep;
        rd_flip(a, "... and back");
    }
    if (v->nrows[COL_TRACK] > 1) {
        int top = v->top[COL_TRACK];
        v->top[COL_TRACK] = top ? 0 : 1;
        rd_flip(a, "a column's first row alone");
        v->top[COL_TRACK] = top;
        rd_flip(a, "... and back");
    }
}

/* The library's counts and state on the bar, each on its own. */
static void rd_library(struct app *a)
{
    a->lib.nartists++;
    rd_flip(a, "an artist more");
    a->lib.nartists--;
    rd_flip(a, "... and back");
    a->lib.nalbums++;
    rd_flip(a, "an album more");
    a->lib.nalbums--;
    rd_flip(a, "... and back");
    a->lib.ntracks++;
    rd_flip(a, "a track more");
    a->lib.ntracks--;
    rd_flip(a, "... and back");
    a->lib.err = ERR_NOT_FOUND;
    a->view_ok = false;
    rd_flip(a, "the library unreadable");
    a->lib.err = ERR_NO_MEMORY;
    rd_flip(a, "... another way");
    a->lib.err = OK;
    a->view_ok = true;
    rd_flip(a, "... readable again");
    a->lib.ready = false;   /* still being read: the count of files on the bar */
    a->lib.nfiles = 10;
    rd_flip(a, "reading the library");
    a->lib.nfiles = 11;
    rd_flip(a, "one more file");
    a->lib.ready = true;
    rd_flip(a, "the library read");
}

/* What the player says: volume, state, timer, folder, note. */
static void rd_player(struct app *a)
{
    a->snap.volume = -300;
    rd_flip(a, "the volume");
    a->drag_vol = true;
    a->vol_shown = -480;
    rd_flip(a, "the volume dragged");
    a->vol_shown = -470;
    rd_flip(a, "dragged on");
    a->drag_vol = false;
    a->ordered = true;
    rd_flip(a, "in order");
    a->snap.playing = 3;
    rd_flip(a, "paused");
    a->snap.playing = 1;
    a->snap.sleep_s = 900;
    rd_flip(a, "a sleep timer");
    a->snap.sleep_s = 899;
    rd_flip(a, "a second less");
    a->snap.sleep_s = 0;
    rd_flip(a, "the sleep timer off");
    snprintf(a->snap.folder, sizeof(a->snap.folder), "%s", "/m/OnTheSpot/JAŸ-Z/2017_4-44");
    rd_flip(a, "the folder playing");
    a->snap.tracks = 2;
    rd_flip(a, "its tracks counted");
    a->snap.tracks = 3;
    rd_flip(a, "... and one more");
    a->now_track++;
    rd_flip(a, "another track marked as playing");
    a->now_track--;
    rd_flip(a, "... and back");
    snprintf(a->snap.note, sizeof(a->snap.note), "stopped by itself");
    rd_flip(a, "a note");
}

/* Track changes and the names, each part of them on its own; the player
 * going quiet and away. */
static void rd_names(struct app *a)
{
    snprintf(a->snap.path, sizeof(a->snap.path), "%s", test_fixture(5));
    snprintf(a->heard_path, sizeof(a->heard_path), "%s", test_fixture(5));
    names_of_path(a->snap.path, &a->now);
    a->now_track = lib_find(&a->lib, a->snap.path);
    a->snap.serial++;
    rd_flip(a, "a new track");
    rd_moving(a, "bars on the new track", 4, 220, 1);
    a->snap.path[0] = 0;
    a->heard_path[0] = 0;
    names_of_path("", &a->now);
    a->now_track = -1;
    rd_flip(a, "nothing playing");
    rd_moving(a, "bars with nothing playing", 2, 0, 1);
    snprintf(a->snap.path, sizeof(a->snap.path), "%s", test_fixture(1));
    names_of_path(a->snap.path, &a->now);
    a->now_track = lib_find(&a->lib, a->snap.path);
    rd_flip(a, "playing again");
    a->now.title[0]++;
    rd_flip(a, "a title alone");
    a->now.title[0]--;
    rd_flip(a, "... and back");
    a->now.artist[0]++;
    rd_flip(a, "an artist's name alone");
    a->now.artist[0]--;
    rd_flip(a, "... and back");
    a->now.album[0]++;
    rd_flip(a, "an album's name alone");
    a->now.album[0]--;
    rd_flip(a, "... and back");
    a->now.year[0] = a->now.year[0] ? 0 : '1';
    rd_flip(a, "a year alone");
    names_of_path(a->snap.path, &a->now);
    rd_flip(a, "... and back");
}

/* The path alone, the player gone quiet and away, the covers arriving. */
static void rd_link(struct app *a)
{
    a->snap.path[strlen(a->snap.path) - 1]++;   /* the same names, another file */
    rd_flip(a, "the path alone");
    a->snap.path[strlen(a->snap.path) - 1]--;
    rd_flip(a, "... and back");
    a->snap.at = 0;   /* the player stops answering */
    rd_step(a, "the player gone quiet", 0, false);
    rd_step(a, "... and still", 1, false);
    a->snap.link = false;
    rd_flip(a, "no player");
    a->snap.link = true;
    rd_flip(a, "a player again");
    uint32_t g0 = cover_gen();
    unsigned jobs = 0;
    while (jobs < 200 && cover_test_work())
        jobs++;
    rd_step(a, "covers read", cover_gen() != g0 ? 0 : -1, true);
    rd_moving(a, "bars after the covers", 2, 200, 1);
}

/* The overlays are always whole, and the frame after them is too. */
static void rd_overlays(struct app *a)
{
    a->help = true;
    rd_moving(a, "the help", 2, 200, 0);
    a->help = false;
    rd_flip(a, "the help closed");
    rd_moving(a, "bars after the help", 3, 200, 1);
    (void)roulette_start(&a->roul, &a->lib, 7, -1, now());
    for (int i = 0; i < 4; i++) {
        (void)roulette_step(&a->roul, now());
        rd_flip(a, "the roulette");
    }
    a->roul.on = false;
    rd_flip(a, "the roulette over");
    rd_moving(a, "bars after the roulette", 3, 200, 1);
}

/* The big view coming, settled (the sunburst alone moves), going. */
static void rd_big_view(struct app *a)
{
    a->full = true;
    for (int i = 1; i < 6; i++) {
        a->full_t = (float)i / 6;
        rd_flip(a, "the big view coming");
    }
    a->full_t = 1.0f;
    rd_flip(a, "the big view");
    rd_moving(a, "the sunburst turning", 12, 230, 1);
    rd_moving(a, "the sunburst quiet", 20, 0, 1);
    rd_moving(a, "the sunburst again", 4, 90, 1);
    snprintf(a->snap.path, sizeof(a->snap.path), "%s", test_fixture(6));
    names_of_path(a->snap.path, &a->now);
    rd_flip(a, "a new track in the big view");
    rd_moving(a, "the sunburst on it", 3, 200, 1);
    a->help = true;
    rd_flip(a, "help over the big view");
    a->help = false;
    rd_flip(a, "... closed");
    rd_moving(a, "the sunburst after it", 3, 200, 1);
    a->full = false;
    for (int i = 5; i >= 1; i--) {
        a->full_t = (float)i / 6;
        rd_flip(a, "the big view going");
    }
    a->full_t = 0.0f;
    rd_flip(a, "back to the panels");
    rd_moving(a, "bars back", 6, 200, 1);
}

/* A new size, the buffer blank: told (draw_forget) and not told (the
 * layout alone gives it away). */
static void rd_resize(struct app *a, int w, int h)
{
    for (int told = 1; told >= 0; told--) {
        int w2 = told ? w * 3 / 4 : w / 2 + 100, h2 = told ? h * 3 / 4 : h / 2 + 60;
        scr.s = (struct surf){ frame_px, w2, h2, w2 };
        scr.w = w2;
        scr.h = h2;
        scr.ui = told && h2 > 1100 ? 2 : 1;
        memset(frame_px, 0, (size_t)w2 * (size_t)h2 * 4);
        layout_make(&a->lo, w2, h2, scr.ui);
        if (told)
            draw_forget(a);
        view_scroll(&a->view, a->lo.rows);
        rd_flip(a, told ? "a new size" : "a new size, not told");
        rd_moving(a, "bars at the new size", 4, 200, 1);
    }
}

/* One screen's sequence. */
static void redraw_screen(int w, int h)
{
    if (!prev_px)
        prev_px = big_alloc(2560ull * 1440 * 4);
    if (!ref_px)
        ref_px = big_alloc(2560ull * 1440 * 4);
    struct app *a = test_frame_app(w, h);
    if (!a || !prev_px || !ref_px) {
        fun_check(false, "redraw: no memory");
        return;
    }
    frame_px = scr.s.px;
    rd_bad = rd_why = NULL;
    rd_steps = rd_partial = 0;
    a->snap.at = now();
    rd_moving_parts(a);
    rd_pointer(a);
    rd_lists(a);
    rd_list_inputs(a);
    rd_library(a);
    rd_player(a);
    rd_names(a);
    rd_link(a);
    rd_overlays(a);
    rd_big_view(a);
    rd_resize(a, w, h);
    char what[112];
    snprintf(what, sizeof(what), "redraw %dx%d: %u frames, %u only what moved, each as a whole one",
             w, h, rd_steps, rd_partial);
    fun_check(!rd_bad && rd_partial * 2 > rd_steps, what);
    if (rd_bad) {
        snprintf(what, sizeof(what), "  ... failed at '%s': %s", rd_bad, rd_why);
        fun_check(false, what);
    }
}

void test_redraw(void)
{
    redraw_screen(1024, 600);
    redraw_screen(1280, 800);
    redraw_screen(2560, 1440);
}
