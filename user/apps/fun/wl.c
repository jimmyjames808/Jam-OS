/* libfun: a window on the compositor (fun.h), what gfx_open opens when the
 * program's namespace has /svc/wayland and the compositor offers windows
 * (xdg_wm_base); presenting into it is wlpaint.c's.
 *
 * The window is libjwl's (<jwl_client.h>): an xdg_toplevel with two
 * buffers in a pool of kept pages, its configure/ack loop and frame
 * callbacks, and reconnecting when the compositor restarts (every object
 * made again and the last buffer shown, then wlpaint.c writes all of it
 * once more from the back buffer). One thread: the app's.
 *
 * Its size. gfx_open takes the size of the first configure (the
 * compositor's for a maximised or full-screen window, else the one asked
 * for), so scr.w and scr.h are what the app draws at. After that:
 *   - an app that took gfx_resizable gets every new size: wl_next swaps
 *     in a back buffer of the new size (the app hears KEY_RESIZE and draws
 *     again);
 *   - any other app keeps its size, and the window shows its picture
 *     centred, on scr.bg round it when the window is bigger (full screen
 *     by the compositor's key), its middle when smaller (wl_place).
 *
 * The compositor's events. What is libfun's own (a frame callback, a
 * configure, a reconnect) is handled as soon as it is read, wherever that
 * is: in gfx_key, or in a present waiting for its frame callback. What is
 * the app's (keys, the pointer, the close box, the end) waits in the
 * stash, in order, until gfx_key takes it; so a present never loses a
 * key. Keys arrive as evdev codes with what they type (libjwl decodes
 * them with the keymap): they become the console's struct
 * input_key_event (the HID usage, the character, INPUT_MOD_*), so
 * key_decode and the apps see the same keys either way. The close box is
 * an Escape press: KEY_QUIT. */
#include <jwl_client.h>
#include <keymap.h>
#include "internal.h"

#define CONNECT_WAIT   (5 * NS_PER_S)   /* the compositor's first answers, at most */
#define CONFIGURE_WAIT (5 * NS_PER_S)   /* the window's first configure, at most */
#define FRAME_WAIT     (1200 * NS_PER_MS) /* a frame callback (a hidden window: once a second) */
#define STASH_MAX      256u             /* the app's events read meanwhile; more are dropped */
#define MIN_W          320              /* scr's smallest size, as on a borrowed screen */
#define MIN_H          200
#define BIG_W          1600             /* the default window on a big screen ... */
#define BIG_H          1000
#define BIG_SCREEN_W   1920             /* ... which is one bigger than this */
#define BIG_SCREEN_H   1200
#define DEFAULT_OUT_W  1280             /* a compositor that names no output size */
#define DEFAULT_OUT_H  800
#define HID_ESCAPE     0x29             /* the HID usage of Escape */
#define BTN_LEFT       0x110            /* evdev's mouse buttons */
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112
#define AXIS_PER_NOTCH (10 * 256)       /* a wheel notch in wl_pointer.axis (fixed 24.8) */

/* What the app asked for before gfx_open (fun.h). */
static const char *title;
static int want_w, want_h;
static bool resizable;
static status_t (*connect_fn)(void *ctx, handle_t *out);
static void *connect_ctx;

/* The window while it is open. */
static struct {
    struct jwl_client *c;
    struct jwl_window *win;
    handle_t first;            /* svc_open's channel, for the first connect only */
    bool     keys;             /* the keys are the app's */
    bool     frame_waiting;    /* the last commit's frame callback hasn't come */
    bool     resize_owed;      /* a new size the app hasn't been given yet */
    bool     repaint_owed;     /* a new size for a picture that keeps its own: show it again */
    bool     dead;             /* the compositor is gone for good */
    int32_t  win_w, win_h;     /* the window's size at the last configure */
    uint8_t  buttons;          /* MOUSE_* held, as the pointer's events said */
    int      px, py;           /* the pointer, scr's pixels */
} wl;

static struct jwl_event stash[STASH_MAX];   /* the app's events, oldest at stash_head */
static unsigned stash_head, stash_len;

void gfx_title(const char *t) { title = t; }
void gfx_resizable(void) { resizable = true; }

void gfx_window_size(int w, int h)
{
    want_w = w;
    want_h = h;
}

void gfx_connect_with(status_t (*connect)(void *ctx, handle_t *out), void *ctx)
{
    connect_fn = connect;
    connect_ctx = ctx;
}

struct jwl_window *wl_window(void) { return wl.win; }
bool wl_dead(void) { return wl.dead; }
void wl_frame_asked(void) { wl.frame_waiting = true; }

/* ---- connecting and opening ---------------------------------------------------------- */

/* libjwl's connect: the channel gfx_open already opened the first time
 * (so a namespace without the compositor is known at once), /svc/wayland
 * again after that (the compositor restarted). */
static status_t connect_svc(void *ctx, handle_t *out)
{
    (void)ctx;
    if (wl.first != HANDLE_INVALID) {
        *out = wl.first;
        wl.first = HANDLE_INVALID;
        return OK;
    }
    return svc_open(JWL_SERVICE, out);
}

/* "<w>x<h>" from $FUN_WINDOW into *w, *h; false if unset or not that. */
static bool env_size(int *w, int *h)
{
    static const char key[] = "FUN_WINDOW=";
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, key, sizeof(key) - 1))
            continue;
        int v[2] = { 0, 0 };
        const char *d = *e + sizeof(key) - 1;
        for (int i = 0; i < 2; i++, d++) {
            while (*d >= '0' && *d <= '9' && v[i] <= JWL_SIZE_MAX)
                v[i] = v[i] * 10 + (*d++ - '0');
            if (*d != (i ? '\0' : 'x'))
                return false;
        }
        *w = v[0];
        *h = v[1];
        return true;
    }
    return false;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The window to ask for (fun.h's gfx_open says which). */
static void pick_size(const struct jwl_client_info *in, bool full, struct jwl_window_config *wc)
{
    int ow = in->output_width > 0 ? in->output_width : DEFAULT_OUT_W;
    int oh = in->output_height > 0 ? in->output_height : DEFAULT_OUT_H;
    int w = want_w, h = want_h;
    if (full) {
        w = ow;
        h = oh;
        wc->fullscreen = true;
    } else if (!w && !env_size(&w, &h)) {
        bool big = ow > BIG_SCREEN_W && oh > BIG_SCREEN_H;
        w = big ? BIG_W : ow;
        h = big ? BIG_H : oh;
        wc->maximized = !big;
    }
    wc->width = clampi(w, MIN_W, JWL_SIZE_MAX);
    wc->height = clampi(h, MIN_H, JWL_SIZE_MAX);
}

/* Read what the compositor sent and act on libfun's own news (below). */
static void drain(void);

/* Wait until something comes from the compositor, until (or the client's
 * own deadline: a key repeat, a reconnect's next try). */
static void wait_once(uint64_t until)
{
    uint64_t d = jwl_client_deadline(wl.c);
    d = until < d ? until : d;
    handle_t ch = jwl_client_channel(wl.c);
    signals_t seen;
    if (ch != HANDLE_INVALID)   /* a timeout or a wake: the caller looks either way */
        (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, d, &seen);
    else
        (void)jam_nanosleep(d);
}

/* The window's first configure, then scr the size it gives. */
static status_t first_configure(void)
{
    uint64_t until = now() + CONFIGURE_WAIT;
    for (;;) {
        drain();
        if (jwl_window_configured(wl.win))
            return OK;
        if (wl.dead)
            return ERR_PEER_CLOSED;
        if (now() >= until)
            return ERR_TIMED_OUT;
        wait_once(until);
    }
}

/* scr for a window w x h, on bg. */
static status_t make_screen(int w, int h, uint32_t bg)
{
    uint64_t px = (uint64_t)w * (uint64_t)h * 4;
    uint32_t *s = big_alloc(px), *shown = big_alloc(px);
    if (!s || !shown) {
        big_free(s, px);
        big_free(shown, px);
        return ERR_NO_MEMORY;
    }
    memset(&scr, 0, sizeof(scr));
    scr.s = (struct surf){ s, w, h, w };
    scr.shown = shown;
    scr.w = w;
    scr.h = h;
    scr.ui = h > 1100 ? 2 : 1;   /* by its own size: the apps' layouts are made that way */
    scr.bg = bg;
    scr.windowed = true;
    scr.open = true;
    if (bg)
        fill(&scr.s, 0, 0, w, h, bg);
    return OK;
}

/* The window, configured, and scr for it. */
static status_t open_window(uint32_t bg, bool full)
{
    const struct jwl_client_info *in = jwl_client_info(wl.c);
    if (!in->wm_base_version)
        return ERR_NOT_SUPPORTED;   /* a compositor without windows (yet) */
    struct jwl_window_config wc = { .title = title ? title : "Jam OS",
                                    .app_id = title ? title : "jamos", .resizable = resizable };
    pick_size(in, full, &wc);
    status_t st = jwl_window_create(wl.c, &wc, &wl.win);
    if (st == OK)
        st = first_configure();
    int32_t w = 0, h = 0;
    if (st == OK)
        jwl_window_size(wl.win, &w, &h);
    if (st == OK)
        st = make_screen(clampi(w, MIN_W, JWL_SIZE_MAX), clampi(h, MIN_H, JWL_SIZE_MAX), bg);
    return st;
}

status_t wl_open(uint32_t bg, bool keys, bool full)
{
    memset(&wl, 0, sizeof(wl));
    wl.first = HANDLE_INVALID;
    stash_head = stash_len = 0;
    if (!connect_fn && svc_open(JWL_SERVICE, &wl.first) != OK)
        return ERR_NOT_FOUND;   /* not in our namespace: the borrowed screen */
    struct jwl_client_config cfg = {
        .connect = connect_fn ? connect_fn : connect_svc, .connect_ctx = connect_ctx,
        .name = title ? title : "libfun", .no_keyboard = !keys,
    };
    status_t st = jwl_client_connect(&cfg, now() + CONNECT_WAIT, &wl.c);
    if (st == OK)
        st = open_window(bg, full);
    if (wl.first != HANDLE_INVALID)
        jam_handle_close(wl.first);   /* the connect never came to it */
    wl.first = HANDLE_INVALID;
    if (st != OK) {
        if (wl.win)
            jwl_window_destroy(wl.win);
        if (wl.c)
            jwl_client_destroy(wl.c);
        memset(&wl, 0, sizeof(wl));
        return st;
    }
    wl.keys = keys;
    jwl_window_size(wl.win, &wl.win_w, &wl.win_h);
    wl.px = scr.w / 2;
    wl.py = scr.h / 2;
    wl_paint_reset();
    return OK;
}

void wl_close(void)
{
    jwl_window_destroy(wl.win);   /* the compositor takes it off the screen */
    jwl_client_destroy(wl.c);
    memset(&wl, 0, sizeof(wl));
    stash_head = stash_len = 0;
    uint64_t px = (uint64_t)scr.w * (uint64_t)scr.h * 4;
    big_free(scr.s.px, px);   /* the back buffers were the window's */
    big_free(scr.shown, px);
    scr.s.px = scr.shown = NULL;
}

/* ---- the compositor's news ----------------------------------------------------------- */

/* A configure: a new size the app takes, if it took gfx_resizable; else
 * its picture is shown again in the window's new size (the app may not
 * present again for a while: mines waits for a click). */
static void configured(const struct jwl_event *ev)
{
    int w = clampi(ev->configure.width, MIN_W, JWL_SIZE_MAX);
    int h = clampi(ev->configure.height, MIN_H, JWL_SIZE_MAX);
    if (resizable && (w != scr.w || h != scr.h))
        wl.resize_owed = true;
    else if (ev->configure.width != wl.win_w || ev->configure.height != wl.win_h)
        wl.repaint_owed = true;
    wl.win_w = ev->configure.width;
    wl.win_h = ev->configure.height;
}

/* libfun's own news, acted on now: true if ev was one. */
static bool absorb(const struct jwl_event *ev)
{
    switch (ev->type) {
    case JWL_EV_FRAME:
        if (ev->win == wl.win)
            wl.frame_waiting = false;
        return true;
    case JWL_EV_CONFIGURE:
        if (ev->win == wl.win && scr.open)
            configured(ev);
        return true;
    case JWL_EV_DISCONNECTED:
        wl.frame_waiting = false;   /* it went with the connection */
        return true;
    case JWL_EV_RECONNECTED:
        wl_paint_reset();   /* the next present shows the whole back buffer again */
        return true;
    case JWL_EV_DEAD:
        wl.dead = true;
        return false;       /* the app hears it too: KEY_QUIT */
    case JWL_EV_KEY:
    case JWL_EV_CLOSE:
    case JWL_EV_POINTER_ENTER:
    case JWL_EV_POINTER_MOTION:
    case JWL_EV_POINTER_BUTTON:
    case JWL_EV_POINTER_AXIS:
        return false;
    default:
        return true;        /* focus and modifier news: the key events carry what we need */
    }
}

static void drain(void)
{
    (void)jwl_client_dispatch(wl.c);   /* a dead client's status: its JWL_EV_DEAD is queued */
    struct jwl_event ev;
    while (jwl_client_next_event(wl.c, &ev) == OK) {
        if (absorb(&ev) || stash_len == STASH_MAX)
            continue;   /* a full stash: the app isn't reading; the newest go */
        stash[(stash_head + stash_len++) % STASH_MAX] = ev;
    }
}

void wl_wait_frame(void)
{
    uint64_t until = now() + FRAME_WAIT;
    drain();
    while (wl.frame_waiting && !wl.dead && now() < until) {
        wait_once(until);
        drain();
    }
}

void wl_wait_until(uint64_t until)
{
    wait_once(until);
    drain();
}

void wl_place(int32_t *w, int32_t *h, int *ox, int *oy)
{
    jwl_window_size(wl.win, w, h);
    *ox = (*w - scr.w) / 2;
    *oy = (*h - scr.h) / 2;
}

/* The new size, into scr: true if it changed (the app hears KEY_RESIZE).
 * Without the memory for it the picture keeps its size, centred. */
static bool take_size(void)
{
    wl.resize_owed = false;
    int32_t w, h;
    jwl_window_size(wl.win, &w, &h);
    w = clampi(w, MIN_W, JWL_SIZE_MAX);
    h = clampi(h, MIN_H, JWL_SIZE_MAX);
    if (w == scr.w && h == scr.h)
        return false;
    uint64_t px = (uint64_t)w * (uint64_t)h * 4, old = (uint64_t)scr.w * (uint64_t)scr.h * 4;
    uint32_t *s = big_alloc(px), *shown = big_alloc(px);
    if (!s || !shown) {
        big_free(s, px);
        big_free(shown, px);
        return false;
    }
    big_free(scr.s.px, old);
    big_free(scr.shown, old);
    scr.s = (struct surf){ s, w, h, w };
    scr.shown = shown;
    scr.w = w;
    scr.h = h;
    scr.ui = h > 1100 ? 2 : 1;
    if (scr.bg)
        fill(&scr.s, 0, 0, w, h, scr.bg);
    wl.px = clampi(wl.px, 0, w - 1);
    wl.py = clampi(wl.py, 0, h - 1);
    wl_paint_reset();
    return true;
}

/* ---- the app's events -------------------------------------------------------------------- */

/* A key as the console sends it (<jam/abi.h>). */
static void key_of(const struct jwl_event *e, struct input_key_event *out)
{
    uint32_t m = e->key.mods;
    out->usage = (uint16_t)keymap_hid_of_evdev(e->key.code);
    out->state = e->key.state == JWL_KEY_RELEASED   ? INPUT_KEY_UP
                 : e->key.state == JWL_KEY_REPEATED ? INPUT_KEY_REPEAT
                                                    : INPUT_KEY_DOWN;
    out->mods = (uint8_t)((m & KEYMAP_MOD_CTRL ? INPUT_MOD_LCTRL : 0) |
                          (m & KEYMAP_MOD_SHIFT ? INPUT_MOD_LSHIFT : 0) |
                          (m & KEYMAP_MOD_ALT ? INPUT_MOD_LALT : 0) |
                          (m & KEYMAP_MOD_SUPER ? INPUT_MOD_LGUI : 0));
    out->codepoint = e->key.cp;
}

/* A surface coordinate (fixed 24.8) as a pixel of scr's: off is where the
 * picture's 0 is in the window. */
static int pixel_of(int32_t fixed, int off)
{
    int32_t px = fixed >= 0 ? fixed / 256 : -((-fixed + 255) / 256);
    return px - off;
}

static uint8_t button_of(uint32_t evdev)
{
    return evdev == BTN_LEFT ? MOUSE_LEFT : evdev == BTN_RIGHT ? MOUSE_RIGHT
           : evdev == BTN_MIDDLE ? MOUSE_MIDDLE : 0;
}

/* A pointer event into mouse.c: MSG_MOVE or MSG_BUTTON. */
static enum msg_kind pointer(const struct jwl_event *e)
{
    int32_t w, h;
    int ox, oy, wheel = 0;
    wl_place(&w, &h, &ox, &oy);
    if (e->type == JWL_EV_POINTER_ENTER || e->type == JWL_EV_POINTER_MOTION) {
        wl.px = clampi(pixel_of(e->pointer.x, ox), 0, scr.w - 1);
        wl.py = clampi(pixel_of(e->pointer.y, oy), 0, scr.h - 1);
    } else if (e->type == JWL_EV_POINTER_BUTTON) {
        uint8_t b = button_of(e->button.button);
        wl.buttons = e->button.pressed ? wl.buttons | b : wl.buttons & ~b;
    } else if (e->axis.axis == JWL_WL_POINTER_AXIS_VERTICAL_SCROLL) {
        /* down the screen is positive here, away from the user in fun.h */
        wheel = e->axis.discrete ? -e->axis.discrete : -e->axis.value / AXIS_PER_NOTCH;
    }
    return mouse_at(wl.px, wl.py, wl.buttons, wheel) ? MSG_BUTTON : MSG_MOVE;
}

/* One of the app's events for keys.c: true with *ev or the mouse's news
 * and *kind, false for one it doesn't see. */
static bool app_event(const struct jwl_event *e, struct input_key_event *ev, enum msg_kind *kind)
{
    switch (e->type) {
    case JWL_EV_KEY:
        key_of(e, ev);
        *kind = MSG_KEY;
        return true;
    case JWL_EV_CLOSE:   /* the close box: as Escape */
        *ev = (struct input_key_event){ HID_ESCAPE, INPUT_KEY_DOWN, 0, 0x1b };
        *kind = MSG_KEY;
        return true;
    case JWL_EV_POINTER_ENTER:
    case JWL_EV_POINTER_MOTION:
    case JWL_EV_POINTER_BUTTON:
    case JWL_EV_POINTER_AXIS:
        if (!mouse_wanted())
            return false;
        *kind = pointer(e);
        return true;
    default:
        return false;
    }
}

status_t wl_next(uint64_t deadline, struct input_key_event *ev, enum msg_kind *kind)
{
    if (!wl.keys)
        return ERR_BAD_HANDLE;
    for (;;) {
        drain();
        if (wl.resize_owed && take_size()) {
            *kind = MSG_RESIZE;
            return OK;
        }
        if (wl.repaint_owed) {
            wl.repaint_owed = false;
            wl_present(true);   /* the whole picture, placed for the new size */
        }
        while (stash_len) {
            struct jwl_event e = stash[stash_head];
            stash_head = (stash_head + 1) % STASH_MAX;
            stash_len--;
            if (e.type == JWL_EV_DEAD)
                return ERR_PEER_CLOSED;
            if (app_event(&e, ev, kind))
                return OK;
        }
        if (wl.dead)
            return ERR_PEER_CLOSED;
        if (!deadline || now() >= deadline)
            return ERR_TIMED_OUT;
        pool_rest();   /* the app waits: no worker spins meanwhile */
        wait_once(deadline);
    }
}
