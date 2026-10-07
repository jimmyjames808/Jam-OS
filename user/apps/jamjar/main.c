/* jamjar: the music player's window, on the screen borrowed from the console.
 *
 *   jamjar, run jamjar         the library and the player
 *   ... noplayer               the library only: the player is left alone
 *   ... root=/usb0/music       the folder to read (default /usb0/music if
 *                              there is one, else /data/music)
 *   ... hz=60                  frames a second while anything moves
 *   ... trace                  say every command, every cover read, and once a
 *                              second each channel's loudest bar (the QEMU
 *                              test reads it)
 *   ... noaccel                the mouse without acceleration (tests)
 *   ... nocovers               jar labels only: no covers read from the tags
 *   run jamjar --selftest      the library, names, search, art and layout
 *
 * It is a remote control and a view of the background player (bin/music,
 * abi/idl/music.idl), never a second player: the music goes on after q.
 * Its list (below) asks for the player, whose channel it opens for itself
 * (/svc/music hands each opener its own), and for the music read-only:
 * /data and the other sticks. Without the player (none runs, or it isn't
 * in its namespace) it shows the library only, and says so.
 * docs/history/MUSIC-GUI.md is the design.
 *
 * The loop: read a little more of the library while it is being read, take
 * the player's latest snapshot (link.c's thread does the calls), move the
 * bars on, draw, present, and wait for a key, a mouse report or the next
 * frame: every 1/hz s while anything moves, four times a second when
 * nothing does (the clock of the progress bar). */
#include <wants.h>
#include "jamjar.h"

JAM_WANTS("svc music\n"
          "mount /data r\n"
          "mount /usb* r\n"
          "svc wayland\n");

static struct app A;

static const char *default_root(void)
{
    bool dir = false;
    if (fs_stat("/usb0/music", NULL, &dir, NULL) == OK && dir)
        return "/usb0/music";
    return "/data/music";
}

/* The library a few entries further; the columns once it is built. */
static void read_library(struct app *a)
{
    struct library *l = &a->lib;
    if (l->ready)
        return;
    if (lib_step(l, 48) == ERR_SHOULD_WAIT || !l->ready)
        return;
    a->view_ok = l->ntracks && view_init(&a->view, l);
    for (uint32_t i = 0; i < l->nalbums; i++)   /* every cover read, behind those drawn */
        (void)cover_ready(l->album[i].hash, l->track[l->album[i].first].path, COVER_SMALL, true);
    if (a->view_ok)
        view_scroll(&a->view, a->lo.rows);
    a->now_track = a->snap.path[0] ? lib_find(l, a->snap.path) : -1;
    if (a->trace)
        say("jamjar: library %s: %u artists, %u albums, %u tracks\n", l->root, l->nartists,
            l->nalbums, l->ntracks);
}

/* The player's news: a new track, a command's outcome. */
static void hear(struct app *a, uint64_t t)
{
    link_get(&a->snap);
    if (a->drag_vol || t - a->vol_asked_at < NS_PER_S)
        a->snap.volume = a->vol_asked;   /* until the player's answer catches up */
    if (strcmp(a->snap.path, a->heard_path)) {
        memcpy(a->heard_path, a->snap.path, sizeof(a->heard_path));
        names_of_path(a->snap.path, &a->now);
        a->now_track = a->lib.ready && a->snap.path[0] ? lib_find(&a->lib, a->snap.path) : -1;
        if (a->trace && a->snap.path[0])
            say("jamjar: hearing %s\n", a->snap.path);
    }
    char msg[96];
    if (link_result(msg, sizeof(msg)))
        app_toast(a, "%s", msg);
    if (a->trace && a->snap.playing == 1 && t - a->trace_at > NS_PER_S) {
        int l = 0, r = 0;
        for (int i = 1; i < BARS; i++) {
            l = a->snap.left[i] > a->snap.left[l] ? i : l;
            r = a->snap.right[i] > a->snap.right[r] ? i : r;
        }
        say("jamjar: spectrum: loudest bars: left %d, right %d (%u, %u), level %u\n", l, r,
            a->snap.left[l], a->snap.right[r], a->snap.level);
        a->trace_at = t;
    }
}

/* Everything that moves by itself, on to time t; true if a frame soon is wanted. */
static bool animate(struct app *a, uint64_t t, float dt)
{
    static const uint8_t quiet[BARS];
    bool heard = a->snap.playing == 1 && !snap_stale(&a->snap, t);   /* else the bars fall */
    bars_step(&a->bars, heard ? a->snap.left : quiet, heard ? a->snap.right : quiet, heard, dt);
    float goal = a->full ? 1.0f : 0.0f;
    if (a->full_t != goal) {
        float step = dt / 0.6f;
        a->full_t += a->full_t < goal ? step : -step;
        a->full_t = a->full_t < 0 ? 0 : a->full_t > 1 ? 1 : a->full_t;
    }
    int64_t album = roulette_step(&a->roul, t);
    if (album >= 0)
        app_play_album(a, (uint32_t)album, NULL);
    return heard || bars_busy(&a->bars) || a->roul.on || a->full_t != goal || !a->lib.ready ||
           a->searching || (a->toast[0] && t - a->toast_at < TOAST_NS + NS_PER_S);
}

/* trace: where the mouse's targets are in the window (the layout's, in
 * its own pixels), for tools/shell-tests/jamjar.txt's clicks: the next
 * button's middle, the volume slider across at its middle, the first
 * track row's middle. Said again for every new layout. */
static void say_targets(const struct app *a)
{
    const struct layout *lo = &a->lo;
    const struct rect *n = &lo->btn[BTN_NEXT], *t = &lo->list[COL_TRACK];
    if (!a->trace)
        return;
    say("jamjar: targets: next %d,%d, volume %d-%d at y %d, first track %d,%d\n",
        n->x + n->w / 2, n->y + n->h / 2, lo->vol.x, lo->vol.x + lo->vol.w - 1,
        lo->vol.y + lo->vol.h / 2, t->x + t->w / 2, t->y + lo->row_h / 2);
}

/* Keys and mouse reports until the deadline (the first wait), then
 * whatever else is queued. */
static void input(struct app *a, uint64_t deadline)
{
    for (int k = gfx_key(deadline); k != KEY_NONE && !a->quit; k = gfx_key(0)) {
        if (k == KEY_MOUSE) {
            struct mouse m;
            gfx_mouse(&m);
            app_mouse(a, &m);
        } else if (k == KEY_RESIZE) {   /* its window took a new size (gfx_resizable) */
            layout_make(&a->lo, scr.w, scr.h, scr.ui);
            say_targets(a);
            if (a->view_ok)
                view_scroll(&a->view, a->lo.rows);
        } else {
            app_key(a, k);
        }
    }
}

static int run(int argc, char **argv)
{
    struct app *a = &A;
    a->trace = has_arg(argc, argv, "trace");
    a->now_track = -1;
    a->hover_col = a->hover_row = a->mx = a->my = -1;
    uint64_t hz = arg_num(argc, argv, "hz", 60);
    uint64_t period = NS_PER_S / (hz < 10 ? 10 : hz > 240 ? 240 : hz);
    const char *root = default_root();
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "root=", 5))
            root = argv[i] + 5;
    handle_t music = HANDLE_INVALID;
    if (!has_arg(argc, argv, "noplayer"))
        (void)svc_open(SVC_MUSIC, &music);   /* none: the library only */
    link_start(music);
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    if (!has_arg(argc, argv, "nocovers"))
        cover_start(a->trace);
    gfx_resizable();   /* the layout takes any size ... */
    gfx_min_size(JAMJAR_MIN_W, JAMJAR_MIN_H);   /* ... from where it fits */
    say("jamjar: its window is at least %dx%d\n", JAMJAR_MIN_W, JAMJAR_MIN_H);
    status_t st = gfx_open_on(C_BG);
    if (st != OK) {
        say("jamjar: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    (void)gfx_mouse_open(!has_arg(argc, argv, "noaccel"));   /* no mouse: keys still work */
    layout_make(&a->lo, scr.w, scr.h, scr.ui);
    bars_init(&a->bars);
    if ((st = lib_begin(&a->lib, root)) != OK) {
        a->lib.ready = true;
        a->lib.err = st;
    }
    if (!music)
        say("jamjar: no player (none runs, or noplayer): the library only\n");
    if (a->trace)   /* the cover test reads where now playing's cover goes */
        say("jamjar: %dx%d, reading %s; now playing's cover at %d,%d, %d px\n", scr.w, scr.h,
            a->lib.root, a->lo.art.x, a->lo.art.y, a->lo.art.w);
    if (a->trace) {   /* ... and jamjar-test.sh where the bars are */
        int x0, x1, y, x2, x3;
        bars_where(&a->lo.jam, 0, &x0, &x1, &y);
        bars_where(&a->lo.jam, BARS - 1, &x2, &x3, &y);
        say("jamjar: the bars: the line at y %d, bar 0 at x %d-%d, bar %d at x %d-%d\n", y, x0,
            x1, BARS - 1, x2, x3);
    }
    say_targets(a);
    uint64_t last = now(), frames = 0, t0 = last, draw_ns = 0;
    while (!a->quit) {
        uint64_t t = now();
        read_library(a);
        hear(a, t);
        bool busy = animate(a, t, (float)(t - last) / 1e9f);
        last = t;
        draw_frame(a, t);
        gfx_present();
        draw_ns += now() - t;
        frames++;
        input(a, t + (busy ? period : 250 * NS_PER_MS));
    }
    gfx_close();
    uint64_t ms = (now() - t0) / NS_PER_MS;
    say("jamjar: %lu frames in %lu.%lu s, %lu us each to draw and present; the music plays on "
        "(`music status`)\n", (unsigned long)frames, (unsigned long)(ms / 1000),
        (unsigned long)(ms % 1000 / 100), (unsigned long)(frames ? draw_ns / frames / 1000 : 0));
    return 0;
}

int main(int argc, char **argv)
{
    gfx_title("Jamjar");
    if (has_arg(argc, argv, "--selftest"))
        return jamjar_selftest();
    return run(argc, argv);
}
