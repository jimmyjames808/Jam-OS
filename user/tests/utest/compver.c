/* utest: the protocol versions bin/compositor offers above G1's (comp.h's
 * table: wl_compositor 6, wl_shm 2, wl_output 4, wl_seat 9, xdg_wm_base 6)
 * and xdg-decoration (xdgdeco.c), on the compositor run headless and
 * driven over real channels as comp.c and compxdg.c drive it: each new
 * event at the version that has it and not below, each new request
 * answered or refused as its protocol says, the decoration modes by their
 * pixels (a title bar or none: the owner's rule, a client that bound the
 * manager gets ours) and by a hostile client, and a client-side window
 * placed by its window geometry, its own shadow round it and none of ours.
 *
 * The output is 320x240 with the desktop off, floating unless a test says;
 * a 64x64 window lands centred, its surface at (128, 88), a title bar's
 * height above that when the compositor draws one. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_decoration_unstable_v1.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "compseat.h"
#include "comptest.h"
#include "look.h"
#include "utest.h"

#define W     320
#define H     240
#define SIDE  64
#define SX    ((W - SIDE) / 2)    /* a SIDE x SIDE window's surface, centred */
#define SY    ((H - SIDE) / 2)
#define TITLE_Y (SY - COMP_TITLE_H / 2)   /* the middle of its title bar, if any */
#define POOL  (1u << 20)
#define RED   0x00ff0000u
#define GREEN 0x0000ff00u

#define CLIENT_SIDE JWL_ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
#define SERVER_SIDE JWL_ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE

/* A client: the core globals, xdg_wm_base at wm_v (0: not bound), a seat
 * 5 with a keyboard (its windows take the focus), a pool, and the
 * decoration manager bound (decos 0: not, by vc_open_bare). */
struct vc {
    struct ct_client k;
    uint32_t wm, seat, kb, pool, decos;
    handle_t vmo;
};

/* A toplevel's objects, its decoration object (0: none). */
struct vt {
    uint32_t s, xs, top, deco;
};

/* x binds the decoration manager (global 9) now. */
static bool vc_bind_decos(struct vc *x)
{
    CHECK((x->decos = ct_new(&x->k, &jwl_zxdg_decoration_manager_v1_interface, 1)) != 0);
    CHECK_ST(jwl_wl_registry_bind(x->k.c, x->k.registry, 9, "zxdg_decoration_manager_v1", 1,
                                  x->decos), OK);
    return true;
}

/* vc_open without the decoration manager: a client that never heard of it
 * (it draws its own decorations). */
static bool vc_open_bare(struct ct_comp *p, struct vc *x, uint32_t wm_v)
{
    *x = (struct vc){ .vmo = HANDLE_INVALID };
    CHECK(ct_open(p, &x->k));
    CHECK(ct_bind_all(&x->k));
    if (wm_v) {
        CHECK((x->wm = ct_new(&x->k, &jwl_xdg_wm_base_interface, wm_v)) != 0);
        CHECK_ST(jwl_wl_registry_bind(x->k.c, x->k.registry, 5, "xdg_wm_base", wm_v, x->wm), OK);
    }
    CHECK((x->seat = ct_new(&x->k, &jwl_wl_seat_interface, 5)) != 0);
    CHECK_ST(jwl_wl_registry_bind(x->k.c, x->k.registry, 4, "wl_seat", 5, x->seat), OK);
    CHECK((x->kb = ct_new(&x->k, &jwl_wl_keyboard_interface, 5)) != 0);
    CHECK_ST(jwl_wl_seat_get_keyboard(x->k.c, x->seat, x->kb), OK);
    CHECK((x->pool = ct_pool(&x->k, POOL, &x->vmo)) != 0);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    return true;
}

static bool vc_open(struct ct_comp *p, struct vc *x, uint32_t wm_v)
{
    CHECK(vc_open_bare(p, x, wm_v));
    CHECK(vc_bind_decos(x));
    CHECK_ST(ct_roundtrip(&x->k), OK);
    return true;
}

static void vc_close(struct vc *x)
{
    if (x->vmo != HANDLE_INVALID)
        jam_handle_close(x->vmo);
    x->vmo = HANDLE_INVALID;
    ct_close(&x->k);
}

/* A toplevel of x's (at its xdg_wm_base's version), no initial commit yet. */
static bool toplevel(struct vc *x, struct vt *t)
{
    uint32_t v = jwl_conn_object(x->k.c, x->wm)->version;
    *t = (struct vt){ 0 };
    CHECK((t->s = ct_new(&x->k, &jwl_wl_surface_interface, 4)) != 0);
    CHECK_ST(jwl_wl_compositor_create_surface(x->k.c, x->k.compositor, t->s), OK);
    CHECK((t->xs = ct_new(&x->k, &jwl_xdg_surface_interface, v)) != 0);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(x->k.c, x->wm, t->xs, t->s), OK);
    CHECK((t->top = ct_new(&x->k, &jwl_xdg_toplevel_interface, v)) != 0);
    CHECK_ST(jwl_xdg_surface_get_toplevel(x->k.c, t->xs, t->top), OK);
    return true;
}

/* t's decoration object (mode 0: none set). */
static bool decoration(struct vc *x, struct vt *t, uint32_t mode)
{
    CHECK((t->deco = ct_new(&x->k, &jwl_zxdg_toplevel_decoration_v1_interface, 1)) != 0);
    CHECK_ST(jwl_zxdg_decoration_manager_v1_get_toplevel_decoration(x->k.c, x->decos, t->deco,
                                                                    t->top), OK);
    if (mode)
        CHECK_ST(jwl_zxdg_toplevel_decoration_v1_set_mode(x->k.c, t->deco, mode), OK);
    return true;
}

static const struct ct_event *last(const struct ct_client *k, const struct jwl_interface *iface,
                                   uint16_t op, uint32_t id)
{
    for (unsigned i = k->nev; i-- > 0;)
        if (k->ev[i].iface == iface && k->ev[i].op == op && k->ev[i].id == id)
            return &k->ev[i];
    return NULL;
}

static unsigned count(const struct ct_client *k, const struct jwl_interface *iface, uint16_t op,
                      uint32_t id)
{
    unsigned n = 0;
    for (unsigned i = 0; i < k->nev; i++)
        n += k->ev[i].iface == iface && k->ev[i].op == op && k->ev[i].id == id;
    return n;
}

/* Everything said so far; t's newest configure's serial (0: none), its
 * toplevel configure in *top. The log is kept (ct_clear is the caller's). */
static uint32_t configured(struct vc *x, const struct vt *t, const struct ct_event **top)
{
    if (ct_roundtrip(&x->k) != OK)
        return 0;
    const struct ct_event *e = last(&x->k, &jwl_xdg_surface_interface,
                                    JWL_XDG_SURFACE_EV_CONFIGURE, t->xs);
    *top = last(&x->k, &jwl_xdg_toplevel_interface, JWL_XDG_TOPLEVEL_EV_CONFIGURE, t->top);
    return e && *top ? e->u[0] : 0;
}

/* Does configure e carry state? */
static bool has_state(const struct ct_event *e, uint32_t state)
{
    for (uint32_t i = 0; i < e->na && i < 8; i++)
        if (e->a[i] == state)
            return true;
    return false;
}

/* A side x side buffer of colour at the pool's start. */
static uint32_t buffer(struct vc *x, int32_t side, uint32_t colour)
{
    static uint32_t row[W];
    for (int32_t i = 0; i < side; i++)
        row[i] = colour;
    for (int32_t y = 0; y < side; y++)
        if (jam_vmo_write(x->vmo, (uint64_t)y * (uint64_t)side * 4, row, (uint64_t)side * 4) != OK)
            return 0;
    uint32_t id = ct_new(&x->k, &jwl_wl_buffer_interface, 1);
    if (!id || jwl_wl_shm_pool_create_buffer(x->k.c, x->pool, id, 0, side, side, side * 4,
                                             JWL_WL_SHM_FORMAT_XRGB8888) != OK)
        return 0;
    return id;
}

/* Ack serial (0: none) and commit buffer b (0: none) on t. */
static bool ack_commit(struct vc *x, const struct vt *t, uint32_t serial, uint32_t b)
{
    if (serial)
        CHECK_ST(jwl_xdg_surface_ack_configure(x->k.c, t->xs, serial), OK);
    if (b) {
        CHECK_ST(jwl_wl_surface_attach(x->k.c, t->s, b, 0, 0), OK);
        CHECK_ST(jwl_wl_surface_damage_buffer(x->k.c, t->s, 0, 0, SIDE, SIDE), OK);
    }
    CHECK_ST(jwl_wl_surface_commit(x->k.c, t->s), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    return true;
}

/* The initial commit, then its configure acked with a SIDE x SIDE red
 * buffer: mapped, and shown once the centre is red. */
static bool map_red(struct ct_comp *p, struct vc *x, const struct vt *t)
{
    const struct ct_event *top;
    CHECK(ack_commit(x, t, 0, 0));
    uint32_t serial = configured(x, t, &top);
    CHECK(serial);
    CHECK(ack_commit(x, t, serial, buffer(x, SIDE, RED)));
    uint64_t until = now() + CT_WAIT;
    const volatile uint32_t *img = p->image;
    while (img[(SY + SIDE / 2) * W + SX + SIDE / 2] != RED && now() < until)
        jam_sleep_until(now() + NS_PER_MS);
    CHECK_EQ(img[(SY + SIDE / 2) * W + SX + SIDE / 2], RED);
    return true;
}

/* Wait (up to CT_WAIT) until pixel (px, py) is want (same) or isn't (!same). */
static bool pixel_is(const struct ct_comp *p, int32_t px, int32_t py, uint32_t want, bool same)
{
    uint64_t until = now() + CT_WAIT;
    const volatile uint32_t *img = p->image;
    while ((img[py * W + px] == want) != same && now() < until)
        jam_sleep_until(now() + NS_PER_MS);
    if ((img[py * W + px] == want) != same)
        FAIL("pixel %d,%d is %06x, want %s%06x", px, py, img[py * W + px], same ? "" : "not ",
             want);
    return true;
}

/* ---- the core interfaces --------------------------------------------------------------- */

/* wl_compositor 6: a surface told its preferred scale and transform; at 4,
 * nothing. */
static bool surface_preferences(struct ct_client *k)
{
    uint32_t comp6 = ct_new(k, &jwl_wl_compositor_interface, 6);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 1, "wl_compositor", 6, comp6), OK);
    uint32_t s6 = ct_new(k, &jwl_wl_surface_interface, 6);
    uint32_t s4 = ct_new(k, &jwl_wl_surface_interface, 4);
    CHECK_ST(jwl_wl_compositor_create_surface(k->c, comp6, s6), OK);
    CHECK_ST(jwl_wl_compositor_create_surface(k->c, k->compositor, s4), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    const struct ct_event *e = ct_find(k, &jwl_wl_surface_interface,
                                       JWL_WL_SURFACE_EV_PREFERRED_BUFFER_SCALE, s6);
    CHECK(e && e->u[0] == 1);
    e = ct_find(k, &jwl_wl_surface_interface, JWL_WL_SURFACE_EV_PREFERRED_BUFFER_TRANSFORM, s6);
    CHECK(e && e->u[0] == JWL_WL_OUTPUT_TRANSFORM_NORMAL);
    CHECK(!ct_find(k, &jwl_wl_surface_interface, JWL_WL_SURFACE_EV_PREFERRED_BUFFER_SCALE, s4));
    CHECK(!ct_find(k, &jwl_wl_surface_interface, JWL_WL_SURFACE_EV_PREFERRED_BUFFER_TRANSFORM,
                   s4));
    /* wl_surface.offset (5): taken, double-buffered, no error */
    CHECK_ST(jwl_wl_surface_offset(k->c, s6, 3, 4), OK);
    CHECK_ST(jwl_wl_surface_commit(k->c, s6), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    CHECK(!k->errored);
    return true;
}

/* wl_output 4: its name and description before done; at 3, neither. */
static bool output_names(struct ct_client *k)
{
    uint32_t o4 = ct_new(k, &jwl_wl_output_interface, 4);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 3, "wl_output", 4, o4), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    const struct ct_event *e = ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_NAME, o4);
    CHECK(e && !strcmp(e->s, "HEADLESS-1"));
    e = ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_DESCRIPTION, o4);
    CHECK(e && e->s[0]);
    unsigned name_at = 0, done_at = 0;
    for (unsigned i = 0; i < k->nev; i++) {
        if (k->ev[i].iface == &jwl_wl_output_interface && k->ev[i].id == o4) {
            if (k->ev[i].op == JWL_WL_OUTPUT_EV_NAME)
                name_at = i + 1;
            if (k->ev[i].op == JWL_WL_OUTPUT_EV_DONE)
                done_at = i + 1;
        }
    }
    CHECK(name_at && done_at > name_at);
    CHECK(!ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_NAME, k->output));
    return true;
}

/* wl_shm 2: release, the pools it made going on. */
static bool shm_release(struct ct_client *k)
{
    uint32_t shm2 = ct_new(k, &jwl_wl_shm_interface, 2);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 2, "wl_shm", 2, shm2), OK);
    handle_t vmo = ct_kept_vmo(4096);
    CHECK(vmo != HANDLE_INVALID);
    uint32_t pool = ct_new(k, &jwl_wl_shm_pool_interface, 2);
    CHECK_ST(jwl_wl_shm_create_pool(k->c, shm2, pool, ct_dup_for_pool(vmo), 4096), OK);
    jam_handle_close(vmo);
    CHECK_ST(jwl_wl_shm_release(k->c, shm2), OK);
    uint32_t b = ct_new(k, &jwl_wl_buffer_interface, 2);
    CHECK_ST(jwl_wl_shm_pool_create_buffer(k->c, pool, b, 0, 16, 16, 64,
                                           JWL_WL_SHM_FORMAT_XRGB8888), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    CHECK(!k->errored);
    return true;
}

bool t_comp_versions(void)
{
    static struct ct_client k, bad;
    struct ct_comp p;
    CHECK(ct_start(&p, W, H));
    CHECK(ct_open(&p, &k));
    CHECK(ct_bind_all(&k));
    bool ok = surface_preferences(&k) && output_names(&k) && shm_release(&k);
    ct_close(&k);
    /* wl_surface 5 and up: attach's offset must be 0 */
    if (ok) {
        CHECK(ct_open(&p, &bad));
        CHECK(ct_bind_all(&bad));
        uint32_t c5 = ct_new(&bad, &jwl_wl_compositor_interface, 5);
        CHECK_ST(jwl_wl_registry_bind(bad.c, bad.registry, 1, "wl_compositor", 5, c5), OK);
        uint32_t s = ct_new(&bad, &jwl_wl_surface_interface, 5);
        CHECK_ST(jwl_wl_compositor_create_surface(bad.c, c5, s), OK);
        CHECK_ST(jwl_wl_surface_attach(bad.c, s, 0, 1, 0), OK);
        ok = ct_expect_error(&bad, s, JWL_WL_SURFACE_ERROR_INVALID_OFFSET);
        ct_close(&bad);
    }
    CHECK(ct_stop(&p));
    return ok;
}

/* ---- wl_seat 9: the wheel ------------------------------------------------------------- */

/* A client with wl_compositor 6 and a pointer of version v. */
static bool wheel_client(struct cs *t, struct sc *c, uint32_t v)
{
    CHECK(ct_open(&t->p, &c->k));
    CHECK(ct_bind_all(&c->k));
    CHECK((c->seat = ct_new(&c->k, &jwl_wl_seat_interface, v)) != 0);
    CHECK_ST(jwl_wl_registry_bind(c->k.c, c->k.registry, 4, "wl_seat", v, c->seat), OK);
    CHECK((c->ptr = ct_new(&c->k, &jwl_wl_pointer_interface, v)) != 0);
    CHECK_ST(jwl_wl_seat_get_pointer(c->k.c, c->seat, c->ptr), OK);
    CHECK_ST(ct_roundtrip(&c->k), OK);
    return true;
}

/* A testwin window at (x, y) on a version 6 surface: placed by
 * wl_surface.offset, attach's own offset 0. */
static uint32_t offset_window(struct sc *c, int32_t x, int32_t y)
{
    struct ct_client *k = &c->k;
    uint32_t comp6 = ct_new(k, &jwl_wl_compositor_interface, 6);
    if (jwl_wl_registry_bind(k->c, k->registry, 1, "wl_compositor", 6, comp6) != OK)
        return 0;
    handle_t vmo = HANDLE_INVALID;
    uint32_t pool = ct_pool(k, 100 * 100 * 4, &vmo), buf = ct_new(k, &jwl_wl_buffer_interface, 1);
    if (vmo != HANDLE_INVALID)
        jam_handle_close(vmo);
    uint32_t s = ct_new(k, &jwl_wl_surface_interface, 6);
    if (!pool || !buf || !s ||
        jwl_wl_shm_pool_create_buffer(k->c, pool, buf, 0, 100, 100, 400,
                                      JWL_WL_SHM_FORMAT_XRGB8888) != OK ||
        jwl_wl_compositor_create_surface(k->c, comp6, s) != OK ||
        jwl_wl_surface_offset(k->c, s, x, y) != OK ||
        jwl_wl_surface_attach(k->c, s, buf, 0, 0) != OK || jwl_wl_surface_commit(k->c, s) != OK ||
        ct_roundtrip(k) != OK)
        return 0;
    return s;
}

/* One notch at (px, py) over c's window, and what c heard: version 9 has
 * axis_value120 and axis_relative_direction and no axis_discrete; 5 the
 * other way round. */
static bool notch(struct cs *t, struct sc *c, int32_t px, int32_t py, uint32_t v)
{
    CHECK(cs_pointer_to(t, px, py));
    CHECK(cs_sync(c, NULL));
    CHECK(find_ev(c, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    ct_clear(&c->k);
    CHECK(cs_mouse(t, 0, 0, 1));
    CHECK(cs_sync(c, NULL));
    const struct jwl_interface *pi = &jwl_wl_pointer_interface;
    const struct ct_event *e = find_ev(c, pi, JWL_WL_POINTER_EV_AXIS);
    CHECK(e && e->u[1] == JWL_WL_POINTER_AXIS_VERTICAL_SCROLL && (int32_t)e->u[2] == -10 * 256);
    e = find_ev(c, pi, JWL_WL_POINTER_EV_AXIS_SOURCE);
    CHECK(e && e->u[0] == JWL_WL_POINTER_AXIS_SOURCE_WHEEL);
    const struct ct_event *v120 = find_ev(c, pi, JWL_WL_POINTER_EV_AXIS_VALUE120);
    const struct ct_event *disc = find_ev(c, pi, JWL_WL_POINTER_EV_AXIS_DISCRETE);
    const struct ct_event *dir = find_ev(c, pi, JWL_WL_POINTER_EV_AXIS_RELATIVE_DIRECTION);
    if (v >= 9) {
        CHECK(v120 && v120->u[0] == JWL_WL_POINTER_AXIS_VERTICAL_SCROLL &&
              (int32_t)v120->u[1] == -120);
        CHECK(dir && dir->u[0] == JWL_WL_POINTER_AXIS_VERTICAL_SCROLL &&
              dir->u[1] == JWL_WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        CHECK(!disc);
    } else {
        CHECK(disc && (int32_t)disc->u[1] == -1);
        CHECK(!v120 && !dir);
    }
    CHECK(find_ev(c, pi, JWL_WL_POINTER_EV_FRAME));
    return true;
}

bool t_comp_seat_v9(void)
{
    static struct sc a, b;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(wheel_client(&t, &a, 9) && wheel_client(&t, &b, 5));
    CHECK(offset_window(&a, 40, 50));       /* placed by wl_surface.offset ... */
    CHECK(cs_window(&b, 300, 50, 100, 100));
    /* ... so (130, 140) is on it: at (0, 0) it would end at (100, 100) */
    bool ok = notch(&t, &a, 130, 140, 9) && notch(&t, &b, 320, 70, 5);
    ct_close(&a.k);
    ct_close(&b.k);
    CHECK(cs_stop(&t));
    return ok;
}

/* ---- xdg_wm_base 6 ---------------------------------------------------------------------- */

/* wm_capabilities and configure_bounds before the first configure, by
 * version: 5 and 4 have them, 1 neither. With a decoration object asking
 * server_side (deco) or without; the bounds leave room for the
 * compositor's frame when x bound the manager, either way (the owner's
 * rule, xdgdeco.c), and none when it never did. */
static bool first_configure(struct vc *x, uint32_t v, bool deco)
{
    struct vt t;
    const struct ct_event *top;
    bool ssd = x->decos != 0;
    CHECK(toplevel(x, &t));
    if (deco)
        CHECK(decoration(x, &t, SERVER_SIDE));
    ct_clear(&x->k);
    CHECK(ack_commit(x, &t, 0, 0));
    CHECK(configured(x, &t, &top));
    const struct jwl_interface *ti = &jwl_xdg_toplevel_interface;
    const struct ct_event *caps = ct_find(&x->k, ti, JWL_XDG_TOPLEVEL_EV_WM_CAPABILITIES, t.top);
    const struct ct_event *b = ct_find(&x->k, ti, JWL_XDG_TOPLEVEL_EV_CONFIGURE_BOUNDS, t.top);
    if (v >= 5) {
        CHECK(caps && caps->na == 3);
        CHECK(caps->a[0] == JWL_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE &&
              caps->a[1] == JWL_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN &&
              caps->a[2] == JWL_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
        CHECK(caps < top);   /* before the configure */
    } else {
        CHECK(!caps);
    }
    if (v >= 4) {
        /* the whole output (no strip) less the frame: a title bar and an
         * outline, or nothing for a client drawing its own */
        int32_t bw = ssd ? W - 2 * DECO_OUTLINE : W;
        int32_t bh = ssd ? H - COMP_TITLE_H - DECO_OUTLINE : H;
        CHECK(b && (int32_t)b->u[0] == bw && (int32_t)b->u[1] == bh);
        CHECK(b < top);
    } else {
        CHECK(!b);
    }
    CHECK(top->na == 0);   /* floating, not activated yet */
    if (t.deco)            /* gone first: else its toplevel's destroy is an error */
        CHECK_ST(jwl_zxdg_toplevel_decoration_v1_destroy(x->k.c, t.deco), OK);
    CHECK_ST(jwl_xdg_toplevel_destroy(x->k.c, t.top), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    CHECK(!x->k.errored);
    return true;
}

/* set_minimized (5) minimises; a toplevel of 6 is told it is suspended. */
static bool minimised(struct ct_comp *p, struct vc *x, uint32_t v)
{
    struct vt t;
    const struct ct_event *top;
    ct_clear(&x->k);   /* (an id used again finds nothing of its last owner's) */
    CHECK(toplevel(x, &t));
    CHECK(decoration(x, &t, SERVER_SIDE));
    CHECK(map_red(p, x, &t));
    uint32_t serial = configured(x, &t, &top);
    CHECK(serial && !has_state(top, JWL_XDG_TOPLEVEL_STATE_SUSPENDED));
    CHECK(count(&x->k, &jwl_xdg_toplevel_interface, JWL_XDG_TOPLEVEL_EV_CONFIGURE_BOUNDS,
                t.top) == 1);   /* only with the first: the bounds didn't change */
    CHECK(ack_commit(x, &t, serial, 0));
    ct_clear(&x->k);
    CHECK_ST(jwl_xdg_toplevel_set_minimized(x->k.c, t.top), OK);
    CHECK_ST(jwl_conn_flush(x->k.c), OK);
    CHECK(pixel_is(p, SX + SIDE / 2, SY + SIDE / 2, RED, false));   /* gone from the screen */
    serial = configured(x, &t, &top);
    if (v >= 6) {
        CHECK(serial && has_state(top, JWL_XDG_TOPLEVEL_STATE_SUSPENDED));
        CHECK(!has_state(top, JWL_XDG_TOPLEVEL_STATE_ACTIVATED));
    } else {
        CHECK(!serial || !has_state(top, JWL_XDG_TOPLEVEL_STATE_SUSPENDED));
    }
    CHECK(!x->k.errored);
    return true;
}

/* The positioner's version 3 requests, and xdg_popup.reposition: a
 * complete positioner taken (before the popup's initial commit nothing is
 * said: that commit's configure will be the first), an incomplete one
 * refused. comppopup.c repositions a shown popup. */
static bool reposition(struct vc *x)
{
    struct vt parent;
    CHECK(toplevel(x, &parent));
    uint32_t pos = ct_new(&x->k, &jwl_xdg_positioner_interface, 3);
    CHECK_ST(jwl_xdg_wm_base_create_positioner(x->k.c, x->wm, pos), OK);
    CHECK_ST(jwl_xdg_positioner_set_size(x->k.c, pos, 10, 10), OK);
    CHECK_ST(jwl_xdg_positioner_set_anchor_rect(x->k.c, pos, 0, 0, 1, 1), OK);
    CHECK_ST(jwl_xdg_positioner_set_reactive(x->k.c, pos), OK);
    CHECK_ST(jwl_xdg_positioner_set_parent_size(x->k.c, pos, 100, 80), OK);
    CHECK_ST(jwl_xdg_positioner_set_parent_configure(x->k.c, pos, 7), OK);
    uint32_t s = ct_new(&x->k, &jwl_wl_surface_interface, 4);
    CHECK_ST(jwl_wl_compositor_create_surface(x->k.c, x->k.compositor, s), OK);
    uint32_t xs = ct_new(&x->k, &jwl_xdg_surface_interface, 3);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(x->k.c, x->wm, xs, s), OK);
    uint32_t pop = ct_new(&x->k, &jwl_xdg_popup_interface, 3);
    CHECK_ST(jwl_xdg_surface_get_popup(x->k.c, xs, pop, parent.xs, pos), OK);
    CHECK_ST(jwl_xdg_popup_reposition(x->k.c, pop, pos, 1), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    CHECK(!x->k.errored);
    CHECK(!ct_find(&x->k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_REPOSITIONED, pop));
    CHECK(!ct_find(&x->k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_POPUP_DONE, pop));
    uint32_t half = ct_new(&x->k, &jwl_xdg_positioner_interface, 3);
    CHECK_ST(jwl_xdg_wm_base_create_positioner(x->k.c, x->wm, half), OK);
    CHECK_ST(jwl_xdg_popup_reposition(x->k.c, pop, half, 2), OK);
    CHECK(ct_expect_error(&x->k, x->wm, JWL_XDG_WM_BASE_ERROR_INVALID_POSITIONER));
    return true;
}

bool t_xdg_versions(void)
{
    static struct vc x6, x5, x4, x1, x3, bare;
    struct ct_comp p;
    CHECK(ct_start(&p, W, H));
    CHECK(vc_open(&p, &x6, 6) && vc_open(&p, &x5, 5) && vc_open(&p, &x4, 4));
    CHECK(vc_open(&p, &x1, 1) && vc_open(&p, &x3, 3) && vc_open_bare(&p, &bare, 6));
    bool ok = first_configure(&x6, 6, true) && first_configure(&x6, 6, false) &&
              first_configure(&bare, 6, false) && first_configure(&x4, 4, true) &&
              first_configure(&x1, 1, true);
    ok = ok && minimised(&p, &x6, 6) && minimised(&p, &x5, 5) && reposition(&x3);
    vc_close(&bare);
    vc_close(&x6);
    vc_close(&x5);
    vc_close(&x4);
    vc_close(&x1);
    vc_close(&x3);
    CHECK(ct_stop(&p));
    return ok;
}

/* Tiling: a toplevel of 2 or more is told its four edges are tiled; one of
 * 1 hears only the states it knows. */
bool t_xdg_tiled(void)
{
    static struct vc x2, x1;
    struct ct_comp p;
    CHECK(ct_start_arg(&p, W, H, "layout=tiling"));
    CHECK(vc_open(&p, &x2, 2) && vc_open(&p, &x1, 1));
    bool ok = true;
    for (unsigned i = 0; i < 2 && ok; i++) {
        struct vc *x = i ? &x1 : &x2;
        struct vt t;
        const struct ct_event *top;
        ok = toplevel(x, &t) && decoration(x, &t, SERVER_SIDE) && ack_commit(x, &t, 0, 0) &&
             configured(x, &t, &top);
        if (!ok)
            break;
        static const uint32_t tiled[] = {
            JWL_XDG_TOPLEVEL_STATE_TILED_LEFT, JWL_XDG_TOPLEVEL_STATE_TILED_RIGHT,
            JWL_XDG_TOPLEVEL_STATE_TILED_TOP, JWL_XDG_TOPLEVEL_STATE_TILED_BOTTOM };
        for (unsigned s = 0; s < 4; s++)
            ok = ok && has_state(top, tiled[s]) == !i;
        if (!ok)
            FAIL("toplevel version %u: the tiled states %s", i ? 1 : 2,
                 i ? "sent" : "missing");
        ok = ok && top->na == (i ? 0u : 4u);
    }
    vc_close(&x2);
    vc_close(&x1);
    CHECK(ct_stop(&p));
    return ok;
}

/* ---- xdg-decoration ------------------------------------------------------------------- */

/* The mode t's last decoration configure said (0: none came). */
static uint32_t told(const struct vc *x, const struct vt *t)
{
    const struct ct_event *e = last(&x->k, &jwl_zxdg_toplevel_decoration_v1_interface,
                                    JWL_ZXDG_TOPLEVEL_DECORATION_V1_EV_CONFIGURE, t->deco);
    return e ? e->u[0] : 0;
}

/* t's decoration mode moved by req (set_mode, or unset_mode for 0, or the
 * object destroyed for ~0u): the configure it brings acked with a redrawn
 * buffer. want: the mode the decoration's configure says (0: none comes).
 * A destroyed object brings nothing (its client bound the manager, so the
 * window stays the compositor's): a buffer committed all the same. */
static bool switch_mode(struct vc *x, struct vt *t, uint32_t req, uint32_t want)
{
    struct jwl_conn *c = x->k.c;
    const struct ct_event *top;
    ct_clear(&x->k);
    if (req == ~0u) {
        CHECK_ST(jwl_zxdg_toplevel_decoration_v1_destroy(c, t->deco), OK);
        t->deco = 0;
        CHECK_ST(ct_roundtrip(&x->k), OK);
        CHECK(!ct_find(&x->k, &jwl_xdg_surface_interface, JWL_XDG_SURFACE_EV_CONFIGURE, t->xs));
        return ack_commit(x, t, 0, buffer(x, SIDE, RED));
    }
    if (req) {
        CHECK_ST(jwl_zxdg_toplevel_decoration_v1_set_mode(c, t->deco, req), OK);
    } else {
        CHECK_ST(jwl_zxdg_toplevel_decoration_v1_unset_mode(c, t->deco), OK);
    }
    uint32_t serial = configured(x, t, &top);
    CHECK(serial);   /* every request answered by a configure */
    if (want)
        CHECK_EQ(told(x, t), want);
    CHECK(ack_commit(x, t, serial, buffer(x, SIDE, RED)));
    return true;
}

/* Server-side: a title bar above the surface, square corners at the
 * surface's top (the frame's corners are the title bar's). Client-side:
 * neither (only round corners and the shadow). bar: the title bar's
 * pixel, learnt from a server-side window. */
static bool looks(struct ct_comp *p, bool ssd, uint32_t bar)
{
    CHECK(pixel_is(p, SX + SIDE / 2, TITLE_Y, bar, ssd));
    CHECK(pixel_is(p, SX, SY, RED, ssd));   /* the surface's top-left pixel: cut round or not */
    CHECK(pixel_is(p, SX + SIDE / 2, SY + SIDE / 2, RED, true));
    return true;
}

/* t (its decoration object gone first, if any) and its surface destroyed,
 * and its pixels gone from the output: the next window's checks see the
 * next window, never t's last picture. */
static bool gone(struct ct_comp *p, struct vc *x, struct vt *t)
{
    if (t->deco)
        CHECK_ST(jwl_zxdg_toplevel_decoration_v1_destroy(x->k.c, t->deco), OK);
    CHECK_ST(jwl_xdg_toplevel_destroy(x->k.c, t->top), OK);
    CHECK_ST(jwl_xdg_surface_destroy(x->k.c, t->xs), OK);
    CHECK_ST(jwl_wl_surface_destroy(x->k.c, t->s), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    CHECK(!x->k.errored);
    CHECK(pixel_is(p, SX + SIDE / 2, SY + SIDE / 2, RED, false));
    return true;
}

/* x, bound to the manager, opened on p: a fresh client each time, so its
 * first window takes the focus and its title bar is the focused colour. */
static bool fresh(struct ct_comp *p, struct vc *x)
{
    vc_close(x);
    return vc_open(p, x, 1);
}

/* The owner's rule (xdgdeco.c), on x (bound the manager) and bare (never
 * did): every window of x's has the compositor's title bar whatever it
 * asks, and is told server_side; bare's draw their own, until bare binds
 * the manager, when its window already mapped gets ours too. Each window
 * is its client's first (fresh): the focused title bar's colour. */
static bool modes(struct ct_comp *p, struct vc *x, struct vc *bare)
{
    struct vt t;
    CHECK(toplevel(x, &t));
    CHECK(decoration(x, &t, 0));   /* no mode asked: server-side */
    CHECK(map_red(p, x, &t));
    CHECK_EQ(told(x, &t), SERVER_SIDE);
    uint32_t bar = p->image[TITLE_Y * W + SX + SIDE / 2];   /* drawn by the paint that showed it */
    CHECK(bar != RED && bar != ref_wallpaper_of(SX + SIDE / 2, TITLE_Y, W, H));
    CHECK(looks(p, true, bar));
    /* client_side asked: answered server_side, the title bar kept; then
     * server_side, client_side again, unset, server_side twice, destroyed:
     * always ours */
    CHECK(switch_mode(x, &t, CLIENT_SIDE, SERVER_SIDE) && looks(p, true, bar));
    CHECK(switch_mode(x, &t, SERVER_SIDE, SERVER_SIDE) && looks(p, true, bar));
    CHECK(switch_mode(x, &t, CLIENT_SIDE, SERVER_SIDE) && looks(p, true, bar));
    CHECK(switch_mode(x, &t, 0, SERVER_SIDE) && looks(p, true, bar));
    CHECK(switch_mode(x, &t, SERVER_SIDE, SERVER_SIDE) && looks(p, true, bar));
    CHECK(switch_mode(x, &t, ~0u, 0) && looks(p, true, bar));
    CHECK(!x->k.errored);
    CHECK(gone(p, x, &t));
    /* a client_side window from the start: ours all the same */
    CHECK(fresh(p, x) && toplevel(x, &t));
    CHECK(decoration(x, &t, CLIENT_SIDE));
    CHECK(map_red(p, x, &t));
    CHECK_EQ(told(x, &t), SERVER_SIDE);
    CHECK(looks(p, true, bar));
    CHECK(gone(p, x, &t));
    /* no decoration object at all, its client bound the manager: ours */
    CHECK(fresh(p, x) && toplevel(x, &t));
    CHECK(map_red(p, x, &t));
    CHECK(looks(p, true, bar));
    CHECK(gone(p, x, &t));
    /* a client that never bound it: its own decorations */
    CHECK(toplevel(bare, &t));
    CHECK(map_red(p, bare, &t));
    CHECK(looks(p, false, bar));
    /* ... until it binds it: its mapped window is told (a configure, no
     * decoration object to say a mode) and gets ours with its next buffer */
    ct_clear(&bare->k);
    CHECK(vc_bind_decos(bare));
    const struct ct_event *top;
    uint32_t serial = configured(bare, &t, &top);
    CHECK(serial);
    CHECK(ack_commit(bare, &t, serial, buffer(bare, SIDE, RED)));
    CHECK(looks(p, true, bar));
    CHECK(!bare->k.errored);
    return true;
}

bool t_xdg_decoration(void)
{
    static struct vc x, bare;
    struct ct_comp p;
    CHECK(ct_start(&p, W, H));
    CHECK(vc_open(&p, &x, 1) && vc_open_bare(&p, &bare, 1));
    bool ok = modes(&p, &x, &bare);
    vc_close(&x);
    vc_close(&bare);
    CHECK(ct_stop(&p));
    return ok;
}

/* A hostile client's decoration requests, one bad thing each, the error
 * xdg-decoration names (or the codec's for a wrong object), on the
 * decoration object or the manager (*named). */
enum { B_DECO, B_MANAGER };
static bool deco_bad_case(unsigned i, struct vc *x, struct vt *t, unsigned *named)
{
    struct jwl_conn *c = x->k.c;
    CHECK(toplevel(x, t));
    *named = B_DECO;
    switch (i) {
    case 0:   /* a mode that is neither */
        CHECK(decoration(x, t, 0));
        return jwl_zxdg_toplevel_decoration_v1_set_mode(c, t->deco, 3) == OK;
    case 1:   /* mode 0 */
        CHECK(decoration(x, t, SERVER_SIDE));
        return jwl_zxdg_toplevel_decoration_v1_set_mode(c, t->deco, 0) == OK;
    case 2:   /* a second decoration object */
        CHECK(decoration(x, t, SERVER_SIDE));
        return decoration(x, t, 0);
    case 3: {   /* made after a buffer was attached: the wrong order */
        CHECK(ack_commit(x, t, 0, 0));
        const struct ct_event *top;
        uint32_t serial = configured(x, t, &top);
        CHECK(serial && ack_commit(x, t, serial, buffer(x, SIDE, RED)));
        return decoration(x, t, SERVER_SIDE);
    }
    case 4:   /* the toplevel destroyed before its decoration object */
        CHECK(decoration(x, t, SERVER_SIDE));
        return jwl_xdg_toplevel_destroy(c, t->top) == OK;
    default: {   /* a decoration for an object that isn't a toplevel (the stub would
                  * refuse to send it: the words by hand, in a batch of their own) */
        *named = B_MANAGER;
        CHECK_ST(jwl_conn_flush(c), OK);
        uint32_t id = ct_new(&x->k, &jwl_zxdg_toplevel_decoration_v1_interface, 1);
        uint32_t op = JWL_ZXDG_DECORATION_MANAGER_V1_REQ_GET_TOPLEVEL_DECORATION;
        const uint32_t b[] = { JWL_MAGIC, 0, 0, 0, x->decos, 16u << 16 | op, id, t->xs };
        return id && jam_channel_write(c->ch, b, sizeof(b), NULL, 0) == OK;
    }
    }
}

#define NDECO_BAD 6u
static const uint32_t deco_bad_code[NDECO_BAD] = {
    JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
    JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE,
    JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
    JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
    JWL_ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
    JWL_ERROR_INVALID_OBJECT,
};

bool t_xdg_decoration_bad(void)
{
    static struct vc x, good;
    struct ct_comp p;
    struct vt t;
    CHECK(ct_start(&p, W, H));
    CHECK(vc_open(&p, &good, 6));   /* connected all along: it must not notice */
    uint64_t base = ct_handles(&p);
    for (unsigned i = 0; i < NDECO_BAD; i++) {
        unsigned named;
        CHECK(vc_open(&p, &x, 1));
        if (!deco_bad_case(i, &x, &t, &named))
            FAIL("case %u: a request didn't go", i);
        uint32_t obj = named == B_MANAGER ? x.decos : t.deco;
        if (!ct_expect_error(&x.k, obj, deco_bad_code[i]))
            FAIL("case %u: not the error it should be", i);
        vc_close(&x);
    }
    CHECK(ct_handles_back(&p, base));   /* every bad client's objects gone */
    CHECK(toplevel(&good, &t) && decoration(&good, &t, CLIENT_SIDE));
    CHECK(map_red(&p, &good, &t));      /* and the compositor still serves */
    CHECK_EQ(told(&good, &t), SERVER_SIDE);   /* (client_side asked: the owner's rule) */
    CHECK(!good.k.errored);
    vc_close(&good);
    CHECK(ct_stop(&p));
    return true;
}

/* ---- the window geometry ------------------------------------------------------------------ */

#define BLUE  0x000000ffu
#define SH_L  8    /* the client's own "shadow" round its SIDE x SIDE window */
#define SH_T  4
#define SH_R  24
#define SH_B  28
#define GW    (SH_L + SIDE + SH_R)   /* its surface */
#define GH    (SH_T + SIDE + SH_B)

/* A GW x GH buffer at the pool's start: blue, the window in it red. */
static uint32_t shadowed(struct vc *x)
{
    static uint32_t row[GW];
    for (int32_t y = 0; y < GH; y++) {
        for (int32_t i = 0; i < GW; i++)
            row[i] = y >= SH_T && y < SH_T + SIDE && i >= SH_L && i < SH_L + SIDE ? RED : BLUE;
        if (jam_vmo_write(x->vmo, (uint64_t)y * GW * 4, row, GW * 4) != OK)
            return 0;
    }
    uint32_t id = ct_new(&x->k, &jwl_wl_buffer_interface, 1);
    if (!id || jwl_wl_shm_pool_create_buffer(x->k.c, x->pool, id, 0, GW, GH, GW * 4,
                                             JWL_WL_SHM_FORMAT_XRGB8888) != OK)
        return 0;
    return id;
}

/* A client that draws its own decorations and its own shadow, with its
 * window geometry set (xdg_surface.set_window_geometry): its window is
 * placed centred by the geometry (the red at (SX, SY), the client's blue
 * left of and above it), its surface's corner isn't cut round, and no
 * shadow of ours darkens the wallpaper past its surface. */
bool t_xdg_geometry(void)
{
    static struct vc bare;
    struct ct_comp p;
    struct vt t;
    const struct ct_event *top;
    CHECK(ct_start(&p, W, H));
    CHECK(vc_open_bare(&p, &bare, 1));
    struct jwl_conn *c = bare.k.c;
    bool ok = toplevel(&bare, &t) && ack_commit(&bare, &t, 0, 0);
    uint32_t serial = ok ? configured(&bare, &t, &top) : 0;
    uint32_t b = serial ? shadowed(&bare) : 0;
    CHECK(b);
    CHECK_ST(jwl_xdg_surface_ack_configure(c, t.xs, serial), OK);
    CHECK_ST(jwl_xdg_surface_set_window_geometry(c, t.xs, SH_L, SH_T, SIDE, SIDE), OK);
    CHECK_ST(jwl_wl_surface_attach(c, t.s, b, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_damage_buffer(c, t.s, 0, 0, GW, GH), OK);
    CHECK_ST(jwl_wl_surface_commit(c, t.s), OK);
    CHECK_ST(ct_roundtrip(&bare.k), OK);
    CHECK(!bare.k.errored);
    /* placed by its geometry: the window centred, the client's blue round it */
    CHECK(pixel_is(&p, SX - 1, SY + SIDE / 2, BLUE, true));
    CHECK(pixel_is(&p, SX, SY, RED, true));             /* no corner of ours cut */
    CHECK(pixel_is(&p, SX + SIDE - 1, SY + SIDE - 1, RED, true));
    CHECK(pixel_is(&p, SX + SIDE, SY + SIDE / 2, BLUE, true));
    CHECK(pixel_is(&p, SX - SH_L, SY - SH_T, BLUE, true));   /* the surface's corner: whole */
    /* no shadow of ours: the wallpaper as it is, just past its surface */
    int32_t bx = SX + SIDE / 2, by = SY + SIDE + SH_B + 2, lx = SX - SH_L - 2;
    CHECK(pixel_is(&p, bx, by, ref_wallpaper_of(bx, by, W, H), true));
    CHECK(pixel_is(&p, lx, SY + SIDE / 2, ref_wallpaper_of(lx, SY + SIDE / 2, W, H), true));
    CHECK(!bare.k.errored);
    vc_close(&bare);
    CHECK(ct_stop(&p));
    return true;
}
