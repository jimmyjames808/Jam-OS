/* wltest: a small Wayland client on libjwl (<jwl_client.h>), to try the
 * compositor by hand and to test it.
 *
 *     wltest [--spawn [size=WxH]] [--frames N] [--script]
 *     wltest --mouse [noaccel] [nomouse] | --hang-test | --crash-test | --selftest
 *
 * By hand it opens a window, draws a moving pattern into it on every
 * frame callback, and prints every event it gets (configures, keys with
 * the characters they type, the pointer, the connection going and coming
 * back); Escape, q, the window's close or N frames end it. `--script` runs
 * the scripted checks instead (script.c) and ends with "wltest: PASS" or
 * "wltest: FAIL: ...". `--spawn` starts a headless compositor of its own
 * (spawn.c) instead of opening /svc/wayland. The rest go through libfun
 * as an app does, and work without a compositor too (fun.c): the mouse,
 * logged and drawn, a program that hangs or crashes holding the screen,
 * and the self-test of libfun's pointer.
 *
 * Its list asks for the compositor (`svc wayland`, below); until init
 * publishes /svc/wayland, `--spawn` is the way to a compositor. */
#include <jwl_client.h>
#include <os.h>
#include <wants.h>
#include "wltest.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc wayland\n");

#define KEY_ESC        1u     /* evdev */
#define CONNECT_WAIT   (5 * NS_PER_S)
#define WIN_W          480
#define WIN_H          320

void wl_draw(const struct jwl_frame *fr, uint32_t t)
{
    for (int32_t y = 0; y < fr->height; y++) {
        uint32_t *row = fr->px + y * (fr->stride / 4);
        uint32_t g = (uint32_t)(y * 255 / (fr->height > 1 ? fr->height - 1 : 1));
        for (int32_t x = 0; x < fr->width; x++) {
            bool band = ((uint32_t)x + t * 4) / 32 % 2;
            row[x] = 0xff000000u | (band ? 0xd0u << 16 : 0x30u << 16) | g << 8 | 0x60u;
        }
    }
}

void wl_print_event(const struct jwl_event *ev)
{
    switch (ev->type) {
    case JWL_EV_CONFIGURE:
        printf("wltest: configure %dx%d (asked %dx%d) states %#x%s\n", ev->configure.width,
               ev->configure.height, ev->configure.asked_w, ev->configure.asked_h,
               ev->configure.states, ev->configure.rebuilt ? ", made again" : "");
        break;
    case JWL_EV_KEY:
        printf("wltest: key %u %s, character %#x, keysym %#x, mods %#x\n", ev->key.code,
               ev->key.state == JWL_KEY_PRESSED    ? "pressed"
               : ev->key.state == JWL_KEY_REPEATED ? "repeated"
                                                   : "released",
               ev->key.cp, ev->key.sym, ev->key.mods);
        break;
    case JWL_EV_POINTER_ENTER:
    case JWL_EV_POINTER_MOTION:
        printf("wltest: pointer %s at %d,%d\n", ev->type == JWL_EV_POINTER_ENTER ? "in" : "moved",
               ev->pointer.x / 256, ev->pointer.y / 256);
        break;
    case JWL_EV_POINTER_BUTTON:
        printf("wltest: button %#x %s\n", ev->button.button,
               ev->button.pressed ? "pressed" : "released");
        break;
    case JWL_EV_POINTER_AXIS:
        printf("wltest: wheel %d notches on axis %u\n", ev->axis.discrete, ev->axis.axis);
        break;
    case JWL_EV_DISCONNECTED:
    case JWL_EV_DEAD:
        printf("wltest: the connection %s (%s)\n",
               ev->type == JWL_EV_DEAD ? "is gone for good" : "went", status_str(ev->conn.why));
        break;
    case JWL_EV_RECONNECTED:
        printf("wltest: connected again (connection %u)\n", ev->conn.generation);
        break;
    case JWL_EV_FRAME:
        break;   /* one a frame: too many to print */
    default:
        printf("wltest: event %u\n", ev->type);
    }
}

/* Dispatch once, waiting at most until deadline (or the client's own). */
static void wait_once(struct jwl_client *c, uint64_t deadline)
{
    uint64_t d = jwl_client_deadline(c);
    if (deadline < d)
        d = deadline;
    handle_t ch = jwl_client_channel(c);
    signals_t seen;
    if (ch != HANDLE_INVALID)
        (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, d, &seen);
    else
        (void)jam_nanosleep(d);
    (void)jwl_client_dispatch(c);
}

bool wl_wait_flag(struct jwl_client *c, const bool *flag, uint64_t deadline)
{
    struct jwl_event ev;
    for (;;) {
        (void)jwl_client_dispatch(c);
        while (jwl_client_next_event(c, &ev) == OK)
            wl_print_event(&ev);
        if (*flag)
            return true;
        status_t st = jwl_client_status(c);
        if (now() >= deadline || (st != OK && st != ERR_SHOULD_WAIT))
            return false;
        wait_once(c, deadline);
    }
}

bool wl_wait_event(struct jwl_client *c, uint32_t type, uint64_t deadline, struct jwl_event *out)
{
    for (;;) {
        if (jwl_client_wait_event(c, deadline, out) != OK)
            return false;
        wl_print_event(out);
        if (out->type == type)
            return true;
    }
}

/* By hand: a window animated on frame callbacks until closed, Escape, or
 * frames (0: no end). */
static int by_hand(struct jwl_client *c, unsigned frames)
{
    struct jwl_window_config wc = { .width = WIN_W, .height = WIN_H, .title = "wltest",
                                    .app_id = "wltest", .resizable = true };
    struct jwl_window *w;
    status_t st = jwl_window_create(c, &wc, &w);
    if (st != OK) {
        printf("wltest: can't open a window: %s%s\n", status_str(st),
               st == ERR_NOT_SUPPORTED ? " (no xdg_wm_base: try --script)" : "");
        return 1;
    }
    uint32_t t = 0;
    uint64_t t0 = now();
    bool quit = false;
    while (!quit && (!frames || t < frames)) {
        struct jwl_event ev;
        st = jwl_client_wait_event(c, DEADLINE_NEVER, &ev);
        if (st != OK)
            break;
        wl_print_event(&ev);
        quit = ev.type == JWL_EV_CLOSE ||
               (ev.type == JWL_EV_KEY && ev.key.state &&
                (ev.key.code == KEY_ESC || ev.key.cp == 'q'));
        struct jwl_frame fr;
        bool draw = ev.win == w && (ev.type == JWL_EV_CONFIGURE || ev.type == JWL_EV_FRAME);
        if (draw && jwl_window_begin(w, &fr) == OK) {
            wl_draw(&fr, t++);
            (void)jwl_window_present(w, NULL, 0, true);   /* a lost connection: drawn again */
        }
    }
    uint64_t ms = (now() - t0) / NS_PER_MS;
    printf("wltest: %u frames in %llu ms\n", t, (unsigned long long)ms);
    jwl_window_destroy(w);
    return st == OK ? 0 : 1;
}

void wl_print_info(const struct jwl_client_info *in)
{
    printf("wltest: connected: wl_compositor %u, wl_shm %u (formats %#x), wl_output %u "
           "(%dx%d at %d mHz), wl_seat %u, xdg_wm_base %u\n",
           in->compositor_version, in->shm_version, in->shm_formats, in->output_version,
           in->output_width, in->output_height, in->output_refresh, in->seat_version,
           in->wm_base_version);
}

/* A decimal number of at most 6 digits; 0 for anything else. */
static unsigned number(const char *s)
{
    unsigned v = 0, n = 0;
    for (; *s >= '0' && *s <= '9' && n < 6; s++, n++)
        v = v * 10 + (unsigned)(*s - '0');
    return *s ? 0 : v;
}

/* "<w>x<h>", each 1 to JWL_SIZE_MAX. */
static bool parse_size(const char *s, int32_t *w, int32_t *h)
{
    const char *x = strchr(s, 'x');
    char first[8];
    if (!x || x == s || (size_t)(x - s) >= sizeof(first))
        return false;
    memcpy(first, s, (size_t)(x - s));
    first[x - s] = 0;
    unsigned a = number(first), b = number(x + 1);
    if (!a || !b || a > JWL_SIZE_MAX || b > JWL_SIZE_MAX)
        return false;
    *w = (int32_t)a;
    *h = (int32_t)b;
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--mouse"))
        return wl_fun_mouse(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--hang-test"))
        return wl_fun_hang();
    if (argc == 2 && !strcmp(argv[1], "--crash-test"))
        return wl_fun_crash();
    if (argc == 2 && !strcmp(argv[1], "--selftest"))
        return wl_fun_selftest();
    bool spawn_own = false, script = false;
    unsigned frames = 0;
    struct comp_child k = { .svc = HANDLE_INVALID, .proc = HANDLE_INVALID,
                            .job = HANDLE_INVALID, .w = 640, .h = 400 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spawn")) {
            spawn_own = true;
        } else if (!strcmp(argv[i], "--script")) {
            script = true;
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = number(argv[++i]);
        } else if (!strncmp(argv[i], "size=", 5) && parse_size(argv[i] + 5, &k.w, &k.h)) {
            continue;
        } else {
            printf("usage: wltest [--spawn [size=WxH]] [--frames N] [--script]\n"
                   "       wltest --mouse [noaccel] [nomouse] | --hang-test | --crash-test | "
                   "--selftest\n");
            return 2;
        }
    }
    struct jwl_client_config cfg = { .name = "wltest" };
    if (spawn_own) {
        cfg.connect = comp_connect;
        cfg.connect_ctx = &k;
    }
    int rc;
    if (script) {
        rc = wl_script(&cfg, spawn_own ? &k : NULL, frames ? frames : 3);
    } else {
        struct jwl_client *c;
        status_t st = jwl_client_connect(&cfg, now() + CONNECT_WAIT, &c);
        if (st != OK) {
            printf("wltest: can't reach the compositor: %s\n", status_str(st));
            rc = 1;
        } else {
            wl_print_info(jwl_client_info(c));
            rc = by_hand(c, frames);
            jwl_client_destroy(c);
        }
    }
    comp_stop(&k);
    return rc;
}
