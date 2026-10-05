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
 * tiles it (no title bar: a border all round). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "compseat.h"
#include "look.h"
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
