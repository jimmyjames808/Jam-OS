/* utest: the compositor's seat (user/services/compositor: seat.c,
 * keyboard.c, pointer.c, focus.c, sources.c, ctl.c) headless, with a fake
 * input source on a compctl channel and test clients speaking Wayland: the
 * harness the seat's tests share (compseat.h), and the tests of what isn't
 * input routing (compinput.c has that).
 *
 * t_comp_seat_keymap: wl_seat (capabilities, name), the keymap (a VMO the
 * client can read and map but not write, resize or pass on; its first line
 * names the layout), repeat_info, the per-client cap on seat objects, no
 * wl_touch.
 * t_comp_seat_ctl: compctl's levels, blank, new_client, the source cap.
 * t_termkeys: <termkeys.h>'s decoding of a terminal's bytes. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/compctl.h>
#include <idl/input.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <keymap.h>
#include <os.h>
#include <termkeys.h>
#include "compseat.h"
#include "utest.h"

/* The compositor with the desktop or not, layout= (NULL: none, its
 * default) and one more argument (NULL: none). */
static bool start(struct cs *t, bool desk, const char *layout, const char *more)
{
    memset(t, 0, sizeof(*t));
    handle_t svc, ctl, init, image, note, mix, net;
    t->p.w = OUT_W;
    t->p.h = OUT_H;
    CHECK(ct_image_for(&t->p, &image));   /* what it paints, for the cursor's tests */
    CHECK_ST(jam_channel_create(&t->p.svc, &svc), OK);
    CHECK_ST(jam_channel_create(&t->ctl, &ctl), OK);
    CHECK_ST(jam_channel_create(&t->init, &init), OK);
    CHECK_ST(jam_channel_create(&t->note, &note), OK);
    CHECK_ST(jam_channel_create(&mix, &t->mix), OK);
    CHECK_ST(jam_channel_create(&net, &t->net), OK);
    CHECK_ST(new_job(&t->p.job), OK);
    const char *argv[7] = { "bin/compositor", "headless", "size=640x480", "testwin" };
    int argc = 4;
    if (layout)
        argv[argc++] = layout;
    if (!desk)
        argv[argc++] = "nodesk";
    if (more)
        argv[argc++] = more;
    struct spawn_handle x[] = { { SR_USER + 0, svc }, { SR_USER + 1, image },
                                { SR_USER + 2, ctl }, { SR_USER + 3, init },
                                { SR_USER + 5, note }, { SR_USER + 6, mix },
                                { SR_USER + 7, net } };
    struct spawn_args a = { .path = "bin/compositor", .argc = argc, .argv = argv,
                            .job = t->p.job,
                            .extra = x, .nextra = 7 };
    CHECK_ST(spawn(&a, &t->p.proc), OK);
    CHECK_ST(compctl_connect_input_within(t->ctl, CT_WAIT, &t->src), OK);
    CHECK_ST(input_ready_within(t->src, CT_WAIT, INPUT_READY_KEYBOARD, 0x1234, 1), OK);
    CHECK_ST(input_ready_within(t->src, CT_WAIT, INPUT_READY_MOUSE, 0x1234, 2), OK);
    t->x = OUT_W / 2;   /* where the pointer starts */
    t->y = OUT_H / 2;
    return true;
}

bool cs_start(struct cs *t)
{
    return start(t, false, "layout=floating", NULL);
}

bool cs_start_desk(struct cs *t)
{
    return start(t, true, "layout=floating", NULL);
}

bool cs_start_splash(struct cs *t)
{
    return start(t, true, "layout=floating", "splash");
}

bool cs_start_default(struct cs *t)
{
    return start(t, false, NULL, NULL);
}

bool cs_stop(struct cs *t)
{
    jam_handle_close(t->src);
    jam_handle_close(t->ctl);
    jam_handle_close(t->init);
    jam_handle_close(t->note);
    jam_handle_close(t->mix);
    jam_handle_close(t->net);
    return ct_stop(&t->p);
}

bool cs_client(struct cs *t, struct sc *c)
{
    CHECK(ct_open(&t->p, &c->k));
    CHECK(ct_bind_all(&c->k));
    struct jwl_conn *k = c->k.c;
    CHECK((c->seat = ct_new(&c->k, &jwl_wl_seat_interface, 5)) != 0);
    CHECK_ST(jwl_wl_registry_bind(k, c->k.registry, 4, "wl_seat", 5, c->seat), OK);
    CHECK((c->kb = ct_new(&c->k, &jwl_wl_keyboard_interface, 5)) != 0);
    CHECK_ST(jwl_wl_seat_get_keyboard(k, c->seat, c->kb), OK);
    CHECK((c->ptr = ct_new(&c->k, &jwl_wl_pointer_interface, 5)) != 0);
    CHECK_ST(jwl_wl_seat_get_pointer(k, c->seat, c->ptr), OK);
    CHECK_ST(ct_roundtrip(&c->k), OK);
    CHECK(!c->k.errored);
    return true;
}

/* A w x h window at (x, y) (the testwin power): its surface's id. */
uint32_t cs_window(struct sc *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct ct_client *k = &c->k;
    uint32_t size = ((uint32_t)(w * h * 4) + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    handle_t vmo = HANDLE_INVALID;
    uint32_t pool = ct_pool(k, size, &vmo), buf = pool ? ct_new(k, &jwl_wl_buffer_interface, 1) : 0;
    if (vmo != HANDLE_INVALID)
        jam_handle_close(vmo);
    uint32_t s = buf ? ct_new(k, &jwl_wl_surface_interface, 4) : 0;
    if (!s || jwl_wl_shm_pool_create_buffer(k->c, pool, buf, 0, w, h, w * 4,
                                            JWL_WL_SHM_FORMAT_XRGB8888) != OK ||
        jwl_wl_compositor_create_surface(k->c, k->compositor, s) != OK ||
        jwl_wl_surface_attach(k->c, s, buf, x, y) != OK || jwl_wl_surface_commit(k->c, s) != OK ||
        ct_roundtrip(k) != OK)
        return 0;
    return s;
}

/* ---- the fake source ---------------------------------------------------------------- */

bool cs_key(struct cs *t, uint16_t usage, uint8_t state, uint8_t mods)
{
    CHECK_ST(input_key_within(t->src, CT_WAIT, usage, state, mods, 0), OK);
    return true;
}

/* A key pressed and released with mods. */
bool cs_tap(struct cs *t, uint16_t usage, uint8_t mods)
{
    return cs_key(t, usage, INPUT_KEY_DOWN, mods) && cs_key(t, usage, INPUT_KEY_UP, mods);
}

bool cs_mouse(struct cs *t, int16_t dx, int16_t dy, int8_t wheel)
{
    CHECK_ST(input_mouse_within(t->src, CT_WAIT, dx, dy, wheel, t->buttons), OK);
    return true;
}

bool cs_button(struct cs *t, bool down)
{
    t->buttons = down ? 1 : 0;
    return cs_mouse(t, 0, 0, 0);
}

/* The pointer to (x, y) in steps of at most 6 counts (1:1, no acceleration). */
bool cs_pointer_to(struct cs *t, int32_t x, int32_t y)
{
    while (t->x != x || t->y != y) {
        int32_t dx = x - t->x, dy = y - t->y;
        dx = dx > 6 ? 6 : dx < -6 ? -6 : dx;
        dy = dy > 6 ? 6 : dy < -6 ? -6 : dy;
        CHECK(cs_mouse(t, (int16_t)dx, (int16_t)dy, 0));
        t->x += dx;
        t->y += dy;
    }
    return true;
}

/* Every client's events up to now are in. */
bool cs_sync(struct sc *a, struct sc *b)
{
    CHECK_ST(ct_roundtrip(&a->k), OK);
    if (b)
        CHECK_ST(ct_roundtrip(&b->k), OK);
    return true;
}

/* ---- reading what a client got -------------------------------------------------------- */

/* The keys (code << 1 | pressed) of k's wl_keyboard.key events, in order. */
unsigned keys_got(const struct ct_client *k, uint32_t *out, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < k->nev && n < max; i++)
        if (k->ev[i].iface == &jwl_wl_keyboard_interface && k->ev[i].op == JWL_WL_KEYBOARD_EV_KEY)
            out[n++] = k->ev[i].u[2] << 1 | (k->ev[i].u[3] == 1);
    return n;
}

bool no_keys(const struct ct_client *k)
{
    uint32_t got[4];
    unsigned n = keys_got(k, got, 4);
    if (n)
        FAIL("a key event (code %u) reached a client that shouldn't see it", got[0] >> 1);
    return true;
}

/* k's keys since the last ct_clear are exactly want[0..n). */
bool keys_are(const struct ct_client *k, const uint32_t *want, unsigned n)
{
    uint32_t got[32];
    unsigned m = keys_got(k, got, 32);
    for (unsigned i = 0; i < n || i < m; i++)
        if (i >= n || i >= m || got[i] != want[i])
            FAIL("key event %u: got %u %s, want %u %s (%u events, want %u)", i,
                 i < m ? got[i] >> 1 : 0, i < m && (got[i] & 1) ? "down" : "up",
                 i < n ? want[i] >> 1 : 0, i < n && (want[i] & 1) ? "down" : "up", m, n);
    return true;
}


const struct ct_event *find_ev(const struct sc *c, const struct jwl_interface *iface,
                                      uint16_t op)
{
    return ct_find(&c->k, iface, op, 0);
}

/* c's last wl_pointer.motion was to (x, y), wl_fixed. */
bool motion_at(const struct sc *c, uint32_t x, uint32_t y)
{
    const struct ct_event *last = NULL;
    for (unsigned i = 0; i < c->k.nev; i++)
        if (c->k.ev[i].iface == &jwl_wl_pointer_interface &&
            c->k.ev[i].op == JWL_WL_POINTER_EV_MOTION)
            last = &c->k.ev[i];
    if (!last)
        FAIL("no motion event");
    if (last->u[1] != x || last->u[2] != y)
        FAIL("the last motion was to %u/256, %u/256; want %u/256, %u/256", last->u[1], last->u[2],
             x, y);
    return true;
}
/* ---- the tests -------------------------------------------------------------------- */

/* The keymap c's keyboard was sent: readable and mappable, nothing more,
 * and it names its layout. */
static bool keymap_ok(struct sc *c)
{
    const struct ct_event *e = find_ev(c, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_KEYMAP);
    CHECK(e);
    CHECK_EQ(e->u[0], JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    CHECK_EQ(e->u[2], keymap_us.xkb_size);
    handle_t v = c->k.kept;
    CHECK(v != HANDLE_INVALID);
    uint64_t size = 0, addr = 0;
    CHECK_ST(jam_vmo_get_size(v, &size), OK);
    CHECK(size >= keymap_us.xkb_size);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, size, VMAR_READ, &addr), OK);
    const char *text = (const char *)(uintptr_t)addr;
    CHECK(!strncmp(text, "// jamos-keymap us:", 19));
    CHECK(keymap_of_xkb(text, keymap_us.xkb_size) == &keymap_us);
    CHECK(!memcmp(text, keymap_us.xkb, keymap_us.xkb_size));
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, size), OK);
    CHECK(jam_vmo_write(v, 0, "x", 1) != OK);              /* no write right */
    CHECK(jam_vmo_set_size(v, size * 2) != OK);           /* no resize right */
    handle_t d;
    CHECK(jam_handle_duplicate(v, RIGHT_SAME, &d) != OK);  /* not passed on */
    return true;
}

bool t_comp_seat_keymap(void)
{
    static struct sc a;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    const struct ct_event *e = find_ev(&a, &jwl_wl_seat_interface, JWL_WL_SEAT_EV_CAPABILITIES);
    CHECK(e && e->u[0] == (JWL_WL_SEAT_CAPABILITY_POINTER | JWL_WL_SEAT_CAPABILITY_KEYBOARD));
    e = find_ev(&a, &jwl_wl_seat_interface, JWL_WL_SEAT_EV_NAME);
    CHECK(e && !strcmp(e->s, "seat0"));
    CHECK(keymap_ok(&a));
    e = find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_REPEAT_INFO);
    CHECK(e && e->u[0] == 30 && e->u[1] == 500);
    /* nothing focused yet: no enter */
    CHECK(!find_ev(&a, &jwl_wl_keyboard_interface, JWL_WL_KEYBOARD_EV_ENTER));
    /* wl_touch isn't there; and at most 8 keyboards a client */
    static struct sc b;
    CHECK(cs_client(&t, &b));
    for (unsigned i = 1; i < 8; i++) {
        uint32_t kb = ct_new(&b.k, &jwl_wl_keyboard_interface, 5);
        CHECK_ST(jwl_wl_seat_get_keyboard(b.k.c, b.seat, kb), OK);
    }
    CHECK_ST(ct_roundtrip(&b.k), OK);
    uint32_t ninth = ct_new(&b.k, &jwl_wl_keyboard_interface, 5);
    CHECK_ST(jwl_wl_seat_get_keyboard(b.k.c, b.seat, ninth), OK);
    CHECK(ct_expect_error(&b.k, ninth, JWL_ERROR_NO_MEMORY));
    uint32_t touch = ct_new(&a.k, &jwl_wl_touch_interface, 5);
    CHECK_ST(jwl_wl_seat_get_touch(a.k.c, a.seat, touch), OK);
    CHECK(ct_expect_error(&a.k, a.seat, JWL_WL_SEAT_ERROR_MISSING_CAPABILITY));
    ct_close(&a.k);
    ct_close(&b.k);
    CHECK(cs_stop(&t));
    return true;
}


static bool ctl_levels(struct cs *t)
{
    handle_t in, h;
    CHECK_ST(compctl_new_client_within(t->ctl, CT_WAIT, 1, &in), OK);
    CHECK_ST(compctl_new_client_within(t->ctl, CT_WAIT, 3, &h), ERR_INVALID_ARGS);
    CHECK_ST(compctl_new_client_within(t->ctl, CT_WAIT, 0, &h), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_new_client_within(in, CT_WAIT, 1, &h), ERR_ACCESS_DENIED);
    CHECK_ST(compctl_new_client_within(in, CT_WAIT, 2, &h), ERR_ACCESS_DENIED);   /* ADMIN's */
    CHECK_ST(compctl_blank_within(in, CT_WAIT, 1), ERR_ACCESS_DENIED);
    uint64_t u[11];
    uint32_t v[4];
    CHECK_ST(compctl_stats_within(in, CT_WAIT, &u[0], &u[1], &u[2], &u[3], &v[0], &v[1], &v[2],
                                  &u[4], &u[5], &u[6], &u[7], &u[8], &v[3], &u[9], &u[10]),
             ERR_ACCESS_DENIED);
    CHECK_ST(compctl_connect_input_within(in, CT_WAIT, &h), OK);
    CHECK_ST(input_ready_within(h, CT_WAIT, INPUT_READY_MOUSE, 1, 2), OK);
    CHECK_ST(input_ready_within(h, CT_WAIT, 7, 1, 2), ERR_INVALID_ARGS);
    jam_handle_close(h);
    jam_handle_close(in);
    CHECK_ST(compctl_blank_within(t->ctl, CT_WAIT, 2), ERR_INVALID_ARGS);
    CHECK_ST(compctl_blank_within(t->ctl, CT_WAIT, 1), OK);
    CHECK_ST(compctl_blank_within(t->ctl, CT_WAIT, 0), OK);
    return true;
}

bool t_comp_seat_ctl(void)
{
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(ctl_levels(&t));
    /* 16 sources at once (the test's own is one of them) */
    handle_t more[16];
    for (unsigned i = 0; i < 15; i++)
        CHECK_ST(compctl_connect_input_within(t.ctl, CT_WAIT, &more[i]), OK);
    CHECK_ST(compctl_connect_input_within(t.ctl, CT_WAIT, &more[15]), ERR_NO_RESOURCES);
    for (unsigned i = 0; i < 15; i++)
        jam_handle_close(more[i]);
    /* a source going lets its slot be used again */
    uint64_t until = now() + CT_WAIT;
    status_t st;
    while ((st = compctl_connect_input_within(t.ctl, CT_WAIT, &more[0])) == ERR_NO_RESOURCES &&
           now() < until)
        jam_nanosleep(now() + NS_PER_MS);
    CHECK_ST(st, OK);
    jam_handle_close(more[0]);
    CHECK(cs_stop(&t));
    return true;
}

/* With xdg-shell offered: a toplevel of a's, the pointer on it. */
/* ---- <termkeys.h> ------------------------------------------------------------------- */

/* The keys of bytes s, as (usage << 8 | cp) words. */
static unsigned decode(const char *s, uint32_t *out)
{
    struct termkeys t = { 0 };
    unsigned n = 0;
    for (; *s; s++) {
        struct termkey k[2];
        unsigned got = termkeys_byte(&t, (uint8_t)*s, k);
        for (unsigned i = 0; i < got; i++)
            out[n++] = (uint32_t)k[i].usage << 8 | k[i].cp;
    }
    return n;
}

bool t_termkeys(void)
{
    uint32_t k[16];
    CHECK_EQ(decode("a\x03", k), 2);
    CHECK(k[0] == 'a' && k[1] == 3);
    CHECK_EQ(decode("\x1b[A\x1b[B\x1bOC\x1b[D\x1b[H\x1b[F", k), 6);
    CHECK(k[0] == 0x5200 && k[1] == 0x5100 && k[2] == 0x4f00 && k[3] == 0x5000 &&
          k[4] == 0x4a00 && k[5] == 0x4d00);
    CHECK_EQ(decode("\x1b[1~\x1b[4~\x1b[3~\x1b[5~\x1b[6~\x1b[9~\x1b[3;5~", k), 6);
    CHECK(k[0] == 0x4a00 && k[1] == 0x4d00 && k[2] == 0x4c00 && k[3] == 0x4b00 &&
          k[4] == 0x4e00 && k[5] == 0x4c00);
    /* CR, LF and CR LF: one Enter each; Backspace both ways; Tab */
    CHECK_EQ(decode("\r\n\n\r\x7f\x08\t", k), 6);
    CHECK(k[0] == 0x280a && k[1] == 0x280a && k[2] == 0x280a && k[3] == 0x2a08 &&
          k[4] == 0x2a08 && k[5] == 0x2b09);
    /* a lone ESC: Escape, then the byte as itself */
    CHECK_EQ(decode("\x1bx", k), 2);
    CHECK(k[0] == 0x291b && k[1] == 'x');
    /* a long parameter list is cut, not overflowed */
    CHECK_EQ(decode("\x1b[1111111111111111111111~z", k), 1);
    CHECK(k[0] == 'z');
    return true;
}
