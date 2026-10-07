/* utest: libjwl's client side (<jwl_client.h>) against the fake
 * compositor (jwlcfake.h), one side then the other in this thread:
 *   jwlc_setup        the registry dance: each global bound at the lower
 *                     of the fake's version and the library's, its first
 *                     events read before the client says ready (shm
 *                     formats, the output's mode, the seat's devices, the
 *                     keymap and repeat rate); a compositor offering less
 *                     (no seat, no output, wl_compositor 3; no
 *                     xdg_wm_base: no windows); one without wl_shm or
 *                     without xrgb8888 and argb8888 (the client dies:
 *                     ERR_NOT_SUPPORTED);
 *   jwlc_window       a window: its surface, role, title and fixed size,
 *                     the first configure, two buffers in a kept pool the
 *                     fake maps read-only (the pixels it reads are the
 *                     ones drawn), the ack with the next commit, damage,
 *                     frame callbacks on a version 4 surface, release;
 *   jwlc_window_sizes a resizable window maximised to a size the pool
 *                     grows for, a configure that changes only states
 *                     (acked at once), new sizes drawn without touching
 *                     the shown buffer's pixels, a fixed-size window
 *                     keeping its size, full screen, a resizable one's
 *                     minimum (set_min_size, no max), close, a new title.
 * Each ends with the job's handles and message bytes where they began. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl_client.h>
#include <os.h>
#include "jwlcfake.h"
#include "utest.h"

#define ACTIVATED JWL_STATE(JWL_XDG_TOPLEVEL_STATE_ACTIVATED)
#define MAXIMIZED JWL_STATE(JWL_XDG_TOPLEVEL_STATE_MAXIMIZED)
#define FULL      JWL_STATE(JWL_XDG_TOPLEVEL_STATE_FULLSCREEN)

bool t_jwlc_setup(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client_config cfg = fake_config(&f);
    struct jwl_client *c;
    CHECK_ST(jwl_client_create(&cfg, &c), OK);
    CHECK_ST(jwl_client_status(c), ERR_SHOULD_WAIT);
    CHECK(fake_pump(&f, c));
    CHECK_ST(jwl_client_status(c), OK);
    const struct jwl_client_info *in = jwl_client_info(c);
    /* offered 6, 1, 3, 7, 4: bound at the library's 4, 1, 1, 5, 3 */
    CHECK(in->compositor_version == 4 && in->shm_version == 1 && in->wm_base_version == 1);
    CHECK(in->seat_version == 5 && in->output_version == 3 && in->generation == 1);
    CHECK(f.bound[FAKE_COMPOSITOR] == 4 && f.bound[FAKE_SEAT] == 5 && f.bound[FAKE_OUTPUT] == 3);
    CHECK_EQ(f.binds, 5);
    CHECK_EQ(f.syncs, 2);
    CHECK_EQ(in->shm_formats, f.formats);
    CHECK(in->output_width == 1280 && in->output_height == 800 && in->output_refresh == 60000);
    CHECK_EQ(in->seat_caps, 3);
    CHECK(f.keyboard && f.pointer);
    CHECK(in->keymap == &keymap_us);
    CHECK(in->repeat_rate == 30 && in->repeat_delay == 500);
    CHECK(jwl_client_channel(c) != HANDLE_INVALID);
    CHECK(fake_all_gone(&f, c, h0, b0));

    /* less offered: no seat, no output, wl_compositor 3 */
    fake_init(&f);
    f.offer[FAKE_SEAT] = f.offer[FAKE_OUTPUT] = 0;
    f.offer[FAKE_COMPOSITOR] = 3;
    c = fake_ready(&f);
    CHECK(c);
    in = jwl_client_info(c);
    CHECK(in->compositor_version == 3 && !in->seat_version && !in->output_version);
    CHECK(!in->keymap && !in->output_width);
    CHECK(fake_all_gone(&f, c, h0, b0));

    /* no xdg_wm_base: a client, but no windows */
    fake_init(&f);
    f.offer[FAKE_WM_BASE] = 0;
    c = fake_ready(&f);
    CHECK(c && !jwl_client_info(c)->wm_base_version);
    struct jwl_window_config wc = { .width = 10, .height = 10 };
    struct jwl_window *w;
    CHECK_ST(jwl_window_create(c, &wc, &w), ERR_NOT_SUPPORTED);
    CHECK(fake_all_gone(&f, c, h0, b0));

    /* no wl_shm, or none of the two formats: dead for good */
    for (unsigned k = 0; k < 2; k++) {
        fake_init(&f);
        if (k == 0)
            f.offer[FAKE_SHM] = 0;
        else
            f.formats = 1u << 5;
        cfg = fake_config(&f);
        CHECK_ST(jwl_client_create(&cfg, &c), OK);
        fake_pump(&f, c);
        CHECK_ST(jwl_client_status(c), ERR_NOT_SUPPORTED);
        struct jwl_event ev;
        CHECK(fake_next_of(c, JWL_EV_DEAD, &ev) && ev.conn.why == ERR_NOT_SUPPORTED);
        CHECK_ST(jwl_client_dispatch(c), ERR_NOT_SUPPORTED);
        CHECK_EQ(f.connects, 1);
        CHECK_ST(jwl_window_create(c, &wc, &w), ERR_NOT_SUPPORTED);
        CHECK(fake_all_gone(&f, c, h0, b0));
    }
    return true;
}

bool t_jwlc_window(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    f.cfg_states = ACTIVATED;
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_window_config wc = { .width = 64, .height = 48, .title = "hello", .app_id = "ut" };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    CHECK(!strcmp(s->title, "hello"));
    CHECK(s->min_w == 64 && s->min_h == 48 && s->max_w == 64 && s->max_h == 48);
    int32_t ww, wh;
    jwl_window_size(w, &ww, &wh);
    CHECK(ww == 64 && wh == 48 && jwl_window_states(w) == ACTIVATED);

    unsigned slot;
    CHECK(fake_draw(w, 0xff112233u, NULL, &slot));
    CHECK_EQ(slot, 0);
    CHECK(fake_pump(&f, c));
    CHECK(s->acks == 1 && s->acked == s->sent_serial);
    CHECK(s->attaches == 1 && s->commits == 2);
    CHECK(s->last_damage.x == 0 && s->last_damage.w == 64 && s->last_damage.h == 48);
    CHECK_EQ(s->pixel, 0xff112233u);   /* read through the fake's kept mapping */
    struct jwl_event ev;
    CHECK(fake_next_of(c, JWL_EV_FRAME, &ev) && ev.win == w && !ev.frame.made_up);

    struct jwl_rect r = { 1, 2, 3, 4 };
    CHECK(fake_draw(w, 0xff445566u, &r, &slot));
    CHECK_EQ(slot, 1);   /* slot 0 is still shown */
    CHECK(fake_pump(&f, c));
    CHECK_EQ(s->pixel, 0xff445566u);
    CHECK(s->last_damage.x == 1 && s->last_damage.y == 2 && s->last_damage.w == 3);
    CHECK_EQ(s->acks, 1);   /* nothing more to ack */
    CHECK(fake_next_of(c, JWL_EV_FRAME, &ev));
    CHECK(fake_draw(w, 0xff778899u, NULL, &slot));
    CHECK_EQ(slot, 0);   /* released by the commit before */
    CHECK(fake_pump(&f, c));
    CHECK(f.pools_made == 1 && f.buffers_made == 2);
    CHECK_ST(jwl_window_present(w, NULL, 0, false), ERR_BAD_STATE);   /* no begin */

    jwl_window_destroy(w);
    CHECK(fake_pump(&f, c));
    CHECK(!fake_surface(&f, 0));
    CHECK_ST(jwl_client_next_event(c, &ev), ERR_SHOULD_WAIT);   /* its FRAME dropped */
    return fake_all_gone(&f, c, h0, b0);
}

bool t_jwlc_window_sizes(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_window_config wc = { .width = 100, .height = 80, .resizable = true };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    CHECK(!s->min_w && !s->max_w);   /* resizable: no limits */
    unsigned slot;
    CHECK(fake_draw(w, 0xff000001u, NULL, &slot));
    CHECK(fake_pump(&f, c));

    /* maximised to 300 x 200: the pool grows, the fake maps it again */
    CHECK_ST(fake_configure(&f, s, 300, 200, MAXIMIZED | ACTIVATED), OK);
    CHECK(fake_pump(&f, c));
    struct jwl_event ev;
    CHECK(fake_next_of(c, JWL_EV_CONFIGURE, &ev));
    CHECK(ev.configure.width == 300 && ev.configure.height == 200);
    CHECK(ev.configure.states == (MAXIMIZED | ACTIVATED));
    struct jwl_frame fr;
    CHECK_ST(jwl_window_begin(w, &fr), OK);
    CHECK(fr.width == 300 && fr.height == 200 && fr.stride == 1200);
    fake_fill(&fr, 0xff000002u);
    fr.px[0] = 0xff0000ffu;
    CHECK_ST(jwl_window_present(w, NULL, 0, false), OK);
    CHECK(fake_pump(&f, c));
    CHECK(s->acked == s->sent_serial && s->pixel == 0xff0000ffu);

    /* states only: acked and committed at once, nothing attached */
    unsigned attaches = s->attaches, commits = s->commits;
    CHECK_ST(fake_configure(&f, s, 300, 200, MAXIMIZED), OK);
    CHECK(fake_pump(&f, c));
    CHECK(s->acked == s->sent_serial && s->commits == commits + 1 && s->attaches == attaches);
    CHECK(fake_next_of(c, JWL_EV_CONFIGURE, &ev) && ev.configure.states == MAXIMIZED);

    /* a new size again (a resize): the picture shown keeps its pixels while
     * the new size's frame is drawn, until that frame's commit replaces it */
    const struct fake_buffer *shown = NULL;
    for (unsigned i = 0; i < FAKE_BUFFERS; i++)
        if (f.b[i].id && f.b[i].id == s->current)
            shown = &f.b[i];
    CHECK(shown);
    const volatile uint32_t *old = (const volatile uint32_t *)(uintptr_t)(shown->pool->addr +
                                                                         (uint64_t)shown->offset);
    size_t last = (size_t)(shown->h - 1) * (size_t)(shown->stride / 4) + (size_t)shown->w - 1;
    uint32_t first_px = 0xff0000ffu, last_px = 0xff000002u;   /* what the shown one has */
    for (uint32_t k = 0; k < 3; k++) {   /* bigger, then smaller twice */
        int32_t nw = 400 - 60 * (int32_t)k, nh = 300 - 40 * (int32_t)k;
        CHECK_ST(fake_configure(&f, s, nw, nh, MAXIMIZED), OK);
        CHECK(fake_pump(&f, c));
        CHECK(fake_next_of(c, JWL_EV_CONFIGURE, &ev));
        CHECK_ST(jwl_window_begin(w, &fr), OK);
        CHECK(fr.width == nw && fr.height == nh);
        fake_fill(&fr, 0xff000003u + k);
        CHECK(old[0] == first_px && old[last] == last_px);   /* not drawn over */
        CHECK_ST(jwl_window_present(w, NULL, 0, false), OK);
        CHECK(fake_pump(&f, c));
        CHECK(s->pixel == 0xff000003u + k);
        first_px = last_px = 0xff000003u + k;
        /* the next round's shown buffer: this one */
        shown = NULL;
        for (unsigned i = 0; i < FAKE_BUFFERS; i++)
            if (f.b[i].id && f.b[i].id == s->current)
                shown = &f.b[i];
        CHECK(shown);
        old = (const volatile uint32_t *)(uintptr_t)(shown->pool->addr + (uint64_t)shown->offset);
        last = (size_t)(shown->h - 1) * (size_t)(shown->stride / 4) + (size_t)shown->w - 1;
    }

    /* a fixed-size window keeps its size whatever it is asked */
    struct jwl_window_config fixed = { .width = 50, .height = 40 };
    struct jwl_window *w2;
    struct fake_surface *s2;
    CHECK(fake_window(&f, c, &fixed, 1, &w2, &s2));
    CHECK_ST(fake_configure(&f, s2, 500, 400, 0), OK);
    CHECK(fake_pump(&f, c));
    CHECK(fake_next_of(c, JWL_EV_CONFIGURE, &ev) && ev.win == w2);
    CHECK(ev.configure.width == 50 && ev.configure.height == 40 && ev.configure.asked_w == 500);

    /* full screen from the start takes the compositor's size */
    struct jwl_window_config fs = { .width = 64, .height = 64, .fullscreen = true };
    struct jwl_window *w3;
    struct fake_surface *s3;
    CHECK(fake_window(&f, c, &fs, 2, &w3, &s3));
    CHECK(s3->fullscreen);
    CHECK_ST(fake_configure(&f, s3, 1280, 800, FULL), OK);
    CHECK(fake_pump(&f, c));
    CHECK(fake_next_of(c, JWL_EV_CONFIGURE, &ev) && ev.configure.width == 1280);
    CHECK_ST(jwl_window_set_fullscreen(w3, false), OK);

    /* a resizable window with a minimum: set_min_size, no max */
    struct jwl_window_config mn = { .width = 120, .height = 90, .resizable = true,
                                    .min_width = 100, .min_height = 70 };
    struct jwl_window *w4;
    struct fake_surface *s4;
    CHECK(fake_window(&f, c, &mn, 3, &w4, &s4));
    CHECK(s4->min_w == 100 && s4->min_h == 70 && !s4->max_w && !s4->max_h);

    CHECK_ST(fake_close(&f, s), OK);
    CHECK_ST(jwl_window_set_title(w, "renamed"), OK);
    CHECK(fake_pump(&f, c));
    CHECK(fake_next_of(c, JWL_EV_CLOSE, &ev) && ev.win == w);
    CHECK(!strcmp(s->title, "renamed") && !s3->fullscreen);
    return fake_all_gone(&f, c, h0, b0);
}
