/* utest: the fake compositor's other half (jwlcfake.h): what the tests
 * make it send (configures, input, close, ping), the thread a test with
 * blocking calls runs it on, and the helpers the libjwl client tests
 * share. jwlc_fake.c serves the requests. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <os.h>
#include "jwlcfake.h"
#include "utest.h"

/* ---- what the tests make the fake send ------------------------------------------------- */

struct fake_surface *fake_surface(struct fake *f, unsigned i)
{
    return i < FAKE_SURFACES && f->s[i].id ? &f->s[i] : NULL;
}

status_t fake_configure(struct fake *f, struct fake_surface *s, int32_t w, int32_t h,
                        uint32_t states)
{
    uint32_t list[32];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 32; i++)
        if (states >> i & 1)
            list[n++] = i;
    status_t st = jwl_xdg_toplevel_send_configure(f->conn, s->toplevel, w, h, list, n * 4);
    s->sent_serial = f->serial++;
    if (st == OK)
        st = jwl_xdg_surface_send_configure(f->conn, s->xdg, s->sent_serial);
    return st;
}

status_t fake_close(struct fake *f, struct fake_surface *s)
{
    return jwl_xdg_toplevel_send_close(f->conn, s->toplevel);
}

status_t fake_ping(struct fake *f)
{
    return jwl_xdg_wm_base_send_ping(f->conn, f->wm_base, f->serial++);
}

status_t fake_kb_enter(struct fake *f, struct fake_surface *s)
{
    return jwl_wl_keyboard_send_enter(f->conn, f->keyboard, f->serial++, s->id, NULL, 0);
}

status_t fake_key(struct fake *f, uint32_t code, bool pressed)
{
    return jwl_wl_keyboard_send_key(f->conn, f->keyboard, f->serial++, 1000, code,
                                    pressed ? JWL_WL_KEYBOARD_KEY_STATE_PRESSED
                                            : JWL_WL_KEYBOARD_KEY_STATE_RELEASED);
}

status_t fake_mods(struct fake *f, uint32_t depressed, uint32_t locked)
{
    return jwl_wl_keyboard_send_modifiers(f->conn, f->keyboard, f->serial++, depressed, 0,
                                          locked, 0);
}

status_t fake_ptr_enter(struct fake *f, struct fake_surface *s, int32_t x, int32_t y)
{
    return jwl_wl_pointer_send_enter(f->conn, f->pointer, f->serial++, s->id, x * 256, y * 256);
}

status_t fake_motion(struct fake *f, int32_t x, int32_t y)
{
    return jwl_wl_pointer_send_motion(f->conn, f->pointer, 1000, x * 256, y * 256);
}

status_t fake_button(struct fake *f, uint32_t button, bool pressed)
{
    return jwl_wl_pointer_send_button(f->conn, f->pointer, f->serial++, 1000, button,
                                      pressed ? JWL_WL_POINTER_BUTTON_STATE_PRESSED
                                              : JWL_WL_POINTER_BUTTON_STATE_RELEASED);
}

status_t fake_wheel(struct fake *f, int32_t notches)
{
    status_t st = jwl_wl_pointer_send_axis_discrete(f->conn, f->pointer, 0, notches);
    if (st == OK)
        st = jwl_wl_pointer_send_axis(f->conn, f->pointer, 1000, 0, notches * 10 * 256);
    if (st == OK)
        st = jwl_wl_pointer_send_frame(f->conn, f->pointer);
    return st;
}

/* ---- on a thread of its own ------------------------------------------------------------- */

static uint8_t thread_stack[64 * 1024] __attribute__((aligned(16)));

static void serve_loop(void *arg)
{
    struct fake *f = arg;
    while (!__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE)) {
        fake_serve(f);
        signals_t seen;
        if (f->conn)
            (void)jam_object_wait_one(f->conn->ch, SIG_READABLE | SIG_PEER_CLOSED,
                                      now() + 5 * NS_PER_MS, &seen);
        else
            (void)jam_nanosleep(now() + 2 * NS_PER_MS);
    }
}

status_t fake_thread_start(struct fake *f, handle_t *thread)
{
    __atomic_store_n(&f->stop, false, __ATOMIC_RELEASE);
    return thread_spawn("fake compositor", serve_loop, f, thread_stack, sizeof(thread_stack),
                        thread);
}

void fake_thread_stop(struct fake *f, handle_t thread)
{
    __atomic_store_n(&f->stop, true, __ATOMIC_RELEASE);
    signals_t seen;
    (void)jam_object_wait_one(thread, SIG_TERMINATED, now() + 5 * NS_PER_S, &seen);
    jam_handle_close(thread);
}

/* ---- test helpers ----------------------------------------------------------------------- */

struct jwl_client *fake_ready(struct fake *f)
{
    struct jwl_client_config cfg = fake_config(f);
    struct jwl_client *c;
    if (jwl_client_create(&cfg, &c) != OK)
        return NULL;
    if (!fake_pump(f, c) || jwl_client_status(c) != OK) {
        jwl_client_destroy(c);
        return NULL;
    }
    return c;
}

bool fake_next_of(struct jwl_client *c, uint32_t type, struct jwl_event *ev)
{
    while (jwl_client_next_event(c, ev) == OK)
        if (ev->type == type)
            return true;
    return false;
}

void fake_held(uint64_t *handles, uint64_t *bytes)
{
    struct job_info ji;
    *handles = *bytes = 0;
    if (jam_job_get_info(own_job(), &ji) == OK) {
        *handles = ji.used[JOB_LIMIT_HANDLES];
        *bytes = ji.used[JOB_LIMIT_MSG_BYTES];
    }
}

void fake_fill(const struct jwl_frame *fr, uint32_t px)
{
    for (int32_t y = 0; y < fr->height; y++)
        for (int32_t x = 0; x < fr->width; x++)
            fr->px[y * (fr->stride / 4) + x] = px;
}

/* Everything gone: the client, the fake, and what they held. */
bool fake_all_gone(struct fake *f, struct jwl_client *c, uint64_t h0, uint64_t b0)
{
    if (f->unexpected[0])
        FAIL("the fake saw %s", f->unexpected);
    jwl_client_destroy(c);
    fake_serve(f);
    fake_free(f);
    uint64_t h1, b1;
    fake_held(&h1, &b1);
    CHECK_EQ(h1, h0);
    CHECK_EQ(b1, b0);
    return true;
}

/* A window of size w x h, configured; its fake surface in *s. */
bool fake_window(struct fake *f, struct jwl_client *c, const struct jwl_window_config *wc,
                       unsigned slot, struct jwl_window **w, struct fake_surface **s)
{
    CHECK_ST(jwl_window_create(c, wc, w), OK);
    CHECK(!jwl_window_configured(*w));
    struct jwl_frame fr;
    CHECK_ST(jwl_window_begin(*w, &fr), ERR_BAD_STATE);
    CHECK(fake_pump(f, c));
    *s = fake_surface(f, slot);
    CHECK(*s && (*s)->xdg && (*s)->toplevel && (*s)->sent_serial);
    CHECK_EQ((*s)->commits, 1);   /* the first, with no buffer */
    struct jwl_event ev;
    CHECK(fake_next_of(c, JWL_EV_CONFIGURE, &ev) && ev.win == *w && !ev.configure.rebuilt);
    CHECK(jwl_window_configured(*w));
    return true;
}

/* Draw px into the next buffer and present it with damage r (NULL: all). */
bool fake_draw(struct jwl_window *w, uint32_t px, const struct jwl_rect *r, unsigned *slot)
{
    struct jwl_frame fr;
    CHECK_ST(jwl_window_begin(w, &fr), OK);
    fake_fill(&fr, px);
    *slot = fr.slot;
    CHECK_ST(jwl_window_present(w, r, r ? 1 : 0, true), OK);
    return true;
}
