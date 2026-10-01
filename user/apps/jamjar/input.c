/* jamjar: keys and the mouse, turned into what the app does: moving in
 * the lists, searching, and commands for the player (queued to link.c's
 * thread, so nothing here waits). The overlays take the input first: the
 * roulette (any key or click stops it), the help (any key closes it),
 * then the search box while it is being typed in. With `trace` every
 * command is also said on the console (the QEMU test reads it there). */
#include "jamjar.h"

#define DOUBLE_CLICK (400 * NS_PER_MS)
#define VOL_STEP     20      /* centibels a key or a wheel notch */
static const uint32_t sleep_steps[] = { 0, 15, 30, 60, 90 };   /* minutes */

void app_toast(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->toast, sizeof(a->toast), fmt, ap);
    va_end(ap);
    a->toast_at = now();
    if (a->trace)
        say("jamjar: %s\n", a->toast);
}

static void send(struct app *a, const struct cmd *c)
{
    if (!a->snap.link) {
        app_toast(a, "no player: start jamjar with the shell's jamjar command");
        return;
    }
    if (!link_cmd(c))
        app_toast(a, "busy: try again");
}

static void simple(struct app *a, enum cmd_kind k, int32_t arg, const char *what)
{
    struct cmd c = { .kind = k, .arg = arg };
    send(a, &c);
    if (what)
        app_toast(a, "%s", what);
}

static void play_folder(struct app *a, const char *folder, const char *first, const char *name)
{
    struct cmd c = { .kind = CMD_PLAY, .ordered = a->ordered };
    snprintf(c.folder, sizeof(c.folder), "%s", folder);
    snprintf(c.first, sizeof(c.first), "%s", first ? first : "");
    send(a, &c);
    app_toast(a, "play %s (%s)", name, a->ordered ? "in order" : "shuffle");
}

void app_play_album(struct app *a, uint32_t album, const char *first)
{
    const struct lib_album *b = &a->lib.album[album];
    play_folder(a, b->dir, first, b->name);
}

void app_play_row(struct app *a, int c, int row)
{
    const struct view *v = &a->view;
    if (!a->view_ok || row < 0 || (uint32_t)row >= v->nrows[c])
        return;
    uint32_t x = v->row[c][row];
    const struct library *l = &a->lib;
    if (c == COL_ARTIST && x == ROW_ALL)
        play_folder(a, l->root, NULL, "everything");
    else if (c == COL_ARTIST)
        play_folder(a, l->artist[x].dir, NULL, l->artist[x].name);
    else if (c == COL_ALBUM)
        app_play_album(a, x, NULL);
    else
        app_play_album(a, l->track[x].album, l->track[x].path);
}

/* ---- keys ------------------------------------------------------------------------- */

static void move(struct app *a, int c, int delta)
{
    if (!a->view_ok)
        return;
    view_select(&a->view, &a->lib, c, a->view.sel[c] + delta);
    view_scroll(&a->view, a->lo.rows);
}

/* Presses in quick succession count from what the app last asked for,
 * not from the snapshot, which may not have caught up yet. */
#define RECENT (2 * NS_PER_S)

static void volume_by(struct app *a, int32_t d)
{
    uint64_t t = now();
    int32_t from = t - a->vol_asked_at < RECENT ? a->vol_asked : a->snap.volume;
    int32_t cb = from + d;
    cb = cb > 0 ? 0 : cb < -960 ? -960 : cb;
    a->vol_asked = cb;
    a->vol_asked_at = t;
    simple(a, CMD_VOLUME, cb, NULL);
    char db[24];
    vol_text(cb, db, sizeof(db));
    app_toast(a, "volume %s", db);
}

static void sleep_next(struct app *a)
{
    uint64_t t = now();
    uint32_t m = t - a->sleep_asked_at < RECENT ? a->sleep_asked
                 : a->snap.sleep_s ? (a->snap.sleep_s + 59) / 60 : 0;
    uint32_t next = 0;
    for (unsigned i = 0; i < sizeof(sleep_steps) / sizeof(sleep_steps[0]); i++)
        if (sleep_steps[i] > m) {
            next = sleep_steps[i];
            break;
        }
    simple(a, CMD_SLEEP, (int32_t)(next * 60), NULL);
    a->sleep_asked = next;
    a->sleep_asked_at = t;
    if (next)
        app_toast(a, "sleep in %u minutes", next);
    else
        app_toast(a, "sleep timer off");
}

static void play_pause(struct app *a)
{
    if (a->snap.playing == 1)
        simple(a, CMD_PAUSE, 1, "pause");
    else if (a->snap.playing == 3)
        simple(a, CMD_PAUSE, 0, "play on");
    else
        app_play_row(a, a->view.col, a->view.sel[a->view.col]);
}

/* The library's album playing, or -1. */
static int64_t playing_album(const struct app *a)
{
    return a->now_track >= 0 ? (int64_t)a->lib.track[a->now_track].album : -1;
}

static void locate(struct app *a)
{
    if (a->now_track < 0 || !view_locate(&a->view, &a->lib, (uint32_t)a->now_track)) {
        app_toast(a, "the track playing is not in this list");
        return;
    }
    a->view.col = COL_TRACK;
    view_scroll(&a->view, a->lo.rows);
}

static void search_key(struct app *a, int k)
{
    char q[QUERY_MAX];
    snprintf(q, sizeof(q), "%s", a->view.query);
    size_t n = strlen(q);
    if (k == KEY_QUIT) {
        if (!n)
            a->searching = false;
        q[0] = '\0';
    } else if (k == KEY_BACKSPACE) {
        while (n && (q[n - 1] & 0xc0) == 0x80)   /* a whole UTF-8 character */
            n--;
        q[n ? n - 1 : 0] = '\0';
    } else if (k == KEY_ENTER) {
        a->searching = false;
        app_play_row(a, a->view.col, a->view.sel[a->view.col]);
        return;
    } else if (k >= 32 && k < 127 && n + 1 < sizeof(q)) {
        q[n] = (char)k;
        q[n + 1] = '\0';
    } else {
        return;
    }
    if (a->view_ok)
        view_query(&a->view, &a->lib, q);
    view_scroll(&a->view, a->lo.rows);
}

/* Keys that move in the lists; false if k is not one. */
static bool nav_key(struct app *a, int k)
{
    struct view *v = &a->view;
    int page = a->lo.rows > 1 ? a->lo.rows - 1 : 1;
    switch (k) {
    case KEY_UP: move(a, v->col, -1); return true;
    case KEY_DOWN: move(a, v->col, 1); return true;
    case KEY_PGUP: move(a, v->col, -page); return true;
    case KEY_PGDN: move(a, v->col, page); return true;
    case KEY_HOME: move(a, v->col, -1000000); return true;
    case KEY_END: move(a, v->col, 1000000); return true;
    case KEY_LEFT: v->col = v->col > 0 ? v->col - 1 : 0; return true;
    case KEY_RIGHT: v->col = v->col < NCOLS - 1 ? v->col + 1 : NCOLS - 1; return true;
    case KEY_TAB: v->col = (v->col + 1) % NCOLS; return true;
    }
    return false;
}

/* The one-letter commands; false if k is not one. */
static bool command_key(struct app *a, int k)
{
    switch (k) {
    case ' ': play_pause(a); return true;
    case KEY_ENTER: app_play_row(a, a->view.col, a->view.sel[a->view.col]); return true;
    case 'n': case '.': case '>': simple(a, CMD_NEXT, 0, "next"); return true;
    case 'p': case ',': case '<': simple(a, CMD_PREV, 0, "back"); return true;
    case 'x': simple(a, CMD_STOP, 0, "stop"); return true;
    case '+': case '=': volume_by(a, VOL_STEP); return true;
    case '-': case '_': volume_by(a, -VOL_STEP); return true;
    case '/': a->searching = true; return true;
    case 'a': play_folder(a, a->lib.root, NULL, "everything"); return true;
    case 's':
        a->ordered = !a->ordered;
        app_toast(a, "next play: %s", a->ordered ? "in order" : "shuffle");
        return true;
    case 'r':
        if (!roulette_start(&a->roul, &a->lib, now(), playing_album(a), now()))
            app_toast(a, "no albums to spin");
        return true;
    case 'z': sleep_next(a); return true;
    case 'f': a->full = !a->full; return true;
    case 'l': locate(a); return true;
    case '?': case 'h': a->help = true; return true;
    }
    return false;
}

void app_key(struct app *a, int k)
{
    if (a->roul.on) {   /* any key stops the spin */
        a->roul.on = false;
        return;
    }
    if (a->help) {
        a->help = false;
        return;
    }
    if (nav_key(a, k)) {
        view_scroll(&a->view, a->lo.rows);
        return;
    }
    if (a->searching) {
        search_key(a, k);
        return;
    }
    if (k == KEY_QUIT || k == 'q' || k == 'Q') {
        if (a->full)
            a->full = false;
        else
            a->quit = true;
        return;
    }
    (void)command_key(a, k);
}

/* ---- the mouse ---------------------------------------------------------------------- */

/* The list row at (x, y): its column and row, false if none. */
static bool row_at(const struct app *a, int x, int y, int *col, int *row)
{
    const struct layout *lo = &a->lo;
    for (int c = 0; a->view_ok && c < NCOLS; c++) {
        if (!rect_has(&lo->list[c], x, y))
            continue;
        int r = a->view.top[c] + (y - lo->list[c].y) / lo->row_h;
        if ((y - lo->list[c].y) / lo->row_h >= lo->rows || r >= (int)a->view.nrows[c])
            return false;
        *col = c;
        *row = r;
        return true;
    }
    return false;
}

static void wheel(struct app *a, const struct mouse *m)
{
    const struct layout *lo = &a->lo;
    if (rect_has(&(struct rect){ lo->vol.x - 30 * lo->u, lo->vol.y - 10 * lo->u,
                                 lo->vol.w + 120 * lo->u, lo->vol.h + 20 * lo->u }, m->x, m->y)) {
        volume_by(a, m->wheel * VOL_STEP);
        return;
    }
    for (int c = 0; a->view_ok && c < NCOLS; c++)
        if (rect_has(&lo->list[c], m->x, m->y)) {
            int max = (int)a->view.nrows[c] - lo->rows, top = a->view.top[c] - 3 * m->wheel;
            a->view.top[c] = top > max ? (max > 0 ? max : 0) : top < 0 ? 0 : top;
        }
}

static void click_row(struct app *a, int c, int r)
{
    uint64_t t = now();
    bool twice = t - a->click_at < DOUBLE_CLICK && c == a->click_col && r == a->click_row;
    a->click_at = t;
    a->click_col = c;
    a->click_row = r;
    a->view.col = c;
    view_select(&a->view, &a->lib, c, r);
    if (twice)
        app_play_row(a, c, r);
}

/* A left press at (x, y) outside the overlays. */
static void press(struct app *a, int x, int y)
{
    const struct layout *lo = &a->lo;
    int c, r;
    if (a->full)
        return;   /* the big view has nothing to click */
    if (row_at(a, x, y, &c, &r)) {
        click_row(a, c, r);
        return;
    }
    static const enum cmd_kind kinds[NBTNS] = { CMD_PREV, CMD_PAUSE, CMD_NEXT, CMD_STOP };
    for (int b = 0; b < NBTNS; b++)
        if (rect_has(&lo->btn[b], x, y)) {
            if (b == BTN_PLAY)
                play_pause(a);
            else
                simple(a, kinds[b], 0, b == BTN_PREV ? "back" : b == BTN_NEXT ? "next" : "stop");
            return;
        }
    struct rect vol = { lo->vol.x - 10 * lo->u, lo->vol.y - 10 * lo->u, lo->vol.w + 20 * lo->u,
                        lo->vol.h + 20 * lo->u };
    if (rect_has(&vol, x, y)) {
        a->drag_vol = true;
        a->vol_shown = vol_at(lo, x);
        simple(a, CMD_VOLUME, a->vol_shown, NULL);
    } else if (rect_has(&lo->mode, x, y)) {
        (void)command_key(a, 's');
    } else if (rect_has(&lo->search, x, y)) {
        a->searching = true;
    } else if (rect_has(&lo->art, x, y)) {
        (void)command_key(a, 'r');
    } else if (rect_has(&lo->progress, x, y + 8 * lo->u) && rect_has(&lo->now, x, y)) {
        app_toast(a, "the player can't seek");
    }
}

void app_mouse(struct app *a, const struct mouse *m)
{
    a->mx = m->x;
    a->my = m->y;
    if (!row_at(a, m->x, m->y, &a->hover_col, &a->hover_row))
        a->hover_col = a->hover_row = -1;
    if (m->pressed & MOUSE_LEFT) {
        if (a->trace)   /* before what it does, which may say more */
            say("jamjar: click %d,%d\n", m->x, m->y);
        if (a->roul.on)
            a->roul.on = false;
        else if (a->help)
            a->help = false;
        else
            press(a, m->x, m->y);
    }
    if (a->drag_vol && (m->moved || (m->released & MOUSE_LEFT))) {
        a->vol_shown = vol_at(&a->lo, m->x);
        simple(a, CMD_VOLUME, a->vol_shown, NULL);
        a->vol_asked = a->vol_shown;
        a->vol_asked_at = now();
    }
    if (m->released & MOUSE_LEFT)
        a->drag_vol = false;
    if (m->wheel)
        wheel(a, m);
}
