/* utest: xdg_popup on bin/compositor headless with the real seat
 * (user/services/compositor xdgpopup.c; compseat.h's harness: 640x480,
 * floating, the desktop off, a fake keyboard and mouse). Each client's
 * toplevel is 100x80 with the compositor's title bar, centred: its
 * surface at (270, 200).
 *
 * t_popup_show: a popup configured at its positioner's place and shown
 * there, its pixels exactly its buffer (no title bar, outline, corners or
 * shadow of ours), over its parent; following its parent (maximised: moved
 * with it and kept above it, as the parent was raised); reposition
 * (repositioned, then a configure; the old place kept until the ack and
 * the commit); at the output's right edge flipped to the left of its
 * anchor, and at the bottom slid up (the screenshots of a popup at the
 * screen's edges); all dismissed (popup_done, gone from the screen) when
 * the parent is unmapped.
 * t_popup_grab: a menu taking the grab with the serial of a click on its
 * toplevel: the keys go to it (wl_keyboard.leave on the toplevel, enter
 * on the popup), and still to its client; while it holds the grab another
 * client's window gets no pointer; a submenu's grab takes the keys on, and
 * gives them back when it is destroyed; a click outside (the background)
 * dismisses the chain and reaches no one, and the keys go back to the
 * toplevel; the client's next grabs, with its old serial or the enter the
 * keys coming back gave it, are refused (popup_done at once) and another
 * client's window sees the pointer, until a key press lets it grab again
 * (review GR's finding 1). A grab with a serial the client was never
 * sent, or while its toplevel doesn't have the keys, is refused:
 * popup_done at once. A tooltip (no grab) takes no keys and stays up when
 * another window is clicked; the focus moving away (Alt+Tab) dismisses a
 * grab. A menu hidden by a commit with no buffer takes the popups on it
 * along, shown or not, out of the grab (review GR's Low 4): a grab on it
 * after that is dismissed, not a protocol error, and it can grab again.
 * t_popup_bad: the protocol errors: a grab after the popup was mapped, or
 * on a popup that took no grab (invalid_grab); a popup destroyed while
 * another is on it (not_the_topmost_popup); a popup with no parent at its
 * initial commit (invalid_popup_parent). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/input.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "compseat.h"
#include "utest.h"

#define TOP_W  100
#define TOP_H  80
#define TOP_X  ((OUT_W - TOP_W) / 2)   /* 270: its surface on the output */
#define TOP_Y  ((OUT_H - TOP_H) / 2)   /* 200 */
#define TITLE  28
#define POOL   (4u << 20)
#define RED    0x00ff0000u
#define GREEN  0x0000ff00u
#define BLUE   0x000000ffu
#define YELLOW 0x00ffff00u
#define CYAN   0x0000ffffu
#define BR     JWL_XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT

/* A client with a seat (compseat.h's), xdg_wm_base 6 and a pool. */
struct pc {
    struct sc c;
    uint32_t wm, pool;
    handle_t vmo;
    uint32_t used;                 /* bytes of the pool given to buffers */
};

/* A toplevel's objects. */
struct top {
    uint32_t s, xs, top;
};

/* A popup's objects, and its last configure. */
struct pop {
    uint32_t s, xs, popup;
    int32_t x, y, w, h;
    uint32_t serial;
};

/* What a positioner says. */
struct place {
    int32_t w, h, ax, ay, aw, ah;
    uint32_t anchor, gravity, adjust;
};

static bool pc_open(struct cs *t, struct pc *p)
{
    memset(p, 0, sizeof(*p));
    CHECK(cs_client(t, &p->c));
    struct ct_client *k = &p->c.k;
    CHECK((p->wm = ct_new(k, &jwl_xdg_wm_base_interface, 6)) != 0);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 5, "xdg_wm_base", 6, p->wm), OK);
    CHECK((p->pool = ct_pool(k, POOL, &p->vmo)) != 0);
    CHECK_ST(ct_roundtrip(k), OK);
    return true;
}

static void pc_close(struct pc *p)
{
    if (p->vmo)
        jam_handle_close(p->vmo);
    p->vmo = HANDLE_INVALID;
    ct_close(&p->c.k);
}

/* A w x h xrgb buffer of one colour, from the pool. */
static uint32_t buffer(struct pc *p, int32_t w, int32_t h, uint32_t colour)
{
    static uint32_t row[OUT_W];
    uint32_t off = p->used, bytes = (uint32_t)(w * h * 4);
    if (off + bytes > POOL)
        off = 0;   /* the oldest buffers are long released */
    for (int32_t i = 0; i < w; i++)
        row[i] = colour;
    for (int32_t y = 0; y < h; y++)
        if (jam_vmo_write(p->vmo, off + (uint64_t)y * (uint64_t)w * 4, row, (uint64_t)w * 4) != OK)
            return 0;
    p->used = off + bytes;
    uint32_t id = ct_new(&p->c.k, &jwl_wl_buffer_interface, 1);
    if (!id || jwl_wl_shm_pool_create_buffer(p->c.k.c, p->pool, id, (int32_t)off, w, h, w * 4,
                                             JWL_WL_SHM_FORMAT_XRGB8888) != OK)
        return 0;
    return id;
}

static bool attach(struct pc *p, uint32_t s, uint32_t buf)
{
    struct jwl_conn *c = p->c.k.c;
    CHECK_ST(jwl_wl_surface_attach(c, s, buf, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_damage_buffer(c, s, 0, 0, OUT_W, OUT_H), OK);
    CHECK_ST(jwl_wl_surface_commit(c, s), OK);
    CHECK_ST(ct_roundtrip(&p->c.k), OK);
    return true;
}

/* The newest xdg_surface.configure on xs since ct_clear: its serial (0: none). */
static uint32_t surface_serial(const struct ct_client *k, uint32_t xs)
{
    uint32_t serial = 0;
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface == &jwl_xdg_surface_interface && k->ev[i].id == xs &&
            k->ev[i].op == JWL_XDG_SURFACE_EV_CONFIGURE)
            serial = k->ev[i].u[0];
    return serial;
}

/* A red toplevel with the compositor's title bar, mapped (centred). */
static bool toplevel(struct pc *p, struct top *t)
{
    struct ct_client *k = &p->c.k;
    t->s = ct_new(k, &jwl_wl_surface_interface, 4);
    t->xs = ct_new(k, &jwl_xdg_surface_interface, 6);
    t->top = ct_new(k, &jwl_xdg_toplevel_interface, 6);
    CHECK_ST(jwl_wl_compositor_create_surface(k->c, k->compositor, t->s), OK);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(k->c, p->wm, t->xs, t->s), OK);
    CHECK_ST(jwl_xdg_surface_get_toplevel(k->c, t->xs, t->top), OK);
    CHECK(ct_server_side(k, t->top));
    ct_clear(k);
    CHECK(attach(p, t->s, 0));   /* the initial commit */
    uint32_t serial = surface_serial(k, t->xs);
    CHECK(serial);
    CHECK_ST(jwl_xdg_surface_ack_configure(k->c, t->xs, serial), OK);
    CHECK(attach(p, t->s, buffer(p, TOP_W, TOP_H, RED)));
    return true;
}

/* A positioner saying pl: its id (0 on failure). */
static uint32_t positioner(struct pc *p, const struct place *pl)
{
    struct jwl_conn *c = p->c.k.c;
    uint32_t pos = ct_new(&p->c.k, &jwl_xdg_positioner_interface, 6);
    if (!pos || jwl_xdg_wm_base_create_positioner(c, p->wm, pos) != OK ||
        jwl_xdg_positioner_set_size(c, pos, pl->w, pl->h) != OK ||
        jwl_xdg_positioner_set_anchor_rect(c, pos, pl->ax, pl->ay, pl->aw, pl->ah) != OK ||
        jwl_xdg_positioner_set_anchor(c, pos, pl->anchor) != OK ||
        jwl_xdg_positioner_set_gravity(c, pos, pl->gravity) != OK ||
        jwl_xdg_positioner_set_constraint_adjustment(c, pos, pl->adjust) != OK)
        return 0;
    return pos;
}

/* u's newest configure since ct_clear, if one came (u->serial 0 if not). */
static void configured(struct pc *p, struct pop *u)
{
    const struct ct_client *k = &p->c.k;
    u->serial = 0;
    for (unsigned i = 0; i < k->nev; i++) {
        const struct ct_event *e = &k->ev[i];
        if (e->iface != &jwl_xdg_popup_interface || e->id != u->popup ||
            e->op != JWL_XDG_POPUP_EV_CONFIGURE)
            continue;
        u->x = (int32_t)e->u[0], u->y = (int32_t)e->u[1];
        u->w = (int32_t)e->u[2], u->h = (int32_t)e->u[3];
        u->serial = surface_serial(k, u->xs);
    }
}

/* A popup's objects on parent (an xdg_surface), placed by pl, with no
 * commit yet. */
static bool popup_objects(struct pc *p, struct pop *u, uint32_t parent, const struct place *pl)
{
    struct ct_client *k = &p->c.k;
    struct jwl_conn *c = k->c;
    memset(u, 0, sizeof(*u));
    uint32_t pos = positioner(p, pl);
    CHECK(pos);
    u->s = ct_new(k, &jwl_wl_surface_interface, 4);
    u->xs = ct_new(k, &jwl_xdg_surface_interface, 6);
    u->popup = ct_new(k, &jwl_xdg_popup_interface, 6);
    CHECK_ST(jwl_wl_compositor_create_surface(c, k->compositor, u->s), OK);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(c, p->wm, u->xs, u->s), OK);
    CHECK_ST(jwl_xdg_surface_get_popup(c, u->xs, u->popup, parent, pos), OK);
    CHECK_ST(jwl_xdg_positioner_destroy(c, pos), OK);
    return true;
}

/* A popup on parent placed by pl, up to its initial commit (grab: with
 * that serial first, 0: none); its configure in *u if one came. */
static bool popup(struct pc *p, struct pop *u, uint32_t parent, const struct place *pl,
                  uint32_t grab)
{
    CHECK(popup_objects(p, u, parent, pl));
    if (grab)
        CHECK_ST(jwl_xdg_popup_grab(p->c.k.c, u->popup, p->c.seat, grab), OK);
    ct_clear(&p->c.k);
    CHECK(attach(p, u->s, 0));
    configured(p, u);
    return true;
}

/* u's configure is (x, y) w x h; acked and drawn in colour. */
static bool show(struct pc *p, struct pop *u, int32_t x, int32_t y, int32_t w, int32_t h,
                 uint32_t colour)
{
    if (u->x != x || u->y != y || u->w != w || u->h != h || !u->serial)
        FAIL("popup configured at %d,%d %dx%d (serial %u), want %d,%d %dx%d", u->x, u->y, u->w,
             u->h, u->serial, x, y, w, h);
    CHECK(!ct_find(&p->c.k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_POPUP_DONE, u->popup));
    CHECK_ST(jwl_xdg_surface_ack_configure(p->c.k.c, u->xs, u->serial), OK);
    CHECK(attach(p, u->s, buffer(p, w, h, colour)));
    CHECK(!p->c.k.errored);
    return true;
}

/* Wait (up to CT_WAIT) until the output has want at (x, y). */
static bool pixel(const struct cs *t, int32_t x, int32_t y, uint32_t want)
{
    uint64_t until = now() + CT_WAIT;
    const volatile uint32_t *img = t->p.image;
    while (img[y * OUT_W + x] != want && now() < until)
        jam_sleep_until(now() + NS_PER_MS);
    if (img[y * OUT_W + x] != want)
        FAIL("pixel %d,%d is %06x, want %06x", x, y, img[y * OUT_W + x], want);
    return true;
}

static uint32_t at(const struct cs *t, int32_t x, int32_t y)
{
    return ((const volatile uint32_t *)t->p.image)[y * OUT_W + x];
}

static bool done(struct pc *p, const struct pop *u)
{
    return ct_await(&p->c.k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_POPUP_DONE, u->popup,
                    CT_WAIT) != NULL;
}

static bool destroy(struct pc *p, const struct pop *u)
{
    struct jwl_conn *c = p->c.k.c;
    CHECK_ST(jwl_xdg_popup_destroy(c, u->popup), OK);
    CHECK_ST(jwl_xdg_surface_destroy(c, u->xs), OK);
    CHECK_ST(jwl_wl_surface_destroy(c, u->s), OK);
    CHECK_ST(ct_roundtrip(&p->c.k), OK);
    CHECK(!p->c.k.errored);
    return true;
}

/* Wait (up to CT_WAIT) until (x, y) is no longer colour. */
static bool gone(const struct cs *t, int32_t x, int32_t y, uint32_t colour)
{
    uint64_t until = now() + CT_WAIT;
    while (at(t, x, y) == colour && now() < until)
        jam_sleep_until(now() + NS_PER_MS);
    if (at(t, x, y) == colour)
        FAIL("pixel %d,%d is still %06x", x, y, colour);
    return true;
}

/* Since ct_clear, p's keyboard got leave on surface from (0: any; a surface
 * the client destroyed meanwhile comes as no object), then enter on to
 * (and nothing entered after). */
static bool keys_moved(struct pc *p, uint32_t from, uint32_t to)
{
    const struct ct_client *k = &p->c.k;
    int left = -1, entered = -1;
    for (unsigned i = 0; i < k->nev; i++) {
        const struct ct_event *e = &k->ev[i];
        if (e->iface != &jwl_wl_keyboard_interface)
            continue;
        if (e->op == JWL_WL_KEYBOARD_EV_LEAVE && (e->u[1] == from || !from))
            left = (int)i;
        if (e->op == JWL_WL_KEYBOARD_EV_ENTER)
            entered = e->u[1] == to ? (int)i : -1;
    }
    if (left < 0 || entered < left)
        FAIL("the keys didn't move from surface %u to %u (leave at %d, enter at %d)", from, to,
             left, entered);
    return true;
}

/* Did p's keyboard get an enter on surface s since ct_clear? */
static bool entered_on(const struct pc *p, uint32_t s)
{
    const struct ct_client *k = &p->c.k;
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface == &jwl_wl_keyboard_interface &&
            k->ev[i].op == JWL_WL_KEYBOARD_EV_ENTER && k->ev[i].u[1] == s)
            return true;
    return false;
}

/* A click (press and release) at (x, y); every client's events in. */
static bool click(struct cs *t, int32_t x, int32_t y, struct sc *a, struct sc *b)
{
    CHECK(cs_pointer_to(t, x, y));
    CHECK(cs_button(t, true) && cs_button(t, false));
    return cs_sync(a, b);
}

/* ---- t_popup_show ------------------------------------------------------------------------- */

/* u acked and drawn again in colour at the configure it got last. */
static bool redraw(struct pc *p, struct pop *u, uint32_t colour)
{
    configured(p, u);
    CHECK(u->serial);
    CHECK_ST(jwl_xdg_surface_ack_configure(p->c.k.c, u->xs, u->serial), OK);
    return attach(p, u->s, buffer(p, u->w, u->h, colour));
}

/* Two popups shown on t: one past its bottom right corner, one over it. */
static bool shown_steps(struct cs *t, struct pc *a, struct top *tl, struct pop *p, struct pop *q)
{
    CHECK(pixel(t, TOP_X + 50, TOP_Y + 40, RED));
    /* what is beside and above where the first popup goes, before it */
    uint32_t left = at(t, TOP_X + 99, TOP_Y + 100), above = at(t, TOP_X + 130, TOP_Y + 79);
    const struct place pp = { 60, 40, 90, 70, 10, 10, BR, BR, 0 };
    CHECK(popup(a, p, tl->xs, &pp, 0));
    CHECK(show(a, p, 100, 80, 60, 40, GREEN));
    CHECK(pixel(t, TOP_X + 130, TOP_Y + 100, GREEN));
    CHECK(pixel(t, TOP_X + 100, TOP_Y + 80, GREEN));   /* square corners: none of ours */
    CHECK(pixel(t, TOP_X + 159, TOP_Y + 119, GREEN));
    CHECK_EQ(at(t, TOP_X + 99, TOP_Y + 100), left);    /* no outline, title bar or shadow */
    CHECK_EQ(at(t, TOP_X + 130, TOP_Y + 79), above);
    const struct place qp = { 40, 30, 10, 10, 10, 10, BR, BR, 0 };
    CHECK(popup(a, q, tl->xs, &qp, 0));
    CHECK(show(a, q, 20, 20, 40, 30, BLUE));
    CHECK(pixel(t, TOP_X + 30, TOP_Y + 30, BLUE));     /* over its parent */
    CHECK(pixel(t, TOP_X + 5, TOP_Y + 5, RED));
    return true;
}

/* The parent maximised: raised and moved; the popups follow, above it. */
static bool follow_steps(struct cs *t, struct pc *a, struct top *tl)
{
    struct ct_client *k = &a->c.k;
    ct_clear(k);
    CHECK_ST(jwl_xdg_toplevel_set_maximized(k->c, tl->top), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    uint32_t serial = surface_serial(k, tl->xs);
    CHECK(serial);
    CHECK_ST(jwl_xdg_surface_ack_configure(k->c, tl->xs, serial), OK);
    CHECK(attach(a, tl->s, buffer(a, OUT_W, OUT_H - TITLE, RED)));
    CHECK(pixel(t, 100 + 30, TITLE + 80 + 20, GREEN));
    CHECK(pixel(t, 20 + 10, TITLE + 20 + 10, BLUE));
    CHECK(pixel(t, 5, TITLE + 5, RED));
    return true;
}

/* reposition: repositioned with the token, then a configure; the place
 * changes with the ack and the commit. */
static bool reposition_steps(struct cs *t, struct pc *a, struct pop *p)
{
    struct ct_client *k = &a->c.k;
    const struct place np = { 60, 40, 200, 100, 10, 10, JWL_XDG_POSITIONER_ANCHOR_TOP_LEFT, BR,
                              0 };
    uint32_t pos = positioner(a, &np);
    CHECK(pos);
    ct_clear(k);
    CHECK_ST(jwl_xdg_popup_reposition(k->c, p->popup, pos, 7), OK);
    CHECK_ST(jwl_xdg_positioner_destroy(k->c, pos), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    const struct ct_event *r = ct_find(k, &jwl_xdg_popup_interface,
                                       JWL_XDG_POPUP_EV_REPOSITIONED, p->popup);
    const struct ct_event *c = ct_find(k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_CONFIGURE,
                                       p->popup);
    CHECK(r && c && r < c);
    CHECK_EQ(r->u[0], 7u);
    CHECK(pixel(t, 100 + 30, TITLE + 80 + 20, GREEN));   /* where it was, until it draws */
    CHECK(redraw(a, p, GREEN));
    CHECK(p->x == 200 && p->y == 100);
    CHECK(pixel(t, 200 + 30, TITLE + 100 + 20, GREEN));
    CHECK(pixel(t, 100 + 30, TITLE + 80 + 20, RED));
    return true;
}

/* At the output's edges: flipped left of its anchor at the right edge,
 * slid up at the bottom. */
static bool edge_steps(struct cs *t, struct pc *a, struct top *tl, struct pop *e, struct pop *s)
{
    const struct place ep = { 80, 40, 630, 100, 10, 10, JWL_XDG_POSITIONER_ANCHOR_RIGHT,
                              JWL_XDG_POSITIONER_ANCHOR_RIGHT,
                              JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X };
    CHECK(popup(a, e, tl->xs, &ep, 0));
    CHECK(show(a, e, 550, 85, 80, 40, YELLOW));
    CHECK(pixel(t, 550, TITLE + 85, YELLOW));
    CHECK(pixel(t, 629, TITLE + 124, YELLOW));
    CHECK(pixel(t, 635, TITLE + 105, RED));   /* the anchor's right of it: its parent */
    const struct place sp = { 60, 40, 300, 440, 10, 10, JWL_XDG_POSITIONER_ANCHOR_BOTTOM,
                              JWL_XDG_POSITIONER_ANCHOR_BOTTOM,
                              JWL_XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y };
    CHECK(popup(a, s, tl->xs, &sp, 0));
    CHECK(show(a, s, 275, OUT_H - TITLE - 40, 60, 40, CYAN));
    CHECK(pixel(t, 275, OUT_H - 40, CYAN));
    CHECK(pixel(t, 334, OUT_H - 1, CYAN));
    return true;
}

static bool show_steps(struct cs *t, struct pc *a)
{
    struct top tl;
    struct pop p, q, e, s;
    CHECK(toplevel(a, &tl));
    CHECK(shown_steps(t, a, &tl, &p, &q));
    CHECK(follow_steps(t, a, &tl));
    CHECK(reposition_steps(t, a, &p));
    CHECK(edge_steps(t, a, &tl, &e, &s));
    /* the parent unmapped: every popup on it dismissed, gone from the screen */
    ct_clear(&a->c.k);
    CHECK(attach(a, tl.s, 0));
    CHECK(done(a, &p) && done(a, &q) && done(a, &e) && done(a, &s));
    CHECK(gone(t, 200 + 30, TITLE + 100 + 20, GREEN));
    CHECK(gone(t, 550, TITLE + 85, YELLOW));
    CHECK(destroy(a, &p) && destroy(a, &q) && destroy(a, &e) && destroy(a, &s));
    return true;
}

bool t_popup_show(void)
{
    static struct cs t;
    static struct pc a;
    CHECK(cs_start(&t));
    bool ok = pc_open(&t, &a) && show_steps(&t, &a);
    pc_close(&a);
    CHECK(cs_stop(&t));
    return ok;
}

/* ---- t_popup_grab --------------------------------------------------------------------------- */

static const struct place menu = { 60, 40, 90, 70, 10, 10, BR, BR, 0 };

/* A menu grabbing with a click's serial: the keys go to it, the pointer
 * to its client alone. *serial: a serial a submenu may grab with. */
static bool menu_steps(struct cs *t, struct pc *a, struct sc *b, struct top *tl, struct pop *m,
                       uint32_t *serial)
{
    struct ct_client *k = &a->c.k;
    ct_clear(k);
    CHECK(click(t, TOP_X + 50, TOP_Y + 40, &a->c, b));
    const struct ct_event *press = find_ev(&a->c, &jwl_wl_pointer_interface,
                                           JWL_WL_POINTER_EV_BUTTON);
    CHECK(press);
    CHECK(popup(a, m, tl->xs, &menu, press->u[0]));
    CHECK(show(a, m, 100, 80, 60, 40, GREEN));
    CHECK(keys_moved(a, tl->s, m->s));
    ct_clear(k);
    ct_clear(&b->k);
    CHECK(cs_tap(t, U_A, 0) && cs_sync(&a->c, b));
    const uint32_t ka[] = { DOWN(KEY_A), UP(KEY_A) };
    CHECK(keys_are(k, ka, 2) && no_keys(&b->k));
    const struct ct_event *key = find_ev(&a->c, &jwl_wl_keyboard_interface,
                                         JWL_WL_KEYBOARD_EV_KEY);
    CHECK(key);
    *serial = key->u[0];
    /* over the other client's window: it sees nothing of the pointer */
    CHECK(cs_pointer_to(t, 70, 70) && cs_sync(&a->c, b));
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    /* over the menu (past its toplevel): its client's pointer there */
    ct_clear(k);
    CHECK(cs_pointer_to(t, TOP_X + 130, TOP_Y + 100) && cs_sync(&a->c, NULL));
    uint32_t over = 0;
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface == &jwl_wl_pointer_interface &&
            k->ev[i].op == JWL_WL_POINTER_EV_ENTER)
            over = k->ev[i].u[1];
    CHECK_EQ(over, m->s);
    return true;
}

/* After a click outside dismissed its menu, the client can't take the grab
 * straight back (review GR's finding 1): not with the serial it grabbed
 * with, nor with the enter the keys coming back gave it; and the pointer is
 * free again: another client's window sees it. */
static bool regrab_steps(struct cs *t, struct pc *a, struct sc *b, struct top *tl,
                         uint32_t serial, uint32_t enter)
{
    const uint32_t old[] = { serial, enter };
    for (unsigned i = 0; i < 2; i++) {
        struct pop r;
        CHECK(popup(a, &r, tl->xs, &menu, old[i]));
        CHECK(done(a, &r) && !r.serial);   /* dismissed at once, never configured */
        CHECK(destroy(a, &r));
    }
    ct_clear(&b->k);
    CHECK(cs_pointer_to(t, 70, 70) && cs_sync(&a->c, b));
    CHECK(find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    return true;
}

/* A submenu takes the keys on, and gives them back as it goes; a click on
 * the background dismisses the menu and reaches no one, and the client may
 * grab again only after its next key press. */
static bool chain_steps(struct cs *t, struct pc *a, struct sc *b, struct top *tl, struct pop *m,
                        uint32_t serial)
{
    struct ct_client *k = &a->c.k;
    struct pop s;
    const struct place sp = { 50, 30, 50, 10, 10, 10, JWL_XDG_POSITIONER_ANCHOR_TOP_RIGHT, BR,
                              0 };
    CHECK(popup(a, &s, m->xs, &sp, serial));
    CHECK(show(a, &s, 60, 10, 50, 30, BLUE));
    CHECK(keys_moved(a, m->s, s.s));
    CHECK(pixel(t, TOP_X + 100 + 60 + 10, TOP_Y + 80 + 10 + 10, BLUE));
    ct_clear(k);
    CHECK(destroy(a, &s));
    CHECK(keys_moved(a, 0, m->s));   /* (its wl_surface went in the same batch) */
    ct_clear(k);
    ct_clear(&b->k);
    CHECK(click(t, 600, 450, &a->c, b));
    CHECK(done(a, m));
    CHECK(!find_ev(&a->c, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    const struct ct_event *back = ct_await(k, &jwl_wl_keyboard_interface,
                                           JWL_WL_KEYBOARD_EV_ENTER, 0, CT_WAIT);
    CHECK(back && back->u[1] == tl->s);
    uint32_t enter = back->u[0];
    CHECK(gone(t, TOP_X + 130, TOP_Y + 100, GREEN));
    CHECK(regrab_steps(t, a, b, tl, serial, enter));
    CHECK(destroy(a, m));
    ct_clear(k);
    CHECK(cs_tap(t, U_A, 0) && cs_sync(&a->c, b));   /* the keys still its */
    const uint32_t ka[] = { DOWN(KEY_A), UP(KEY_A) };
    CHECK(keys_are(k, ka, 2));
    /* a key press: it may grab again */
    const struct ct_event *key = find_ev(&a->c, &jwl_wl_keyboard_interface,
                                         JWL_WL_KEYBOARD_EV_KEY);
    CHECK(key);
    struct pop r;
    CHECK(popup(a, &r, tl->xs, &menu, key->u[0]));
    CHECK(show(a, &r, 100, 80, 60, 40, GREEN));
    CHECK(keys_moved(a, tl->s, r.s));
    CHECK(destroy(a, &r));
    return true;
}

/* Grabs refused: a serial never sent, a toplevel without the keys. */
static bool refused_steps(struct cs *t, struct pc *a, struct sc *b, struct top *tl)
{
    struct ct_client *k = &a->c.k;
    struct pop d;
    ct_clear(k);
    CHECK(click(t, TOP_X + 50, TOP_Y + 40, &a->c, b));
    const struct ct_event *press = find_ev(&a->c, &jwl_wl_pointer_interface,
                                           JWL_WL_POINTER_EV_BUTTON);
    CHECK(press);
    uint32_t good = press->u[0];
    CHECK(popup(a, &d, tl->xs, &menu, good + 100000));
    CHECK(done(a, &d) && !d.serial);   /* dismissed at once, never configured */
    CHECK(destroy(a, &d));
    CHECK(click(t, 70, 70, &a->c, b));   /* the other window takes the keys */
    CHECK(popup(a, &d, tl->xs, &menu, good));
    CHECK(done(a, &d) && !d.serial);
    CHECK(destroy(a, &d));
    return true;
}

/* A tooltip takes no keys and stays up while another window is clicked;
 * the focus going elsewhere dismisses a menu. */
static bool tooltip_steps(struct cs *t, struct pc *a, struct sc *b, struct top *tl)
{
    struct ct_client *k = &a->c.k;
    struct pop tip, m;
    CHECK(click(t, TOP_X + 50, TOP_Y + 40, &a->c, b));   /* its toplevel focused again */
    const struct place tp = { 40, 20, 50, 40, 1, 1, BR, BR, 0 };
    CHECK(popup(a, &tip, tl->xs, &tp, 0));
    CHECK(show(a, &tip, 51, 41, 40, 20, YELLOW));
    CHECK(!entered_on(a, tip.s));
    /* its far corner: the cursor's arrow is at its top left (the click) */
    CHECK(pixel(t, TOP_X + 89, TOP_Y + 59, YELLOW));
    ct_clear(&b->k);
    CHECK(click(t, 70, 70, &a->c, b));
    CHECK(find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    CHECK(!ct_find(k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_POPUP_DONE, tip.popup));
    CHECK(pixel(t, TOP_X + 60, TOP_Y + 50, YELLOW));   /* the pointer gone from it */
    CHECK(destroy(a, &tip));
    ct_clear(k);
    CHECK(click(t, TOP_X + 50, TOP_Y + 40, &a->c, b));
    const struct ct_event *press = find_ev(&a->c, &jwl_wl_pointer_interface,
                                           JWL_WL_POINTER_EV_BUTTON);
    CHECK(press);
    CHECK(popup(a, &m, tl->xs, &menu, press->u[0]));
    CHECK(show(a, &m, 100, 80, 60, 40, GREEN));
    CHECK(cs_key(t, U_LALT, INPUT_KEY_DOWN, INPUT_MOD_LALT));
    CHECK(cs_tap(t, U_TAB, INPUT_MOD_LALT));   /* Alt+Tab: the other window */
    CHECK(cs_key(t, U_LALT, INPUT_KEY_UP, 0));
    CHECK(done(a, &m));
    CHECK(destroy(a, &m));
    return true;
}

/* A menu hidden by a commit with no buffer takes the popups on it along
 * (review GR's Low 4). First a menu and a submenu that both took the
 * grab and were never shown: the commit dismisses the submenu (popup_done)
 * and the chain is empty (another client's window sees the pointer
 * again); a grab on the hidden menu is dismissed at once, not a protocol
 * error; the menu can grab and be shown again. Then a submenu shown on it
 * goes with its next hide. */
static bool hide_steps(struct cs *t, struct pc *a, struct sc *b, struct top *tl)
{
    struct ct_client *k = &a->c.k;
    struct pop m, s, g, h;
    const struct place sp = { 50, 30, 50, 10, 10, 10, JWL_XDG_POSITIONER_ANCHOR_TOP_RIGHT, BR,
                              0 };
    ct_clear(k);
    CHECK(click(t, TOP_X + 50, TOP_Y + 40, &a->c, b));
    const struct ct_event *press = find_ev(&a->c, &jwl_wl_pointer_interface,
                                           JWL_WL_POINTER_EV_BUTTON);
    CHECK(press);
    uint32_t serial = press->u[0];
    CHECK(popup(a, &m, tl->xs, &menu, serial));
    CHECK(popup_objects(a, &s, m.xs, &sp));
    CHECK_ST(jwl_xdg_popup_grab(k->c, s.popup, a->c.seat, serial), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    CHECK(!k->errored);
    ct_clear(&b->k);
    CHECK(cs_pointer_to(t, 70, 70) && cs_sync(&a->c, b));   /* the chain holds the pointer */
    CHECK(!find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    ct_clear(k);
    CHECK(attach(a, m.s, 0));   /* the menu hidden */
    CHECK(done(a, &s));
    CHECK(!ct_find(k, &jwl_xdg_popup_interface, JWL_XDG_POPUP_EV_POPUP_DONE, m.popup));
    ct_clear(&b->k);
    CHECK(cs_pointer_to(t, 75, 75) && cs_sync(&a->c, b));
    CHECK(find_ev(b, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_ENTER));
    CHECK(popup(a, &g, m.xs, &sp, serial));   /* a grab on the hidden menu */
    CHECK(done(a, &g) && !g.serial);
    CHECK(!k->errored);
    /* the menu grabs again and is shown; a submenu on it, shown */
    CHECK_ST(jwl_xdg_popup_grab(k->c, m.popup, a->c.seat, serial), OK);
    ct_clear(k);
    CHECK(attach(a, m.s, 0));
    configured(a, &m);
    CHECK(show(a, &m, 100, 80, 60, 40, GREEN));
    CHECK(keys_moved(a, tl->s, m.s));
    CHECK(popup(a, &h, m.xs, &sp, serial));
    CHECK(show(a, &h, 60, 10, 50, 30, BLUE));
    CHECK(pixel(t, TOP_X + 100 + 60 + 10, TOP_Y + 80 + 10 + 10, BLUE));
    ct_clear(k);
    CHECK(attach(a, m.s, 0));
    CHECK(done(a, &h));
    CHECK(gone(t, TOP_X + 100 + 60 + 10, TOP_Y + 80 + 10 + 10, BLUE));
    CHECK(gone(t, TOP_X + 130, TOP_Y + 100, GREEN));
    CHECK(!k->errored);
    CHECK(destroy(a, &h) && destroy(a, &g) && destroy(a, &s) && destroy(a, &m));
    return true;
}

static bool grab_steps(struct cs *t, struct pc *a, struct sc *b)
{
    struct top tl;
    struct pop m;
    uint32_t serial;
    CHECK(cs_window(b, 20, 20, 100, 100));   /* another client's window (testwin) */
    CHECK(toplevel(a, &tl));                 /* its client's first window: the keys */
    CHECK(menu_steps(t, a, b, &tl, &m, &serial));
    CHECK(chain_steps(t, a, b, &tl, &m, serial));
    CHECK(refused_steps(t, a, b, &tl));
    CHECK(tooltip_steps(t, a, b, &tl));
    CHECK(hide_steps(t, a, b, &tl));
    return true;
}

bool t_popup_grab(void)
{
    static struct cs t;
    static struct pc a;
    static struct sc b;
    CHECK(cs_start(&t));
    bool ok = pc_open(&t, &a) && cs_client(&t, &b) && grab_steps(&t, &a, &b);
    ct_close(&b.k);
    pc_close(&a);
    CHECK(cs_stop(&t));
    return ok;
}

/* ---- t_popup_bad ------------------------------------------------------------------------ */

/* Each case on a client of its own, which the error ends. */
static bool bad_case(struct cs *t, unsigned n)
{
    static struct pc c;
    struct top tl;
    struct pop u, v;
    CHECK(pc_open(t, &c));
    struct ct_client *k = &c.c.k;
    bool ok = toplevel(&c, &tl);
    switch (n) {
    case 0:   /* a grab after the popup was mapped */
        ok = ok && popup(&c, &u, tl.xs, &menu, 0) && show(&c, &u, 100, 80, 60, 40, GREEN) &&
             jwl_xdg_popup_grab(k->c, u.popup, c.c.seat, 1) == OK &&
             ct_expect_error(k, u.popup, JWL_XDG_POPUP_ERROR_INVALID_GRAB);
        break;
    case 1:   /* a grab on a popup that took none */
        ok = ok && popup_objects(&c, &u, tl.xs, &menu) && popup_objects(&c, &v, u.xs, &menu) &&
             jwl_xdg_popup_grab(k->c, v.popup, c.c.seat, 1) == OK &&
             ct_expect_error(k, v.popup, JWL_XDG_POPUP_ERROR_INVALID_GRAB);
        break;
    case 2:   /* a popup destroyed while another is on it */
        ok = ok && popup_objects(&c, &u, tl.xs, &menu) && popup_objects(&c, &v, u.xs, &menu) &&
             jwl_xdg_popup_destroy(k->c, u.popup) == OK &&
             ct_expect_error(k, c.wm, JWL_XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP);
        break;
    default:   /* no parent at the initial commit */
        ok = ok && popup_objects(&c, &u, 0, &menu) && jwl_wl_surface_commit(k->c, u.s) == OK &&
             ct_expect_error(k, c.wm, JWL_XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT);
        break;
    }
    pc_close(&c);
    if (!ok)
        FAIL("case %u", n);
    return true;
}

bool t_popup_bad(void)
{
    static struct cs t;
    CHECK(cs_start(&t));
    bool ok = true;
    for (unsigned n = 0; n < 4 && ok; n++)
        ok = bad_case(&t, n);
    CHECK(cs_stop(&t));
    return ok;
}
