/* utest: xdg-shell on bin/compositor headless (user/services/compositor
 * xdg.c, xdgtop.c), driven over real channels by test clients through
 * libjwl, as comp.c drives the core (comptest.h). The window manager's
 * own decisions are compwm.c's; here, what a client sees of them: the
 * configure and ack sequence, where its pixels land, every protocol error
 * xdg-shell names, and popups dismissed at once.
 *
 * The output is 320x240. Decorations are drawn (title.c), so the checks
 * that want the background look past them: a floating window's 2-pixel
 * border, a maximised one's 24-pixel title bar (its only decoration), a
 * tiled one's border and the gap between tiles. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include <splash.h>
#include "comptest.h"
#include "utest.h"

#define W 320
#define H 240
#define TITLE 24
#define POOL (1u << 20)

#define RED   0x00ff0000u
#define GREEN 0x0000ff00u
#define BLUE  0x000000ffu

/* A client with xdg_wm_base bound, and a pool to draw from. */
struct xc {
    struct ct_client k;
    uint32_t wm, pool;
    handle_t vmo;
};

/* A toplevel's three objects. */
struct tl {
    uint32_t s, xs, top;
};

/* The newest configure pair a toplevel got. */
struct cfg {
    int32_t w, h;
    uint32_t serial;
    uint32_t states[4], nstates;
};

static bool xc_open(struct ct_comp *p, struct xc *x)
{
    CHECK(ct_open(p, &x->k));
    CHECK(ct_bind_all(&x->k));
    CHECK((x->wm = ct_new(&x->k, &jwl_xdg_wm_base_interface, 1)) != 0);
    CHECK_ST(jwl_wl_registry_bind(x->k.c, x->k.registry, 5, "xdg_wm_base", 1, x->wm), OK);
    CHECK((x->pool = ct_pool(&x->k, POOL, &x->vmo)) != 0);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    return true;
}

static void xc_close(struct xc *x)
{
    if (x->vmo)
        jam_handle_close(x->vmo);
    x->vmo = HANDLE_INVALID;
    ct_close(&x->k);
}

static bool surface(struct xc *x, uint32_t *s)
{
    CHECK((*s = ct_new(&x->k, &jwl_wl_surface_interface, 4)) != 0);
    CHECK_ST(jwl_wl_compositor_create_surface(x->k.c, x->k.compositor, *s), OK);
    return true;
}

static bool xdg_surface(struct xc *x, uint32_t s, uint32_t *xs)
{
    CHECK((*xs = ct_new(&x->k, &jwl_xdg_surface_interface, 1)) != 0);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(x->k.c, x->wm, *xs, s), OK);
    return true;
}

static bool toplevel(struct xc *x, struct tl *t)
{
    CHECK(surface(x, &t->s) && xdg_surface(x, t->s, &t->xs));
    CHECK((t->top = ct_new(&x->k, &jwl_xdg_toplevel_interface, 1)) != 0);
    CHECK_ST(jwl_xdg_surface_get_toplevel(x->k.c, t->xs, t->top), OK);
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

/* Everything the compositor has said, then t's newest configure. */
static bool configured(struct xc *x, const struct tl *t, struct cfg *c)
{
    CHECK_ST(ct_roundtrip(&x->k), OK);
    const struct ct_event *e = last(&x->k, &jwl_xdg_surface_interface,
                                    JWL_XDG_SURFACE_EV_CONFIGURE, t->xs);
    const struct ct_event *te = last(&x->k, &jwl_xdg_toplevel_interface,
                                     JWL_XDG_TOPLEVEL_EV_CONFIGURE, t->top);
    CHECK(e && te);
    uint32_t n = te->na < 4 ? te->na : 4;   /* words: we send at most four states */
    *c = (struct cfg){ (int32_t)te->u[0], (int32_t)te->u[1], e->u[0], { 0 }, n };
    memcpy(c->states, te->a, n * sizeof(c->states[0]));
    ct_clear(&x->k);
    return true;
}

static bool has_state(const struct cfg *c, uint32_t state)
{
    for (uint32_t i = 0; i < c->nstates; i++)
        if (c->states[i] == state)
            return true;
    return false;
}

/* A w by h xrgb buffer at off in the pool, every pixel colour. */
static uint32_t buffer(struct xc *x, uint32_t off, int32_t w, int32_t h, uint32_t colour)
{
    static uint32_t row[W];
    for (int32_t i = 0; i < w; i++)
        row[i] = colour;
    for (int32_t y = 0; y < h; y++)
        if (jam_vmo_write(x->vmo, off + (uint64_t)y * (uint64_t)w * 4, row, (uint64_t)w * 4) != OK)
            return 0;
    uint32_t id = ct_new(&x->k, &jwl_wl_buffer_interface, 1);
    if (!id || jwl_wl_shm_pool_create_buffer(x->k.c, x->pool, id, (int32_t)off, w, h, w * 4,
                                             JWL_WL_SHM_FORMAT_XRGB8888) != OK)
        return 0;
    return id;
}

/* Ack c and commit buffer b (0: none) on t. */
static bool ack_commit(struct xc *x, const struct tl *t, const struct cfg *c, uint32_t b)
{
    if (c)
        CHECK_ST(jwl_xdg_surface_ack_configure(x->k.c, t->xs, c->serial), OK);
    CHECK_ST(jwl_wl_surface_attach(x->k.c, t->s, b, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_damage_buffer(x->k.c, t->s, 0, 0, W, H), OK);
    CHECK_ST(jwl_wl_surface_commit(x->k.c, t->s), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    return true;
}

/* Wait (up to CT_WAIT) until the image has want at (px, py). */
static bool pixel(const struct ct_comp *p, int32_t px, int32_t py, uint32_t want)
{
    uint64_t until = now() + CT_WAIT;
    const volatile uint32_t *img = p->image;   /* the compositor writes it */
    while (img[py * W + px] != want && now() < until)
        jam_nanosleep(now() + NS_PER_MS);
    if (img[py * W + px] != want)
        FAIL("pixel %d,%d is %06x, want %06x", px, py, img[py * W + px], want);
    return true;
}

/* ---- a toplevel's life -------------------------------------------------------------- */

static bool map_floating(struct ct_comp *p, struct xc *x, struct tl *t)
{
    struct cfg c;
    CHECK(toplevel(x, t));
    CHECK_ST(jwl_xdg_toplevel_set_title(x->k.c, t->top, "hello"), OK);
    CHECK_ST(jwl_xdg_toplevel_set_app_id(x->k.c, t->top, "utest"), OK);
    CHECK(ack_commit(x, t, NULL, 0));   /* the initial commit */
    CHECK(configured(x, t, &c));
    CHECK(c.w == 0 && c.h == 0 && c.nstates == 0);   /* floating: its own size */
    CHECK(ack_commit(x, t, &c, buffer(x, 0, 64, 64, RED)));
    CHECK(pixel(p, (W - 64) / 2 + 32, (H - 64) / 2 + 32, RED));   /* centred */
    CHECK(pixel(p, (W - 64) / 2 - 3, (H - 64) / 2 + 32, SPLASH_BG));   /* past its border */
    /* its client's first window took the keys: it is told it is activated */
    CHECK(configured(x, t, &c));
    CHECK(c.w == 64 && c.h == 64 && c.nstates == 1);
    CHECK(has_state(&c, JWL_XDG_TOPLEVEL_STATE_ACTIVATED));
    return true;
}

static bool maximise_and_back(struct ct_comp *p, struct xc *x, struct tl *t)
{
    struct cfg c;
    CHECK_ST(jwl_xdg_toplevel_set_maximized(x->k.c, t->top), OK);
    CHECK(configured(x, t, &c));
    CHECK(c.w == W && c.h == H - TITLE);
    CHECK(c.nstates == 2 && has_state(&c, JWL_XDG_TOPLEVEL_STATE_MAXIMIZED));
    CHECK(has_state(&c, JWL_XDG_TOPLEVEL_STATE_ACTIVATED));
    CHECK(ack_commit(x, t, &c, buffer(x, 16384, W, H - TITLE, GREEN)));
    CHECK(pixel(p, 0, TITLE, GREEN));
    CHECK(pixel(p, W - 1, H - 1, GREEN));
    uint32_t bar = p->image[(TITLE - 1) * W];   /* the title bar, drawn by the same paint */
    CHECK(bar != GREEN && bar != SPLASH_BG);
    CHECK_ST(jwl_xdg_toplevel_unset_maximized(x->k.c, t->top), OK);
    CHECK(configured(x, t, &c));
    CHECK(c.w == 64 && c.h == 64 && c.nstates == 1);   /* the size it had, activated */
    CHECK(ack_commit(x, t, &c, buffer(x, 0, 64, 64, RED)));
    CHECK(pixel(p, 0, TITLE, SPLASH_BG));
    CHECK(pixel(p, (W - 64) / 2 + 32, (H - 64) / 2 + 32, RED));
    /* full screen: all of it, no title bar */
    CHECK_ST(jwl_xdg_toplevel_set_fullscreen(x->k.c, t->top, 0), OK);
    CHECK(configured(x, t, &c));
    CHECK(c.w == W && c.h == H && has_state(&c, JWL_XDG_TOPLEVEL_STATE_FULLSCREEN));
    CHECK(ack_commit(x, t, &c, buffer(x, 16384 + W * H * 4, W, H, BLUE)));
    CHECK(pixel(p, 0, 0, BLUE));
    CHECK_ST(jwl_xdg_toplevel_unset_fullscreen(x->k.c, t->top), OK);
    CHECK(configured(x, t, &c));
    CHECK(c.w == 64 && c.h == 64 && c.nstates == 1);
    CHECK(ack_commit(x, t, &c, buffer(x, 0, 64, 64, RED)));
    CHECK(pixel(p, 0, 0, SPLASH_BG));
    return true;
}

/* A null buffer unmaps; the initial commit and a configure map it again. */
static bool unmap_remap(struct ct_comp *p, struct xc *x, struct tl *t)
{
    struct cfg c;
    CHECK(ack_commit(x, t, NULL, 0));
    CHECK(pixel(p, (W - 64) / 2 + 32, (H - 64) / 2 + 32, SPLASH_BG));
    CHECK(ack_commit(x, t, NULL, 0));   /* the initial commit, again */
    CHECK(configured(x, t, &c));
    CHECK(ack_commit(x, t, &c, buffer(x, 0, 64, 64, RED)));
    CHECK(pixel(p, (W - 64) / 2 + 32, (H - 64) / 2 + 32, RED));
    /* the role object, then the xdg_surface, then the surface: no error */
    CHECK_ST(jwl_xdg_toplevel_destroy(x->k.c, t->top), OK);
    CHECK_ST(jwl_xdg_surface_destroy(x->k.c, t->xs), OK);
    CHECK_ST(jwl_wl_surface_destroy(x->k.c, t->s), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    CHECK(!x->k.errored);
    CHECK(pixel(p, (W - 64) / 2 + 32, (H - 64) / 2 + 32, SPLASH_BG));
    return true;
}

bool t_xdg_toplevel(void)
{
    static struct xc x;
    struct ct_comp p;
    struct tl t;
    CHECK(ct_start(&p, W, H));
    CHECK(xc_open(&p, &x));
    bool ok = map_floating(&p, &x, &t) && maximise_and_back(&p, &x, &t) &&
              unmap_remap(&p, &x, &t);
    xc_close(&x);
    CHECK(ct_stop(&p));
    return ok;
}

/* ---- tiling, from the start argument -------------------------------------------------- */

static bool tiling_steps(struct ct_comp *p, struct xc *x)
{
    struct tl a, b;
    struct cfg c;
    /* alone: the whole output less the gap (6) and its border (2): no
     * title bar when tiling */
    CHECK(toplevel(x, &a));
    CHECK(ack_commit(x, &a, NULL, 0));
    CHECK(configured(x, &a, &c));
    CHECK(c.w == W - 16 && c.h == H - 16);
    CHECK(ack_commit(x, &a, &c, buffer(x, 0, c.w, c.h, RED)));
    CHECK(pixel(p, 8, 8, RED));
    /* a window that can't resize joins: it is centred in the right half,
     * the first one is asked to take the left half */
    CHECK(toplevel(x, &b));
    CHECK_ST(jwl_xdg_toplevel_set_min_size(x->k.c, b.top, 64, 64), OK);
    CHECK_ST(jwl_xdg_toplevel_set_max_size(x->k.c, b.top, 64, 64), OK);
    CHECK(ack_commit(x, &b, NULL, 0));
    struct cfg cb;
    CHECK(configured(x, &b, &cb));
    CHECK(cb.w == 0 && cb.h == 0);
    CHECK(ack_commit(x, &b, &cb, buffer(x, 300000, 64, 64, GREEN)));
    CHECK(pixel(p, 165 + (147 - 64) / 2 + 32, 8 + (224 - 64) / 2 + 32, GREEN));
    CHECK(configured(x, &a, &c));
    CHECK(c.w == 147 && c.h == 224);
    CHECK(ack_commit(x, &a, &c, buffer(x, 600000, c.w, c.h, BLUE)));
    CHECK(pixel(p, 8, 8, BLUE));
    CHECK(pixel(p, 159, 100, SPLASH_BG));   /* the gap between the tiles */
    return true;
}

bool t_xdg_tiling(void)
{
    static struct xc x;
    struct ct_comp p;
    CHECK(ct_start_arg(&p, W, H, "layout=tiling"));
    CHECK(xc_open(&p, &x));
    bool ok = tiling_steps(&p, &x);
    xc_close(&x);
    CHECK(ct_stop(&p));
    return ok;
}

/* ---- popups, and requests that do nothing ---------------------------------------------- */

static bool popup_steps(struct xc *x)
{
    struct tl t;
    struct cfg c;
    CHECK(toplevel(x, &t));
    CHECK(ack_commit(x, &t, NULL, 0));
    CHECK(configured(x, &t, &c));
    uint32_t pos = ct_new(&x->k, &jwl_xdg_positioner_interface, 1), ps, pxs;
    CHECK(pos);
    CHECK_ST(jwl_xdg_wm_base_create_positioner(x->k.c, x->wm, pos), OK);
    CHECK_ST(jwl_xdg_positioner_set_size(x->k.c, pos, 100, 50), OK);
    CHECK_ST(jwl_xdg_positioner_set_anchor_rect(x->k.c, pos, 0, 0, 10, 10), OK);
    CHECK_ST(jwl_xdg_positioner_set_anchor(x->k.c, pos, JWL_XDG_POSITIONER_ANCHOR_BOTTOM), OK);
    CHECK_ST(jwl_xdg_positioner_set_gravity(x->k.c, pos, JWL_XDG_POSITIONER_GRAVITY_BOTTOM), OK);
    CHECK_ST(jwl_xdg_positioner_set_constraint_adjustment(x->k.c, pos, 63), OK);
    CHECK_ST(jwl_xdg_positioner_set_offset(x->k.c, pos, -5, 5), OK);
    CHECK(surface(x, &ps) && xdg_surface(x, ps, &pxs));
    uint32_t popup = ct_new(&x->k, &jwl_xdg_popup_interface, 1);
    CHECK(popup);
    CHECK_ST(jwl_xdg_surface_get_popup(x->k.c, pxs, popup, t.xs, pos), OK);
    CHECK(ct_await(&x->k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_POPUP_DONE, popup, CT_WAIT));
    /* minimising: ignored, as the protocol allows (move, resize, the window
     * menu and a popup's grab name a wl_seat: the seat's tests) */
    CHECK_ST(jwl_xdg_toplevel_set_minimized(x->k.c, t.top), OK);
    CHECK_ST(jwl_xdg_popup_destroy(x->k.c, popup), OK);
    CHECK_ST(jwl_xdg_surface_destroy(x->k.c, pxs), OK);
    CHECK_ST(jwl_wl_surface_destroy(x->k.c, ps), OK);
    CHECK_ST(jwl_xdg_positioner_destroy(x->k.c, pos), OK);
    CHECK_ST(ct_roundtrip(&x->k), OK);
    CHECK(!x->k.errored);
    return true;
}

bool t_xdg_popup(void)
{
    static struct xc x;
    struct ct_comp p;
    CHECK(ct_start(&p, W, H));
    CHECK(xc_open(&p, &x));
    bool ok = popup_steps(&x);
    xc_close(&x);
    CHECK(ct_stop(&p));
    return ok;
}

/* ---- configures a client doesn't ack -------------------------------------------------- */

static unsigned configures(const struct xc *x, const struct tl *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < x->k.nev; i++)
        n += x->k.ev[i].iface == &jwl_xdg_surface_interface &&
             x->k.ev[i].op == JWL_XDG_SURFACE_EV_CONFIGURE && x->k.ev[i].id == t->xs;
    return n;
}

/* At most XDG_CONFIGS_MAX (8) wait for an ack; past that the newest wish
 * waits, and goes when the client acks. */
static bool unacked_steps(struct xc *x)
{
    struct tl t;
    struct cfg c;
    CHECK(toplevel(x, &t));
    CHECK(ack_commit(x, &t, NULL, 0));
    for (unsigned i = 0; i < 10; i++) {
        CHECK_ST(jwl_xdg_toplevel_set_maximized(x->k.c, t.top), OK);
        CHECK_ST(jwl_xdg_toplevel_unset_maximized(x->k.c, t.top), OK);
    }
    CHECK_ST(ct_roundtrip(&x->k), OK);
    CHECK_EQ(configures(x, &t), 8);
    CHECK(configured(x, &t, &c));
    CHECK(has_state(&c, JWL_XDG_TOPLEVEL_STATE_MAXIMIZED));   /* the 8th: initial, max, ... */
    CHECK_ST(jwl_xdg_surface_ack_configure(x->k.c, t.xs, c.serial), OK);
    CHECK(configured(x, &t, &c));                            /* the newest wish, now */
    CHECK(c.nstates == 0);
    CHECK(ack_commit(x, &t, &c, buffer(x, 0, 64, 64, RED)));
    CHECK(!x->k.errored);
    return true;
}

bool t_xdg_unacked(void)
{
    static struct xc x;
    struct ct_comp p;
    CHECK(ct_start(&p, W, H));
    CHECK(xc_open(&p, &x));
    bool ok = unacked_steps(&x);
    xc_close(&x);
    CHECK(ct_stop(&p));
    return ok;
}

/* ---- protocol errors ----------------------------------------------------------------- */

/* What a bad client made, and which object the error must name. */
struct bad {
    struct tl t;
    uint32_t pos, s2, xs2;
};

enum named { N_XS, N_TOP, N_WM, N_POS, N_DISPLAY };

static bool positioner(struct xc *x, struct bad *b, bool complete)
{
    CHECK((b->pos = ct_new(&x->k, &jwl_xdg_positioner_interface, 1)) != 0);
    CHECK_ST(jwl_xdg_wm_base_create_positioner(x->k.c, x->wm, b->pos), OK);
    CHECK_ST(jwl_xdg_positioner_set_size(x->k.c, b->pos, 10, 10), OK);
    if (complete)
        CHECK_ST(jwl_xdg_positioner_set_anchor_rect(x->k.c, b->pos, 0, 0, 1, 1), OK);
    return true;
}

/* Attach buffer (0: none) and commit, without waiting: the error comes. */
static bool commit_raw(struct xc *x, const struct tl *t, uint32_t buffer)
{
    return jwl_wl_surface_attach(x->k.c, t->s, buffer, 0, 0) == OK &&
           jwl_wl_surface_commit(x->k.c, t->s) == OK;
}

static bool configured_toplevel(struct xc *x, struct bad *b, struct cfg *c)
{
    CHECK(toplevel(x, &b->t));
    CHECK(ack_commit(x, &b->t, NULL, 0));
    return configured(x, &b->t, c);
}

/* One bad thing each (the error codes are xdg-shell.xml's, wl_surface's
 * defunct_role_object wayland.xml's). */
static bool bad_case(unsigned i, struct xc *x, struct bad *b)
{
    struct cfg c;
    struct jwl_conn *k = x->k.c;
    switch (i) {
    case 0:   /* ack a configure never sent */
        CHECK(configured_toplevel(x, b, &c));
        return jwl_xdg_surface_ack_configure(k, b->t.xs, c.serial + 1000) == OK;
    case 1:   /* a buffer on the initial commit */
        CHECK(toplevel(x, &b->t));
        return commit_raw(x, &b->t, buffer(x, 0, 8, 8, RED));
    case 2:   /* a buffer before the configure was acked */
        CHECK(configured_toplevel(x, b, &c));
        return commit_raw(x, &b->t, buffer(x, 0, 8, 8, RED));
    case 3:   /* two xdg_surfaces for one wl_surface */
        CHECK(surface(x, &b->t.s) && xdg_surface(x, b->t.s, &b->t.xs));
        return xdg_surface(x, b->t.s, &b->xs2);
    case 4:   /* a second role object */
        CHECK(toplevel(x, &b->t));
        b->s2 = ct_new(&x->k, &jwl_xdg_toplevel_interface, 1);
        return jwl_xdg_surface_get_toplevel(k, b->t.xs, b->s2) == OK;
    case 5:   /* the xdg_surface before its toplevel */
        CHECK(toplevel(x, &b->t));
        return jwl_xdg_surface_destroy(k, b->t.xs) == OK;
    case 6:   /* the xdg_wm_base before its xdg_surfaces */
        CHECK(toplevel(x, &b->t));
        return jwl_xdg_wm_base_destroy(k, x->wm) == OK;
    case 7:   /* a negative min size */
        CHECK(toplevel(x, &b->t));
        return jwl_xdg_toplevel_set_min_size(k, b->t.top, -1, 5) == OK;
    case 8:   /* max under min, found at the commit */
        CHECK(toplevel(x, &b->t));
        CHECK_ST(jwl_xdg_toplevel_set_min_size(k, b->t.top, 20, 20), OK);
        CHECK_ST(jwl_xdg_toplevel_set_max_size(k, b->t.top, 10, 30), OK);
        return commit_raw(x, &b->t, 0);
    case 9:   /* an empty window geometry */
        CHECK(configured_toplevel(x, b, &c));
        return jwl_xdg_surface_set_window_geometry(k, b->t.xs, 0, 0, 0, 10) == OK;
    case 10:   /* a positioner of no size */
        CHECK((b->pos = ct_new(&x->k, &jwl_xdg_positioner_interface, 1)) != 0);
        CHECK_ST(jwl_xdg_wm_base_create_positioner(k, x->wm, b->pos), OK);
        return jwl_xdg_positioner_set_size(k, b->pos, 0, 5) == OK;
    case 11:   /* an anchor past the enum */
        CHECK(positioner(x, b, true));
        return jwl_xdg_positioner_set_anchor(k, b->pos, 9) == OK;
    case 12:   /* a popup on an incomplete positioner */
        CHECK(toplevel(x, &b->t) && positioner(x, b, false));
        CHECK(surface(x, &b->s2) && xdg_surface(x, b->s2, &b->xs2));
        return jwl_xdg_surface_get_popup(k, b->xs2, ct_new(&x->k, &jwl_xdg_popup_interface, 1),
                                         b->t.xs, b->pos) == OK;
    case 13:   /* a popup that is its own parent */
        CHECK(positioner(x, b, true));
        CHECK(surface(x, &b->s2) && xdg_surface(x, b->s2, &b->xs2));
        return jwl_xdg_surface_get_popup(k, b->xs2, ct_new(&x->k, &jwl_xdg_popup_interface, 1),
                                         b->xs2, b->pos) == OK;
    case 14:   /* a commit on an xdg_surface with no role */
        CHECK(surface(x, &b->t.s) && xdg_surface(x, b->t.s, &b->t.xs));
        return jwl_wl_surface_commit(k, b->t.s) == OK;
    case 15:   /* an xdg_surface for a surface with a buffer attached */
        CHECK(surface(x, &b->t.s));
        CHECK_ST(jwl_wl_surface_attach(k, b->t.s, buffer(x, 0, 8, 8, RED), 0, 0), OK);
        return xdg_surface(x, b->t.s, &b->t.xs);
    case 16:   /* its own parent */
        CHECK(toplevel(x, &b->t));
        return jwl_xdg_toplevel_set_parent(k, b->t.top, b->t.top) == OK;
    case 17:   /* the wl_surface before its toplevel */
        CHECK(toplevel(x, &b->t));
        return jwl_wl_surface_destroy(k, b->t.s) == OK;
    case 18:   /* an ack before the role */
        CHECK(surface(x, &b->t.s) && xdg_surface(x, b->t.s, &b->t.xs));
        return jwl_xdg_surface_ack_configure(k, b->t.xs, 1) == OK;
    default:   /* an ack of a configure an ack of a newer one took */
        CHECK(configured_toplevel(x, b, &c));
        CHECK_ST(jwl_xdg_toplevel_set_maximized(k, b->t.top), OK);
        uint32_t first = c.serial;
        CHECK(configured(x, &b->t, &c));
        CHECK_ST(jwl_xdg_surface_ack_configure(k, b->t.xs, c.serial), OK);
        return jwl_xdg_surface_ack_configure(k, b->t.xs, first) == OK;
    }
}

#define NBAD 20u

/* Each case's error: the object it names (a destroyed one is named as the
 * display: its id is gone) and its code. */
static const struct {
    enum named obj;
    uint32_t code;
} bad_want[NBAD] = {
    { N_XS, JWL_XDG_SURFACE_ERROR_INVALID_SERIAL },
    { N_XS, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER },
    { N_XS, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER },
    { N_WM, JWL_XDG_WM_BASE_ERROR_ROLE },
    { N_XS, JWL_XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED },
    { N_DISPLAY, JWL_XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT },
    { N_DISPLAY, JWL_XDG_WM_BASE_ERROR_DEFUNCT_SURFACES },
    { N_TOP, JWL_XDG_TOPLEVEL_ERROR_INVALID_SIZE },
    { N_TOP, JWL_XDG_TOPLEVEL_ERROR_INVALID_SIZE },
    { N_XS, JWL_XDG_SURFACE_ERROR_INVALID_SIZE },
    { N_POS, JWL_XDG_POSITIONER_ERROR_INVALID_INPUT },
    { N_POS, JWL_XDG_POSITIONER_ERROR_INVALID_INPUT },
    { N_WM, JWL_XDG_WM_BASE_ERROR_INVALID_POSITIONER },
    { N_WM, JWL_XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT },
    { N_XS, JWL_XDG_SURFACE_ERROR_NOT_CONSTRUCTED },
    { N_XS, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER },
    { N_TOP, JWL_XDG_TOPLEVEL_ERROR_INVALID_PARENT },
    { N_DISPLAY, JWL_WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT },
    { N_XS, JWL_XDG_SURFACE_ERROR_NOT_CONSTRUCTED },
    { N_XS, JWL_XDG_SURFACE_ERROR_INVALID_SERIAL },
};

static uint32_t named(enum named n, const struct xc *x, const struct bad *b)
{
    switch (n) {
    case N_XS:
        return b->t.xs;
    case N_TOP:
        return b->t.top;
    case N_WM:
        return x->wm;
    case N_POS:
        return b->pos;
    default:
        return JWL_DISPLAY_ID;
    }
}

bool t_xdg_errors(void)
{
    static struct xc x, good;
    struct ct_comp p;
    CHECK(ct_start(&p, W, H));
    CHECK(xc_open(&p, &good));   /* connected all along: it must not notice */
    uint64_t base = ct_handles(&p);
    for (unsigned i = 0; i < NBAD; i++) {
        struct bad b = { 0 };
        CHECK(xc_open(&p, &x));
        if (!bad_case(i, &x, &b))
            FAIL("case %u: a request didn't go", i);
        if (!ct_expect_error(&x.k, named(bad_want[i].obj, &x, &b), bad_want[i].code))
            FAIL("case %u: not the error it should be", i);
        xc_close(&x);
    }
    CHECK(ct_handles_back(&p, base));   /* every bad client's objects gone */
    struct tl t;
    CHECK(map_floating(&p, &good, &t));   /* and the compositor still serves */
    CHECK(!good.k.errored);
    xc_close(&good);
    CHECK(ct_stop(&p));
    return true;
}
