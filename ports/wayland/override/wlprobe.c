/* wlprobe: a test client on the real libwayland that checks, case by case and
 * by name, what Chromium's Ozone/Wayland backend relies on and Jam OS's
 * compositor offers (G2's track GT, docs/G2-PLAN.md section 11, rung 1).
 * It is the end-to-end test of the libc Wayland socket (GS1) and of the
 * compositor as a libwayland client sees it; tools/wl-port-test.sh runs the
 * cases in QEMU, injects input and checks screenshots.
 *
 *     wlprobe -l                       list the cases and what each needs
 *     wlprobe [options] <case>...      run the named cases, in order
 *     wlprobe [options] all            every case that needs no one's help and
 *                                      no particular layout (not input, hostile,
 *                                      states-*, subcompositor: ask for those by
 *                                      name)
 *     wlprobe hold [seconds]           a window that stays (the soak's), redrawn
 *                                      on every frame callback, until closed
 *
 *     options:  -s <seconds>   how long a case holds a window still for a
 *                              screenshot (default 4; 0: no hold)
 *               -t <seconds>   how long a case that waits for injected input
 *                              waits for it (default 30)
 *
 * Every case prints "PASS <case>: <what it saw>" or "FAIL <case>: <why>" (the
 * first thing that failed; the case goes on and tells the rest as "wlprobe:
 * <case>: ..." lines before). A case that needs help says so first:
 *     wlprobe: <case>: ready          the window is up: inject the input now
 *     wlprobe: <case>: shot <name>    hold still: take the screenshot <name>
 * The hostile-* cases are clients that break the rules on purpose (track
 * GR, docs/G2-REVIEW.md): each wants the protocol's answer, and a plain
 * window served after. The exit status is 0 if every case passed.
 *
 * The compositor offers what the plan says (docs/G2-PLAN.md, "What Chromium's
 * Wayland backend needs"); each case binds at the version Chromium would.
 * This is Jam OS's (BSD-2-Clause), not libwayland's. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* linux/input-event-codes.h is not on Jam OS yet. */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define KEY_A      30

/* ---- results ------------------------------------------------------------------------ */

struct probe {
    const char *name;
    bool failed;
    char why[300];
    char seen[400];
};

static struct probe *cur;
static int g_hold = 4;     /* -s */
static int g_wait = 30;    /* -t */
static const char *g_arg;  /* the text after = in a case's name */

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    printf("wlprobe: %s: ", cur ? cur->name : "-");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* The case failed, for this reason (the first one is the verdict). */
static void fail(const char *fmt, ...)
{
    va_list ap;
    char buf[300];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!cur->failed) {
        cur->failed = true;
        snprintf(cur->why, sizeof(cur->why), "%s", buf);
    } else {
        say("also: %s", buf);
    }
}

/* Something the case saw, for the PASS line. */
static void seen(const char *fmt, ...)
{
    va_list ap;
    char buf[200];
    size_t n = strlen(cur->seen);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    snprintf(cur->seen + n, sizeof(cur->seen) - n, "%s%s", n ? ", " : "", buf);
}

#define CHECK(cond, ...) do { if (!(cond)) fail(__VA_ARGS__); } while (0)
#define REQUIRE(cond, ...) do { if (!(cond)) { fail(__VA_ARGS__); return; } } while (0)

/* ---- one connection, and everything events tell ----------------------------------------- */

#define MAXG 48
#define MAXPE 512
#define MAXKE 256
#define MAXCFG 64

struct glob {
    char iface[56];
    uint32_t name, version;
};

enum { PE_ENTER, PE_LEAVE, PE_MOTION, PE_BUTTON, PE_AXIS, PE_AXIS120, PE_AXISD, PE_FRAME };

struct pev {          /* a wl_pointer event */
    int t;
    double x, y;      /* enter, motion: surface coordinates */
    uint32_t serial;  /* enter, leave, button */
    uint32_t a, b;    /* button: button, state; axis: axis */
    int32_t v;        /* axis: value * 256 (wl_fixed); axis120, discrete: the count */
    uint32_t time;
    struct wl_surface *surface;
};

enum { KE_ENTER, KE_LEAVE, KE_KEY, KE_MODS };

struct kev {          /* a wl_keyboard event */
    int t;
    uint32_t serial, code, state;
    xkb_keysym_t sym;
    char utf8[8];
    uint32_t depressed;
};

struct offer {
    struct wl_data_offer *o;
    char mime[8][64];
    int n;
};

struct win;

struct env {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct glob globs[MAXG];
    int nglobs;
    bool dead;
    int err_code;
    const char *err_iface;

    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct wl_output *out;
    struct wl_data_device_manager *ddm;
    struct wp_cursor_shape_manager_v1 *csm;
    struct zxdg_decoration_manager_v1 *dm;
    uint32_t comp_ver, shm_ver, wm_ver, seat_ver, out_ver, ddm_ver, csm_ver, dm_ver;

    bool shm_argb, shm_xrgb;
    unsigned shm_formats;
    unsigned releases;           /* wl_buffer.release events */
    unsigned pings;              /* xdg_wm_base.ping events */

    uint32_t seat_caps;
    char seat_name[64];
    bool got_seat_caps;
    struct wl_pointer *ptr;
    struct wl_keyboard *kbd;
    struct wl_data_device *ddev;
    struct wp_cursor_shape_device_v1 *shape;
    uint32_t last_serial;        /* the newest serial an input event carried */

    /* wl_output */
    bool got_geom, got_mode, got_done, got_scale, got_name, got_desc;
    int32_t out_w, out_h, out_refresh, out_scale, geom_x, geom_y, phys_w, phys_h;
    uint32_t mode_flags;
    char out_name[64], out_desc[128];

    /* wl_pointer, wl_keyboard */
    struct pev pe[MAXPE];
    int npe;
    uint32_t ptr_enter_serial;
    struct kev ke[MAXKE];
    int nke;
    uint32_t kbd_enter_serial;
    struct wl_surface *kbd_on;   /* the surface the last enter named, NULL after a leave */
    struct xkb_context *xctx;
    struct xkb_keymap *xmap;
    struct xkb_state *xstate;
    bool got_keymap;
    unsigned shift_bit;          /* the keymap's Shift, as a modifier mask bit */
    uint32_t keymap_format;
    uint32_t keymap_size;
    char *keymap_text;
    int32_t repeat_rate, repeat_delay;
    bool got_repeat;

    /* the clipboard */
    struct offer *sel;           /* the current selection's offer (NULL: none) */
    unsigned nsel_events;
    struct wl_data_source *src;
    const char *src_text;
    unsigned src_sends;          /* wl_data_source.send events served */
    char src_mime[64];
    bool src_cancelled;
};

/* The events of the pointer and keyboard are logged; a case reads the log. */
static void pe_add(struct env *e, struct pev ev)
{
    if (e->npe < MAXPE)
        e->pe[e->npe++] = ev;
}

static void ke_add(struct env *e, struct kev ev)
{
    if (e->nke < MAXKE)
        e->ke[e->nke++] = ev;
}

/* ---- the registry ---------------------------------------------------------------------- */

static void on_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
                      uint32_t version)
{
    struct env *e = data;
    (void)r;
    if (e->nglobs < MAXG) {
        snprintf(e->globs[e->nglobs].iface, sizeof(e->globs[0].iface), "%s", iface);
        e->globs[e->nglobs].name = name;
        e->globs[e->nglobs].version = version;
        e->nglobs++;
    }
}

static void on_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data, (void)r, (void)name;
}

static const struct wl_registry_listener registry_listener = { on_global, on_global_remove };

static const struct glob *env_glob(const struct env *e, const char *iface)
{
    for (int i = 0; i < e->nglobs; i++)
        if (!strcmp(e->globs[i].iface, iface))
            return &e->globs[i];
    return NULL;
}

/* ---- pumping events --------------------------------------------------------------------- */

static void check_error(struct env *e)
{
    int err = wl_display_get_error(e->dpy);
    if (err && !e->dead) {
        e->dead = true;
        e->err_code = err;
        e->err_iface = NULL;
        if (err == EPROTO) {
            const struct wl_interface *iface = NULL;
            uint32_t id = 0;
            e->err_code = (int)wl_display_get_protocol_error(e->dpy, &iface, &id);
            e->err_iface = iface ? iface->name : "?";
        }
    }
}

/* Read and dispatch whatever arrives within ms milliseconds (0: what is
 * there); returns false once the connection is dead. */
static bool pump(struct env *e, int ms)
{
    if (e->dead)
        return false;
    wl_display_dispatch_pending(e->dpy);
    if (wl_display_flush(e->dpy) < 0 && errno != EAGAIN) {
        check_error(e);
        e->dead = true;
        return false;
    }
    while (wl_display_prepare_read(e->dpy) != 0)
        wl_display_dispatch_pending(e->dpy);
    struct pollfd p = { .fd = wl_display_get_fd(e->dpy), .events = POLLIN };
    int r = poll(&p, 1, ms);
    if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
        if (wl_display_read_events(e->dpy) < 0) {
            check_error(e);
            e->dead = true;
            return false;
        }
    } else {
        wl_display_cancel_read(e->dpy);
    }
    wl_display_dispatch_pending(e->dpy);
    check_error(e);
    return !e->dead;
}

/* Pump until cond holds or ms pass; true if it held. */
#define WAIT(e, cond, ms) ({                                          \
    double wait_end_ = now_s() + (double)(ms) / 1000.0;               \
    bool wait_ok_ = (cond);                                           \
    while (!wait_ok_ && now_s() < wait_end_ && !(e)->dead) {          \
        pump((e), 50);                                                \
        wait_ok_ = (cond);                                            \
    }                                                                 \
    wait_ok_;                                                         \
})

/* Pump for s seconds whatever happens (a window held still for a screenshot). */
static void hold_for(struct env *e, double s)
{
    double end = now_s() + s;
    while (now_s() < end && !e->dead)
        pump(e, 50);
}

static bool roundtrip(struct env *e)
{
    if (e->dead)
        return false;
    if (wl_display_roundtrip(e->dpy) < 0) {
        check_error(e);
        e->dead = true;
        return false;
    }
    check_error(e);
    return !e->dead;
}

/* ---- wl_shm, wl_output, wl_seat ---------------------------------------------------------- */

static void on_shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    struct env *e = data;
    (void)shm;
    e->shm_formats++;
    if (format == WL_SHM_FORMAT_ARGB8888)
        e->shm_argb = true;
    if (format == WL_SHM_FORMAT_XRGB8888)
        e->shm_xrgb = true;
}

static const struct wl_shm_listener shm_listener = { on_shm_format };

static void on_out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
    struct env *e = data;
    (void)o, (void)subpixel, (void)make, (void)model, (void)transform;
    e->got_geom = true;
    e->geom_x = x, e->geom_y = y, e->phys_w = pw, e->phys_h = ph;
}

static void on_out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                        int32_t refresh)
{
    struct env *e = data;
    (void)o;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        e->got_mode = true;
        e->out_w = w, e->out_h = h, e->out_refresh = refresh;
        e->mode_flags = flags;
    }
}

static void on_out_done(void *data, struct wl_output *o)
{
    struct env *e = data;
    (void)o;
    e->got_done = true;
}

static void on_out_scale(void *data, struct wl_output *o, int32_t factor)
{
    struct env *e = data;
    (void)o;
    e->got_scale = true;
    e->out_scale = factor;
}

static void on_out_name(void *data, struct wl_output *o, const char *name)
{
    struct env *e = data;
    (void)o;
    e->got_name = true;
    snprintf(e->out_name, sizeof(e->out_name), "%s", name);
}

static void on_out_desc(void *data, struct wl_output *o, const char *desc)
{
    struct env *e = data;
    (void)o;
    e->got_desc = true;
    snprintf(e->out_desc, sizeof(e->out_desc), "%s", desc);
}

static const struct wl_output_listener output_listener = {
    on_out_geometry, on_out_mode, on_out_done, on_out_scale, on_out_name, on_out_desc,
};

static void on_seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
    struct env *e = data;
    (void)s;
    e->seat_caps = caps;
    e->got_seat_caps = true;
}

static void on_seat_name(void *data, struct wl_seat *s, const char *name)
{
    struct env *e = data;
    (void)s;
    snprintf(e->seat_name, sizeof(e->seat_name), "%s", name);
}

static const struct wl_seat_listener seat_listener = { on_seat_caps, on_seat_name };

/* ---- wl_pointer --------------------------------------------------------------------------- */

static void on_ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                         wl_fixed_t x, wl_fixed_t y)
{
    struct env *e = data;
    (void)p;
    e->last_serial = serial;
    e->ptr_enter_serial = serial;
    pe_add(e, (struct pev){ .t = PE_ENTER, .serial = serial, .x = wl_fixed_to_double(x),
                            .y = wl_fixed_to_double(y), .surface = s });
}

static void on_ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    struct env *e = data;
    (void)p;
    e->last_serial = serial;
    pe_add(e, (struct pev){ .t = PE_LEAVE, .serial = serial, .surface = s });
}

static void on_ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
                          wl_fixed_t y)
{
    struct env *e = data;
    (void)p;
    pe_add(e, (struct pev){ .t = PE_MOTION, .time = time, .x = wl_fixed_to_double(x),
                            .y = wl_fixed_to_double(y) });
}

static void on_ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                          uint32_t button, uint32_t state)
{
    struct env *e = data;
    (void)p;
    e->last_serial = serial;
    /* where it happened: the pointer's last place */
    double x = 0, y = 0;
    for (int i = e->npe - 1; i >= 0; i--)
        if (e->pe[i].t == PE_ENTER || e->pe[i].t == PE_MOTION) {
            x = e->pe[i].x, y = e->pe[i].y;
            break;
        }
    pe_add(e, (struct pev){ .t = PE_BUTTON, .serial = serial, .time = time, .a = button,
                            .b = state, .x = x, .y = y });
}

static void on_ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
                        wl_fixed_t value)
{
    struct env *e = data;
    (void)p;
    pe_add(e, (struct pev){ .t = PE_AXIS, .time = time, .a = axis, .v = value });
}

static void on_ptr_frame(void *data, struct wl_pointer *p)
{
    struct env *e = data;
    (void)p;
    pe_add(e, (struct pev){ .t = PE_FRAME });
}

static void on_ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
    (void)data, (void)p, (void)source;
}

static void on_ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
    (void)data, (void)p, (void)time, (void)axis;
}

static void on_ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t d)
{
    struct env *e = data;
    (void)p;
    pe_add(e, (struct pev){ .t = PE_AXISD, .a = axis, .v = d });
}

static void on_ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t v)
{
    struct env *e = data;
    (void)p;
    pe_add(e, (struct pev){ .t = PE_AXIS120, .a = axis, .v = v });
}

static void on_ptr_axis_dir(void *data, struct wl_pointer *p, uint32_t axis, uint32_t dir)
{
    (void)data, (void)p, (void)axis, (void)dir;
}

static void on_ptr_warp(void *data, struct wl_pointer *p, wl_fixed_t x, wl_fixed_t y)
{
    (void)data, (void)p, (void)x, (void)y;   /* wl_pointer v11: never bound that high */
}

static const struct wl_pointer_listener pointer_listener = {
    on_ptr_enter, on_ptr_leave, on_ptr_motion, on_ptr_button, on_ptr_axis, on_ptr_frame,
    on_ptr_axis_source, on_ptr_axis_stop, on_ptr_axis_discrete, on_ptr_axis_value120,
    on_ptr_axis_dir, on_ptr_warp,
};

/* ---- wl_keyboard -------------------------------------------------------------------------- */

static void on_kbd_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
                          uint32_t size)
{
    struct env *e = data;
    (void)k;
    e->keymap_format = format;
    e->keymap_size = size;
    e->got_keymap = true;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0 && size < (1u << 24)) {
        void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            free(e->keymap_text);
            e->keymap_text = calloc(1, size + 1);
            if (e->keymap_text)
                memcpy(e->keymap_text, map, size);
            munmap(map, size);
        } else {
            e->keymap_text = NULL;
        }
    }
    close(fd);
}

static void on_kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                         struct wl_surface *s, struct wl_array *keys)
{
    struct env *e = data;
    (void)k, (void)keys;
    e->last_serial = serial;
    e->kbd_enter_serial = serial;
    e->kbd_on = s;
    ke_add(e, (struct kev){ .t = KE_ENTER, .serial = serial });
}

static void on_kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    struct env *e = data;
    (void)k;
    e->last_serial = serial;
    if (e->kbd_on == s)
        e->kbd_on = NULL;
    ke_add(e, (struct kev){ .t = KE_LEAVE, .serial = serial });
}

static void on_kbd_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
                       uint32_t key, uint32_t state)
{
    struct env *e = data;
    (void)k, (void)time;
    e->last_serial = serial;
    struct kev ev = { .t = KE_KEY, .serial = serial, .code = key, .state = state };
    if (e->xstate) {
        ev.sym = xkb_state_key_get_one_sym(e->xstate, key + 8);
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            xkb_state_key_get_utf8(e->xstate, key + 8, ev.utf8, sizeof(ev.utf8));
    }
    ke_add(e, ev);
}

static void on_kbd_mods(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
                        uint32_t latched, uint32_t locked, uint32_t group)
{
    struct env *e = data;
    (void)k;
    e->last_serial = serial;
    if (e->xstate)
        xkb_state_update_mask(e->xstate, depressed, latched, locked, 0, 0, group);
    ke_add(e, (struct kev){ .t = KE_MODS, .serial = serial, .depressed = depressed });
}

static void on_kbd_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    struct env *e = data;
    (void)k;
    e->got_repeat = true;
    e->repeat_rate = rate, e->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    on_kbd_keymap, on_kbd_enter, on_kbd_leave, on_kbd_key, on_kbd_mods, on_kbd_repeat,
};

/* ---- the clipboard -------------------------------------------------------------------------- */

static void on_offer_offer(void *data, struct wl_data_offer *o, const char *mime)
{
    struct offer *of = data;
    (void)o;
    if (of->n < 8) {
        snprintf(of->mime[of->n], sizeof(of->mime[0]), "%s", mime);
        of->n++;
    }
}

static void on_offer_actions(void *data, struct wl_data_offer *o, uint32_t actions)
{
    (void)data, (void)o, (void)actions;
}

static void on_offer_action(void *data, struct wl_data_offer *o, uint32_t action)
{
    (void)data, (void)o, (void)action;
}

static const struct wl_data_offer_listener offer_listener = {
    on_offer_offer, on_offer_actions, on_offer_action,
};

static void on_dd_data_offer(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
    struct offer *of = calloc(1, sizeof(*of));
    (void)data, (void)d;
    if (!of)
        return;
    of->o = o;
    wl_data_offer_add_listener(o, &offer_listener, of);
}

static void on_dd_enter(void *data, struct wl_data_device *d, uint32_t serial,
                        struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
    (void)data, (void)d, (void)serial, (void)s, (void)x, (void)y, (void)o;
}

static void on_dd_leave(void *data, struct wl_data_device *d)
{
    (void)data, (void)d;
}

static void on_dd_motion(void *data, struct wl_data_device *d, uint32_t time, wl_fixed_t x,
                         wl_fixed_t y)
{
    (void)data, (void)d, (void)time, (void)x, (void)y;
}

static void on_dd_drop(void *data, struct wl_data_device *d)
{
    (void)data, (void)d;
}

static void on_dd_selection(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
    struct env *e = data;
    (void)d;
    e->nsel_events++;
    e->sel = o ? wl_data_offer_get_user_data(o) : NULL;
}

static const struct wl_data_device_listener data_device_listener = {
    on_dd_data_offer, on_dd_enter, on_dd_leave, on_dd_motion, on_dd_drop, on_dd_selection,
};

static void on_src_target(void *data, struct wl_data_source *s, const char *mime)
{
    (void)data, (void)s, (void)mime;
}

/* The compositor passes a reader's pipe on: write the text and close it. */
static void on_src_send(void *data, struct wl_data_source *s, const char *mime, int32_t fd)
{
    struct env *e = data;
    (void)s;
    size_t n = strlen(e->src_text), done = 0;
    while (done < n) {
        ssize_t w = write(fd, e->src_text + done, n - done);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    close(fd);
    snprintf(e->src_mime, sizeof(e->src_mime), "%s", mime);
    if (done == n)
        e->src_sends++;
}

static void on_src_cancelled(void *data, struct wl_data_source *s)
{
    struct env *e = data;
    (void)s;
    e->src_cancelled = true;
}

static void on_src_dnd_drop(void *data, struct wl_data_source *s)
{
    (void)data, (void)s;
}

static void on_src_dnd_finished(void *data, struct wl_data_source *s)
{
    (void)data, (void)s;
}

static void on_src_action(void *data, struct wl_data_source *s, uint32_t action)
{
    (void)data, (void)s, (void)action;
}

static const struct wl_data_source_listener source_listener = {
    on_src_target, on_src_send, on_src_cancelled, on_src_dnd_drop, on_src_dnd_finished,
    on_src_action,
};

/* ---- xdg_wm_base, buffers, windows ----------------------------------------------------------- */

static void on_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    struct env *e = data;
    e->pings++;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { on_ping };

static void on_buffer_release(void *data, struct wl_buffer *b)
{
    struct env *e = data;
    e->releases++;
    wl_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = { on_buffer_release };

typedef void (*fill_fn)(uint32_t *px, int w, int h, void *user);

/* A buffer of w x h pixels in format fmt, filled by fill, with no listener
 * (it is the caller's for good). NULL if the memory can't be had. */
static struct wl_buffer *buf_keep(struct env *e, int w, int h, uint32_t fmt, fill_fn fill,
                                  void *user)
{
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("wlprobe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    fill(px, w, h, user);
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(e->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

/* The same, used once: destroyed when the compositor releases it. */
static struct wl_buffer *buf_make(struct env *e, int w, int h, uint32_t fmt, fill_fn fill,
                                  void *user)
{
    struct wl_buffer *b = buf_keep(e, w, h, fmt, fill, user);
    if (b)
        wl_buffer_add_listener(b, &buffer_listener, e);
    return b;
}

static void rect(uint32_t *px, int w, int h, int x, int y, int rw, int rh, uint32_t c)
{
    for (int j = y; j < y + rh && j < h; j++)
        for (int i = x; i < x + rw && i < w; i++)
            if (i >= 0 && j >= 0)
                px[j * w + i] = c;
}

struct cfg {
    int w, h;
    uint32_t states;      /* bit n: xdg_toplevel_state n */
};

#define ST(s) (1u << (s))

struct win {
    struct env *e;
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_toplevel *top;
    struct zxdg_toplevel_decoration_v1 *deco;
    int pw, ph;               /* the toplevel's configure, until xdg_surface's */
    uint32_t pstates;
    struct cfg cfgs[MAXCFG];
    int ncfg;
    uint32_t last_serial;
    unsigned acked;
    int bw, bh;               /* the committed buffer's size */
    bool closed;
    bool got_caps, got_bounds;
    uint32_t caps;            /* bit n: wm_capabilities n */
    int bound_w, bound_h;
    int modes[16];            /* zxdg_toplevel_decoration_v1.configure modes, in order */
    int nmodes;
    unsigned frames;          /* frame callbacks done */
    uint32_t frame_time;
    bool frame_pending;
    fill_fn fill;             /* what win_paint draws (NULL: the default pattern) */
    void *fill_user;
};

static void on_top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                             struct wl_array *states)
{
    struct win *win = data;
    (void)t;
    win->pw = w, win->ph = h;
    win->pstates = 0;
    uint32_t *s;
    wl_array_for_each(s, states) if (*s < 32) win->pstates |= ST(*s);
}

static void on_top_close(void *data, struct xdg_toplevel *t)
{
    struct win *win = data;
    (void)t;
    win->closed = true;
}

static void on_top_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
    struct win *win = data;
    (void)t;
    win->got_bounds = true;
    win->bound_w = w, win->bound_h = h;
}

static void on_top_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
    struct win *win = data;
    (void)t;
    win->got_caps = true;
    win->caps = 0;
    uint32_t *c;
    wl_array_for_each(c, caps) if (*c < 32) win->caps |= ST(*c);
}

static const struct xdg_toplevel_listener toplevel_listener = {
    on_top_configure, on_top_close, on_top_bounds, on_top_caps,
};

/* Every xdg_surface.configure is acked at once (the buffer that follows is
 * the case's: win_follow). */
static void on_xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    struct win *win = data;
    xdg_surface_ack_configure(xs, serial);
    win->acked++;
    win->last_serial = serial;
    if (win->ncfg < MAXCFG)
        win->cfgs[win->ncfg++] = (struct cfg){ win->pw, win->ph, win->pstates };
}

static const struct xdg_surface_listener xsurface_listener = { on_xs_configure };

static void on_deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode)
{
    struct win *win = data;
    (void)d;
    if (win->nmodes < 16)
        win->modes[win->nmodes++] = (int)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = { on_deco_configure };

static void on_frame(void *data, struct wl_callback *cb, uint32_t time)
{
    struct win *win = data;
    wl_callback_destroy(cb);
    win->frames++;
    win->frame_time = time;
    win->frame_pending = false;
}

static const struct wl_callback_listener frame_listener = { on_frame };

/* The default picture: dark ground, a checker corner and a marker block at the
 * top left (so a screenshot says where the surface is). */
#define MARK 0xffff00ffu
static void fill_default(uint32_t *px, int w, int h, void *user)
{
    (void)user;
    rect(px, w, h, 0, 0, w, h, 0xff202830u);
    rect(px, w, h, 0, 0, 16, 16, MARK);
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++)
            rect(px, w, h, 24 + i * 8, 24 + j * 8, 8, 8, (i + j) & 1 ? 0xffe0e0e0u : 0xff303030u);
}

/* Draw the buffer (w x h), attach, damage it whole, ask for a frame callback
 * and commit. */
static bool win_paint_sized(struct win *win, int w, int h)
{
    struct wl_buffer *b = buf_make(win->e, w, h, WL_SHM_FORMAT_ARGB8888,
                                   win->fill ? win->fill : fill_default, win->fill_user);
    if (!b)
        return false;
    wl_surface_attach(win->surf, b, 0, 0);
    wl_surface_damage_buffer(win->surf, 0, 0, w, h);
    if (!win->frame_pending) {
        struct wl_callback *cb = wl_surface_frame(win->surf);
        wl_callback_add_listener(cb, &frame_listener, win);
        win->frame_pending = true;
    }
    wl_surface_commit(win->surf);
    win->bw = w, win->bh = h;
    return true;
}

#define DEF_W 400
#define DEF_H 300

/* The size the window should be now: the newest configure's, else ours. */
static void win_size(const struct win *win, int *w, int *h)
{
    *w = DEF_W, *h = DEF_H;
    for (int i = win->ncfg - 1; i >= 0; i--)
        if (win->cfgs[i].w > 0 && win->cfgs[i].h > 0) {
            *w = win->cfgs[i].w, *h = win->cfgs[i].h;
            return;
        }
}

static bool win_paint(struct win *win)
{
    int w, h;
    win_size(win, &w, &h);
    return win_paint_sized(win, w, h);
}

static const struct cfg *win_cfg(const struct win *win)
{
    return win->ncfg ? &win->cfgs[win->ncfg - 1] : NULL;
}

/* Did any configure from index i on have state s? */
static bool win_state_since(const struct win *win, int i, int s)
{
    for (; i < win->ncfg; i++)
        if (win->cfgs[i].states & ST(s))
            return true;
    return false;
}

/* Repaint at the size of the newest configure if the buffer isn't that size. */
static void win_follow(struct win *win)
{
    int w, h;
    win_size(win, &w, &h);
    if (w != win->bw || h != win->bh)
        win_paint_sized(win, w, h);
}

enum { WF_DECO = 1, WF_DECO_CLIENT = 2, WF_NOMAP = 4, WF_NOWAIT = 8, WF_KEYLESS = 16 };

static bool env_pointer(struct env *e);
static bool env_keyboard(struct env *e);

/* The usual way to a window: surface, xdg_surface, toplevel, (decoration), title
 * and app id, the initial commit, the first configure, a buffer, the first
 * frame. */
static bool win_open(struct env *e, struct win *win, int flags, const char *title)
{
    memset(win, 0, sizeof(*win));
    win->e = e;
    if (!e->dm)
        flags &= ~(WF_DECO | WF_DECO_CLIENT);
    /* The compositor gives the keys to a mapped window only if its client has a
     * wl_keyboard (a window of a client that takes no keys is "keyless": it is not
     * activated, and a click is what focuses it). Chromium makes the pointer and
     * the keyboard as soon as the seat says it has them; so does a probe window,
     * unless WF_KEYLESS asks for a window with no keyboard behind it. */
    if (e->seat) {
        if (!(flags & WF_KEYLESS) && (e->seat_caps & WL_SEAT_CAPABILITY_KEYBOARD))
            env_keyboard(e);
        if (e->seat_caps & WL_SEAT_CAPABILITY_POINTER)
            env_pointer(e);
    }
    win->surf = wl_compositor_create_surface(e->comp);
    win->xs = xdg_wm_base_get_xdg_surface(e->wm, win->surf);
    xdg_surface_add_listener(win->xs, &xsurface_listener, win);
    win->top = xdg_surface_get_toplevel(win->xs);
    xdg_toplevel_add_listener(win->top, &toplevel_listener, win);
    if (flags & WF_DECO) {
        win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(e->dm, win->top);
        zxdg_toplevel_decoration_v1_add_listener(win->deco, &deco_listener, win);
        if (flags & WF_DECO_CLIENT)
            zxdg_toplevel_decoration_v1_set_mode(win->deco,
                                                 ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }
    xdg_toplevel_set_title(win->top, title ? title : "wlprobe");
    xdg_toplevel_set_app_id(win->top, "wlprobe");
    wl_surface_commit(win->surf);
    if (flags & WF_NOMAP)
        return true;
    if (!WAIT(e, win->acked > 0, 5000)) {
        fail("no configure within 5 s of the initial commit%s", e->dead ? " (connection dead)" : "");
        return false;
    }
    if (!win_paint(win)) {
        fail("no memory for a buffer");
        return false;
    }
    if (!(flags & WF_NOWAIT) && !WAIT(e, win->frames > 0, 5000)) {
        fail("no frame callback within 5 s of the first buffer");
        return false;
    }
    return true;
}

static void win_destroy(struct win *win)
{
    if (win->deco)
        zxdg_toplevel_decoration_v1_destroy(win->deco);
    xdg_toplevel_destroy(win->top);
    xdg_surface_destroy(win->xs);
    wl_surface_destroy(win->surf);
    win->deco = NULL;
}

/* A screenshot point: hold the window still (and answering) for the script. */
static void shot(struct env *e, const char *name)
{
    say("shot %s", name);
    hold_for(e, g_hold);
}

/* ---- connecting ------------------------------------------------------------------------------- */

enum { B_NODECO = 1, B_NOSEAT = 2, B_NOOUT = 4 };

/* Connect and bind the globals as Chromium would: the compositor at 4, the
 * others at the lower of Chromium's newest and what is offered. The seat's
 * devices wait for the case that wants them (env_pointer, env_keyboard). */
static bool env_open(struct env *e, int flags)
{
    memset(e, 0, sizeof(*e));
    e->dpy = wl_display_connect(NULL);
    if (!e->dpy) {
        fail("can't connect: %s", strerror(errno));
        return false;
    }
    e->reg = wl_display_get_registry(e->dpy);
    wl_registry_add_listener(e->reg, &registry_listener, e);
    if (!roundtrip(e)) {
        fail("the registry roundtrip failed");
        return false;
    }
    const struct glob *g;
#define BIND(field, ver_field, iface_name, iface, want)                                        \
    do {                                                                                       \
        if ((g = env_glob(e, iface_name))) {                                                   \
            e->ver_field = g->version < (want) ? g->version : (want);                          \
            e->field = wl_registry_bind(e->reg, g->name, &iface, e->ver_field);                \
        }                                                                                      \
    } while (0)
    BIND(comp, comp_ver, "wl_compositor", wl_compositor_interface, 4u);
    BIND(shm, shm_ver, "wl_shm", wl_shm_interface, 2u);
    BIND(wm, wm_ver, "xdg_wm_base", xdg_wm_base_interface, 6u);
    if (!(flags & B_NOSEAT))
        BIND(seat, seat_ver, "wl_seat", wl_seat_interface, 9u);
    if (!(flags & B_NOOUT))
        BIND(out, out_ver, "wl_output", wl_output_interface, 4u);
    BIND(ddm, ddm_ver, "wl_data_device_manager", wl_data_device_manager_interface, 3u);
    BIND(csm, csm_ver, "wp_cursor_shape_manager_v1", wp_cursor_shape_manager_v1_interface, 2u);
    if (!(flags & B_NODECO))
        BIND(dm, dm_ver, "zxdg_decoration_manager_v1", zxdg_decoration_manager_v1_interface, 1u);
#undef BIND
    if (e->shm)
        wl_shm_add_listener(e->shm, &shm_listener, e);
    if (e->wm)
        xdg_wm_base_add_listener(e->wm, &wm_listener, e);
    if (e->out)
        wl_output_add_listener(e->out, &output_listener, e);
    if (e->seat)
        wl_seat_add_listener(e->seat, &seat_listener, e);
    if (!roundtrip(e)) {
        fail("the binds' roundtrip failed");
        return false;
    }
    return true;
}

static void env_close(struct env *e)
{
    if (e->dpy)
        wl_display_disconnect(e->dpy);
    if (e->xstate)
        xkb_state_unref(e->xstate);
    if (e->xmap)
        xkb_keymap_unref(e->xmap);
    if (e->xctx)
        xkb_context_unref(e->xctx);
    free(e->keymap_text);
    memset(e, 0, sizeof(*e));
}

/* "Chromium needs this" for a case: the globals a window takes. */
static bool env_need_window(const struct env *e)
{
    if (!e->comp || !e->shm || !e->wm) {
        fail("missing %s%s%s", e->comp ? "" : "wl_compositor ", e->shm ? "" : "wl_shm ",
             e->wm ? "" : "xdg_wm_base");
        return false;
    }
    return true;
}

static bool env_pointer(struct env *e)
{
    if (e->ptr)
        return true;
    if (!e->seat || !(e->seat_caps & WL_SEAT_CAPABILITY_POINTER)) {
        fail("the seat has no pointer (caps %u)", e->seat_caps);
        return false;
    }
    e->ptr = wl_seat_get_pointer(e->seat);
    wl_pointer_add_listener(e->ptr, &pointer_listener, e);
    return true;
}

static bool env_keyboard(struct env *e)
{
    if (e->kbd)
        return true;
    if (!e->seat || !(e->seat_caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        fail("the seat has no keyboard (caps %u)", e->seat_caps);
        return false;
    }
    e->kbd = wl_seat_get_keyboard(e->seat);
    wl_keyboard_add_listener(e->kbd, &keyboard_listener, e);
    if (!roundtrip(e))
        return false;
    if (e->got_keymap && e->keymap_text) {
        e->xctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
        if (e->xctx)
            e->xmap = xkb_keymap_new_from_string(e->xctx, e->keymap_text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                 XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (e->xmap) {
            e->xstate = xkb_state_new(e->xmap);
            xkb_mod_index_t sh = xkb_keymap_mod_get_index(e->xmap, XKB_MOD_NAME_SHIFT);
            e->shift_bit = sh == XKB_MOD_INVALID ? 1u : 1u << sh;
        }
    }
    return true;
}

/* ---- the cases ---------------------------------------------------------------------------------- */

/* globals: the registry lists what the plan promises, at the versions it
 * promises (Chromium binds each at the lower of its own newest and the offered). */
static void case_globals(void)
{
    struct env e;
    if (!env_open(&e, B_NOSEAT | B_NOOUT | B_NODECO))
        return;
    static const struct { const char *iface; uint32_t ver; } need[] = {
        { "wl_compositor", 6 }, { "wl_shm", 2 }, { "wl_output", 4 }, { "wl_seat", 9 },
        { "xdg_wm_base", 6 }, { "wl_data_device_manager", 3 }, { "wp_cursor_shape_manager_v1", 2 },
        { "zxdg_decoration_manager_v1", 1 },
    };
    for (int i = 0; i < e.nglobs; i++)
        say("global %s v%u", e.globs[i].iface, e.globs[i].version);
    for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        const struct glob *g = env_glob(&e, need[i].iface);
        if (!g)
            fail("%s is not offered", need[i].iface);
        else if (g->version < need[i].ver)
            fail("%s is offered at v%u, the plan says v%u", need[i].iface, g->version, need[i].ver);
    }
    seen("%d globals", e.nglobs);
    env_close(&e);
}

/* subcompositor: wl_subcompositor (version 1), which Chromium's bubbles and
 * its omnibox list are subsurfaces of (GC2). */
static void case_subcompositor(void)
{
    struct env e;
    if (!env_open(&e, B_NOSEAT | B_NOOUT | B_NODECO))
        return;
    const struct glob *g = env_glob(&e, "wl_subcompositor");
    CHECK(g, "wl_subcompositor is not offered (GC2)");
    if (g)
        seen("wl_subcompositor v%u", g->version);
    env_close(&e);
}

/* bind: every global bound at Chromium's version and at its newest, the
 * objects Chromium makes from them, a roundtrip, no protocol error. */
static void case_bind(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    /* the second set: each at the newest the compositor offers */
    const struct glob *g;
    struct wl_compositor *comp2 = NULL;
    struct wl_seat *seat2 = NULL;
    struct xdg_wm_base *wm2 = NULL;
    struct wl_output *out2 = NULL;
    if ((g = env_glob(&e, "wl_compositor")))
        comp2 = wl_registry_bind(e.reg, g->name, &wl_compositor_interface, g->version);
    if ((g = env_glob(&e, "wl_seat")))
        seat2 = wl_registry_bind(e.reg, g->name, &wl_seat_interface, g->version);
    if ((g = env_glob(&e, "xdg_wm_base")))
        wm2 = wl_registry_bind(e.reg, g->name, &xdg_wm_base_interface, g->version);
    if ((g = env_glob(&e, "wl_output")))
        out2 = wl_registry_bind(e.reg, g->name, &wl_output_interface, g->version);
    struct wl_surface *s = wl_compositor_create_surface(e.comp);
    struct wl_surface *s2 = comp2 ? wl_compositor_create_surface(comp2) : NULL;
    struct wl_region *r = wl_compositor_create_region(e.comp);
    wl_region_add(r, 0, 0, 10, 10);
    wl_surface_set_opaque_region(s, r);
    wl_surface_set_input_region(s, r);
    wl_region_destroy(r);
    if (!roundtrip(&e)) {
        fail("a protocol error after the binds: %s code %d", e.err_iface ? e.err_iface : "-",
             e.err_code);
    } else {
        struct wl_pointer *p = e.seat && (e.seat_caps & WL_SEAT_CAPABILITY_POINTER)
                                   ? wl_seat_get_pointer(e.seat) : NULL;
        struct wl_keyboard *k = e.seat && (e.seat_caps & WL_SEAT_CAPABILITY_KEYBOARD)
                                    ? wl_seat_get_keyboard(e.seat) : NULL;
        CHECK(p && k, "seat capabilities %u lack a pointer or a keyboard", e.seat_caps);
        if (e.ddm && e.seat) {
            struct wl_data_device *dd = wl_data_device_manager_get_data_device(e.ddm, e.seat);
            struct wl_data_source *ds = wl_data_device_manager_create_data_source(e.ddm);
            wl_data_source_offer(ds, "text/plain;charset=utf-8");
            wl_data_source_destroy(ds);
            wl_data_device_release(dd);
        }
        if (e.csm && p) {
            struct wp_cursor_shape_device_v1 *cd = wp_cursor_shape_manager_v1_get_pointer(e.csm, p);
            wp_cursor_shape_device_v1_destroy(cd);
        }
        if (!roundtrip(&e))
            fail("a protocol error after the objects: %s code %d", e.err_iface ? e.err_iface : "-",
                 e.err_code);
        if (p)
            wl_pointer_release(p);
        if (k)
            wl_keyboard_release(k);
    }
    seen("compositor v%u/%u, shm v%u, xdg_wm_base v%u/%u, seat v%u/%u, output v%u, data v%u, "
         "cursor_shape v%u",
         e.comp_ver, comp2 ? wl_proxy_get_version((struct wl_proxy *)comp2) : 0u, e.shm_ver,
         e.wm_ver, wm2 ? wl_proxy_get_version((struct wl_proxy *)wm2) : 0u, e.seat_ver,
         seat2 ? wl_proxy_get_version((struct wl_proxy *)seat2) : 0u, e.out_ver, e.ddm_ver,
         e.csm_ver);
    (void)s, (void)s2, (void)out2;
    env_close(&e);
}

/* shm: ARGB8888 and XRGB8888 announced (Chromium's software output wants
 * ARGB for a translucent window). */
static void case_shm(void)
{
    struct env e;
    if (!env_open(&e, B_NOSEAT | B_NOOUT | B_NODECO))
        return;
    REQUIRE(e.shm, "no wl_shm");
    CHECK(e.shm_argb, "wl_shm did not announce ARGB8888");
    CHECK(e.shm_xrgb, "wl_shm did not announce XRGB8888");
    seen("%u formats, argb %d xrgb %d", e.shm_formats, e.shm_argb, e.shm_xrgb);
    /* a pool that grows (wl_shm_pool.resize) and a buffer from its second page */
    int fd = memfd_create("wlprobe-pool", MFD_CLOEXEC);
    REQUIRE(fd >= 0 && ftruncate(fd, 4096) == 0, "memfd_create: %s", strerror(errno));
    struct wl_shm_pool *pool = wl_shm_create_pool(e.shm, fd, 4096);
    CHECK(ftruncate(fd, 16384) == 0, "ftruncate: %s", strerror(errno));
    wl_shm_pool_resize(pool, 16384);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 4096, 32, 32, 128, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_destroy(b);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!roundtrip(&e))
        fail("a protocol error after resizing a pool: %s code %d", e.err_iface ? e.err_iface : "-",
             e.err_code);
    env_close(&e);
}

/* output: geometry, a current mode, scale, name and description, then done. */
static void case_output(void)
{
    struct env e;
    if (!env_open(&e, B_NOSEAT | B_NODECO))
        return;
    REQUIRE(e.out, "no wl_output");
    CHECK(e.got_geom, "no wl_output.geometry");
    CHECK(e.got_mode, "no current wl_output.mode");
    CHECK(e.got_done, "no wl_output.done");
    if (e.got_mode)
        CHECK(e.out_w > 0 && e.out_h > 0, "mode %dx%d", e.out_w, e.out_h);
    if (e.out_ver >= 2) {
        CHECK(e.got_scale, "no wl_output.scale");
        CHECK(e.out_scale == 1, "scale %d, not 1", e.out_scale);
    }
    if (e.out_ver >= 4) {
        CHECK(e.got_name && e.out_name[0], "no wl_output.name");
        CHECK(e.got_desc && e.out_desc[0], "no wl_output.description");
    }
    seen("%dx%d @ %d mHz, scale %d, name %s", e.out_w, e.out_h, e.out_refresh, e.out_scale,
         e.out_name);
    env_close(&e);
}

/* keymap: the seat's capabilities, the keyboard's XKB keymap in a file, compiled
 * by xkbcommon (the library Chromium links), and repeat_info. */
static void case_keymap(void)
{
    struct env e;
    if (!env_open(&e, B_NODECO | B_NOOUT))
        return;
    REQUIRE(e.seat, "no wl_seat");
    CHECK(e.seat_caps & WL_SEAT_CAPABILITY_POINTER, "no pointer capability");
    CHECK(e.seat_caps & WL_SEAT_CAPABILITY_KEYBOARD, "no keyboard capability");
    if (e.seat_ver >= 2)
        CHECK(e.seat_name[0], "no wl_seat.name");
    if (!env_keyboard(&e)) {
        env_close(&e);
        return;
    }
    CHECK(e.got_keymap, "no wl_keyboard.keymap");
    CHECK(e.keymap_format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, "keymap format %u, not xkb_v1",
          e.keymap_format);
    CHECK(e.keymap_text != NULL, "the keymap's file could not be mapped (size %u)", e.keymap_size);
    if (e.keymap_text) {
        CHECK(e.xmap != NULL, "xkbcommon refused the keymap (%u bytes)", e.keymap_size);
        if (e.xmap) {
            xkb_keycode_t ac01 = xkb_keymap_key_by_name(e.xmap, "AC01");
            CHECK(ac01 == KEY_A + 8, "key AC01 is keycode %u, not %u", (unsigned)ac01, KEY_A + 8u);
            xkb_state_update_mask(e.xstate, 0, 0, 0, 0, 0, 0);
            CHECK(xkb_state_key_get_one_sym(e.xstate, ac01) == XKB_KEY_a, "AC01 is not 'a'");
            xkb_mod_index_t shift = xkb_keymap_mod_get_index(e.xmap, XKB_MOD_NAME_SHIFT);
            CHECK(shift != XKB_MOD_INVALID, "the keymap has no Shift");
            if (shift != XKB_MOD_INVALID) {
                xkb_state_update_mask(e.xstate, 1u << shift, 0, 0, 0, 0, 0);
                CHECK(xkb_state_key_get_one_sym(e.xstate, ac01) == XKB_KEY_A,
                      "Shift+AC01 is not 'A'");
            }
        }
    }
    CHECK(e.got_repeat, "no wl_keyboard.repeat_info");
    CHECK(!e.got_repeat || (e.repeat_rate > 0 && e.repeat_delay >= 0), "repeat_info %d/%d",
          e.repeat_rate, e.repeat_delay);
    seen("seat %s, keymap %u bytes, repeat %d/s after %d ms", e.seat_name, e.keymap_size,
         e.repeat_rate, e.repeat_delay);
    env_close(&e);
}

/* toplevel: an xdg toplevel from the first commit to the first frame, as Chromium
 * makes one. expect: 0 nothing about layout, 1 tiled states, 2 none. */
static void toplevel_check(int expect)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe toplevel")) {
        env_close(&e);
        return;
    }
    CHECK(w.ncfg >= 1, "no configure");
    const struct cfg *c0 = &w.cfgs[0];
    seen("first configure %dx%d states %#x", c0->w, c0->h, c0->states);
    CHECK(c0->w >= 0 && c0->h >= 0, "configure %dx%d", c0->w, c0->h);
    CHECK(w.frames >= 1, "no frame callback");
    if (e.wm_ver >= 5) {
        CHECK(w.got_caps, "no wm_capabilities");
        if (w.got_caps) {
            CHECK(w.caps & ST(XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE), "wm_capabilities: no maximize");
            CHECK(w.caps & ST(XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN), "wm_capabilities: no fullscreen");
            CHECK(w.caps & ST(XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE), "wm_capabilities: no minimize");
            seen("capabilities %#x", w.caps);
        }
    }
    if (e.wm_ver >= 4) {
        CHECK(w.got_bounds, "no configure_bounds");
        if (w.got_bounds) {
            CHECK(w.bound_w > 0 && w.bound_h > 0, "configure_bounds %dx%d", w.bound_w, w.bound_h);
            seen("bounds %dx%d", w.bound_w, w.bound_h);
        }
    }
    /* the window is the only one that was just mapped: activated, within a moment */
    bool act = WAIT(&e, win_state_since(&w, 0, XDG_TOPLEVEL_STATE_ACTIVATED), 3000);
    CHECK(act, "never activated (states of the %d configures: first %#x)", w.ncfg, c0->states);
    uint32_t all = 0;
    for (int i = 0; i < w.ncfg; i++)
        all |= w.cfgs[i].states;
    uint32_t tiled = ST(XDG_TOPLEVEL_STATE_TILED_LEFT) | ST(XDG_TOPLEVEL_STATE_TILED_RIGHT) |
                     ST(XDG_TOPLEVEL_STATE_TILED_TOP) | ST(XDG_TOPLEVEL_STATE_TILED_BOTTOM);
    if (expect == 1)
        CHECK((all & tiled) == tiled, "tiling layout: states %#x lack the four tiled sides", all);
    if (expect == 2)
        CHECK((all & tiled) == 0, "floating layout: states %#x have tiled sides", all);
    CHECK(!(all & (ST(XDG_TOPLEVEL_STATE_MAXIMIZED) | ST(XDG_TOPLEVEL_STATE_FULLSCREEN))),
          "a new window is maximised or full screen (%#x)", all);
    win_follow(&w);
    seen("%d configures, all states %#x, %d frame(s)", w.ncfg, all, (int)w.frames);
    win_destroy(&w);
    roundtrip(&e);
    env_close(&e);
}

static void case_toplevel(void) { toplevel_check(0); }

/* keyless: a window of a client that made no wl_keyboard is not activated when
 * it maps (the compositor's rule since 50088991: the keys stay where they were;
 * a click makes it the active window). */
static void case_keyless(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_KEYLESS, "wlprobe keyless")) {
        env_close(&e);
        return;
    }
    hold_for(&e, 1.5);
    CHECK(!e.kbd, "the probe made a keyboard");
    CHECK(!win_state_since(&w, 0, XDG_TOPLEVEL_STATE_ACTIVATED),
          "a keyless window was activated at map");
    seen("%d configure(s), none activated", w.ncfg);
    env_close(&e);
}
static void case_states_tiled(void) { toplevel_check(1); }
static void case_states_floating(void) { toplevel_check(2); }

/* deco-server: a client that bound the decoration manager is answered
 * server_side, whatever it asks (the owner's rule, docs/G2-PLAN.md section 3). */
static void case_deco_server(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    REQUIRE(e.dm, "no zxdg_decoration_manager_v1");
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    /* 1: a decoration object that asks nothing */
    struct win a;
    if (win_open(&e, &a, WF_DECO, "wlprobe deco-default")) {
        WAIT(&e, a.nmodes > 0, 2000);
        CHECK(a.nmodes > 0, "no decoration.configure for a toplevel with a decoration object");
        if (a.nmodes)
            CHECK(a.modes[a.nmodes - 1] == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
                  "no request: mode %d, not server_side", a.modes[a.nmodes - 1]);
        seen("default -> %d", a.nmodes ? a.modes[a.nmodes - 1] : 0);
    }
    /* 2: one that asks client_side before the first commit */
    struct win b;
    if (win_open(&e, &b, WF_DECO | WF_DECO_CLIENT, "wlprobe deco-client")) {
        WAIT(&e, b.nmodes > 0, 2000);
        CHECK(b.nmodes > 0, "no decoration.configure after set_mode(client_side)");
        for (int i = 0; i < b.nmodes; i++)
            CHECK(b.modes[i] != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE,
                  "asked client_side, was told client_side");
        if (b.nmodes)
            CHECK(b.modes[b.nmodes - 1] == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
                  "asked client_side: mode %d, not server_side", b.modes[b.nmodes - 1]);
        /* and again once mapped */
        zxdg_toplevel_decoration_v1_set_mode(b.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
        hold_for(&e, 1.0);
        for (int i = 0; i < b.nmodes; i++)
            CHECK(b.modes[i] != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE,
                  "asked client_side when mapped, was told client_side");
        win_follow(&b);
        seen("client_side -> %d (%d event(s))", b.nmodes ? b.modes[b.nmodes - 1] : 0, b.nmodes);
    }
    /* 3: unset_mode (back to the compositor's choice) */
    zxdg_toplevel_decoration_v1_unset_mode(b.deco);
    hold_for(&e, 0.5);
    for (int i = 0; i < b.nmodes; i++)
        CHECK(b.modes[i] != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE, "unset_mode: client_side");
    CHECK(!e.dead, "protocol error: %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    env_close(&e);
}

/* title: set_title / set_app_id, changed while mapped, with UTF-8 and a long
 * one; the script's screenshots show the title bar change (a floating window
 * has one). */
static void case_title(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe title A")) {
        env_close(&e);
        return;
    }
    xdg_toplevel_set_title(w.top, "wlprobe title A");
    xdg_toplevel_set_app_id(w.top, "wlprobe.title.a");
    win_paint(&w);
    shot(&e, "title-a");
    xdg_toplevel_set_title(w.top, "B: caf\xc3\xa9 \xe2\x9c\x93 WWWW MMMM");
    xdg_toplevel_set_app_id(w.top, "wlprobe.title.b");
    win_paint(&w);
    shot(&e, "title-b");
    char big[1000], app[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    memset(app, 'a', sizeof(app) - 1);
    app[sizeof(app) - 1] = 0;
    xdg_toplevel_set_title(w.top, big);
    xdg_toplevel_set_app_id(w.top, app);
    win_paint(&w);
    xdg_toplevel_set_title(w.top, "");
    win_paint(&w);
    CHECK(roundtrip(&e), "a protocol error after long and empty titles: %s code %d",
          e.err_iface ? e.err_iface : "-", e.err_code);
    seen("titles set, changed, 999 bytes, empty");
    env_close(&e);
}

/* The test pictures for shm-argb. */
static void fill_argb(uint32_t *px, int w, int h, void *user)
{
    (void)user;
    rect(px, w, h, 0, 0, w, h, 0xff202020u);
    rect(px, w, h, 0, 0, 16, 16, MARK);
    rect(px, w, h, 32, 32, 64, 64, 0xff3366ccu);     /* opaque */
    rect(px, w, h, 112, 32, 64, 64, 0x80800000u);    /* 50% red, premultiplied */
    rect(px, w, h, 192, 32, 64, 64, 0x00000000u);    /* transparent */
    rect(px, w, h, 32, 112, 64, 64, 0x40400000u);    /* 25% red, premultiplied */
    rect(px, w, h, 112, 112, 64, 64, 0xc0606060u);   /* 75% grey, premultiplied */
}

static void case_shm_argb(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    memset(&w, 0, sizeof(w));
    if (!win_open(&e, &w, WF_DECO | WF_NOMAP, "wlprobe shm-argb")) {
        env_close(&e);
        return;
    }
    w.fill = fill_argb;
    REQUIRE(WAIT(&e, w.acked > 0, 5000), "no configure");
    win_paint(&w);
    REQUIRE(WAIT(&e, w.frames > 0, 5000), "no frame callback");
    say("blocks (64x64) at 32,32 opaque 3366cc; 112,32 50%% red; 192,32 transparent; 32,112 25%% red; 112,112 75%% grey");
    shot(&e, "shm-argb");
    seen("%dx%d buffer drawn", w.bw, w.bh);
    env_close(&e);
}

/* damage: wl_surface.damage_buffer and wl_surface.damage on parts of a buffer
 * whose other parts are unchanged: the screen shows the new parts and the old. */
struct dmg {
    int stage;
};

static void fill_dmg(uint32_t *px, int w, int h, void *user)
{
    struct dmg *d = user;
    fill_default(px, w, h, NULL);
    rect(px, w, h, 0, 0, w, h, 0xff204060u);
    rect(px, w, h, 0, 0, 16, 16, MARK);
    if (d->stage >= 1) {
        rect(px, w, h, 40, 40, 80, 60, 0xffe0a030u);
        rect(px, w, h, 200, 120, 50, 50, 0xff30a0e0u);
    }
    if (d->stage >= 2)
        rect(px, w, h, 320, 200, 60, 60, 0xffa0e060u);
}

static void case_damage(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    REQUIRE(e.comp_ver >= 4, "wl_compositor v%u has no damage_buffer", e.comp_ver);
    struct win w;
    struct dmg d = { 0 };
    memset(&w, 0, sizeof(w));
    if (!win_open(&e, &w, WF_DECO | WF_NOMAP, "wlprobe damage")) {
        env_close(&e);
        return;
    }
    w.fill = fill_dmg;
    w.fill_user = &d;
    REQUIRE(WAIT(&e, w.acked > 0, 5000), "no configure");
    win_paint_sized(&w, DEF_W, DEF_H);   /* a fixed size: the rectangles below fit it */
    REQUIRE(WAIT(&e, w.frames > 0, 5000), "no frame callback");
    say("stage 0: all 204060");
    shot(&e, "damage-0");
    unsigned f = w.frames;
    d.stage = 1;
    struct wl_buffer *b = buf_make(&e, DEF_W, DEF_H, WL_SHM_FORMAT_ARGB8888, fill_dmg, &d);
    REQUIRE(b, "no buffer");
    wl_surface_attach(w.surf, b, 0, 0);
    wl_surface_damage_buffer(w.surf, 40, 40, 80, 60);
    wl_surface_damage_buffer(w.surf, 200, 120, 50, 50);
    struct wl_callback *cb = wl_surface_frame(w.surf);
    wl_callback_add_listener(cb, &frame_listener, &w);
    wl_surface_commit(w.surf);
    CHECK(WAIT(&e, w.frames > f, 3000), "no frame callback after a damage_buffer commit");
    say("stage 1: 40,40 80x60 e0a030 and 200,120 50x50 30a0e0 (damage_buffer)");
    shot(&e, "damage-1");
    f = w.frames;
    d.stage = 2;
    b = buf_make(&e, DEF_W, DEF_H, WL_SHM_FORMAT_ARGB8888, fill_dmg, &d);
    REQUIRE(b, "no buffer");
    wl_surface_attach(w.surf, b, 0, 0);
    wl_surface_damage(w.surf, 320, 200, 60, 60);
    cb = wl_surface_frame(w.surf);
    wl_callback_add_listener(cb, &frame_listener, &w);
    wl_surface_commit(w.surf);
    CHECK(WAIT(&e, w.frames > f, 3000), "no frame callback after a damage commit");
    say("stage 2: 320,200 60x60 a0e060 (damage)");
    shot(&e, "damage-2");
    seen("3 stages");
    env_close(&e);
}

/* frames: frame callbacks keep coming while a new buffer is committed after
 * each, in order, and the old buffers are released. */
static void fill_moving(uint32_t *px, int w, int h, void *user)
{
    int n = *(int *)user;
    rect(px, w, h, 0, 0, w, h, 0xff102030u);
    rect(px, w, h, (n * 7) % (w - 40), (n * 5) % (h - 40), 40, 40, 0xffe08030u);
}

static void case_frames(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    int n = 0;
    memset(&w, 0, sizeof(w));
    if (!win_open(&e, &w, WF_DECO, "wlprobe frames")) {
        env_close(&e);
        return;
    }
    w.fill = fill_moving;
    w.fill_user = &n;
    double t0 = now_s();
    uint32_t last = w.frame_time, prev_ms = 0;
    unsigned done = 0;
    bool monotonic = true;
    for (n = 1; n <= 60; n++) {
        unsigned before = w.frames;
        if (!win_paint(&w)) {
            fail("no buffer at frame %d", n);
            break;
        }
        if (!WAIT(&e, w.frames > before, 2000)) {
            fail("frame %d: no callback within 2 s", n);
            break;
        }
        done++;
        if (w.frame_time < prev_ms)
            monotonic = false;
        prev_ms = w.frame_time;
    }
    (void)last;
    double dt = now_s() - t0;
    CHECK(monotonic, "frame callback times went backwards");
    hold_for(&e, 0.3);
    CHECK(e.releases + 4 >= done, "%u buffers committed, only %u released", done, e.releases);
    seen("%u frames in %.2f s (%.1f/s), %u buffers released", done, dt, dt > 0 ? done / dt : 0,
         e.releases);
    env_close(&e);
}

/* geometry: set_window_geometry (valid), then the errors the protocol names: a
 * geometry of no size, and a maximum under the minimum. Each error ends its
 * connection, so each has one of its own. */
static void case_geometry(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe geometry")) {
        env_close(&e);
        return;
    }
    int cw = w.bw, ch = w.bh;
    xdg_surface_set_window_geometry(w.xs, 8, 8, cw - 16, ch - 16);
    xdg_toplevel_set_min_size(w.top, 100, 80);
    xdg_toplevel_set_max_size(w.top, 4000, 3000);
    win_paint_sized(&w, cw, ch);
    CHECK(roundtrip(&e), "a valid geometry and limits: protocol error %s code %d",
          e.err_iface ? e.err_iface : "-", e.err_code);
    xdg_surface_set_window_geometry(w.xs, 0, 0, cw, ch);
    xdg_toplevel_set_min_size(w.top, 0, 0);
    xdg_toplevel_set_max_size(w.top, 0, 0);
    win_paint_sized(&w, cw, ch);
    CHECK(roundtrip(&e), "geometry reset: protocol error");
    seen("geometry and limits accepted");
    env_close(&e);
}

static void expect_error(const char *what, void (*bad)(struct win *), const char *iface, int code)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe error")) {
        env_close(&e);
        return;
    }
    bad(&w);
    wl_display_flush(e.dpy);
    WAIT(&e, e.dead, 3000);
    if (!e.dead)
        roundtrip(&e);
    CHECK(e.dead, "%s: no protocol error", what);
    if (e.dead) {
        CHECK(e.err_code == code && e.err_iface && !strcmp(e.err_iface, iface),
              "%s: error %s code %d, wanted %s code %d", what, e.err_iface ? e.err_iface : "?",
              e.err_code, iface, code);
        seen("%s -> %s code %d", what, e.err_iface ? e.err_iface : "?", e.err_code);
    }
    env_close(&e);
}

static void bad_geometry(struct win *w) { xdg_surface_set_window_geometry(w->xs, 0, 0, 0, 0); }
static void bad_limits(struct win *w)
{
    xdg_toplevel_set_min_size(w->top, 300, 300);
    xdg_toplevel_set_max_size(w->top, 100, 100);
    wl_surface_commit(w->surf);
}
static void bad_serial(struct win *w) { xdg_surface_ack_configure(w->xs, 0xdeadbeef); }

static void case_geometry_error(void)
{
    expect_error("set_window_geometry 0x0", bad_geometry, "xdg_surface",
                 XDG_SURFACE_ERROR_INVALID_SIZE);
}

static void case_limits_error(void)
{
    expect_error("max under min", bad_limits, "xdg_toplevel", XDG_TOPLEVEL_ERROR_INVALID_SIZE);
}

static void case_serial_error(void)
{
    expect_error("ack of a serial never sent", bad_serial, "xdg_surface",
                 XDG_SURFACE_ERROR_INVALID_SERIAL);
}

/* The window-state requests: the compositor's configure answers them. */
static void state_request(const char *what, int state)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe states")) {
        env_close(&e);
        return;
    }
    WAIT(&e, win_state_since(&w, 0, XDG_TOPLEVEL_STATE_ACTIVATED), 2000);
    const struct cfg *c = win_cfg(&w);
    int w0 = c->w, h0 = c->h, at = w.ncfg;
    switch (state) {
    case XDG_TOPLEVEL_STATE_MAXIMIZED:
        xdg_toplevel_set_maximized(w.top);
        break;
    case XDG_TOPLEVEL_STATE_FULLSCREEN:
        xdg_toplevel_set_fullscreen(w.top, NULL);
        break;
    default:
        xdg_toplevel_set_minimized(w.top);
        break;
    }
    bool got = WAIT(&e, win_state_since(&w, at, state), 3000);
    CHECK(got, "%s: no configure with state %d within 3 s (%d configures since)", what, state,
          w.ncfg - at);
    if (got) {
        win_follow(&w);
        c = win_cfg(&w);
        int i = at;
        while (i < w.ncfg && !(w.cfgs[i].states & ST(state)))
            i++;
        if (state == XDG_TOPLEVEL_STATE_FULLSCREEN && e.got_mode)
            CHECK(w.cfgs[i].w == e.out_w && w.cfgs[i].h == e.out_h,
                  "full screen configure %dx%d, the output is %dx%d", w.cfgs[i].w, w.cfgs[i].h,
                  e.out_w, e.out_h);
        if (state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            CHECK(w.cfgs[i].w > 0 && w.cfgs[i].h > 0, "maximized configure %dx%d", w.cfgs[i].w,
                  w.cfgs[i].h);
        seen("%s -> %dx%d states %#x (from %dx%d)", what, w.cfgs[i].w, w.cfgs[i].h,
             w.cfgs[i].states, w0, h0);
        if (state != XDG_TOPLEVEL_STATE_SUSPENDED) {
            /* and back */
            at = w.ncfg;
            if (state == XDG_TOPLEVEL_STATE_MAXIMIZED)
                xdg_toplevel_unset_maximized(w.top);
            else
                xdg_toplevel_unset_fullscreen(w.top);
            bool back = WAIT(&e, w.ncfg > at && !(win_cfg(&w)->states & ST(state)), 3000);
            CHECK(back, "%s: the state stayed after the unset request", what);
            win_follow(&w);
            if (back && w0 > 0 && h0 > 0)
                CHECK(win_cfg(&w)->w == w0 && win_cfg(&w)->h == h0,
                      "%s: back to %dx%d, was %dx%d", what, win_cfg(&w)->w, win_cfg(&w)->h, w0, h0);
        }
    }
    CHECK(!e.dead, "protocol error: %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    env_close(&e);
}

static void case_maximize(void) { state_request("set_maximized", XDG_TOPLEVEL_STATE_MAXIMIZED); }
static void case_fullscreen(void) { state_request("set_fullscreen", XDG_TOPLEVEL_STATE_FULLSCREEN); }
static void case_minimize(void) { state_request("set_minimized", XDG_TOPLEVEL_STATE_SUSPENDED); }

/* parent: set_parent (Chromium's dialogs and menus name their parent window),
 * and a window that is its own parent is the invalid_parent error. */
static void case_parent(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win a, b;
    if (win_open(&e, &a, WF_DECO, "wlprobe parent") && win_open(&e, &b, WF_DECO, "wlprobe child")) {
        xdg_toplevel_set_parent(b.top, a.top);
        win_paint(&b);
        xdg_toplevel_set_parent(b.top, NULL);
        xdg_toplevel_set_parent(b.top, a.top);
        CHECK(roundtrip(&e), "set_parent: protocol error %s code %d", e.err_iface ? e.err_iface : "-",
              e.err_code);
        seen("set_parent, unset, set again");
        xdg_toplevel_set_parent(b.top, b.top);
        wl_display_flush(e.dpy);
        WAIT(&e, e.dead, 2000);
        if (!e.dead)
            roundtrip(&e);
        CHECK(e.dead, "a window as its own parent: no protocol error");
        if (e.dead)
            CHECK(e.err_code == XDG_TOPLEVEL_ERROR_INVALID_PARENT && e.err_iface &&
                      !strcmp(e.err_iface, "xdg_toplevel"),
                  "own parent: error %s code %d, wanted invalid_parent",
                  e.err_iface ? e.err_iface : "?", e.err_code);
    }
    env_close(&e);
}

/* ---- popups (Chromium's menus, tooltips and <select> lists) -------------------------------------- */

/* A popup and what the compositor told it. */
struct pup {
    struct env *e;
    struct wl_surface *surf;
    struct xdg_surface *xs;
    struct xdg_popup *p;
    bool configured, done;
    int x, y, w, h;           /* the newest xdg_popup.configure */
    unsigned confs, acked;    /* xdg_popup.configure and xdg_surface.configure events */
    bool repositioned;
    uint32_t token;
};

/* What a positioner says. */
struct pspec {
    int w, h, ax, ay, aw, ah;
    uint32_t anchor, gravity, adjust;
};

static void on_pup_configure(void *data, struct xdg_popup *p, int32_t x, int32_t y, int32_t w,
                             int32_t h)
{
    struct pup *pu = data;
    (void)p;
    pu->configured = true;
    pu->confs++;
    pu->x = x, pu->y = y, pu->w = w, pu->h = h;
}

static void on_pup_done(void *data, struct xdg_popup *p)
{
    struct pup *pu = data;
    (void)p;
    pu->done = true;
}

static void on_pup_repositioned(void *data, struct xdg_popup *p, uint32_t token)
{
    struct pup *pu = data;
    (void)p;
    pu->repositioned = true;
    pu->token = token;
}

static const struct xdg_popup_listener pup_listener = {
    on_pup_configure, on_pup_done, on_pup_repositioned,
};

static void on_pup_xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    struct pup *pu = data;
    xdg_surface_ack_configure(xs, serial);
    pu->acked++;
}

static const struct xdg_surface_listener pup_xs_listener = { on_pup_xs_configure };

static struct xdg_positioner *pos_make(struct env *e, const struct pspec *s)
{
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(e->wm);
    xdg_positioner_set_size(pos, s->w, s->h);
    xdg_positioner_set_anchor_rect(pos, s->ax, s->ay, s->aw, s->ah);
    xdg_positioner_set_anchor(pos, s->anchor);
    xdg_positioner_set_gravity(pos, s->gravity);
    xdg_positioner_set_constraint_adjustment(pos, s->adjust);
    return pos;
}

/* A popup on parent placed by s, grabbing with serial (0: no grab), up to its
 * initial commit: true once its configure (or popup_done) came, within 3 s. */
static bool pup_open(struct env *e, struct pup *pu, struct xdg_surface *parent,
                     const struct pspec *s, uint32_t grab)
{
    memset(pu, 0, sizeof(*pu));
    pu->e = e;
    struct xdg_positioner *pos = pos_make(e, s);
    pu->surf = wl_compositor_create_surface(e->comp);
    pu->xs = xdg_wm_base_get_xdg_surface(e->wm, pu->surf);
    xdg_surface_add_listener(pu->xs, &pup_xs_listener, pu);
    pu->p = xdg_surface_get_popup(pu->xs, parent, pos);
    xdg_popup_add_listener(pu->p, &pup_listener, pu);
    if (grab)
        xdg_popup_grab(pu->p, e->seat, grab);
    wl_surface_commit(pu->surf);
    xdg_positioner_destroy(pos);
    return WAIT(e, pu->acked > 0 || pu->done, 3000);
}

static void fill_colour(uint32_t *px, int w, int h, void *user)
{
    rect(px, w, h, 0, 0, w, h, *(const uint32_t *)user);
}

/* Drawn in colour at the size of its newest configure. */
static void pup_draw(struct pup *pu, uint32_t colour)
{
    struct wl_buffer *b = buf_make(pu->e, pu->w, pu->h, WL_SHM_FORMAT_ARGB8888, fill_colour,
                                   &colour);
    if (!b)
        return;
    wl_surface_attach(pu->surf, b, 0, 0);
    wl_surface_damage_buffer(pu->surf, 0, 0, pu->w, pu->h);
    wl_surface_commit(pu->surf);
}

static void pup_close(struct pup *pu)
{
    if (!pu->surf)
        return;
    xdg_popup_destroy(pu->p);
    xdg_surface_destroy(pu->xs);
    wl_surface_destroy(pu->surf);
    pu->surf = NULL;
}

/* The usual start of a popup case: a decorated window with the keys. */
static bool pup_window(struct env *e, struct win *w, const char *title)
{
    if (!env_open(e, 0))
        return false;
    if (!env_need_window(e) || !env_keyboard(e) || !win_open(e, w, WF_DECO, title)) {
        env_close(e);
        return false;
    }
    if (!WAIT(e, e->kbd_enter_serial != 0, 3000)) {
        fail("the new window got no wl_keyboard.enter");
        env_close(e);
        return false;
    }
    return true;
}

#define PA(x) XDG_POSITIONER_ANCHOR_##x
#define PG(x) XDG_POSITIONER_GRAVITY_##x

/* REQUIRE that closes the case's connection (its window goes, so the next
 * case's window takes the keys): to the case's `out` label. */
#define NEED(cond, ...) do { if (!(cond)) { fail(__VA_ARGS__); goto out; } } while (0)

/* The serial of the newest button press the pointer got, 0: none. */
static uint32_t pressed_serial(const struct env *e)
{
    for (int i = e->npe - 1; i >= 0; i--)
        if (e->pe[i].t == PE_BUTTON && e->pe[i].b == WL_POINTER_BUTTON_STATE_PRESSED)
            return e->pe[i].serial;
    return 0;
}

/* popup: an xdg_popup with a grab (a menu) is configured at the place its
 * positioner says, stays up (popup_done must not come while nothing was
 * clicked), and has the keys while it is up (wl_keyboard.enter on it). */
static void case_popup(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup"))
        return;
    struct pup pu;
    const struct pspec s = { 120, 80, 20, 20, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    bool conf = pup_open(&e, &pu, w.xs, &s, e.kbd_enter_serial);
    CHECK(conf, "no configure for the popup within 3 s");
    if (conf && !pu.done)
        pup_draw(&pu, 0xffe0c030u);
    say("popup configure %d,%d %dx%d%s", pu.x, pu.y, pu.w, pu.h, pu.done ? " then popup_done" : "");
    hold_for(&e, 1.5);
    CHECK(!pu.done, "the popup was dismissed (popup_done) with nothing clicked");
    CHECK(pu.configured, "no xdg_popup.configure");
    if (pu.configured)
        CHECK(pu.x == 30 && pu.y == 30 && pu.w == 120 && pu.h == 80,
              "popup placed at %d,%d %dx%d, wanted 30,30 120x80", pu.x, pu.y, pu.w, pu.h);
    CHECK(e.kbd_on == pu.surf, "the grabbing popup did not get the keys (wl_keyboard.enter)");
    if (!pu.done)
        seen("popup at %d,%d %dx%d stayed up with the keys", pu.x, pu.y, pu.w, pu.h);
    pup_close(&pu);
    CHECK(WAIT(&e, e.kbd_on == w.surf, 2000), "the keys did not come back to the window");
    env_close(&e);
}

/* popup-nested: a menu and its submenu, both grabbing: the keys go to the
 * topmost, and back down the chain as each is destroyed. */
static void case_popup_nested(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup-nested"))
        return;
    struct pup m, sub;
    const struct pspec ms = { 120, 80, 20, 20, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    const struct pspec ss = { 100, 60, 110, 10, 10, 10, PA(TOP_RIGHT), PG(BOTTOM_RIGHT), 0 };
    NEED(pup_open(&e, &m, w.xs, &ms, e.kbd_enter_serial) && !m.done, "the menu was not configured");
    pup_draw(&m, 0xffe0c030u);
    NEED(WAIT(&e, e.kbd_on == m.surf, 2000), "the menu did not get the keys");
    NEED(pup_open(&e, &sub, m.xs, &ss, e.kbd_enter_serial) && !sub.done,
            "the submenu was not configured");
    CHECK(sub.x == 120 && sub.y == 10, "submenu placed at %d,%d, wanted 120,10", sub.x, sub.y);
    pup_draw(&sub, 0xff30a0e0u);
    CHECK(WAIT(&e, e.kbd_on == sub.surf, 2000), "the submenu did not get the keys");
    hold_for(&e, 0.5);
    CHECK(!m.done && !sub.done, "dismissed with nothing clicked");
    pup_close(&sub);
    CHECK(WAIT(&e, e.kbd_on == m.surf, 2000), "the keys did not go back to the menu");
    pup_close(&m);
    CHECK(WAIT(&e, e.kbd_on == w.surf, 2000), "the keys did not go back to the window");
    CHECK(roundtrip(&e), "protocol error %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    seen("menu, submenu, the keys up the chain and back");
out:
    env_close(&e);
}

/* popup-reposition: xdg_popup.reposition (version 3): repositioned with the
 * token, then a configure at the new place. */
static void case_popup_reposition(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup-reposition"))
        return;
    NEED(e.wm_ver >= 3, "xdg_wm_base bound at %u: no reposition", e.wm_ver);
    struct pup pu;
    const struct pspec a = { 80, 40, 10, 10, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    const struct pspec b = { 90, 50, 50, 60, 10, 10, PA(TOP_LEFT), PG(BOTTOM_RIGHT), 0 };
    NEED(pup_open(&e, &pu, w.xs, &a, 0) && !pu.done, "the popup was not configured");
    pup_draw(&pu, 0xffe0c030u);
    struct xdg_positioner *pos = pos_make(&e, &b);
    xdg_popup_reposition(pu.p, pos, 42);
    xdg_positioner_destroy(pos);
    bool moved = WAIT(&e, pu.confs >= 2, 3000);
    CHECK(moved, "no configure after reposition");
    CHECK(pu.repositioned && pu.token == 42, "repositioned %s (token %u)",
          pu.repositioned ? "came" : "did not come", pu.token);
    if (moved)
        CHECK(pu.x == 50 && pu.y == 60 && pu.w == 90 && pu.h == 50,
              "placed again at %d,%d %dx%d, wanted 50,60 90x50", pu.x, pu.y, pu.w, pu.h);
    pup_draw(&pu, 0xffe0c030u);
    CHECK(roundtrip(&e) && !pu.done, "protocol error or dismissed");
    if (moved)
        seen("repositioned 42, then %d,%d %dx%d", pu.x, pu.y, pu.w, pu.h);
out:
    env_close(&e);
}

/* popup-flip: a popup taller than the room below its anchor at the window's
 * bottom, with flip_y: placed above the anchor instead. */
static void case_popup_flip(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup-flip"))
        return;
    int h = w.bh - 20;
    const struct pspec s = { 100, h, 10, w.bh - 10, 10, 10, PA(BOTTOM), PG(BOTTOM),
                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y };
    struct pup pu;
    NEED(pup_open(&e, &pu, w.xs, &s, 0) && !pu.done, "the popup was not configured");
    CHECK(pu.y == 10 && pu.h == h && pu.x == -35,
          "a %dx%d popup at the %dx%d window's bottom placed at %d,%d %dx%d, wanted -35,10 (flipped up)",
          100, h, w.bw, w.bh, pu.x, pu.y, pu.w, pu.h);
    pup_draw(&pu, 0xffe0c030u);
    CHECK(roundtrip(&e) && !pu.done, "protocol error or dismissed");
    seen("flipped above its anchor: %d,%d %dx%d", pu.x, pu.y, pu.w, pu.h);
out:
    env_close(&e);
}

/* popup-tooltip: a popup with no grab (a tooltip) takes no keys and stays up. */
static void case_popup_tooltip(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup-tooltip"))
        return;
    const struct pspec s = { 100, 24, 40, 40, 1, 1, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    struct pup pu;
    NEED(pup_open(&e, &pu, w.xs, &s, 0) && !pu.done, "the tooltip was not configured");
    pup_draw(&pu, 0xfff0f0c0u);
    hold_for(&e, 1.0);
    CHECK(!pu.done, "the tooltip was dismissed");
    CHECK(e.kbd_on == w.surf, "the keys left the window for the tooltip");
    seen("at %d,%d %dx%d, up, the keys still the window's", pu.x, pu.y, pu.w, pu.h);
out:
    env_close(&e);
}

/* popup-parent: the parent window unmapped (a null buffer): its popup is
 * dismissed. */
static void case_popup_parent(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup-parent"))
        return;
    const struct pspec s = { 80, 40, 10, 10, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    struct pup pu;
    NEED(pup_open(&e, &pu, w.xs, &s, 0) && !pu.done, "the popup was not configured");
    pup_draw(&pu, 0xffe0c030u);
    hold_for(&e, 0.3);
    wl_surface_attach(w.surf, NULL, 0, 0);
    wl_surface_commit(w.surf);
    CHECK(WAIT(&e, pu.done, 3000), "no popup_done when its parent was unmapped");
    if (pu.done)
        seen("popup_done when its parent went");
out:
    env_close(&e);
}

/* popup-click: a click on the window opens a menu that grabs with the click's
 * serial; a click outside the menu's client (the script's, on the terminal)
 * dismisses it, and the keys come back to the window. */
static void case_popup_click(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe popup-click"))
        return;
    NEED(env_pointer(&e), "no pointer");
    say("ready");
    uint32_t serial = 0;
    bool pressed = WAIT(&e, (serial = pressed_serial(&e)) != 0, g_wait * 1000);
    NEED(pressed, "no button press on the window within %d s", g_wait);
    const struct pspec s = { 160, 100, 20, 20, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    struct pup m;
    NEED(pup_open(&e, &m, w.xs, &s, serial) && !m.done, "the menu was not configured");
    pup_draw(&m, 0xffe0c030u);
    CHECK(WAIT(&e, e.kbd_on == m.surf, 2000), "the menu did not get the keys");
    say("menu up");
    double t0 = now_s();
    bool done = WAIT(&e, m.done, g_wait * 1000);
    CHECK(done, "no popup_done within %d s of the menu showing", g_wait);
    CHECK(WAIT(&e, e.kbd_on == w.surf, 2000), "the keys did not come back to the window");
    if (done)
        seen("dismissed by the click outside after %.1f s, the keys back", now_s() - t0);
out:
    env_close(&e);
}

/* big-buffer: Chromium's software output draws whole-screen buffers (2560x1440
 * ARGB is 14.7 MB) and keeps a few; three of them in a window, one committed
 * after the other, each a new pool of that size. */
static void fill_big(uint32_t *px, int w, int h, void *user)
{
    int n = *(int *)user;
    rect(px, w, h, 0, 0, w, h, 0xff000000u | (uint32_t)(n * 0x203040));
    rect(px, w, h, 0, 0, 16, 16, MARK);
}

static void case_big_buffer(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    int n = 0;
    if (!win_open(&e, &w, WF_DECO, "wlprobe big-buffer")) {
        env_close(&e);
        return;
    }
    w.fill = fill_big;
    w.fill_user = &n;
    double t0 = now_s();
    for (n = 1; n <= 3; n++) {
        unsigned before = w.frames;
        if (!win_paint_sized(&w, 2560, 1440)) {
            fail("no memory for a 2560x1440 buffer (%d)", n);
            break;
        }
        CHECK(WAIT(&e, w.frames > before, 5000), "buffer %d (2560x1440): no frame callback within 5 s%s", n,
              e.dead ? " (connection dead)" : "");
    }
    CHECK(!e.dead, "protocol error: %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    seen("3 buffers of 2560x1440 in %.2f s", now_s() - t0);
    env_close(&e);
}

/* resize-storm: maximize and unmaximize 40 times without waiting for the
 * answers, repainting at each configure's size; the window ends where it started. */
static void case_resize_storm(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe resize-storm")) {
        env_close(&e);
        return;
    }
    int w0 = w.bw, h0 = w.bh;
    for (int i = 0; i < 40 && !e.dead; i++) {
        if (i & 1)
            xdg_toplevel_unset_maximized(w.top);
        else
            xdg_toplevel_set_maximized(w.top);
        pump(&e, 5);
        win_follow(&w);
    }
    xdg_toplevel_unset_maximized(w.top);
    xdg_toplevel_unset_fullscreen(w.top);
    bool back = WAIT(&e, (win_follow(&w), w.ncfg > 0 && !(win_cfg(&w)->states & ST(XDG_TOPLEVEL_STATE_MAXIMIZED)) &&
                         w.bw == w0 && w.bh == h0), 5000);
    CHECK(!e.dead, "protocol error in the storm: %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    CHECK(back, "the window did not come back to %dx%d (buffer %dx%d, %d configures)", w0, h0, w.bw, w.bh, w.ncfg);
    unsigned f = w.frames;
    win_paint(&w);
    CHECK(WAIT(&e, w.frames > f, 3000), "no frame callback after the storm");
    seen("%d configures, back at %dx%d", w.ncfg, w.bw, w.bh);
    env_close(&e);
}

/* reuse: a wl_surface keeps its xdg role when the xdg_toplevel and xdg_surface
 * are destroyed: it can be given a new xdg_surface and toplevel and maps again
 * (Chromium recreates its window objects this way when it changes a window's
 * type). */
static void case_reuse(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe reuse")) {
        env_close(&e);
        return;
    }
    zxdg_toplevel_decoration_v1_destroy(w.deco);
    w.deco = NULL;
    xdg_toplevel_destroy(w.top);
    xdg_surface_destroy(w.xs);
    wl_surface_attach(w.surf, NULL, 0, 0);
    wl_surface_commit(w.surf);
    CHECK(roundtrip(&e), "destroying the role objects: protocol error %s code %d",
          e.err_iface ? e.err_iface : "-", e.err_code);
    w.acked = 0;
    w.ncfg = 0;
    w.bw = w.bh = 0;
    w.frames = 0;
    w.frame_pending = false;
    w.xs = xdg_wm_base_get_xdg_surface(e.wm, w.surf);
    xdg_surface_add_listener(w.xs, &xsurface_listener, &w);
    w.top = xdg_surface_get_toplevel(w.xs);
    xdg_toplevel_add_listener(w.top, &toplevel_listener, &w);
    xdg_toplevel_set_title(w.top, "wlprobe reuse again");
    wl_surface_commit(w.surf);
    bool conf = WAIT(&e, w.acked > 0, 5000);
    CHECK(conf, "no configure for the surface's second xdg role%s", e.dead ? " (connection dead)" : "");
    if (conf) {
        win_paint(&w);
        CHECK(WAIT(&e, w.frames > 0, 5000), "no frame callback after the second map");
    }
    CHECK(!e.dead, "protocol error: %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    seen("mapped twice on one wl_surface");
    env_close(&e);
}

/* clipboard-serial: set_selection with a serial the seat never sent is not
 * honoured (the source is told cancelled), and with the serial of the window's
 * keyboard enter it is (docs/G2-PLAN.md section 8, the compositor's rules). */
static void case_clipboard_serial(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_keyboard(&e)) {
        env_close(&e);
        return;
    }
    REQUIRE(e.ddm, "no wl_data_device_manager");
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe clipboard-serial")) {
        env_close(&e);
        return;
    }
    REQUIRE(WAIT(&e, e.kbd_enter_serial != 0, 3000), "the new window got no wl_keyboard.enter");
    e.ddev = wl_data_device_manager_get_data_device(e.ddm, e.seat);
    wl_data_device_add_listener(e.ddev, &data_device_listener, &e);
    e.src_text = "wlprobe-bad-serial";
    e.src = wl_data_device_manager_create_data_source(e.ddm);
    wl_data_source_add_listener(e.src, &source_listener, &e);
    wl_data_source_offer(e.src, "text/plain;charset=utf-8");
    wl_data_device_set_selection(e.ddev, e.src, 0x7ffffff0u);
    roundtrip(&e);
    hold_for(&e, 0.3);
    CHECK(e.src_cancelled, "a selection set with a serial the seat never sent was not cancelled");
    e.src_cancelled = false;
    struct wl_data_source *src2 = wl_data_device_manager_create_data_source(e.ddm);
    wl_data_source_add_listener(src2, &source_listener, &e);
    wl_data_source_offer(src2, "text/plain;charset=utf-8");
    wl_data_device_set_selection(e.ddev, src2, e.kbd_enter_serial);
    roundtrip(&e);
    hold_for(&e, 0.3);
    CHECK(!e.src_cancelled, "a selection set with the keyboard enter's serial was cancelled");
    seen("a bad serial cancelled, the enter's serial held");
    env_close(&e);
}

/* csd-geometry: a client that never bound the decoration manager draws its own
 * decorations; with a window geometry smaller than its surface (a shadow
 * margin) the compositor puts no corners or shadow of its own on it. The
 * surface's corner pixel shows the client's colour exactly (stage b) and does
 * not when the geometry is the whole surface (stage a: rounded corners). */
static void fill_csd(uint32_t *px, int w, int h, void *user)
{
    (void)user;
    rect(px, w, h, 0, 0, w, h, 0xff5ac8fau);
    rect(px, w, h, 24, 24, 16, 16, MARK);
    rect(px, w, h, 0, 0, 3, 3, 0xffff8000u);   /* the corner pixels' own colour */
    rect(px, w, h, w - 3, 0, 3, 3, 0xffff8000u);
    rect(px, w, h, 0, h - 3, 3, 3, 0xffff8000u);
    rect(px, w, h, w - 3, h - 3, 3, 3, 0xffff8000u);
}

static void case_csd_geometry(void)
{
    struct env e;
    if (!env_open(&e, B_NODECO))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    memset(&w, 0, sizeof(w));
    if (!win_open(&e, &w, WF_NOMAP, "wlprobe csd-geometry")) {
        env_close(&e);
        return;
    }
    w.fill = fill_csd;
    REQUIRE(WAIT(&e, w.acked > 0, 5000), "no configure");
    win_paint_sized(&w, DEF_W, DEF_H);
    REQUIRE(WAIT(&e, w.frames > 0, 5000), "no frame callback");
    say("a: no geometry; corners of the surface orange ff8000, marker magenta at 24,24");
    shot(&e, "csd-a");
    unsigned f = w.frames;
    xdg_surface_set_window_geometry(w.xs, 20, 20, DEF_W - 40, DEF_H - 40);
    win_paint_sized(&w, DEF_W, DEF_H);
    CHECK(WAIT(&e, w.frames > f, 3000), "no frame callback after the geometry");
    say("b: geometry 20,20 %dx%d", DEF_W - 40, DEF_H - 40);
    shot(&e, "csd-b");
    seen("no deco manager bound, geometry set");
    env_close(&e);
}

/* ---- input cases -------------------------------------------------------------------------------- */

/* pointer: enter, motion (1:1 with QEMU's mouse steps), buttons, the wheel and
 * leave through injected input (tools/wl-port-test.sh). The script puts the
 * pointer at a place in the window, clicks, moves 30 pixels right, clicks, the
 * wheel up and down, a right click, and moves out. */
static void case_pointer(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_pointer(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe pointer")) {
        env_close(&e);
        return;
    }
    say("ready");
    say("surface %dx%d", w.bw, w.bh);
    double end = now_s() + g_wait;
    bool left = false;
    while (now_s() < end && !left && !e.dead) {
        pump(&e, 100);
        bool entered = false, wheel = false;
        for (int i = 0; i < e.npe; i++) {
            if (e.pe[i].t == PE_ENTER)
                entered = true;
            if (e.pe[i].t == PE_AXIS)
                wheel = true;
            if (e.pe[i].t == PE_LEAVE && entered && wheel)
                left = true;
        }
    }
    CHECK(left, "the script's input did not all arrive within %d s", g_wait);
    /* what arrived */
    int enters = 0, leaves = 0, motions = 0, presses = 0, releases = 0, rights = 0, frames = 0;
    double ax = -1, ay = -1, bx = -1, by = -1;
    int clicks = 0;
    int axis_up = 0, axis_down = 0, v120_up = 0, v120_down = 0, discrete = 0;
    for (int i = 0; i < e.npe; i++) {
        struct pev *p = &e.pe[i];
        switch (p->t) {
        case PE_ENTER:
            enters++;
            CHECK(p->x >= 0 && p->x < w.bw && p->y >= 0 && p->y < w.bh,
                  "enter at %.1f,%.1f outside the %dx%d surface", p->x, p->y, w.bw, w.bh);
            CHECK(p->surface == w.surf, "enter on another surface");
            break;
        case PE_LEAVE: leaves++; break;
        case PE_MOTION: motions++; break;
        case PE_FRAME: frames++; break;
        case PE_BUTTON:
            if (p->a == BTN_LEFT && p->b == WL_POINTER_BUTTON_STATE_PRESSED) {
                presses++;
                if (clicks == 0)
                    ax = p->x, ay = p->y;
                else if (clicks == 1)
                    bx = p->x, by = p->y;
                clicks++;
            } else if (p->a == BTN_LEFT) {
                releases++;
            } else if (p->a == BTN_RIGHT && p->b == WL_POINTER_BUTTON_STATE_PRESSED) {
                rights++;
            }
            break;
        case PE_AXIS:
            if (p->a == WL_POINTER_AXIS_VERTICAL_SCROLL) {
                if (p->v < 0)
                    axis_up++;
                else if (p->v > 0)
                    axis_down++;
            }
            break;
        case PE_AXIS120:
            if (p->v < 0)
                v120_up++;
            else if (p->v > 0)
                v120_down++;
            break;
        case PE_AXISD: discrete++; break;
        }
    }
    CHECK(enters >= 1, "no wl_pointer.enter");
    CHECK(motions >= 1, "no wl_pointer.motion");
    CHECK(presses >= 2 && releases >= 2, "left button: %d presses, %d releases (wanted 2 and 2)",
          presses, releases);
    CHECK(rights >= 1, "no right button press");
    CHECK(frames >= 1, "no wl_pointer.frame (the seat is v%u)", e.seat_ver);
    CHECK(leaves >= 1, "no wl_pointer.leave");
    if (ax >= 0 && bx >= 0) {
        CHECK(bx - ax > 29.9 && bx - ax < 30.1 && by - ay > -0.1 && by - ay < 0.1,
              "30 steps right moved the pointer by %.2f,%.2f", bx - ax, by - ay);
        say("click A at %.0f,%.0f", ax, ay);
        say("click B at %.0f,%.0f", bx, by);
    } else {
        fail("fewer than two left clicks at known places");
    }
    CHECK(axis_up >= 1 && axis_down >= 1, "wheel: %d events one way, %d the other", axis_up, axis_down);
    if (e.seat_ver >= 8)
        CHECK(v120_up >= 1 && v120_down >= 1, "axis_value120: %d one way, %d the other", v120_up,
              v120_down);
    seen("enter %d, motion %d, left %d/%d, right %d, wheel %d/%d, frames %d, leave %d", enters,
         motions, presses, releases, rights, axis_up, axis_down, frames, leaves);
    (void)discrete;
    env_close(&e);
}

/* keyboard: the window has the keys when it maps (wl_keyboard.enter) and keys
 * typed on the USB keyboard arrive as evdev codes the compositor's keymap turns
 * into the characters typed. The script types ab1, Shift+C and Return. */
static void case_keyboard(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_keyboard(&e)) {
        env_close(&e);
        return;
    }
    REQUIRE(e.xstate, "no usable keymap (case keymap says why)");
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe keyboard")) {
        env_close(&e);
        return;
    }
    bool entered = WAIT(&e, e.kbd_enter_serial != 0, 3000);
    CHECK(entered, "the new window got no wl_keyboard.enter");
    say("ready");
    double end = now_s() + g_wait;
    bool ret = false;
    while (now_s() < end && !ret && !e.dead) {
        pump(&e, 100);
        for (int i = 0; i < e.nke; i++)
            if (e.ke[i].t == KE_KEY && e.ke[i].state == WL_KEYBOARD_KEY_STATE_PRESSED &&
                e.ke[i].sym == XKB_KEY_Return)
                ret = true;
    }
    CHECK(ret, "Return never arrived within %d s", g_wait);
    hold_for(&e, 0.7);   /* its release, and the others' */
    char typed[64] = "";
    int presses = 0, releases = 0, shift_down = 0;
    uint32_t codes[32];
    for (int i = 0; i < e.nke; i++) {
        struct kev *k = &e.ke[i];
        if (k->t == KE_KEY && k->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            if (presses < 32)
                codes[presses] = k->code;
            presses++;
            if (k->utf8[0] && strlen(typed) + strlen(k->utf8) < sizeof(typed) && k->sym != XKB_KEY_Return)
                strcat(typed, k->utf8);
        } else if (k->t == KE_KEY) {
            releases++;
        } else if (k->t == KE_MODS && (k->depressed & e.shift_bit)) {
            shift_down++;
        }
    }
    CHECK(!strcmp(typed, "ab1C"), "typed '%s', wanted 'ab1C'", typed);
    CHECK(presses == releases, "%d presses, %d releases", presses, releases);
    CHECK(shift_down >= 1, "no modifiers event with Shift down");
    if (presses >= 4)
        CHECK(codes[0] == KEY_A && codes[1] == 48 && codes[2] == 2,
              "codes %u %u %u, wanted 30 48 2 (a b 1)", codes[0], codes[1], codes[2]);
    seen("typed '%s', %d keys, shift modifiers %d, enter serial %u", typed, presses, shift_down,
         e.kbd_enter_serial);
    env_close(&e);
}

/* cursor-shape: wp_cursor_shape_device_v1.set_shape with the pointer's enter serial
 * changes the cursor drawn over the window (the script compares the screenshots
 * around the pointer); a shape the protocol doesn't have is the invalid_shape
 * error. */
static void case_cursor_shape(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_pointer(&e)) {
        env_close(&e);
        return;
    }
    REQUIRE(e.csm, "no wp_cursor_shape_manager_v1");
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe cursor")) {
        env_close(&e);
        return;
    }
    e.shape = wp_cursor_shape_manager_v1_get_pointer(e.csm, e.ptr);
    say("ready");
    bool in = WAIT(&e, e.ptr_enter_serial != 0, g_wait * 1000);
    REQUIRE(in, "the pointer never entered the window within %d s", g_wait);
    hold_for(&e, 0.5);
    say("default cursor (no request)");
    shot(&e, "cursor-default");
    wp_cursor_shape_device_v1_set_shape(e.shape, e.ptr_enter_serial,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    say("text");
    shot(&e, "cursor-text");
    wp_cursor_shape_device_v1_set_shape(e.shape, e.ptr_enter_serial,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    say("pointer (the hand)");
    shot(&e, "cursor-hand");
    CHECK(roundtrip(&e), "protocol error after the shapes: %s code %d",
          e.err_iface ? e.err_iface : "-", e.err_code);
    /* a number the protocol has no shape for */
    wp_cursor_shape_device_v1_set_shape(e.shape, e.ptr_enter_serial, 9999);
    wl_display_flush(e.dpy);
    WAIT(&e, e.dead, 2000);
    if (!e.dead)
        roundtrip(&e);
    CHECK(e.dead, "set_shape 9999: no protocol error");
    if (e.dead)
        CHECK(e.err_code == WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
              "set_shape 9999: error %s code %d, wanted invalid_shape", e.err_iface ? e.err_iface : "?",
              e.err_code);
    seen("default, text, pointer, then invalid_shape for 9999");
    env_close(&e);
}

/* clipboard-out: the window sets the selection (text/plain;charset=utf-8,
 * with the serial of its keyboard enter) and a terminal pastes it: the
 * compositor passes the terminal's pipe on and wl_data_source.send arrives. */
#define CLIP_TEXT "wlprobe-clip-out-4242"

static void case_clipboard_out(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_keyboard(&e)) {
        env_close(&e);
        return;
    }
    REQUIRE(e.ddm, "no wl_data_device_manager");
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe clipboard-out")) {
        env_close(&e);
        return;
    }
    REQUIRE(WAIT(&e, e.kbd_enter_serial != 0, 3000), "the new window got no wl_keyboard.enter");
    e.ddev = wl_data_device_manager_get_data_device(e.ddm, e.seat);
    wl_data_device_add_listener(e.ddev, &data_device_listener, &e);
    e.src = wl_data_device_manager_create_data_source(e.ddm);
    e.src_text = CLIP_TEXT;
    wl_data_source_add_listener(e.src, &source_listener, &e);
    wl_data_source_offer(e.src, "text/plain;charset=utf-8");
    wl_data_source_offer(e.src, "text/plain");
    wl_data_device_set_selection(e.ddev, e.src, e.kbd_enter_serial);
    REQUIRE(roundtrip(&e), "protocol error: %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    CHECK(!e.src_cancelled, "the selection was cancelled at once (serial %u)", e.kbd_enter_serial);
    say("ready");
    bool sent = WAIT(&e, e.src_sends > 0 || e.src_cancelled, g_wait * 1000);
    CHECK(sent && e.src_sends > 0, "nobody pasted within %d s%s", g_wait,
          e.src_cancelled ? " (the selection was cancelled)" : "");
    if (e.src_sends)
        seen("wl_data_source.send for %s, %zu bytes written", e.src_mime, strlen(CLIP_TEXT));
    CHECK(!e.src_cancelled, "the selection was cancelled while pasted");
    env_close(&e);
}

/* clipboard-in: a terminal's selection reaches the window: wl_data_device.selection
 * with an offer of text, and the text through wl_data_offer.receive. The text the
 * script copied is the case's argument (empty: any text). */

static void case_clipboard_in(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_keyboard(&e)) {
        env_close(&e);
        return;
    }
    REQUIRE(e.ddm, "no wl_data_device_manager");
    e.ddev = wl_data_device_manager_get_data_device(e.ddm, e.seat);
    wl_data_device_add_listener(e.ddev, &data_device_listener, &e);
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe clipboard-in")) {
        env_close(&e);
        return;
    }
    say("ready");
    REQUIRE(WAIT(&e, e.sel != NULL, g_wait * 1000), "no selection offer within %d s (%u selection events)",
            g_wait, e.nsel_events);
    roundtrip(&e);
    struct offer *of = e.sel;
    REQUIRE(of, "the selection went");
    bool utf8 = false;
    for (int i = 0; i < of->n; i++) {
        say("offer type %s", of->mime[i]);
        if (!strcmp(of->mime[i], "text/plain;charset=utf-8"))
            utf8 = true;
    }
    CHECK(utf8, "no text/plain;charset=utf-8 among %d offered types", of->n);
    int fds[2];
    REQUIRE(pipe(fds) == 0, "pipe: %s", strerror(errno));
    wl_data_offer_receive(of->o, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    wl_display_flush(e.dpy);
    char buf[512];
    size_t n = 0;
    double end = now_s() + 5;
    while (now_s() < end && n < sizeof(buf) - 1) {
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        int r = poll(&p, 1, 100);
        pump(&e, 0);
        if (r > 0) {
            ssize_t got = read(fds[0], buf + n, sizeof(buf) - 1 - n);
            if (got <= 0)
                break;
            n += (size_t)got;
        }
    }
    buf[n] = 0;
    close(fds[0]);
    CHECK(n > 0, "the offer gave no data");
    if (g_arg && *g_arg)
        CHECK(!strcmp(buf, g_arg), "received '%s', wanted '%s'", buf, g_arg);
    seen("received %zu bytes: '%s'", n, buf);
    env_close(&e);
}

/* keyless-click: a click on a window of a client that took no keys makes it the
 * active window (the compositor's rule since 50088991): xdg_toplevel.configure
 * with the activated state. */
static void case_keyless_click(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_KEYLESS, "wlprobe keyless-click")) {
        env_close(&e);
        return;
    }
    CHECK(!e.kbd, "the probe made a keyboard");
    hold_for(&e, 0.5);
    CHECK(!win_state_since(&w, 0, XDG_TOPLEVEL_STATE_ACTIVATED), "a keyless window was activated at map");
    int before = w.ncfg;
    say("ready");
    double t0 = now_s();
    bool act = WAIT(&e, win_state_since(&w, before, XDG_TOPLEVEL_STATE_ACTIVATED), g_wait * 1000);
    CHECK(act, "a click on the window did not activate it within %d s (%d configures since)", g_wait,
          w.ncfg - before);
    if (act)
        seen("activated by a click after %.1f s", now_s() - t0);
    env_close(&e);
}

/* close: the compositor's close request (Super+Q on the window with the keys)
 * reaches the client as xdg_toplevel.close, and it asks xdg_wm_base.ping
 * whether the client is alive. */
static void case_close(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe close")) {
        env_close(&e);
        return;
    }
    REQUIRE(WAIT(&e, e.kbd_enter_serial != 0, 3000), "the new window got no wl_keyboard.enter");
    say("ready");
    double t0 = now_s();
    bool closed = WAIT(&e, w.closed, g_wait * 1000);
    CHECK(closed, "no xdg_toplevel.close within %d s", g_wait);
    if (closed) {
        seen("close after %.1f s", now_s() - t0);
        bool pinged = WAIT(&e, e.pings > 0, 3000);
        CHECK(pinged, "no xdg_wm_base.ping after the close request");
        if (pinged)
            seen("%u ping(s), answered", e.pings);
    }
    env_close(&e);
}

/* cursor-surface: wl_pointer.set_cursor with a surface of the client's own and
 * a hotspot (Chromium's custom cursors, and the one it hides while typing: a NULL
 * surface). The script shoots the screen under the pointer. */
static void fill_cursor(uint32_t *px, int w, int h, void *user)
{
    (void)user;
    rect(px, w, h, 0, 0, w, h, 0xffff0000u);
    rect(px, w, h, 8, 8, 8, 8, 0xff00ff00u);
}

static void case_cursor_surface(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !env_pointer(&e)) {
        env_close(&e);
        return;
    }
    struct win w;
    if (!win_open(&e, &w, WF_DECO, "wlprobe cursor-surface")) {
        env_close(&e);
        return;
    }
    say("ready");
    REQUIRE(WAIT(&e, e.ptr_enter_serial != 0, g_wait * 1000),
            "the pointer never entered the window within %d s", g_wait);
    hold_for(&e, 0.5);
    struct wl_surface *cs = wl_compositor_create_surface(e.comp);
    struct wl_buffer *b = buf_make(&e, 24, 24, WL_SHM_FORMAT_ARGB8888, fill_cursor, NULL);
    REQUIRE(b, "no buffer");
    wl_surface_attach(cs, b, 0, 0);
    wl_surface_damage_buffer(cs, 0, 0, 24, 24);
    wl_surface_commit(cs);
    wl_pointer_set_cursor(e.ptr, e.ptr_enter_serial, cs, 12, 12);
    CHECK(roundtrip(&e), "protocol error after set_cursor: %s code %d", e.err_iface ? e.err_iface : "-",
          e.err_code);
    say("a 24x24 red cursor with a green centre, hotspot 12,12");
    shot(&e, "cursor-surface");
    wl_pointer_set_cursor(e.ptr, e.ptr_enter_serial, NULL, 0, 0);
    CHECK(roundtrip(&e), "protocol error after set_cursor(NULL)");
    say("hidden");
    shot(&e, "cursor-hidden");
    seen("own surface with a hotspot, then none");
    env_close(&e);
}

/* ---- hostile clients (track GR, docs/G2-REVIEW.md) ----------------------------------------- */

/* Clients that do what a broken or hostile program might, against the live
 * compositor: each must get the protocol's answer (or be served), and the
 * desktop must go on serving a plain client after. */

/* wl_subcompositor bound on e; NULL (and the case failed) if not offered. */
static struct wl_subcompositor *env_subcompositor(struct env *e)
{
    const struct glob *g = env_glob(e, "wl_subcompositor");
    if (!g) {
        fail("wl_subcompositor is not offered");
        return NULL;
    }
    return wl_registry_bind(e->reg, g->name, &wl_subcompositor_interface, 1);
}

/* A connection, a decorated window mapped and the subcompositor. */
static bool sub_window(struct env *e, struct win *w, struct wl_subcompositor **sc,
                       const char *title)
{
    if (!env_open(e, 0))
        return false;
    if (env_need_window(e) && win_open(e, w, WF_DECO, title) && (*sc = env_subcompositor(e)))
        return true;
    env_close(e);
    return false;
}

/* A buffer attached again and again (to many surfaces at once): kept. */
static struct wl_buffer *colour_buffer(struct env *e, int side, uint32_t colour)
{
    return buf_keep(e, side, side, WL_SHM_FORMAT_XRGB8888, fill_colour, &colour);
}

/* Flush e's requests, waiting for room while the socket is full (2 s at
 * most): false once that fails (the connection gone, or no room). */
static bool flush_out(struct env *e)
{
    double end = now_s() + 2;
    while (wl_display_flush(e->dpy) < 0) {
        if (errno != EAGAIN || now_s() > end)
            return false;
        struct pollfd p = { .fd = wl_display_get_fd(e->dpy), .events = POLLOUT };
        poll(&p, 1, 100);
    }
    return true;
}

/* The protocol error that ended e's connection: its code, and in *iface the
 * interface of the object it named ("?": none). libwayland's errno is
 * EPROTO for most, but ENOMEM, EINVAL or EFAULT for one on wl_display
 * itself, which check_error keeps as an errno: asked here either way. */
static int proto_error(struct env *e, const char **iface)
{
    const struct wl_interface *i = NULL;
    uint32_t id = 0;
    int code = (int)wl_display_get_protocol_error(e->dpy, &i, &id);
    *iface = i ? i->name : "?";
    return code;
}

/* The connection ends within 3 s with error code on an object of interface
 * iface. */
static void expect_dead(struct env *e, const char *what, const char *iface, int code)
{
    wl_display_flush(e->dpy);
    WAIT(e, e->dead, 3000);
    if (!e->dead)
        roundtrip(e);
    if (!e->dead) {
        fail("%s: no protocol error", what);
        return;
    }
    const char *got;
    int got_code = proto_error(e, &got);
    CHECK(got_code == code && !strcmp(got, iface), "%s: error %s code %d, wanted %s code %d",
          what, got, got_code, iface, code);
    seen("%s: %s %d", what, got, got_code);
}

/* The desktop still serves a plain client: a new window maps and draws. */
static void still_serving(void)
{
    struct env e;
    struct win w;
    if (!env_open(&e, 0))
        return;
    if (env_need_window(&e) && win_open(&e, &w, WF_DECO, "wlprobe after"))
        seen("a new window mapped after");
    env_close(&e);
}

/* One past a subsurface cap, on a client of its own: 33 children of one
 * parent (which 0), 9 nested (1), 49 in all (2). */
static void sub_past_cap(int which)
{
    static const char *const what[] = { "a 33rd child", "9 nested", "a 49th subsurface" };
    struct env e;
    struct win w;
    struct wl_subcompositor *sc;
    if (!sub_window(&e, &w, &sc, "wlprobe hostile-sub-cap"))
        return;
    struct wl_surface *s[49];
    int n = which == 0 ? 33 : which == 1 ? 9 : 49;
    for (int k = 0; k < n; k++) {
        s[k] = wl_compositor_create_surface(e.comp);
        struct wl_surface *parent = which == 1 && k ? s[k - 1]
                                    : which == 2 && k >= 32 ? s[k - 32] : w.surf;
        wl_subcompositor_get_subsurface(sc, s[k], parent);
    }
    expect_dead(&e, what[which], "wl_subcompositor", WL_DISPLAY_ERROR_NO_MEMORY);
    env_close(&e);
}

/* hostile-sub-storm: subsurfaces made, drawn and destroyed 1,000 times under
 * a mapped window (half of them desynchronized, the parent committing now
 * and then), then trees at every cap at once (32 children of the window, a
 * chain 8 deep, 48 in all) drawn and committed; then one past each cap,
 * each on a client of its own: no_memory. */
static void case_hostile_sub_storm(void)
{
    struct env e;
    struct win w;
    struct wl_subcompositor *sc;
    if (!sub_window(&e, &w, &sc, "wlprobe hostile-sub-storm"))
        return;
    struct wl_buffer *b = colour_buffer(&e, 8, 0xff3060c0u);
    if (!b) {
        fail("no memory for a buffer");
        env_close(&e);
        return;
    }
    double t0 = now_s();
    int i = 0;
    for (; i < 1000 && !e.dead; i++) {
        struct wl_surface *s = wl_compositor_create_surface(e.comp);
        struct wl_subsurface *ss = wl_subcompositor_get_subsurface(sc, s, w.surf);
        wl_subsurface_set_position(ss, i % 300, i % 200);
        if (i & 1)
            wl_subsurface_set_desync(ss);
        wl_surface_attach(s, b, 0, 0);
        wl_surface_commit(s);
        if (i % 7 == 0)
            wl_surface_commit(w.surf);   /* the parent applies what waits */
        wl_subsurface_destroy(ss);
        wl_surface_destroy(s);
        if (i % 100 == 99)
            roundtrip(&e);
    }
    CHECK(roundtrip(&e), "the storm: protocol error %s code %d at %d",
          e.err_iface ? e.err_iface : "-", e.err_code, i);
    seen("1000 made and destroyed in %.0f ms", (now_s() - t0) * 1000);
    struct wl_surface *s[48];
    struct wl_subsurface *ss[48];
    for (int k = 0; k < 48 && !e.dead; k++) {
        /* 0-7 a chain under the window, 8-38 more children of it (32 with
         * the chain's first), 39-47 one each on the chain's 0-6 (depth 8 at most) */
        struct wl_surface *parent = k < 8 ? (k ? s[k - 1] : w.surf)
                                    : k < 39 ? w.surf : s[(k - 39) % 7];
        s[k] = wl_compositor_create_surface(e.comp);
        ss[k] = wl_subcompositor_get_subsurface(sc, s[k], parent);
        wl_subsurface_set_position(ss[k], 4 + k * 2, 4 + (k % 8) * 3);
        wl_surface_attach(s[k], b, 0, 0);
        wl_surface_commit(s[k]);
    }
    for (int k = 7; k >= 0; k--)
        wl_surface_commit(s[k]);   /* the chain's caches up to the window */
    unsigned before = w.frames;
    struct wl_callback *cb = wl_surface_frame(w.surf);
    wl_callback_add_listener(cb, &frame_listener, &w);
    wl_surface_commit(w.surf);
    CHECK(WAIT(&e, w.frames > before, 5000), "the trees at the caps: no frame callback%s",
          e.dead ? " (connection dead)" : "");
    CHECK(roundtrip(&e), "the trees at the caps: protocol error %s code %d",
          e.err_iface ? e.err_iface : "-", e.err_code);
    if (!e.dead)
        seen("32 wide, 8 deep, 48 in all shown");
    env_close(&e);
    for (int which = 0; which < 3; which++)
        sub_past_cap(which);
    still_serving();
}

/* hostile-desync-flood: a desynchronized child drawn again 2,000 times
 * with its parent idle, its requests flushed flat out (events read only at
 * every 250th): the client stays connected, the parent's frame callback
 * still comes and a round trip is answered. */
static void case_hostile_desync_flood(void)
{
    struct env e;
    struct win w;
    struct wl_subcompositor *sc;
    if (!sub_window(&e, &w, &sc, "wlprobe hostile-desync-flood"))
        return;
    struct wl_buffer *b[2] = { colour_buffer(&e, 64, 0xffc03060u),
                               colour_buffer(&e, 64, 0xff30c060u) };
    if (!b[0] || !b[1]) {
        fail("no memory for a buffer");
        env_close(&e);
        return;
    }
    struct wl_surface *s = wl_compositor_create_surface(e.comp);
    struct wl_subsurface *ss = wl_subcompositor_get_subsurface(sc, s, w.surf);
    wl_subsurface_set_position(ss, 40, 40);
    wl_subsurface_set_desync(ss);
    wl_surface_commit(w.surf);   /* the child into the tree */
    double t0 = now_s();
    int i = 0;
    bool flushed = true;
    for (; i < 2000 && flushed && !e.dead; i++) {
        wl_surface_attach(s, b[i & 1], 0, 0);
        wl_surface_damage_buffer(s, 0, 0, 64, 64);
        wl_surface_commit(s);
        flushed = flush_out(&e);
        if (i % 250 == 249)
            pump(&e, 0);
    }
    CHECK(flushed, "the flood stopped at commit %d: %s", i, strerror(errno));
    double t1 = now_s();
    unsigned before = w.frames;
    struct wl_callback *cb = wl_surface_frame(w.surf);
    wl_callback_add_listener(cb, &frame_listener, &w);
    wl_surface_commit(w.surf);
    CHECK(WAIT(&e, w.frames > before, 5000), "no frame callback on the parent after the flood%s",
          e.dead ? " (connection dead)" : "");
    CHECK(roundtrip(&e), "protocol error %s code %d", e.err_iface ? e.err_iface : "-", e.err_code);
    if (!e.dead)
        seen("%d desynchronized commits in %.0f ms, the parent's frame after", i, (t1 - t0) * 1000);
    env_close(&e);
    still_serving();
}

/* hostile-popup-cap: a chain of 8 grabbing popups, each on the one before,
 * the keys going up it to the eighth; a ninth is no_memory. Then 32 popups
 * on one toplevel and a 33rd, on another client: no_memory. */
static void case_hostile_popup_cap(void)
{
    struct env e;
    struct win w;
    if (!pup_window(&e, &w, "wlprobe hostile-popup-cap"))
        return;
    struct pup pu[9];
    memset(pu, 0, sizeof(pu));
    const struct pspec s = { 60, 40, 10, 10, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    for (int k = 0; k < 8; k++) {
        NEED(pup_open(&e, &pu[k], k ? pu[k - 1].xs : w.xs, &s, e.kbd_enter_serial) && !pu[k].done,
             "popup %d of the chain was not configured", k + 1);
        pup_draw(&pu[k], 0xff000000u | (uint32_t)(0x1c1c1c * (k + 1)));
        NEED(WAIT(&e, e.kbd_on == pu[k].surf, 2000), "popup %d of the chain didn't get the keys",
             k + 1);
    }
    seen("a chain of 8 with the keys at its top");
    (void)pup_open(&e, &pu[8], pu[7].xs, &s, 0);
    expect_dead(&e, "a ninth nested", "xdg_popup", WL_DISPLAY_ERROR_NO_MEMORY);
    env_close(&e);
    if (!pup_window(&e, &w, "wlprobe hostile-popup-cap 33"))
        return;
    struct xdg_positioner *pos = pos_make(&e, &s);
    for (int k = 0; k < 33; k++) {
        struct wl_surface *ps = wl_compositor_create_surface(e.comp);
        struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(e.wm, ps);
        xdg_surface_get_popup(xs, w.xs, pos);
    }
    expect_dead(&e, "a 33rd popup", "xdg_popup", WL_DISPLAY_ERROR_NO_MEMORY);
out:
    env_close(&e);
    still_serving();
}

/* hostile-deco: a decoration object whose toplevel is destroyed first is
 * `orphaned`; a decorated toplevel's wl_surface destroyed first is
 * wl_surface's defunct_role_object (said on wl_display: the surface's id is
 * gone by then). */
static void deco_case(bool surface_first)
{
    struct env e;
    struct win w;
    if (!env_open(&e, 0))
        return;
    if (!env_need_window(&e) || !win_open(&e, &w, WF_DECO, "wlprobe hostile-deco")) {
        env_close(&e);
        return;
    }
    if (!w.deco) {
        fail("no decoration object (the manager isn't offered)");
        env_close(&e);
        return;
    }
    if (surface_first) {
        wl_surface_destroy(w.surf);
        expect_dead(&e, "the wl_surface first", "wl_display", WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT);
    } else {
        xdg_toplevel_destroy(w.top);
        expect_dead(&e, "the toplevel first", "zxdg_toplevel_decoration_v1",
                    ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED);
    }
    env_close(&e);
}

static void case_hostile_deco(void)
{
    deco_case(false);
    deco_case(true);
    still_serving();
}

/* sendmsg on the Wayland socket of one byte and n descriptors: its result,
 * errno in *err. */
static ssize_t send_fds(int sock, const int *fds, int n, int *err)
{
    char byte = 0;
    struct iovec iov = { &byte, 1 };
    union {
        char buf[CMSG_SPACE(32 * sizeof(int))];
        struct cmsghdr align;
    } ctl;
    memset(&ctl, 0, sizeof(ctl));
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf,
                        .msg_controllen = CMSG_SPACE((size_t)n * sizeof(int)) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN((size_t)n * sizeof(int));
    memcpy(CMSG_DATA(c), fds, (size_t)n * sizeof(int));
    ssize_t r = sendmsg(sock, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
    *err = errno;
    return r;
}

/* hostile-handles: 28 pipe ends given as 28 wl_shm pools, flushed as one
 * batch of handles: invalid_fd (the compositor closes them all). Then on
 * the socket itself, one byte with descriptors that can't cross (eventfds,
 * /dev/null, an AF_UNIX socket, an epoll set: 28 of them; 27 memfds and an
 * eventfd; 29 memfds): EINVAL each, nothing sent, the connection unharmed. */
static void case_hostile_handles(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    if (!e.shm) {
        fail("no wl_shm");
        env_close(&e);
        return;
    }
    for (int k = 0; k < 28; k++) {
        int p[2];
        if (pipe(p) < 0) {
            fail("pipe: %s", strerror(errno));
            break;
        }
        wl_shm_create_pool(e.shm, p[1], 4096);   /* libwayland sends a duplicate */
        close(p[0]);
        close(p[1]);
    }
    expect_dead(&e, "28 pipe ends as pools", "wl_shm", WL_SHM_ERROR_INVALID_FD);
    env_close(&e);
    if (!env_open(&e, 0))
        return;
    int sock = wl_display_get_fd(e.dpy), bad[29], mem[29], err = 0;
    for (int k = 0; k < 29; k++) {
        bad[k] = k % 4 == 0 ? eventfd(0, 0)
                 : k % 4 == 1 ? open("/dev/null", O_RDONLY)
                 : k % 4 == 2 ? socket(AF_UNIX, SOCK_STREAM, 0) : epoll_create1(0);
        mem[k] = memfd_create("wlprobe-hostile", MFD_CLOEXEC);
        if (bad[k] < 0 || mem[k] < 0 || ftruncate(mem[k], 4096) < 0)
            fail("descriptor %d: %s", k, strerror(errno));
    }
    ssize_t r = send_fds(sock, bad, 28, &err);
    CHECK(r < 0 && err == EINVAL, "28 descriptors that can't cross: %zd (%s), wanted EINVAL", r,
          strerror(err));
    int mixed[28];
    memcpy(mixed, mem, 27 * sizeof(int));
    mixed[27] = bad[0];
    r = send_fds(sock, mixed, 28, &err);
    CHECK(r < 0 && err == EINVAL, "27 memfds and an eventfd: %zd (%s), wanted EINVAL", r,
          strerror(err));
    r = send_fds(sock, mem, 29, &err);
    CHECK(r < 0 && err == EINVAL, "29 memfds: %zd (%s), wanted EINVAL", r, strerror(err));
    CHECK(roundtrip(&e), "the connection after the refused sends: error %s code %d",
          e.err_iface ? e.err_iface : "-", e.err_code);
    if (!e.dead)
        seen("each sendmsg EINVAL, nothing sent");
    for (int k = 0; k < 29; k++) {
        close(bad[k]);
        close(mem[k]);
    }
    env_close(&e);
    still_serving();
}

/* hostile-never-reads: a client asks for 2,000 registries (each answered
 * by a global event for every global; no id it uses comes back, so none
 * is used twice) and reads nothing: the compositor drops it as too slow,
 * with wl_display's no_memory, which the client reads first when it reads
 * at last; and goes on. */
static void case_hostile_never_reads(void)
{
    struct env e;
    if (!env_open(&e, 0))
        return;
    int i = 0;
    bool flushed = true;
    for (; i < 2000 && flushed; i++) {
        wl_display_get_registry(e.dpy);
        if (i % 16 == 15)
            flushed = flush_out(&e);
    }
    say("%d registries asked for, %s", i,
        flushed ? "reading nothing for 3 s" : "the socket stopped taking them");
    struct timespec three = { 3, 0 };
    nanosleep(&three, NULL);   /* not a byte read meanwhile */
    /* now read: libwayland's dispatch reads what waits even after a write
     * found the compositor gone */
    int fd = wl_display_get_fd(e.dpy);
    for (int k = 0; k < 100 && wl_display_get_error(e.dpy) == 0; k++) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 100) > 0)
            wl_display_dispatch(e.dpy);
    }
    const char *got;
    int code = proto_error(&e, &got);
    CHECK(wl_display_get_error(e.dpy) != 0, "still connected after %d registries never read", i);
    CHECK(code == WL_DISPLAY_ERROR_NO_MEMORY && !strcmp(got, "wl_display"),
          "ended by %s code %d (errno %d), wanted wl_display no_memory", got, code,
          wl_display_get_error(e.dpy));
    seen("dropped after %d registries: %s %d", i, got, code);
    env_close(&e);
    still_serving();
}

/* hostile-regrab (review finding 1): a menu opened by the script's click
 * on the window, dismissed by its click on the terminal; the client tries
 * to take the grab straight back, with that click's serial, with the
 * keyboard enter the keys coming back gave it, and with a new wl_keyboard's
 * enter: each popup is dismissed at once, and the script's next click on
 * the terminal reaches it (the window loses the keys). */
static void case_hostile_regrab(void)
{
    struct env e;
    struct win w;
    struct wl_keyboard *k2 = NULL;
    if (!pup_window(&e, &w, "wlprobe hostile-regrab"))
        return;
    NEED(env_pointer(&e), "no pointer");
    say("ready");
    uint32_t serial = 0;
    bool pressed = WAIT(&e, (serial = pressed_serial(&e)) != 0, g_wait * 1000);
    NEED(pressed, "no button press on the window within %d s", g_wait);
    const struct pspec s = { 160, 100, 20, 20, 10, 10, PA(BOTTOM_RIGHT), PG(BOTTOM_RIGHT), 0 };
    struct pup m, r[3];
    NEED(pup_open(&e, &m, w.xs, &s, serial) && !m.done, "the menu was not configured");
    pup_draw(&m, 0xffe0c030u);
    NEED(WAIT(&e, e.kbd_on == m.surf, 2000), "the menu did not get the keys");
    say("menu up");
    NEED(WAIT(&e, m.done, g_wait * 1000), "no popup_done within %d s of the menu showing", g_wait);
    NEED(WAIT(&e, e.kbd_on == w.surf, 2000), "the keys did not come back to the window");
    pup_close(&m);
    for (int i = 0; i < 3; i++) {
        uint32_t with = serial;
        if (i == 1)
            with = e.kbd_enter_serial;
        if (i == 2) {   /* a second wl_keyboard: its own enter */
            uint32_t was = e.kbd_enter_serial;
            k2 = wl_seat_get_keyboard(e.seat);
            wl_keyboard_add_listener(k2, &keyboard_listener, &e);
            NEED(WAIT(&e, e.kbd_enter_serial != was, 2000), "the second wl_keyboard got no enter");
            with = e.kbd_enter_serial;
        }
        bool answered = pup_open(&e, &r[i], w.xs, &s, with);
        CHECK(answered && r[i].done && !r[i].configured,
              "grab %d after the dismissal (serial %u) was not refused%s", i + 1, with,
              r[i].configured ? ": the popup was configured" : "");
        pup_close(&r[i]);
    }
    say("refused");
    bool left = WAIT(&e, e.kbd_on == NULL, g_wait * 1000);
    CHECK(left, "the next click outside didn't reach the terminal (the window kept the keys)");
    if (left && !cur->failed)
        seen("3 grabs refused after the dismissal; the next click outside went through");
out:
    if (k2)
        wl_keyboard_release(k2);
    env_close(&e);
}

/* ---- hold ----------------------------------------------------------------------------------------- */

static void fill_hold(uint32_t *px, int w, int h, void *user)
{
    unsigned n = *(unsigned *)user;
    static const uint32_t bands[] = { 0xffc2185b, 0xfff2a541, 0xff4a2c5e, 0xfff6efe6 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * w + x] = bands[((unsigned)(x + y) + n * 2) / 48 % 4];
}

/* hold: a window that stays, redrawn on every frame callback, for the soak. */
static int run_hold(double seconds)
{
    struct probe p = { .name = "hold" };
    cur = &p;
    struct env e;
    if (!env_open(&e, 0) || !env_need_window(&e)) {
        printf("FAIL hold: %s\n", p.why);
        return 1;
    }
    struct win w;
    unsigned n = 0;
    memset(&w, 0, sizeof(w));
    if (!win_open(&e, &w, WF_DECO | WF_NOMAP, "wlprobe hold")) {
        printf("FAIL hold: %s\n", p.why);
        return 1;
    }
    w.fill = fill_hold;
    w.fill_user = &n;
    if (!WAIT(&e, w.acked > 0, 5000)) {
        printf("FAIL hold: no configure\n");
        return 1;
    }
    win_paint(&w);
    say("holding%s", seconds > 0 ? "" : " until closed");
    double end = seconds > 0 ? now_s() + seconds : 0;
    unsigned seen_frames = 0;
    while (!w.closed && !e.dead && (end == 0 || now_s() < end)) {
        pump(&e, 100);
        win_follow(&w);
        if (w.frames != seen_frames) {
            seen_frames = w.frames;
            n++;
            win_paint(&w);
        }
    }
    printf("frames %u\n", w.frames);
    bool ok = !e.dead;
    printf("%s hold: %u frames%s\n", ok ? "PASS" : "FAIL", w.frames, w.closed ? ", closed" : "");
    env_close(&e);
    return ok ? 0 : 1;
}

/* ---- the table and main ---------------------------------------------------------------------------- */

enum { K_AUTO, K_SHOT, K_INPUT, K_HOSTILE };

static const struct {
    const char *name;
    void (*fn)(void);
    int kind;
    const char *what;
} cases[] = {
    { "globals", case_globals, K_AUTO, "the registry: the globals and versions the plan promises" },
    { "subcompositor", case_subcompositor, K_AUTO, "wl_subcompositor offered (GC2)" },
    { "bind", case_bind, K_AUTO, "every global bound at Chromium's and the newest version, objects made" },
    { "shm", case_shm, K_AUTO, "wl_shm formats ARGB8888 and XRGB8888, a pool that grows" },
    { "output", case_output, K_AUTO, "wl_output v4: geometry, mode, scale, name, description, done" },
    { "keymap", case_keymap, K_AUTO, "seat caps, the XKB keymap compiled by xkbcommon, repeat_info" },
    { "toplevel", case_toplevel, K_AUTO, "an xdg toplevel mapped: configures, capabilities, bounds, activated, frame" },
    { "keyless", case_keyless, K_AUTO, "a window of a client with no keyboard is not activated at map" },
    { "states-tiled", case_states_tiled, K_AUTO, "the same, in a tiling layout: the four tiled states" },
    { "states-floating", case_states_floating, K_AUTO, "the same, in a floating layout: no tiled state" },
    { "deco-server", case_deco_server, K_AUTO, "a client that bound the decoration manager is told server_side, always" },
    { "frames", case_frames, K_AUTO, "60 frame callbacks in order, buffers released" },
    { "geometry", case_geometry, K_AUTO, "set_window_geometry and size limits accepted" },
    { "geometry-error", case_geometry_error, K_AUTO, "set_window_geometry 0x0 is xdg_surface invalid_size" },
    { "limits-error", case_limits_error, K_AUTO, "a maximum under the minimum is xdg_toplevel invalid_size" },
    { "serial-error", case_serial_error, K_AUTO, "ack_configure of a serial never sent is invalid_serial" },
    { "parent", case_parent, K_AUTO, "set_parent accepted; a window as its own parent is invalid_parent" },
    { "popup", case_popup, K_AUTO, "an xdg_popup with a grab is configured where its positioner says, stays up, has the keys" },
    { "popup-nested", case_popup_nested, K_AUTO, "a menu and a submenu grabbing: the keys to the topmost and back down" },
    { "popup-reposition", case_popup_reposition, K_AUTO, "xdg_popup.reposition: repositioned with the token, a new configure" },
    { "popup-flip", case_popup_flip, K_AUTO, "a popup with no room below its anchor flipped above it (flip_y)" },
    { "popup-tooltip", case_popup_tooltip, K_AUTO, "a popup with no grab takes no keys and stays up" },
    { "popup-parent", case_popup_parent, K_AUTO, "a popup dismissed when its parent window is unmapped" },
    { "big-buffer", case_big_buffer, K_AUTO, "three 2560x1440 buffers (14.7 MB each), each a new pool" },
    { "resize-storm", case_resize_storm, K_AUTO, "40 maximize/unmaximize requests unanswered, repainted at each configure" },
    { "reuse", case_reuse, K_AUTO, "a wl_surface mapped twice: its xdg objects destroyed and made again" },
    { "clipboard-serial", case_clipboard_serial, K_AUTO, "set_selection with a serial the seat never sent is cancelled" },
    { "maximize", case_maximize, K_AUTO, "set_maximized: a configure with the state, and back" },
    { "fullscreen", case_fullscreen, K_AUTO, "set_fullscreen: the output's size and state, and back" },
    { "minimize", case_minimize, K_AUTO, "set_minimized: the suspended state" },
    { "title", case_title, K_SHOT, "set_title and set_app_id, changed, UTF-8, long, empty (title bar shots)" },
    { "shm-argb", case_shm_argb, K_SHOT, "ARGB8888 with alpha: opaque, 50%, transparent blocks (shot)" },
    { "damage", case_damage, K_SHOT, "damage_buffer and damage on parts of a buffer (3 shots)" },
    { "csd-geometry", case_csd_geometry, K_SHOT, "a client-side window with a smaller geometry has no corners of ours (2 shots)" },
    { "pointer", case_pointer, K_INPUT, "pointer enter, motion, buttons, wheel, leave (injected)" },
    { "keyboard", case_keyboard, K_INPUT, "keyboard enter, keys through the keymap, modifiers (injected)" },
    { "cursor-shape", case_cursor_shape, K_INPUT, "wp_cursor_shape_device_v1 shapes (injected pointer, 3 shots)" },
    { "keyless-click", case_keyless_click, K_INPUT, "a click on a keyless window activates it (injected)" },
    { "popup-click", case_popup_click, K_INPUT, "a click opens a grabbing menu, a click outside dismisses it (injected)" },
    { "close", case_close, K_INPUT, "Super+Q: xdg_toplevel.close, then a ping (injected)" },
    { "cursor-surface", case_cursor_surface, K_INPUT, "wl_pointer.set_cursor with the client's own surface, then none (injected pointer, 2 shots)" },
    { "clipboard-out", case_clipboard_out, K_INPUT, "the window's selection pasted into a terminal (injected)" },
    { "clipboard-in", case_clipboard_in, K_INPUT, "a terminal's selection read by the window (clipboard-in=TEXT)" },
    { "hostile-sub-storm", case_hostile_sub_storm, K_HOSTILE, "1,000 subsurfaces made and destroyed, trees at every cap, one past each: no_memory" },
    { "hostile-desync-flood", case_hostile_desync_flood, K_HOSTILE, "a desynchronized child drawn 2,000 times flat out, its parent idle" },
    { "hostile-popup-cap", case_hostile_popup_cap, K_HOSTILE, "grabbing popups 8 deep, a ninth; 32 popups, a 33rd: no_memory" },
    { "hostile-deco", case_hostile_deco, K_HOSTILE, "a decoration object's toplevel or wl_surface destroyed first: the errors" },
    { "hostile-handles", case_hostile_handles, K_HOSTILE, "28 pipe ends as pools; descriptors that can't cross the socket: EINVAL" },
    { "hostile-never-reads", case_hostile_never_reads, K_HOSTILE, "20,000 syncs never read: dropped as too slow" },
    { "hostile-regrab", case_hostile_regrab, K_INPUT, "a menu dismissed by a click outside can't grab straight back (injected)" },
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

static void list_cases(void)
{
    static const char *const kinds[] = { "auto", "shot", "input", "hostile" };
    for (unsigned i = 0; i < NCASES; i++)
        printf("%-16s %-5s %s\n", cases[i].name, kinds[cases[i].kind], cases[i].what);
    printf("%-16s %-5s %s\n", "hold", "-", "a window that stays (hold [seconds])");
}

static int run_case(unsigned i)
{
    struct probe p = { .name = cases[i].name };
    cur = &p;
    cases[i].fn();
    if (p.failed)
        printf("FAIL %s: %s\n", p.name, p.why);
    else
        printf("PASS %s: %s\n", p.name, p.seen[0] ? p.seen : "ok");
    cur = NULL;
    return p.failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-l")) {
            list_cases();
            return 0;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            g_hold = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            g_wait = atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: wlprobe [-s hold] [-t wait] <case>... | all | hold [seconds] | -l\n");
            return 2;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: wlprobe [-s hold] [-t wait] <case>... | all | hold [seconds] | -l\n");
        return 2;
    }
    if (!strcmp(argv[i], "hold"))
        return run_hold(i + 1 < argc ? atof(argv[i + 1]) : 0);
    int failed = 0, ran = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "all")) {
            int saved = g_hold;
            g_hold = 0;
            for (unsigned c = 0; c < NCASES; c++)
                if (cases[c].kind != K_INPUT && cases[c].kind != K_HOSTILE &&
                    strcmp(cases[c].name, "subcompositor") != 0 &&
                    strncmp(cases[c].name, "states-", 7) != 0) {
                    failed += run_case(c);
                    ran++;
                }
            g_hold = saved;
            continue;
        }
        /* name=text: the case's argument (clipboard-in's expected text) */
        char name[48];
        g_arg = NULL;
        const char *eq = strchr(argv[i], '=');
        snprintf(name, sizeof(name), "%.*s", eq ? (int)(eq - argv[i]) : 40, argv[i]);
        if (eq)
            g_arg = eq + 1;
        unsigned c = 0;
        while (c < NCASES && strcmp(cases[c].name, name) != 0)
            c++;
        if (c == NCASES) {
            printf("FAIL %s: no such case (wlprobe -l lists them)\n", argv[i]);
            failed++;
            continue;
        }
        failed += run_case(c);
        ran++;
    }
    printf("wlprobe: %d case(s), %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
