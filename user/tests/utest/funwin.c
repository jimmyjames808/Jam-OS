/* utest: libfun's window (user/apps/fun/wl.c, wlpaint.c) against the fake
 * compositor (jwlcfake.h): gfx_open on a compositor that offers windows,
 * and the apps' calls working on it as on a borrowed screen.
 *   fun_window_present  the window at the size asked, bg shown at open;
 *                       a change damaged by its pieces and written into
 *                       the free buffer, and the other buffer brought up
 *                       to date at the next present (both changes in it);
 *                       a present of nothing commits nothing; full screen
 *                       by the compositor: the picture centred on bg in
 *                       the bigger window, shown by libfun itself;
 *   fun_window_input    keys as the apps decode them ('a', Ctrl+C as
 *                       KEY_QUIT, a release as nothing), the mouse at the
 *                       window's pixels, a button, the wheel (down the
 *                       screen: negative), the close box as KEY_QUIT;
 *   fun_window_resize   a resizable app (gfx_resizable: kept for the rest
 *                       of the process, so this runs last) hears
 *                       KEY_RESIZE and draws at the new size.
 * The fake runs on a thread while gfx_open waits for it, then in this
 * thread by turns. Each ends with the job's handles and message bytes
 * where they began. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fun.h>
#include <jwl_client.h>
#include <os.h>
#include "jwlcfake.h"
#include "utest.h"

#define BG    0x123456u
#define RED   0xff0000u
#define GREEN 0x00ff00u
#define FULL  (1u << JWL_XDG_TOPLEVEL_STATE_FULLSCREEN)
#define KEY_A 30u     /* evdev */
#define KEY_C 46u

/* The buffer a commit of surface s showed last, NULL if none. */
static const struct fake_buffer *shown_buffer(const struct fake *f, const struct fake_surface *s)
{
    for (unsigned i = 0; i < FAKE_BUFFERS && s->current; i++)
        if (f->b[i].id == s->current)
            return &f->b[i];
    return NULL;
}

/* Pixel (x, y) of what surface s shows, through the fake's mapping. */
static uint32_t shown_px(const struct fake *f, const struct fake_surface *s, int x, int y)
{
    const struct fake_buffer *b = shown_buffer(f, s);
    if (!b || x >= b->w || y >= b->h)
        return 0xdeadbeef;
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->pool->addr + b->offset;
    return ((const uint32_t *)(const void *)(p + (uint64_t)y * (uint64_t)b->stride))[x] &
           0xffffff;
}

/* The pool, started once for every test here; then the job's figures. */
static void start(uint64_t *h0, uint64_t *b0)
{
    static bool pool;
    if (!pool)
        pool_start(2);
    pool = true;
    fake_held(h0, b0);
}

/* gfx_open_on(BG) on f, a w x h window, served on a thread meanwhile;
 * the fake's surface in *s. */
static bool open_on(struct fake *f, int w, int h, struct fake_surface **s)
{
    gfx_connect_with(fake_connect, f);
    gfx_title("funwin");
    gfx_window_size(w, h);
    handle_t th;
    CHECK_ST(fake_thread_start(f, &th), OK);
    status_t st = gfx_open_on(BG);
    fake_thread_stop(f, th);
    fake_serve(f);   /* the first present's commit, if the thread missed it */
    CHECK_ST(st, OK);
    CHECK(scr.windowed && scr.w == w && scr.h == h);
    *s = fake_surface(f, 0);
    CHECK(*s && (*s)->toplevel && !strcmp((*s)->title, "funwin"));
    return true;
}

/* The window gone, the fake too, and the job back where it was. */
static bool close_all(struct fake *f, uint64_t h0, uint64_t b0)
{
    gfx_close();
    gfx_connect_with(NULL, NULL);
    gfx_window_size(0, 0);
    return fake_all_gone(f, NULL, h0, b0);
}

bool t_fun_window_present(void)
{
    uint64_t h0, b0;
    start(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct fake_surface *s;
    if (!open_on(&f, 320, 200, &s))
        return false;
    CHECK_EQ(shown_px(&f, s, 0, 0), BG);
    CHECK_EQ(shown_px(&f, s, 319, 199), BG);

    /* a change: its pieces damaged (x 0-63, rows 0-31), in the free buffer */
    fill(&scr.s, 10, 10, 50, 20, RED);
    uint32_t first = s->current;
    gfx_present();
    fake_serve(&f);
    CHECK(s->current && s->current != first);
    CHECK(s->last_damage.x == 0 && s->last_damage.y == 0);
    CHECK(s->last_damage.w == 64 && s->last_damage.h == 32);
    CHECK_EQ(shown_px(&f, s, 20, 15), RED);
    CHECK_EQ(shown_px(&f, s, 100, 100), BG);

    /* another: the first buffer again, with both changes in it */
    fill(&scr.s, 200, 100, 10, 10, GREEN);
    gfx_present();
    fake_serve(&f);
    CHECK_EQ(s->current, first);
    CHECK(s->last_damage.x == 192 && s->last_damage.y == 96);
    CHECK(s->last_damage.w == 64 && s->last_damage.h == 16);
    CHECK_EQ(shown_px(&f, s, 20, 15), RED);
    CHECK_EQ(shown_px(&f, s, 205, 105), GREEN);

    /* nothing changed: no commit */
    unsigned commits = s->commits;
    gfx_present();
    fake_serve(&f);
    CHECK_EQ(s->commits, commits);

    /* full screen by the compositor: 400 x 300, the picture centred on bg */
    CHECK_ST(fake_configure(&f, s, 400, 300, FULL), OK);
    fake_serve(&f);
    CHECK_EQ(gfx_key(0), KEY_NONE);   /* reads it, and shows the picture again */
    fake_serve(&f);
    CHECK(scr.w == 320 && scr.h == 200);
    const struct fake_buffer *b = shown_buffer(&f, s);
    CHECK(b && b->w == 400 && b->h == 300 && s->acked == s->sent_serial);
    CHECK_EQ(shown_px(&f, s, 0, 0), BG);
    CHECK_EQ(shown_px(&f, s, 40 + 20, 50 + 15), RED);
    CHECK_EQ(shown_px(&f, s, 40 + 205, 50 + 105), GREEN);
    CHECK_EQ(shown_px(&f, s, 399, 299), BG);
    return close_all(&f, h0, b0);
}

/* The next key gfx_key says within a second. */
static int key_soon(void)
{
    return gfx_key(now() + NS_PER_S);
}

bool t_fun_window_input(void)
{
    uint64_t h0, b0;
    start(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct fake_surface *s;
    if (!open_on(&f, 320, 200, &s))
        return false;
    CHECK_ST(gfx_mouse_open(false), OK);
    CHECK_ST(fake_kb_enter(&f, s), OK);
    CHECK_ST(fake_key(&f, KEY_A, true), OK);
    CHECK_ST(fake_key(&f, KEY_A, false), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), 'a');
    CHECK_EQ(gfx_key(0), KEY_NONE);   /* the release: nothing */

    CHECK_ST(fake_mods(&f, KEYMAP_MOD_CTRL, 0), OK);
    CHECK_ST(fake_key(&f, KEY_C, true), OK);
    CHECK_ST(fake_mods(&f, 0, 0), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), KEY_QUIT);

    struct mouse m;
    CHECK_ST(fake_ptr_enter(&f, s, 40, 50), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), KEY_MOUSE);
    gfx_mouse(&m);
    CHECK(m.x == 40 && m.y == 50 && m.moved && m.reports);
    CHECK_ST(fake_button(&f, 0x110, true), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), KEY_MOUSE);
    gfx_mouse(&m);
    CHECK(m.pressed == MOUSE_LEFT && m.buttons == MOUSE_LEFT);
    CHECK_ST(fake_button(&f, 0x110, false), OK);
    CHECK_ST(fake_wheel(&f, 2), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), KEY_MOUSE);   /* the release */
    CHECK_EQ(key_soon(), KEY_MOUSE);   /* the wheel */
    gfx_mouse(&m);
    CHECK(m.released == MOUSE_LEFT && !m.buttons && m.wheel == -2);

    CHECK_ST(fake_close(&f, s), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), KEY_QUIT);
    return close_all(&f, h0, b0);
}

bool t_fun_window_resize(void)
{
    uint64_t h0, b0;
    start(&h0, &b0);
    struct fake f;
    fake_init(&f);
    gfx_resizable();
    struct fake_surface *s;
    if (!open_on(&f, 320, 200, &s))
        return false;
    CHECK(!s->max_w && !s->max_h);   /* no fixed size asked */
    CHECK_ST(fake_configure(&f, s, 500, 260, 0), OK);
    fake_serve(&f);
    CHECK_EQ(key_soon(), KEY_RESIZE);
    CHECK(scr.w == 500 && scr.h == 260 && scr.s.w == 500 && scr.s.h == 260);
    CHECK_EQ(scr.s.px[0], BG);   /* the new back buffer starts as bg */
    fill(&scr.s, 400, 200, 100, 60, RED);
    gfx_present();
    fake_serve(&f);
    const struct fake_buffer *b = shown_buffer(&f, s);
    CHECK(b && b->w == 500 && b->h == 260 && s->acked == s->sent_serial);
    CHECK_EQ(shown_px(&f, s, 450, 230), RED);
    CHECK_EQ(shown_px(&f, s, 10, 10), BG);
    return close_all(&f, h0, b0);
}
