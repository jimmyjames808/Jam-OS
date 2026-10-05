/* wltest --script: checks against a real compositor, each step a line,
 * the last "wltest: PASS" or "wltest: FAIL":
 *   connect    the registry and the globals (wl_compositor and wl_shm at
 *              least, xrgb8888 offered), what was bound printed;
 *   buffers    a kept pool and two 64x64 buffers of known colours in it,
 *              which the compositor maps when the pool is made;
 *   window     with xdg_wm_base: a window, its configure, frames drawn
 *              and answered by frame callbacks, one after the other;
 *   surface    without it (the compositor before its xdg-shell): a
 *              surface of our own, the two buffers attached by turns,
 *              each commit's frame callback answered, the buffer a commit
 *              replaced released;
 *   roundtrip  wl_display.sync answered;
 *   reconnect  (--spawn only) the compositor killed: the connection goes,
 *              wltest's connect function starts another, libjwl binds
 *              again and makes the pool, buffers and window again; frames
 *              go on (a new surface of our own without xdg_wm_base). */
#define CHECK_PROG "wltest"
#define CHECK_CUR  step
#include <check.h>
#include <jwl_client.h>
#include <os.h>
#include "wltest.h"

#define SIDE       64
#define STEP_WAIT  (3 * NS_PER_S)   /* frame callbacks of a surface no window shows: 1 a second */
#define BACK_WAIT  (8 * NS_PER_S)   /* a compositor killed and another started */

static const char *step = "start";

struct pixels {
    struct jwl_pool   *pool;
    struct jwl_buffer *b[2];
};

static bool connected(const struct jwl_client_config *cfg, struct jwl_client **c)
{
    step = "connect";
    CHECK_ST(jwl_client_connect(cfg, now() + STEP_WAIT, c), OK);
    const struct jwl_client_info *in = jwl_client_info(*c);
    wl_print_info(in);
    CHECK(in->compositor_version >= 1 && in->shm_version >= 1);
    CHECK(in->shm_formats >> JWL_WL_SHM_FORMAT_XRGB8888 & 1);
    return true;
}

static bool buffers(struct jwl_client *c, struct pixels *px)
{
    step = "buffers";
    CHECK_ST(jwl_pool_create(c, 2 * SIDE * SIDE * 4, &px->pool), OK);
    static const uint32_t colour[2] = { 0xffc03030u, 0xff30c030u };
    for (unsigned i = 0; i < 2; i++) {
        CHECK_ST(jwl_buffer_create(px->pool, i * SIDE * SIDE * 4, SIDE, SIDE, SIDE * 4,
                                   JWL_WL_SHM_FORMAT_XRGB8888, &px->b[i]), OK);
        uint32_t *p = jwl_buffer_pixels(px->b[i]);
        for (unsigned k = 0; k < SIDE * SIDE; k++)
            p[k] = colour[i];
    }
    CHECK_ST(jwl_client_roundtrip(c, now() + STEP_WAIT), OK);   /* made without complaint */
    printf("wltest: buffers: a %u-byte kept pool, two %ux%u buffers\n", 2 * SIDE * SIDE * 4,
           SIDE, SIDE);
    return true;
}

/* Until b is released (or deadline). */
static bool released(struct jwl_client *c, const struct jwl_buffer *b, uint64_t deadline)
{
    while (jwl_buffer_busy(b) && now() < deadline) {
        bool never = false;
        (void)wl_wait_flag(c, &never, now() + 10 * NS_PER_MS);
    }
    return !jwl_buffer_busy(b);
}

/* Show buffer b on surface sid and wait for the commit's frame callback. */
static bool show(struct jwl_client *c, uint32_t sid, struct jwl_buffer *b, unsigned i)
{
    struct jwl_conn *k = jwl_client_conn(c);
    struct jwl_callback cb = { 0 };
    CHECK(released(c, b, now() + STEP_WAIT));
    CHECK_ST(jwl_wl_surface_attach(k, sid, jwl_buffer_id(b), 0, 0), OK);
    jwl_buffer_attached(b);
    CHECK_ST(jwl_wl_surface_damage(k, sid, 0, 0, SIDE, SIDE), OK);
    CHECK_ST(jwl_client_frame(c, sid, &cb), OK);
    CHECK_ST(jwl_wl_surface_commit(k, sid), OK);
    CHECK_ST(jwl_client_flush(c), OK);
    uint64_t t0 = now();
    CHECK(wl_wait_flag(c, &cb.done, now() + STEP_WAIT));
    printf("wltest: surface: frame %u answered after %llu ms (time %u)\n", i,
           (unsigned long long)((now() - t0) / NS_PER_MS), cb.time);
    return true;
}

static bool surface_frames(struct jwl_client *c, struct pixels *px, unsigned frames)
{
    step = "surface";
    struct jwl_conn *k = jwl_client_conn(c);
    uint32_t comp = jwl_client_global(c, &jwl_wl_compositor_interface), sid;
    CHECK(k && comp);
    CHECK_ST(jwl_conn_make(k, &jwl_wl_surface_interface, jwl_client_info(c)->compositor_version,
                           NULL, &sid), OK);
    CHECK_ST(jwl_wl_compositor_create_surface(k, comp, sid), OK);
    for (unsigned i = 0; i < frames; i++)
        CHECK(show(c, sid, px->b[i % 2], i));
    if (frames >= 2)
        CHECK(released(c, px->b[frames % 2], now() + STEP_WAIT));   /* replaced: released */
    CHECK_ST(jwl_wl_surface_destroy(k, sid), OK);
    CHECK_ST(jwl_client_flush(c), OK);
    return true;
}

static bool window_frames(struct jwl_client *c, unsigned frames, struct jwl_window **w)
{
    step = "window";
    struct jwl_window_config wc = { .width = 160, .height = 100, .title = "wltest script",
                                    .app_id = "wltest" };
    struct jwl_event ev;
    if (!*w) {
        CHECK_ST(jwl_window_create(c, &wc, w), OK);
        CHECK(wl_wait_event(c, JWL_EV_CONFIGURE, now() + STEP_WAIT, &ev));
    }
    for (unsigned i = 0; i < frames; i++) {
        struct jwl_frame fr;
        uint64_t until = now() + STEP_WAIT;
        status_t st;
        while ((st = jwl_window_begin(*w, &fr)) == ERR_SHOULD_WAIT && now() < until) {
            bool never = false;
            (void)wl_wait_flag(c, &never, now() + 10 * NS_PER_MS);
        }
        CHECK_ST(st, OK);
        wl_draw(&fr, i);
        CHECK_ST(jwl_window_present(*w, NULL, 0, true), OK);
        CHECK(wl_wait_event(c, JWL_EV_FRAME, now() + STEP_WAIT, &ev) && ev.win == *w);
        printf("wltest: window: frame %u (%dx%d, slot %u) answered\n", i, fr.width, fr.height,
               fr.slot);
    }
    return true;
}

static bool again(struct jwl_client *c, struct comp_child *k, struct pixels *px,
                  struct jwl_window **w)
{
    step = "reconnect";
    comp_kill(k);
    struct jwl_event ev;
    CHECK(wl_wait_event(c, JWL_EV_RECONNECTED, now() + BACK_WAIT, &ev));
    CHECK(jwl_client_info(c)->generation == 2 && k->starts == 2);
    CHECK(jwl_buffer_id(px->b[0]) && jwl_buffer_id(px->b[1]));   /* made again */
    if (*w) {
        CHECK(wl_wait_event(c, JWL_EV_CONFIGURE, now() + STEP_WAIT, &ev));
        CHECK(ev.configure.rebuilt);
        return window_frames(c, 2, w);
    }
    return surface_frames(c, px, 2);
}

int wl_script(const struct jwl_client_config *cfg, struct comp_child *k, unsigned frames)
{
    struct jwl_client *c = NULL;
    struct pixels px = { 0 };
    struct jwl_window *w = NULL;
    bool ok = connected(cfg, &c) && buffers(c, &px);
    bool windows = ok && jwl_client_info(c)->wm_base_version;
    if (ok && !windows)
        printf("wltest: no xdg_wm_base yet: a surface of our own instead of a window\n");
    ok = ok && (windows ? window_frames(c, frames, &w) : surface_frames(c, &px, frames));
    step = "roundtrip";
    ok = ok && jwl_client_roundtrip(c, now() + STEP_WAIT) == OK;
    ok = ok && (!k || again(c, k, &px, &w));
    if (c) {
        jwl_window_destroy(w);
        for (unsigned i = 0; i < 2; i++)
            jwl_buffer_destroy(px.b[i]);
        jwl_pool_destroy(px.pool);
        jwl_client_destroy(c);
    }
    printf("wltest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
