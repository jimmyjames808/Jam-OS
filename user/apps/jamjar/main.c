/* jamjar: the music player's window, on the screen borrowed from the console.
 *
 *   jamjar                     (the shell command) the library and the player
 *   run jamjar                 the library only: no player channel (it says so)
 *   ... root=/usb0/music       the folder to read (default /usb0/music if
 *                              there is one, else /data/music)
 *   ... hz=60                  frames a second while anything moves
 *   ... trace                  say every command and, once a second, what
 *                              the bands show (the QEMU test reads it)
 *   ... noaccel                the mouse without acceleration (tests)
 *   run jamjar --selftest      the library, names, search, art and layout
 *
 * It is a remote control and a view of the background player (bin/music,
 * abi/idl/music.idl), never a second player: the music goes on after q.
 * The shell's `jamjar` command hands it the player's channel as SR_USER + 4
 * (the shell's own number for it). docs/history/MUSIC-GUI.md is the design.
 *
 * The loop: read a little more of the library while it is being read, take
 * the player's latest snapshot (link.c's thread does the calls), move the
 * jam on, draw, present, and wait for a key, a mouse report or the next
 * frame: every 1/hz s while anything moves, four times a second when
 * nothing does (the clock of the progress bar). */
#include "jamjar.h"

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
        int best = 0;
        for (int i = 1; i < 16; i++)
            best = a->snap.bands[i] > a->snap.bands[best] ? i : best;
        say("jamjar: levels: loudest band %d (%u), level %u\n", best, a->snap.bands[best],
            a->snap.level);
        a->trace_at = t;
    }
}

/* Everything that moves by itself, on to time t; true if a frame soon is wanted. */
static bool animate(struct app *a, uint64_t t, float dt)
{
    static const uint8_t quiet[16];
    bool heard = a->snap.playing == 1;
    simmer_step(&a->sim, heard ? a->snap.bands : quiet, heard ? a->snap.level : 0, dt);
    float goal = a->full ? 1.0f : 0.0f;
    if (a->full_t != goal) {
        float step = dt / 0.6f;
        a->full_t += a->full_t < goal ? step : -step;
        a->full_t = a->full_t < 0 ? 0 : a->full_t > 1 ? 1 : a->full_t;
    }
    int64_t album = roulette_step(&a->roul, t);
    if (album >= 0)
        app_play_album(a, (uint32_t)album, NULL);
    return heard || simmer_busy(&a->sim) || a->roul.on || a->full_t != goal || !a->lib.ready ||
           a->searching || (a->toast[0] && t - a->toast_at < TOAST_NS + NS_PER_S);
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
    link_start(startup_handle(SR_USER + 4));
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    status_t st = gfx_open_on(C_BG);
    if (st != OK) {
        say("jamjar: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    (void)gfx_mouse_open(!has_arg(argc, argv, "noaccel"));   /* no mouse: keys still work */
    layout_make(&a->lo, scr.w, scr.h, scr.ui);
    simmer_init(&a->sim, now());
    if ((st = lib_begin(&a->lib, root)) != OK) {
        a->lib.ready = true;
        a->lib.err = st;
    }
    if (!a->snap.link)
        say("jamjar: no player channel: start it with the shell's `jamjar` command to play\n");
    if (a->trace)
        say("jamjar: %dx%d, reading %s\n", scr.w, scr.h, a->lib.root);
    uint64_t last = now(), frames = 0;
    while (!a->quit) {
        uint64_t t = now();
        read_library(a);
        hear(a, t);
        bool busy = animate(a, t, (float)(t - last) / 1e9f);
        last = t;
        draw_frame(a, t);
        gfx_present();
        frames++;
        input(a, t + (busy ? period : 250 * NS_PER_MS));
    }
    gfx_close();
    say("jamjar: %lu frames; the music plays on (`music status`)\n", (unsigned long)frames);
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return jamjar_selftest();
    return run(argc, argv);
}
