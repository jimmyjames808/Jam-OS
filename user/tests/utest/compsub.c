/* utest: subsurfaces (user/services/compositor subsurface.c, and the trees
 * in surface.c, scene.c, paint.c and pointer.c).
 *
 * t_comp_sub_paint: the test scene's windows with subsurface trees, painted
 * headless and compared pixel for pixel with the reference painter: a main
 * surface with children above it (one translucent, one with a child of its
 * own), two below it (one hidden under it, one showing past it), all
 * outside its surface (a tree without a window geometry is the window);
 * a second window whose window geometry is its surface, its child clipped
 * to it.
 * t_comp_sub_frame: a floating window's frame, title bar and shadow go
 * round its whole tree (a child above and left of its surface), and where
 * the tree has a hole the wallpaper shows, with no shadow on it.
 * t_comp_sub_commits: over the protocol (wl_subcompositor 1, testwin
 * windows): a synchronized child's commits and its position wait for its
 * parent's commit; place_above and place_below among three children and
 * the parent; a desynchronized child (Chromium's bubbles) updated with its
 * parent idle, its frame callbacks answered; a synchronized child of a
 * desynchronized one, applied with that one's commit; set_desync showing
 * what was cached; a buffer offset moving a child until set_position sets
 * its place again; a wl_subsurface destroyed, unmapped at once.
 * t_comp_sub_input: the pointer enters the topmost surface of the tree
 * under it, in that surface's coordinates; a child with an empty input
 * region lets it through; an input region, the order and a position
 * changed by a commit move the focus; a child destroyed under it.
 * t_comp_sub_hostile: each case refused with the protocol's error (the
 * surface its own parent, a cycle, a surface with a role or a
 * wl_subsurface already, place_above naming itself or a non-sibling, a
 * wl_surface destroyed before its wl_subsurface, a tree deeper than 8, a
 * subtree that would make one, 33 children, 49 subsurfaces), and a parent
 * destroyed first (no error: the child inert, its role kept); every bad
 * client's objects gone after, and a good client still served. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "comppaint.h"
#include "compseat.h"
#include "utest.h"

/* ---- the test scene ------------------------------------------------------------------------ */

bool t_comp_sub_paint(void)
{
    static const char *const cmds[] = {
        "win=40,40,160,100,ff3060a0",          /* 1: the main surface */
        "sub=1,-20,-20,60,50,ff70a030",        /* 2: above it, out past its corner */
        "sub=1,120,60,80,70,80602040",         /* 3: translucent, out right and below */
        "sub=1,20,20,40,30,ffa0a0a0,b",        /* 4: below it: hidden */
        "sub=1,-30,90,40,30,ffc08040,b",       /* 5: below it, showing past it */
        "sub=3,10,10,20,20,ff202020",          /* 6: 3's child */
        "win=250,20,50,50,ff405060",           /* 7: a second window */
        "geom=7,0,0,50,50",                    /* its window: its surface */
        "sub=7,30,30,40,40,ffe0c040,s",        /* 8: clipped to it */
        "paint",
    };
    /* the same as windows of the reference's, bottom to top, each child at
     * its place on the output; 8 clipped (solid: its pattern needs no
     * origin) */
    static const struct cp_win v[] = {
        { 10, 130, 40, 30, 0xffc08040, false, false, 0, false },
        { 60, 60, 40, 30, 0xffa0a0a0, false, false, 0, false },
        { 40, 40, 160, 100, 0xff3060a0, false, false, 0, false },
        { 20, 20, 60, 50, 0xff70a030, false, false, 0, false },
        { 160, 100, 80, 70, 0x80602040, false, false, 0, false },
        { 170, 110, 20, 20, 0xff202020, false, false, 0, false },
        { 250, 20, 50, 50, 0xff405060, false, false, 0, false },
        { 280, 50, 20, 20, 0xffe0c040, true, false, 0, false },
    };
    static uint32_t want[CP_W * CP_H];
    static uint8_t unknown[CP_W * CP_H];
    struct cp_run r;
    CHECK(cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r));
    ref_paint(want, unknown, v, sizeof(v) / sizeof(v[0]));
    bool ok = cp_same_image(&r, want, unknown, (struct comp_box){ 0, 0, 0, 0 });
    /* the clipped part of 8 is the wallpaper's */
    ok = ok && r.image[60 * CP_W + 310] == ref_wallpaper(310, 60);
    cp_done(&r);
    return ok;
}

bool t_comp_sub_frame(void)
{
    static const char *const cmds[] = {
        "win=60,60,120,80,ff3060a0,tf",        /* focused, floating */
        "sub=1,-20,-30,60,40,ff70a030",        /* its tree: (40, 30) .. (180, 140) */
        "paint",
    };
    struct cp_run r;
    CHECK(cp_run(cmds, sizeof(cmds) / sizeof(cmds[0]), &r));
    bool ok = true;
    const uint32_t *img = r.image;
#define PX(x, y) img[(y) * CP_W + (x)]
    /* the frame: (39, 2) .. (181, 141), the title bar's 28 rows above the tree */
    ok = ok && PX(176, 16) == LOOK_BAR_FOCUSED;            /* the bar, past the text */
    ok = ok && PX(39, 100) == LOOK_OUTLINE_FOCUSED;        /* the outline, left of the tree */
    ok = ok && PX(100, 140) == LOOK_OUTLINE_FOCUSED;       /* ... below it */
    ok = ok && PX(41, 31) == testscene_rgb(0x70a030, 1, 1, false);   /* the child */
    ok = ok && PX(45, 100) == ref_wallpaper(45, 100);      /* a hole in the tree: no shadow */
    ok = ok && PX(100, 150) != ref_wallpaper(100, 150);    /* the shadow, below the frame */
#undef PX
    if (!ok)
        FAIL("bar %06x outline %06x %06x child %06x hole %06x shadow %06x", r.image[16 * CP_W + 176],
             r.image[100 * CP_W + 39], r.image[140 * CP_W + 100], r.image[31 * CP_W + 41],
             r.image[100 * CP_W + 45], r.image[150 * CP_W + 100]);
    cp_done(&r);
    return ok;
}

/* ---- the protocol ---------------------------------------------------------------------------- */

#define RED     0xff0000u
#define GREEN   0x00ff00u
#define BLUE    0x0000ffu
#define YELLOW  0xffff00u
#define CYAN    0x00ffffu
#define MAGENTA 0xff00ffu
#define WHITE   0xffffffu
#define GREY    0x808080u
#define POOL    (2u << 20)

/* A client's pool, its buffers taken from it in turn. */
struct pool {
    uint32_t id;
    handle_t vmo;
    uint32_t used;
};

/* A client of t's with every global, the subcompositor and a pool. */
struct subc {
    struct sc c;
    uint32_t subc;                 /* wl_subcompositor */
    struct pool pool;
};

/* A surface with the subsurface role: its wl_surface and wl_subsurface. */
struct kid {
    uint32_t s, sub;
};

static bool sub_bind(struct ct_client *k, uint32_t *subc)
{
    CHECK((*subc = ct_new(k, &jwl_wl_subcompositor_interface, 1)) != 0);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, 10, "wl_subcompositor", 1, *subc), OK);
    return true;
}

static bool subc_open(struct cs *t, struct subc *x)
{
    memset(x, 0, sizeof(*x));
    CHECK(cs_client(t, &x->c));
    CHECK(sub_bind(&x->c.k, &x->subc));
    CHECK((x->pool.id = ct_pool(&x->c.k, POOL, &x->pool.vmo)) != 0);
    CHECK_ST(ct_roundtrip(&x->c.k), OK);
    return true;
}

static void subc_close(struct subc *x)
{
    jam_handle_close(x->pool.vmo);
    ct_close(&x->c.k);
}

/* A w x h xrgb buffer all `colour`: its id, 0 on failure. */
static uint32_t solid(struct subc *x, int32_t w, int32_t h, uint32_t colour)
{
    static uint32_t row[256];
    uint32_t bytes = (uint32_t)(w * h * 4);
    if (w > 256 || x->pool.used + bytes > POOL)
        return 0;
    for (int32_t i = 0; i < w; i++)
        row[i] = colour | 0xa5000000u;   /* (x: the top byte means nothing) */
    for (int32_t y = 0; y < h; y++)
        if (jam_vmo_write(x->pool.vmo, x->pool.used + (uint64_t)y * (uint64_t)w * 4, row,
                          (uint64_t)w * 4) != OK)
            return 0;
    uint32_t id = ct_new(&x->c.k, &jwl_wl_buffer_interface, 1);
    if (!id || jwl_wl_shm_pool_create_buffer(x->c.k.c, x->pool.id, id, (int32_t)x->pool.used, w,
                                             h, w * 4, JWL_WL_SHM_FORMAT_XRGB8888) != OK)
        return 0;
    x->pool.used += bytes;
    return id;
}

static uint32_t new_surface(struct ct_client *k)
{
    uint32_t s = ct_new(k, &jwl_wl_surface_interface, 4);
    if (!s || jwl_wl_compositor_create_surface(k->c, k->compositor, s) != OK)
        return 0;
    return s;
}

/* A main surface of w x h in colour, a testwin window at (px, py). */
static uint32_t main_at(struct subc *x, int32_t px, int32_t py, int32_t w, int32_t h,
                        uint32_t colour)
{
    uint32_t b = solid(x, w, h, colour), s = b ? new_surface(&x->c.k) : 0;
    if (!s || jwl_wl_surface_attach(x->c.k.c, s, b, px, py) != OK ||
        jwl_wl_surface_commit(x->c.k.c, s) != OK)
        return 0;
    return s;
}

/* A new subsurface of parent at (px, py), its w x h buffer in colour
 * committed (synchronized: cached until the parent's commit). */
static bool kid_of(struct subc *x, uint32_t parent, int32_t px, int32_t py, int32_t w, int32_t h,
                   uint32_t colour, struct kid *k)
{
    struct jwl_conn *c = x->c.k.c;
    uint32_t b = solid(x, w, h, colour);
    CHECK(b);
    CHECK((k->s = new_surface(&x->c.k)) != 0);
    CHECK((k->sub = ct_new(&x->c.k, &jwl_wl_subsurface_interface, 1)) != 0);
    CHECK_ST(jwl_wl_subcompositor_get_subsurface(c, x->subc, k->sub, k->s, parent), OK);
    CHECK_ST(jwl_wl_subsurface_set_position(c, k->sub, px, py), OK);
    CHECK_ST(jwl_wl_surface_attach(c, k->s, b, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_damage(c, k->s, 0, 0, w, h), OK);
    CHECK_ST(jwl_wl_surface_commit(c, k->s), OK);
    return true;
}

/* s shows a new w x h buffer in colour (a commit). */
static bool redraw(struct subc *x, uint32_t s, int32_t w, int32_t h, uint32_t colour)
{
    uint32_t b = solid(x, w, h, colour);
    CHECK(b);
    CHECK_ST(jwl_wl_surface_attach(x->c.k.c, s, b, 0, 0), OK);
    CHECK_ST(jwl_wl_surface_damage(x->c.k.c, s, 0, 0, w, h), OK);
    CHECK_ST(jwl_wl_surface_commit(x->c.k.c, s), OK);
    return true;
}

static bool commit(struct subc *x, uint32_t s)
{
    CHECK_ST(jwl_wl_surface_commit(x->c.k.c, s), OK);
    return true;
}

/* Every request so far has been served and a paint has come after them:
 * a frame callback on ticker (a window of its own) answered. */
static bool painted(struct subc *x, uint32_t ticker)
{
    struct jwl_conn *c = x->c.k.c;
    uint32_t cb = ct_new(&x->c.k, &jwl_wl_callback_interface, 4);
    CHECK(cb);
    ct_clear(&x->c.k);
    CHECK_ST(jwl_wl_surface_frame(c, ticker, cb), OK);
    CHECK_ST(jwl_wl_surface_damage(c, ticker, 0, 0, 1, 1), OK);
    CHECK_ST(jwl_wl_surface_commit(c, ticker), OK);
    CHECK(ct_await(&x->c.k, &jwl_wl_callback_interface, JWL_WL_CALLBACK_EV_DONE, cb, CT_WAIT));
    CHECK(!x->c.k.errored);
    return true;
}

/* x's requests so far served, then the output's pixel (px, py) becomes
 * want within CT_WAIT. */
static bool becomes(const struct cs *t, struct subc *x, int32_t px, int32_t py, uint32_t want)
{
    CHECK_ST(ct_roundtrip(&x->c.k), OK);
    uint64_t until = now() + CT_WAIT;
    while (t->p.image[py * OUT_W + px] != want && now() < until)
        jam_sleep_until(now() + NS_PER_MS);
    if (t->p.image[py * OUT_W + px] != want)
        FAIL("pixel (%d, %d) is %06x, want %06x", px, py, t->p.image[py * OUT_W + px], want);
    return true;
}

/* ... and is want now (after painted). */
static bool still(const struct cs *t, int32_t px, int32_t py, uint32_t want)
{
    if (t->p.image[py * OUT_W + px] != want)
        FAIL("pixel (%d, %d) is %06x, want %06x", px, py, t->p.image[py * OUT_W + px], want);
    return true;
}

/* The main surface P at (100, 100), 200 x 150. Its children's places on
 * the output, as set below: C1 (160, 120), C2 (175, 135), C3 (190, 150),
 * 40 x 40 each, all three over (195, 155); C2 alone at (210, 140); C3
 * alone at (225, 185). */
#define PX 100
#define PY 100

/* Synchronized commits and positions wait for P's commit; the order. */
static bool order_steps(struct cs *t, struct subc *x, uint32_t tick, uint32_t p, struct kid *k)
{
    struct jwl_conn *c = x->c.k.c;
    CHECK(kid_of(x, p, 20, 20, 40, 40, BLUE, &k[0]));
    CHECK(painted(x, tick));
    CHECK(still(t, 125, 125, RED));   /* cached: not shown */
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 125, 125, BLUE));
    CHECK_ST(jwl_wl_subsurface_set_position(c, k[0].sub, 60, 20), OK);
    CHECK(painted(x, tick));
    CHECK(still(t, 125, 125, BLUE));   /* the position is P's state */
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 165, 125, BLUE));
    CHECK(still(t, 125, 125, RED));
    CHECK(kid_of(x, p, 75, 35, 40, 40, GREEN, &k[1]));
    CHECK(kid_of(x, p, 90, 50, 40, 40, YELLOW, &k[2]));
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 195, 155, YELLOW));   /* each new one on top */
    CHECK_ST(jwl_wl_subsurface_place_below(c, k[2].sub, k[0].s), OK);   /* P C3 C1 C2 */
    CHECK(painted(x, tick));
    CHECK(still(t, 195, 155, YELLOW));   /* the order is P's state too */
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 195, 155, GREEN));
    CHECK_ST(jwl_wl_subsurface_place_above(c, k[0].sub, k[1].s), OK);   /* P C3 C2 C1 */
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 195, 155, BLUE));
    CHECK(still(t, 210, 140, GREEN));
    CHECK_ST(jwl_wl_subsurface_place_below(c, k[1].sub, p), OK);   /* C2 P C3 C1 */
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 210, 140, RED));   /* under its parent */
    CHECK_ST(jwl_wl_subsurface_place_above(c, k[1].sub, p), OK);   /* P C2 C3 C1 */
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 210, 140, GREEN));
    return true;
}

/* C3 desynchronized: it updates with P idle (a bubble), its frame
 * callbacks answered; a synchronized child G of it waits for C3's commit,
 * not P's; set_desync shows what C3 cached while synchronized again. */
static bool desync_steps(struct cs *t, struct subc *x, uint32_t tick, struct kid *k)
{
    struct jwl_conn *c = x->c.k.c;
    struct kid g;
    CHECK_ST(jwl_wl_subsurface_set_desync(c, k[2].sub), OK);
    uint32_t cb = ct_new(&x->c.k, &jwl_wl_callback_interface, 4);
    CHECK(cb);
    ct_clear(&x->c.k);
    CHECK_ST(jwl_wl_surface_frame(c, k[2].s, cb), OK);
    CHECK(redraw(x, k[2].s, 40, 40, CYAN));
    CHECK(becomes(t, x, 225, 185, CYAN));
    CHECK(ct_await(&x->c.k, &jwl_wl_callback_interface, JWL_WL_CALLBACK_EV_DONE, cb, CT_WAIT));
    CHECK(kid_of(x, k[2].s, 25, 25, 10, 10, MAGENTA, &g));   /* at (215, 175) */
    CHECK(painted(x, tick));
    CHECK(still(t, 220, 180, CYAN));
    CHECK(commit(x, k[2].s));   /* C3's own commit, P still idle */
    CHECK(becomes(t, x, 220, 180, MAGENTA));
    CHECK(redraw(x, g.s, 10, 10, WHITE));
    CHECK(painted(x, tick));
    CHECK(still(t, 220, 180, MAGENTA));
    CHECK(commit(x, k[2].s));
    CHECK(becomes(t, x, 220, 180, WHITE));
    CHECK_ST(jwl_wl_subsurface_set_sync(c, k[2].sub), OK);
    CHECK(redraw(x, k[2].s, 40, 40, BLUE));
    CHECK(painted(x, tick));
    CHECK(still(t, 225, 185, CYAN));
    CHECK_ST(jwl_wl_subsurface_set_desync(c, k[2].sub), OK);
    CHECK(becomes(t, x, 225, 185, BLUE));
    /* G's wl_subsurface, then its wl_surface: unmapped at once, no error */
    CHECK_ST(jwl_wl_subsurface_destroy(c, g.sub), OK);
    CHECK(becomes(t, x, 220, 180, BLUE));
    CHECK_ST(jwl_wl_surface_destroy(c, g.s), OK);
    return true;
}

/* A commit's buffer offset (attach's x and y: these surfaces are version
 * 4) moves C3 against P, at once as it is desynchronized; set_position,
 * applied by P's commit, sets its place again (weston's rule; review GR's
 * Low 2). Moved by (-30, 10), C3's 40 x 40 is at (160, 160). */
static bool offset_steps(struct cs *t, struct subc *x, uint32_t p, struct kid *k)
{
    struct jwl_conn *c = x->c.k.c;
    uint32_t b = solid(x, 40, 40, BLUE);
    CHECK(b);
    CHECK_ST(jwl_wl_surface_attach(c, k[2].s, b, -30, 10), OK);
    CHECK_ST(jwl_wl_surface_damage(c, k[2].s, 0, 0, 40, 40), OK);
    CHECK(commit(x, k[2].s));
    CHECK(becomes(t, x, 165, 195, BLUE));
    CHECK(becomes(t, x, 225, 185, RED));   /* gone from where it was */
    CHECK_ST(jwl_wl_subsurface_set_position(c, k[2].sub, 90, 50), OK);
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 225, 185, BLUE));
    CHECK(becomes(t, x, 165, 195, RED));
    return true;
}

bool t_comp_sub_commits(void)
{
    static struct subc x;
    struct cs t;
    struct kid k[3];
    CHECK(cs_start(&t));
    bool ok = subc_open(&t, &x);
    uint32_t tick = ok ? main_at(&x, 600, 440, 8, 8, GREY) : 0;
    uint32_t p = tick ? main_at(&x, PX, PY, 200, 150, RED) : 0;
    ok = p && becomes(&t, &x, 120, 120, RED) && order_steps(&t, &x, tick, p, k) &&
         desync_steps(&t, &x, tick, k) && offset_steps(&t, &x, p, k);
    if (ok) {   /* C1's wl_subsurface destroyed: gone at once, C3 under it shows */
        CHECK_ST(jwl_wl_subsurface_destroy(x.c.k.c, k[0].sub), OK);
        ok = becomes(&t, &x, 195, 155, BLUE) && becomes(&t, &x, 165, 125, RED);
        CHECK_ST(ct_roundtrip(&x.c.k), OK);
        CHECK(!x.c.k.errored);
    }
    subc_close(&x);
    CHECK(cs_stop(&t));
    return ok;
}

/* ---- input ------------------------------------------------------------------------------------ */

/* c got wl_pointer.enter (enter) or leave on surface s since ct_clear; the
 * enter's position in *ex, *ey. */
static bool got_crossing(const struct sc *c, bool enter, uint32_t s, uint32_t *ex, uint32_t *ey)
{
    uint16_t op = enter ? JWL_WL_POINTER_EV_ENTER : JWL_WL_POINTER_EV_LEAVE;
    for (unsigned i = 0; i < c->k.nev; i++) {
        const struct ct_event *e = &c->k.ev[i];
        if (e->iface == &jwl_wl_pointer_interface && e->op == op && e->u[1] == s) {
            if (ex)
                *ex = e->u[2];
            if (ey)
                *ey = e->u[3];
            return true;
        }
    }
    return false;
}

/* c's last pointer position told (an enter's or a motion's) is (px, py). */
static bool pointer_at(const struct sc *c, uint32_t px, uint32_t py)
{
    uint32_t gx = 0, gy = 0;
    bool told = false;
    for (unsigned i = 0; i < c->k.nev; i++) {
        const struct ct_event *e = &c->k.ev[i];
        if (e->iface != &jwl_wl_pointer_interface)
            continue;
        if (e->op == JWL_WL_POINTER_EV_ENTER) {
            gx = e->u[2];
            gy = e->u[3];
            told = true;
        } else if (e->op == JWL_WL_POINTER_EV_MOTION) {
            gx = e->u[1];
            gy = e->u[2];
            told = true;
        }
    }
    if (!told || gx != px || gy != py)
        FAIL("the pointer was told %u/256, %u/256 (%s); want %u/256, %u/256", gx, gy,
             told ? "told" : "never", px, py);
    return true;
}

/* The pointer to (px, py), the events in. */
static bool point(struct cs *t, struct subc *x, int32_t px, int32_t py)
{
    ct_clear(&x->c.k);
    CHECK(cs_pointer_to(t, px, py));
    CHECK(cs_sync(&x->c, NULL));
    return true;
}

/* An enter on s comes (from the compositor's own turn: a commit moved
 * what is under the pointer), at (ex, ey) wl_fixed. */
static bool enters(struct subc *x, uint32_t s, uint32_t ex, uint32_t ey)
{
    uint32_t gx = 0, gy = 0;
    uint64_t until = now() + CT_WAIT;
    while (!got_crossing(&x->c, true, s, &gx, &gy) && now() < until) {
        CHECK_ST(ct_roundtrip(&x->c.k), OK);
        jam_sleep_until(now() + NS_PER_MS);
    }
    if (!got_crossing(&x->c, true, s, &gx, &gy))
        FAIL("no enter on surface %u", s);
    if (gx != ex || gy != ey)
        FAIL("entered %u at %u/256, %u/256; want %u/256, %u/256", s, gx, gy, ex, ey);
    return true;
}

/* P at (100, 100) 200 x 150; S1 at (120, 120), S2 over it at (150, 150),
 * 60 x 60 each, S2 with an empty input region. */
static bool input_steps(struct cs *t, struct subc *x, uint32_t p, struct kid *s1, struct kid *s2)
{
    struct jwl_conn *c = x->c.k.c;
    uint32_t none = ct_new(&x->c.k, &jwl_wl_region_interface, 4);   /* (its compositor's version) */
    CHECK(none);
    CHECK_ST(jwl_wl_compositor_create_region(c, x->c.k.compositor, none), OK);
    CHECK(kid_of(x, p, 20, 20, 60, 60, BLUE, s1));
    CHECK(kid_of(x, p, 50, 50, 60, 60, GREEN, s2));
    CHECK_ST(jwl_wl_surface_set_input_region(c, s2->s, none), OK);
    CHECK(commit(x, s2->s));
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 125, 125, BLUE) && becomes(t, x, 205, 205, GREEN));
    /* onto S1: entered there, in its coordinates */
    CHECK(point(t, x, 130, 130));
    CHECK(got_crossing(&x->c, true, s1->s, NULL, NULL));
    CHECK(pointer_at(&x->c, AT(10), AT(10)));
    /* where S2 is over S1: S2 takes no input, so still S1 */
    CHECK(point(t, x, 160, 160));
    CHECK(!got_crossing(&x->c, false, s1->s, NULL, NULL));
    CHECK(pointer_at(&x->c, AT(40), AT(40)));
    /* S2 alone: through it to P */
    CHECK(point(t, x, 200, 200));
    CHECK(got_crossing(&x->c, false, s1->s, NULL, NULL));
    CHECK(got_crossing(&x->c, true, p, NULL, NULL));
    CHECK(pointer_at(&x->c, AT(100), AT(100)));
    /* S2 takes input from its parent's commit on: the pointer is on it */
    ct_clear(&x->c.k);
    CHECK_ST(jwl_wl_surface_set_input_region(c, s2->s, 0), OK);
    CHECK(commit(x, s2->s));
    CHECK(commit(x, p));
    CHECK(enters(x, s2->s, AT(50), AT(50)));
    CHECK(got_crossing(&x->c, false, p, NULL, NULL));
    /* S1 raised over S2 and moved under the pointer by P's commit */
    ct_clear(&x->c.k);
    CHECK_ST(jwl_wl_subsurface_place_above(c, s1->sub, s2->s), OK);
    CHECK_ST(jwl_wl_subsurface_set_position(c, s1->sub, 70, 70), OK);
    CHECK(commit(x, p));
    CHECK(enters(x, s1->s, AT(30), AT(30)));
    /* a press goes to the client */
    ct_clear(&x->c.k);
    CHECK(cs_button(t, true) && cs_button(t, false) && cs_sync(&x->c, NULL));
    CHECK(find_ev(&x->c, &jwl_wl_pointer_interface, JWL_WL_POINTER_EV_BUTTON));
    /* S1 destroyed under the pointer: S2 has it again */
    ct_clear(&x->c.k);
    CHECK_ST(jwl_wl_subsurface_destroy(c, s1->sub), OK);
    CHECK_ST(jwl_wl_surface_destroy(c, s1->s), OK);
    CHECK(enters(x, s2->s, AT(50), AT(50)));
    CHECK_ST(jwl_wl_region_destroy(c, none), OK);
    return true;
}

bool t_comp_sub_input(void)
{
    static struct subc x;
    struct cs t;
    struct kid s1, s2;
    CHECK(cs_start(&t));
    bool ok = subc_open(&t, &x);
    uint32_t p = ok ? main_at(&x, PX, PY, 200, 150, RED) : 0;
    ok = p && becomes(&t, &x, 110, 110, RED) && input_steps(&t, &x, p, &s1, &s2);
    CHECK_ST(ct_roundtrip(&x.c.k), OK);
    ok = ok && !x.c.k.errored;
    subc_close(&x);
    CHECK(cs_stop(&t));
    return ok;
}

/* ---- hostile clients -------------------------------------------------------------------------- */

/* A client for one case: every core global and the subcompositor. */
struct hc {
    struct ct_client k;
    uint32_t subc;
};

static bool hc_open(struct cs *t, struct hc *h)
{
    memset(h, 0, sizeof(*h));
    CHECK(ct_open(&t->p, &h->k));
    CHECK(ct_bind_all(&h->k));
    CHECK(sub_bind(&h->k, &h->subc));
    return true;
}

/* A new subsurface of parent for surface s (made if 0): its wl_subsurface. */
static uint32_t hsub(struct hc *h, uint32_t *s, uint32_t parent)
{
    if (!*s && !(*s = new_surface(&h->k)))
        return 0;
    uint32_t id = ct_new(&h->k, &jwl_wl_subsurface_interface, 1);
    if (!id || jwl_wl_subcompositor_get_subsurface(h->k.c, h->subc, id, *s, parent) != OK)
        return 0;
    return id;
}

/* A chain of n subsurfaces under top, each the last one's child: the last
 * surface's id. */
static uint32_t chain(struct hc *h, uint32_t top, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint32_t s = 0;
        if (!hsub(h, &s, top))
            return 0;
        top = s;
    }
    return top;
}

enum hcase {
    H_SELF, H_CYCLE, H_TWICE, H_XDG_ROLE, H_TESTWIN_ROLE, H_ABOVE_SELF, H_NOT_SIBLING,
    H_DEFUNCT, H_DEEP, H_DEEP_SUBTREE, H_CHILDREN, H_TOTAL, H_CASES
};

/* Case c sent on h: the object the error must name in *obj and its code
 * in *code. false: a request couldn't go. */
static bool hcase(struct cs *t, struct hc *h, enum hcase c, uint32_t *obj, uint32_t *code)
{
    struct jwl_conn *k = h->k.c;
    uint32_t p = new_surface(&h->k), a = 0, b = 0, s = 0, sub = 0;
    CHECK(p);
    *obj = h->subc;
    *code = JWL_WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE;
    switch (c) {
    case H_SELF:   /* its own parent */
        *code = JWL_WL_SUBCOMPOSITOR_ERROR_BAD_PARENT;
        return hsub(h, &p, p) != 0;
    case H_CYCLE:   /* a under p, b under a; then p under b */
        *code = JWL_WL_SUBCOMPOSITOR_ERROR_BAD_PARENT;
        return hsub(h, &a, p) && hsub(h, &b, a) && hsub(h, &p, b);
    case H_TWICE:   /* a subsurface already */
        return hsub(h, &a, p) && (b = new_surface(&h->k)) && hsub(h, &a, b);
    case H_XDG_ROLE: {   /* an xdg_surface's (the start of a role); ids sent in order */
        uint32_t wm = ct_new(&h->k, &jwl_xdg_wm_base_interface, 1), xs;
        CHECK(wm);
        CHECK_ST(jwl_wl_registry_bind(k, h->k.registry, 5, "xdg_wm_base", 1, wm), OK);
        CHECK((a = new_surface(&h->k)) != 0);
        CHECK((xs = ct_new(&h->k, &jwl_xdg_surface_interface, 1)) != 0);
        CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(k, wm, xs, a), OK);
        return hsub(h, &a, p) != 0;
    }
    case H_TESTWIN_ROLE: {   /* a test window's (role test) */
        uint32_t pool = ct_pool(&h->k, PAGE_SIZE, NULL), buf;
        CHECK(pool && (buf = ct_new(&h->k, &jwl_wl_buffer_interface, 1)) != 0);
        CHECK_ST(jwl_wl_shm_pool_create_buffer(k, pool, buf, 0, 8, 8, 32,
                                               JWL_WL_SHM_FORMAT_XRGB8888), OK);
        CHECK((a = new_surface(&h->k)) != 0);
        CHECK_ST(jwl_wl_surface_attach(k, a, buf, 10, 10), OK);
        CHECK_ST(jwl_wl_surface_commit(k, a), OK);
        return hsub(h, &a, p) != 0;
    }
    case H_ABOVE_SELF:   /* place_above naming itself */
        CHECK((sub = hsub(h, &a, p)) != 0);
        *obj = sub;
        *code = JWL_WL_SUBSURFACE_ERROR_BAD_SURFACE;
        return jwl_wl_subsurface_place_above(k, sub, a) == OK;
    case H_NOT_SIBLING:   /* place_below naming its sibling's child */
        CHECK((sub = hsub(h, &a, p)) != 0 && hsub(h, &b, p) && hsub(h, &s, b));
        *obj = sub;
        *code = JWL_WL_SUBSURFACE_ERROR_BAD_SURFACE;
        return jwl_wl_subsurface_place_below(k, sub, s) == OK;
    case H_DEFUNCT:   /* the wl_surface destroyed before its wl_subsurface */
        CHECK(hsub(h, &a, p));
        *obj = JWL_DISPLAY_ID;   /* (the surface's id is gone: the error names the display) */
        *code = JWL_WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT;
        return jwl_wl_surface_destroy(k, a) == OK;
    case H_DEEP:   /* 8 nested is the most: a 9th */
        *code = JWL_ERROR_NO_MEMORY;
        CHECK((a = chain(h, p, 8)) != 0);
        CHECK_ST(ct_roundtrip(&h->k), OK);
        CHECK(!h->k.errored);
        return hsub(h, &b, a) != 0;
    case H_DEEP_SUBTREE: {   /* b with 4 under it: under a chain of 3 it fits, of 4 not */
        *code = JWL_ERROR_NO_MEMORY;
        uint32_t three = chain(h, p, 3), four = three ? chain(h, three, 1) : 0;
        CHECK(four && (b = new_surface(&h->k)) && chain(h, b, 4));
        CHECK((sub = hsub(h, &b, three)) != 0);
        CHECK_ST(ct_roundtrip(&h->k), OK);
        CHECK(!h->k.errored);
        CHECK_ST(jwl_wl_subsurface_destroy(k, sub), OK);
        return hsub(h, &b, four) != 0;
    }
    case H_CHILDREN:   /* 32 children is the most: a 33rd */
        *code = JWL_ERROR_NO_MEMORY;
        for (unsigned i = 0; i < COMP_SUB_CHILDREN_MAX; i++) {
            s = 0;
            CHECK(hsub(h, &s, p));
        }
        CHECK_ST(ct_roundtrip(&h->k), OK);
        CHECK(!h->k.errored);
        s = 0;
        return hsub(h, &s, p) != 0;
    case H_TOTAL:   /* 48 subsurfaces a client is the most: a 49th */
        *code = JWL_ERROR_NO_MEMORY;
        CHECK((b = new_surface(&h->k)) != 0);
        for (unsigned i = 0; i < COMP_SUBS_MAX; i++) {
            s = 0;
            CHECK(hsub(h, &s, i < COMP_SUB_CHILDREN_MAX ? p : b));
        }
        CHECK_ST(ct_roundtrip(&h->k), OK);
        CHECK(!h->k.errored);
        s = 0;
        return hsub(h, &s, b) != 0;
    case H_CASES:
        break;
    }
    (void)t;
    return false;
}

/* A parent's wl_surface destroyed before its child's: no error; the
 * child's wl_subsurface inert, its commits harmless; once that is
 * destroyed the surface may have a new one (the role kept). */
static bool parent_first(struct cs *t, struct subc *x)
{
    struct jwl_conn *c = x->c.k.c;
    struct kid k;
    uint32_t p = main_at(x, 400, 300, 60, 60, RED);
    CHECK(p && becomes(t, x, 410, 310, RED));
    CHECK(kid_of(x, p, 10, 10, 20, 20, GREEN, &k));
    CHECK(commit(x, p));
    CHECK(becomes(t, x, 415, 315, GREEN));
    CHECK_ST(jwl_wl_surface_destroy(c, p), OK);
    CHECK(becomes(t, x, 415, 315, ref_wallpaper_of(415, 315, OUT_W, OUT_H)));
    CHECK_ST(jwl_wl_subsurface_set_position(c, k.sub, 5, 5), OK);
    CHECK_ST(jwl_wl_subsurface_place_above(c, k.sub, k.s), OK);   /* inert: no error */
    CHECK_ST(jwl_wl_subsurface_set_desync(c, k.sub), OK);
    CHECK(redraw(x, k.s, 20, 20, BLUE));
    CHECK_ST(ct_roundtrip(&x->c.k), OK);
    CHECK(!x->c.k.errored);
    CHECK_ST(jwl_wl_subsurface_destroy(c, k.sub), OK);
    uint32_t q = main_at(x, 400, 300, 60, 60, YELLOW);
    CHECK(q);
    CHECK((k.sub = ct_new(&x->c.k, &jwl_wl_subsurface_interface, 1)) != 0);
    CHECK_ST(jwl_wl_subcompositor_get_subsurface(c, x->subc, k.sub, k.s, q), OK);
    CHECK(commit(x, k.s));
    CHECK(commit(x, q));
    CHECK(becomes(t, x, 405, 305, BLUE));   /* (the position forgotten: 0, 0) */
    CHECK_ST(ct_roundtrip(&x->c.k), OK);
    CHECK(!x->c.k.errored);
    return true;
}

bool t_comp_sub_hostile(void)
{
    static struct hc h;
    static struct subc good;
    struct cs t;
    CHECK(cs_start(&t));
    CHECK(subc_open(&t, &good));   /* connected all along: it must not notice */
    CHECK_ST(ct_roundtrip(&good.c.k), OK);
    uint64_t base = ct_handles_settled(&t.p, NULL);
    for (unsigned i = 0; i < H_CASES; i++) {
        uint32_t obj, code;
        CHECK(hc_open(&t, &h));
        if (!hcase(&t, &h, (enum hcase)i, &obj, &code))
            FAIL("case %u: a request didn't go", i);
        if (!ct_expect_error(&h.k, obj, code))
            FAIL("case %u: not the error it should be", i);
        ct_close(&h.k);
    }
    CHECK(ct_handles_back(&t.p, base));   /* every bad client's objects gone */
    bool ok = parent_first(&t, &good);
    CHECK(!good.c.k.errored);
    subc_close(&good);
    CHECK(cs_stop(&t));
    return ok;
}
