/* utest: the window manager with the real seat (bin/compositor headless,
 * compseat.h's fake input source and clients): what the user does to an
 * xdg toplevel with the mouse and the window keys, end to end. compwm.c
 * plays the seat itself for the window manager's details; here the
 * pieces are all real.
 *
 * t_wm_seat: one 100x80 toplevel, centred on the 640x480 output (its
 * title bar y 172..199, its close circle x 278..289, y 180..191): dragged
 * by its title bar; its close circle and Super+Q each send
 * xdg_toplevel.close and a ping,
 * which the client answers; Super+F asks it to fill the output; Super+T
 * tiles it (no title bar: a border all round).
 * t_wm_seat_cursors: the compositor's cursors over the same window: the
 * resize arrows on its edges and corners, the hand on its circles, the arrow
 * on it; then the client asks for the text bar and the hand by name
 * (wp-cursor-shape-v1), a stale serial ignored, a shape there isn't an
 * error. The pictures are compared pixel for pixel with cursors.c's. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <jwl.h>
#include <jwl/cursor_shape_v1.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "compseat.h"
#include "paint.h"
#include "utest.h"

#define U_Q   0x14
#define SUPER INPUT_MOD_LGUI
#define WIN_W 100
#define WIN_H 80
#define RED   0x00ff0000u

struct top {
    uint32_t wm, s, xs, top;
};

/* The newest xdg_surface.configure's serial since ct_clear (0: none), and
 * the toplevel's size in it. */
static uint32_t last_configure(const struct ct_client *k, const struct top *t, int32_t *w,
                               int32_t *h)
{
    uint32_t serial = 0;
    for (unsigned i = 0; i < k->nev; i++) {
        const struct ct_event *e = &k->ev[i];
        if (e->iface == &jwl_xdg_surface_interface && e->id == t->xs)
            serial = e->u[0];
        if (e->iface == &jwl_xdg_toplevel_interface && e->id == t->top &&
            e->op == JWL_XDG_TOPLEVEL_EV_CONFIGURE) {
            *w = (int32_t)e->u[0];
            *h = (int32_t)e->u[1];
        }
    }
    return serial;
}

/* A red WIN_W x WIN_H toplevel, mapped. */
static bool toplevel(struct sc *a, struct top *t)
{
    struct ct_client *k = &a->k;
    CHECK((t->wm = ct_new(k, &jwl_xdg_wm_base_interface, 1)) != 0);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 5, "xdg_wm_base", 1, t->wm), OK);
    t->s = ct_new(k, &jwl_wl_surface_interface, 4);
    t->xs = ct_new(k, &jwl_xdg_surface_interface, 1);
    t->top = ct_new(k, &jwl_xdg_toplevel_interface, 1);
    CHECK_ST(jwl_wl_compositor_create_surface(k->c, k->compositor, t->s), OK);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(k->c, t->wm, t->xs, t->s), OK);
    CHECK_ST(jwl_xdg_surface_get_toplevel(k->c, t->xs, t->top), OK);
    CHECK_ST(jwl_xdg_toplevel_set_title(k->c, t->top, "seat test"), OK);
    ct_clear(k);
    CHECK_ST(jwl_wl_surface_commit(k->c, t->s), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    int32_t w = -1, h = -1;
    uint32_t serial = last_configure(k, t, &w, &h);
    CHECK(serial && w == 0 && h == 0);
    CHECK_ST(jwl_xdg_surface_ack_configure(k->c, t->xs, serial), OK);
    static uint32_t px[WIN_W * WIN_H];
    for (unsigned i = 0; i < WIN_W * WIN_H; i++)
        px[i] = RED;
    handle_t vmo = HANDLE_INVALID;
    uint32_t pool = ct_pool(k, sizeof(px), &vmo), buf = ct_new(k, &jwl_wl_buffer_interface, 1);
    CHECK(pool && buf);
    CHECK_ST(jam_vmo_write(vmo, 0, px, sizeof(px)), OK);
    jam_handle_close(vmo);
    CHECK_ST(jwl_wl_shm_pool_create_buffer(k->c, pool, buf, 0, WIN_W, WIN_H, WIN_W * 4,
                                           JWL_WL_SHM_FORMAT_XRGB8888), OK);
    CHECK_ST(jwl_wl_surface_attach(k->c, t->s, buf, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_commit(k->c, t->s), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    return true;
}

/* The pointer to output (x, y): the window's pixel (10, 10) is there (the
 * client's last motion says so). */
static bool at(struct cs *t, struct sc *a, int32_t x, int32_t y)
{
    ct_clear(&a->k);
    CHECK(cs_pointer_to(t, x - 10, y - 10));   /* from its top-left corner, */
    CHECK(cs_pointer_to(t, x, y));             /* in: the last motion is here */
    CHECK(cs_sync(a, NULL));
    CHECK(motion_at(a, AT(10), AT(10)));
    return true;
}

/* A close came, and a ping, which the client answers. */
static bool closed_and_pinged(struct sc *a, const struct top *t)
{
    CHECK(ct_await(&a->k, &jwl_xdg_toplevel_interface, JWL_XDG_TOPLEVEL_EV_CLOSE, t->top,
                   CT_WAIT));
    const struct ct_event *e = ct_await(&a->k, &jwl_xdg_wm_base_interface,
                                        JWL_XDG_WM_BASE_EV_PING, t->wm, CT_WAIT);
    CHECK(e);
    CHECK_ST(jwl_xdg_wm_base_pong(a->k.c, t->wm, e->u[0]), OK);
    CHECK(cs_sync(a, NULL));
    ct_clear(&a->k);
    return true;
}

static bool mouse_steps(struct cs *t, struct sc *a, const struct top *w)
{
    int32_t x0 = (OUT_W - WIN_W) / 2, y0 = (OUT_H - WIN_H) / 2;
    /* centred: its pixel (10, 10) is the output's (x0 + 10, y0 + 10) */
    CHECK(at(t, a, x0 + 10, y0 + 10));
    /* dragged by its title bar (right of the circles): 50 right, 30 down;
     * no client sees the drag */
    CHECK(cs_pointer_to(t, x0 + 80, y0 - 10));
    ct_clear(&a->k);
    CHECK(cs_button(t, true));
    CHECK(cs_pointer_to(t, x0 + 130, y0 + 20));
    CHECK(cs_button(t, false));
    CHECK(cs_sync(a, NULL));
    CHECK(!find_ev(a, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    CHECK(at(t, a, x0 + 50 + 10, y0 + 30 + 10));
    ct_clear(&a->k);
    /* its close circle: a click asks the client to close */
    CHECK(cs_pointer_to(t, x0 + 50 - DECO_OUTLINE + LOOK_BTN_LEFT + LOOK_BTN_D / 2,
                        y0 + 30 - COMP_TITLE_H + LOOK_BTN_TOP + LOOK_BTN_D / 2));
    CHECK(cs_button(t, true));
    CHECK(cs_button(t, false));
    return closed_and_pinged(a, w);
}

static bool key_steps(struct cs *t, struct sc *a, const struct top *w)
{
    int32_t cw = -1, ch = -1;
    /* Super+Q: the same, without a close circle */
    CHECK(cs_tap(t, U_Q, SUPER));
    CHECK(closed_and_pinged(a, w));
    /* Super+F: the whole output */
    CHECK(cs_tap(t, U_F, SUPER));
    CHECK(cs_sync(a, NULL));
    CHECK(last_configure(&a->k, w, &cw, &ch) && cw == OUT_W && ch == OUT_H);
    CHECK(cs_tap(t, U_F, SUPER));
    CHECK(cs_sync(a, NULL));
    CHECK(last_configure(&a->k, w, &cw, &ch) && cw == WIN_W && ch == WIN_H);
    ct_clear(&a->k);
    /* Super+T: tiled, alone: the output less the gap (6) and a border (2) */
    CHECK(cs_tap(t, U_T, SUPER));
    CHECK(cs_sync(a, NULL));
    CHECK(last_configure(&a->k, w, &cw, &ch) && cw == OUT_W - 16 && ch == OUT_H - 16);
    CHECK(cs_tap(t, U_T, SUPER));
    CHECK(cs_sync(a, NULL));
    CHECK(last_configure(&a->k, w, &cw, &ch) && cw == WIN_W && ch == WIN_H);
    CHECK(!a->k.errored);
    return true;
}

bool t_wm_seat(void)
{
    static struct sc a;
    struct cs t;
    struct top w;
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    bool ok = toplevel(&a, &w) && mouse_steps(&t, &a, &w) && key_steps(&t, &a, &w);
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return ok;
}

/* ---- the cursor ------------------------------------------------------------------------- */

/* The output's pixel (x, y) becomes want within CT_WAIT (a paint comes). */
static bool pixel_becomes(const struct cs *t, int32_t x, int32_t y, uint32_t want)
{
    uint64_t until = now() + CT_WAIT;
    while (t->p.image[y * OUT_W + x] != want && now() < until)
        jam_nanosleep(now() + NS_PER_MS);
    if (t->p.image[y * OUT_W + x] != want)
        FAIL("pixel (%d, %d) is %06x, want %06x", x, y, t->p.image[y * OUT_W + x], want);
    return true;
}

/* Shape s's picture drawn over the red window with its hot spot at (x, y),
 * pixel for pixel, within CT_WAIT. */
static bool cursor_is(const struct cs *t, enum cursor_shape s, int32_t x, int32_t y)
{
    const struct cursor_image *img = cursors_get(s, 0);
    uint64_t until = now() + CT_WAIT;
    for (;;) {
        unsigned bad = 0;
        for (int32_t j = 0; j < img->h; j++)
            for (int32_t i = 0; i < img->w; i++) {
                int32_t ox = x - img->hot_x + i, oy = y - img->hot_y + j;
                bad += t->p.image[oy * OUT_W + ox] != px_over(RED, img->px[j * img->w + i]);
            }
        if (!bad)
            return true;
        if (now() >= until)
            FAIL("the cursor at (%d, %d) isn't shape %d: %u pixels differ", x, y, s, bad);
        jam_nanosleep(now() + NS_PER_MS);
    }
}

/* The compositor's own cursors over the window's frame: the resize arrows
 * on its edges and corners (all fill at their middle, the hot spot), the
 * hand on its circles; the arrow on its surface (the client asked for
 * nothing). */
static bool frame_cursor_steps(struct cs *t)
{
    int32_t x0 = (OUT_W - WIN_W) / 2, y0 = (OUT_H - WIN_H) / 2;
    const uint32_t fill = 0xf6f3f8;
    CHECK(cs_pointer_to(t, x0 + WIN_W + 3, y0 + WIN_H / 2));     /* the right edge's grab */
    CHECK(pixel_becomes(t, x0 + WIN_W + 3, y0 + WIN_H / 2, fill));
    CHECK(cs_pointer_to(t, x0 + WIN_W + 3, y0 + WIN_H + 3));     /* bottom right */
    CHECK(pixel_becomes(t, x0 + WIN_W + 3, y0 + WIN_H + 3, fill));
    CHECK(cs_pointer_to(t, x0 + WIN_W / 2, y0 + WIN_H + 3));     /* the bottom */
    CHECK(pixel_becomes(t, x0 + WIN_W / 2, y0 + WIN_H + 3, fill));
    /* the hand on the close circle: its palm all fill (the SVG's (14, 14)) */
    int32_t cx = x0 - DECO_OUTLINE + LOOK_BTN_LEFT + LOOK_BTN_D / 2;
    int32_t cy = y0 - COMP_TITLE_H + LOOK_BTN_TOP + LOOK_BTN_D / 2;
    CHECK(cs_pointer_to(t, cx, cy));
    const struct cursor_image *hand = cursors_get(CURSOR_HAND, 0);
    CHECK(pixel_becomes(t, cx - hand->hot_x + 14 + CURSOR_PAD, cy - hand->hot_y + 14 + CURSOR_PAD,
                        fill));
    /* the window's middle: the arrow, the client's default */
    CHECK(cs_pointer_to(t, x0 + WIN_W / 2, y0 + WIN_H / 2));
    CHECK(cursor_is(t, CURSOR_ARROW, x0 + WIN_W / 2, y0 + WIN_H / 2));
    return true;
}

/* wp-cursor-shape-v1: the client asks for the text bar, then the hand;
 * a wrong serial is ignored; a shape there isn't is an error. */
static bool shape_steps(struct cs *t, struct sc *a)
{
    int32_t x = (OUT_W - WIN_W) / 2 + WIN_W / 2, y = (OUT_H - WIN_H) / 2 + WIN_H / 2;
    struct ct_client *k = &a->k;
    uint32_t m = ct_new(k, &jwl_wp_cursor_shape_manager_v1_interface, 2);
    uint32_t d = ct_new(k, &jwl_wp_cursor_shape_device_v1_interface, 2);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 6, "wp_cursor_shape_manager_v1", 2, m), OK);
    CHECK_ST(jwl_wp_cursor_shape_manager_v1_get_pointer(k->c, m, d, a->ptr), OK);
    /* the latest enter's serial: leave the window and come back */
    CHECK(cs_pointer_to(t, 5, 5));
    ct_clear(k);
    CHECK(cs_pointer_to(t, x, y));
    CHECK(cs_sync(a, NULL));
    uint32_t serial = 0;   /* the latest enter's (an older one may come in late) */
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface == &jwl_wl_pointer_interface && k->ev[i].op == JWL_WL_POINTER_EV_ENTER)
            serial = k->ev[i].u[0];
    CHECK(serial);
    CHECK_ST(jwl_wp_cursor_shape_device_v1_set_shape(k->c, d, serial,
                                                     JWL_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT), OK);
    CHECK(cs_sync(a, NULL));
    CHECK(cursor_is(t, CURSOR_TEXT, x, y));
    CHECK_ST(jwl_wp_cursor_shape_device_v1_set_shape(k->c, d, serial + 1,
                                                     JWL_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE), OK);
    CHECK_ST(jwl_wp_cursor_shape_device_v1_set_shape(
                 k->c, d, serial, JWL_WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER), OK);
    CHECK(cs_sync(a, NULL));
    CHECK(cursor_is(t, CURSOR_HAND, x, y));   /* the move with a stale serial: nothing */
    CHECK_ST(jwl_wp_cursor_shape_device_v1_set_shape(k->c, d, serial, 0), OK);
    CHECK(ct_expect_error(k, d, JWL_WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE));
    return true;
}

bool t_wm_seat_cursors(void)
{
    static struct sc a;
    struct cs t;
    struct top w;
    cursors_init();   /* the pictures the compositor draws, to compare with */
    CHECK(cs_start(&t));
    CHECK(cs_client(&t, &a));
    bool ok = toplevel(&a, &w) && frame_cursor_steps(&t) && shape_steps(&t, &a);
    ct_close(&a.k);
    CHECK(cs_stop(&t));
    return ok;
}
