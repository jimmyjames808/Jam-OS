/* utest: libjwl's client side when the compositor goes (the owner's Q8:
 * clients reconnect), against the fake compositor (jwlcfake.h):
 *   jwlc_reconnect       the fake dies with a window shown, a frame
 *                        callback waiting and a pool of the program's
 *                        own: the keyboard focus leaves, the client
 *                        connects again at once, binds again, makes the
 *                        pools and buffers again on the same VMOs and the
 *                        window again with its title, shows its last
 *                        buffer (the same pixels, read through the new
 *                        mapping) and answers the lost frame callback;
 *                        then the next connects are refused twice and the
 *                        client waits 50 ms and 100 ms between tries;
 *   jwlc_protocol_error  the compositor ends the connection with
 *                        wl_display.error: the client is dead (no second
 *                        connection), the error kept in its info;
 *   jwlc_slow_reader     the client stops reading while the compositor sends
 *                        keys, until the transport drops it (no_memory,
 *                        "reads too slowly", M12.1 C12): the client reads
 *                        what was held, hears DISCONNECTED (ERR_NO_MEMORY),
 *                        is not dead, connects again and gets its window
 *                        back with its title and pixels; dropped again and
 *                        again it waits 50, 100, 200, 400 ms (and a
 *                        compositor's other no_memory errors stay fatal);
 *   jwlc_blocking        the blocking calls (connect, roundtrip,
 *                        wait_event) with the fake on a thread of its own,
 *                        a ping answered during the setup; a connect with
 *                        no compositor gives up at its deadline, and
 *                        refused at once with no_reconnect. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl_client.h>
#include <os.h>
#include "jwlcfake.h"
#include "utest.h"

#define SHOWN 0xffabcdefu

/* The first queued event of each type in types[], in that order. */
static bool in_order(struct jwl_client *c, const uint32_t *types, unsigned n,
                     struct jwl_event *evs)
{
    for (unsigned i = 0; i < n; i++)
        if (!fake_next_of(c, types[i], &evs[i]))
            FAIL("no event %u (the %u-th wanted)", types[i], i);
    return true;
}

bool t_jwlc_reconnect(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_pool *p;
    struct jwl_buffer *b;
    CHECK_ST(jwl_pool_create(c, 8192, &p), OK);
    CHECK_ST(jwl_buffer_create(p, 4096, 16, 16, 64, JWL_WL_SHM_FORMAT_XRGB8888, &b), OK);
    struct jwl_window_config wc = { .width = 32, .height = 32, .title = "again" };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    f.hold_frames = true;
    unsigned slot;
    CHECK(fake_draw(w, SHOWN, NULL, &slot));
    CHECK_ST(fake_kb_enter(&f, s), OK);
    CHECK(fake_pump(&f, c));
    CHECK(s->pixel == SHOWN && s->nframes == 1);
    CHECK(f.pools_made == 2 && f.buffers_made == 2);

    f.hold_frames = false;
    fake_drop(&f);
    CHECK(fake_pump(&f, c));
    CHECK_ST(jwl_client_status(c), OK);
    CHECK_EQ(f.connects, 2);
    CHECK_EQ(jwl_client_info(c)->generation, 2);
    CHECK(f.pools_made == 2 && f.buffers_made == 2 && f.binds == 5);
    s = fake_surface(&f, 0);
    CHECK(s && !strcmp(s->title, "again") && s->acks == 1);
    CHECK(s->current && s->pixel == SHOWN);   /* the last buffer, through the new mapping */
    static const uint32_t want[] = { JWL_EV_KEYBOARD_LEAVE, JWL_EV_DISCONNECTED,
                                     JWL_EV_RECONNECTED, JWL_EV_CONFIGURE, JWL_EV_FRAME };
    struct jwl_event ev[5];
    CHECK(in_order(c, want, 5, ev));
    CHECK(ev[0].win == w && ev[1].conn.why == ERR_PEER_CLOSED && ev[2].conn.generation == 2);
    CHECK(ev[3].win == w && ev[3].configure.rebuilt && ev[4].frame.made_up);
    CHECK(fake_draw(w, 0xff000001u, NULL, &slot));   /* and on it goes */
    CHECK(fake_pump(&f, c));
    CHECK(s->pixel == 0xff000001u && fake_next_of(c, JWL_EV_FRAME, &ev[0]));

    /* refused twice: tries at once, then after 50 ms, then 100 ms */
    f.refuse = 2;
    uint64_t t0 = now();
    fake_drop(&f);
    while (jwl_client_info(c)->generation < 3 && now() < t0 + 5 * NS_PER_S) {
        fake_pump(&f, c);
        uint64_t d = jwl_client_deadline(c);
        (void)jam_sleep_until(d < now() + 20 * NS_PER_MS ? d : now() + 20 * NS_PER_MS);
    }
    fake_pump(&f, c);
    CHECK_EQ(jwl_client_info(c)->generation, 3);
    CHECK_ST(jwl_client_status(c), OK);
    CHECK_EQ(f.connects, 3);
    CHECK(now() - t0 >= 150 * NS_PER_MS);
    CHECK(fake_surface(&f, 0) && fake_surface(&f, 0)->pixel == 0xff000001u);
    jwl_buffer_destroy(b);
    jwl_pool_destroy(p);
    return fake_all_gone(&f, c, h0, b0);
}

/* A connect function that says "not yet" (a thread of the program's
 * waiting for the compositor, as the console's connector does) this many
 * times before it connects. */
struct pending_connect {
    struct fake *f;
    unsigned waits;
};

static status_t connect_later(void *ctx, handle_t *out)
{
    struct pending_connect *p = ctx;
    if (p->waits) {
        p->waits--;
        return ERR_SHOULD_WAIT;
    }
    return fake_connect(p->f, out);
}

/* "Not yet" is no failure (M12.1 C13): the client asks again every 50 ms,
 * so six of them cost about 300 ms (before, the pause doubled each time:
 * the answer could wait up to a second after it came, 2.5 s here). */
bool t_jwlc_connect_pending(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct pending_connect pc = { &f, 6 };
    struct jwl_client_config cfg = fake_config(&f);
    cfg.connect = connect_later;
    cfg.connect_ctx = &pc;
    uint64_t t0 = now();
    struct jwl_client *c;
    CHECK_ST(jwl_client_create(&cfg, &c), OK);
    while (jwl_client_status(c) != OK && now() < t0 + 3 * NS_PER_S) {
        uint64_t d = jwl_client_deadline(c);
        (void)jam_sleep_until(d < t0 + 3 * NS_PER_S ? d : t0 + 3 * NS_PER_S);
        (void)fake_pump(&f, c);
    }
    CHECK_ST(jwl_client_status(c), OK);
    CHECK_EQ(pc.waits, 0);
    uint64_t took = now() - t0;
    if (took > NS_PER_S)
        FAIL("ready %lu ms after six \"not yet\"s, want about 300", (unsigned long)(took / NS_PER_MS));
    return fake_all_gone(&f, c, h0, b0);
}

bool t_jwlc_protocol_error(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_window_config wc = { .width = 8, .height = 8 };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    uint32_t sid = s->id;
    CHECK_ST(jwl_conn_post_error(f.conn, sid, 7, "a bad thing"), ERR_INVALID_ARGS);
    fake_pump(&f, c);
    CHECK_ST(jwl_client_status(c), ERR_INVALID_ARGS);
    const struct jwl_client_info *in = jwl_client_info(c);
    CHECK(in->error_object == sid && in->error_code == 7 && !strcmp(in->error_text, "a bad thing"));
    struct jwl_event ev;
    CHECK(fake_next_of(c, JWL_EV_DISCONNECTED, &ev));
    CHECK(fake_next_of(c, JWL_EV_DEAD, &ev) && ev.conn.why == ERR_INVALID_ARGS);
    CHECK_EQ(f.connects, 1);
    CHECK_ST(jwl_client_dispatch(c), ERR_INVALID_ARGS);
    CHECK_ST(jwl_client_wait_event(c, now() + NS_PER_S, &ev), ERR_INVALID_ARGS);
    struct jwl_frame fr;
    CHECK_ST(jwl_window_begin(w, &fr), ERR_BAD_STATE);
    CHECK_ST(jwl_client_roundtrip(c, now() + NS_PER_S), ERR_INVALID_ARGS);
    return fake_all_gone(&f, c, h0, b0);
}

/* Keys sent and flushed with nobody reading, until the transport drops the
 * client (the window's batches, 64 KiB held, then the no_memory error). */
static bool flood(struct fake *f)
{
    CHECK(f->conn && f->keyboard);
    for (unsigned i = 0; i < 20000 && f->conn->status == OK; i++) {
        (void)fake_key(f, 30, i & 1);
        (void)jwl_conn_flush(f->conn);
    }
    CHECK(f->conn->status != OK);
    CHECK_EQ(f->conn->error.code, JWL_ERROR_NO_MEMORY);
    CHECK(!strncmp(f->conn->error.text, JWL_SLOW_READER_TEXT, strlen(JWL_SLOW_READER_TEXT)));
    return true;
}

/* The client reads at last: the keys in the channel, then the error. Stops at its
 * JWL_EV_DISCONNECTED (why in *why); the client must not die. */
static bool read_until_dropped(struct jwl_client *c, unsigned *keys, status_t *why)
{
    struct jwl_event ev;
    for (unsigned i = 0; i < 4000; i++) {
        (void)jwl_client_dispatch(c);
        status_t st = jwl_client_status(c);   /* OK, or ERR_SHOULD_WAIT while it connects again */
        if (st != OK && st != ERR_SHOULD_WAIT)
            FAIL("the slow reader's client ended: %d", (int)st);
        while (jwl_client_next_event(c, &ev) == OK) {
            if (ev.type == JWL_EV_KEY)
                ++*keys;
            if (ev.type == JWL_EV_DEAD)
                FAIL("the slow reader was ended for good (%d)", (int)ev.conn.why);
            if (ev.type == JWL_EV_DISCONNECTED) {
                *why = ev.conn.why;
                return true;
            }
        }
    }
    FAIL("no JWL_EV_DISCONNECTED after the compositor's no_memory");
}

/* Serve and dispatch until the client is on connection gen again (5 s). */
static bool wait_generation(struct fake *f, struct jwl_client *c, uint32_t gen)
{
    uint64_t end = now() + 5 * NS_PER_S;
    while (jwl_client_info(c)->generation < gen && now() < end) {
        (void)fake_pump(f, c);
        uint64_t d = jwl_client_deadline(c);
        (void)jam_sleep_until(d < now() + 10 * NS_PER_MS ? d : now() + 10 * NS_PER_MS);
    }
    (void)fake_pump(f, c);
    CHECK_EQ(jwl_client_info(c)->generation, gen);
    return true;
}

/* M12.1 C12 (the owner's DQ1): a compositor's "reads too slowly" drop is
 * a lost connection, not the end. */
bool t_jwlc_slow_reader(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_pool *p;
    struct jwl_buffer *b;
    CHECK_ST(jwl_pool_create(c, 8192, &p), OK);
    CHECK_ST(jwl_buffer_create(p, 4096, 16, 16, 64, JWL_WL_SHM_FORMAT_XRGB8888, &b), OK);
    struct jwl_window_config wc = { .width = 32, .height = 32, .title = "slow" };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    unsigned slot;
    CHECK(fake_draw(w, SHOWN, NULL, &slot));
    CHECK_ST(fake_kb_enter(&f, s), OK);
    CHECK(fake_pump(&f, c));
    CHECK(s->pixel == SHOWN);
    struct jwl_event ev;
    while (jwl_client_next_event(c, &ev) == OK) {}

    /* dropped four times in a row: 50, 100, 200, 400 ms before each try */
    for (unsigned n = 0; n < 4; n++) {
        CHECK(flood(&f));   /* the client is busy: it reads nothing meanwhile */
        uint64_t t0 = now();
        unsigned keys = 0;
        status_t why = OK;
        CHECK(read_until_dropped(c, &keys, &why));
        CHECK_EQ(why, ERR_NO_MEMORY);
        if (!keys)   /* the window's batches (the held ones are not sent) come first */
            FAIL("drop %u: no key read before the drop", n + 1);
        CHECK_EQ(jwl_client_info(c)->error_code, JWL_ERROR_NO_MEMORY);
        CHECK(wait_generation(&f, c, n + 2));
        uint64_t took = now() - t0, want = (50u << n) * NS_PER_MS;
        if (took < want)
            FAIL("drop %u: connected again after %lu ms, want at least %lu",
                 n + 1, (unsigned long)(took / NS_PER_MS), (unsigned long)(want / NS_PER_MS));
        CHECK_ST(jwl_client_status(c), OK);
        CHECK_EQ(f.connects, n + 2);
        s = fake_surface(&f, 0);   /* the window is back: title, buffer, pixels */
        CHECK(s && !strcmp(s->title, "slow") && s->current && s->pixel == SHOWN);
        bool reconnected = false, rebuilt = false;
        while (jwl_client_next_event(c, &ev) == OK) {
            reconnected |= ev.type == JWL_EV_RECONNECTED && ev.conn.generation == n + 2;
            rebuilt |= ev.type == JWL_EV_CONFIGURE && ev.win == w && ev.configure.rebuilt;
        }
        CHECK(reconnected && rebuilt);
        CHECK_ST(fake_kb_enter(&f, s), OK);   /* keys reach the program on this connection too */
        CHECK(fake_pump(&f, c));
        while (jwl_client_next_event(c, &ev) == OK) {}
    }
    CHECK(fake_draw(w, 0xff000002u, NULL, &slot));   /* and on it goes */
    CHECK(fake_pump(&f, c));
    CHECK(fake_surface(&f, 0)->pixel == 0xff000002u);

    /* any other no_memory (a cap the program hit) is still the end */
    CHECK_ST(jwl_conn_post_error(f.conn, JWL_DISPLAY_ID, JWL_ERROR_NO_MEMORY,
                                 "too many regions (64)"), ERR_INVALID_ARGS);
    (void)fake_pump(&f, c);
    CHECK_ST(jwl_client_status(c), ERR_INVALID_ARGS);
    jwl_buffer_destroy(b);
    jwl_pool_destroy(p);
    return fake_all_gone(&f, c, h0, b0);
}

/* The next event of type, waiting for it (others are skipped). */
static bool wait_for(struct jwl_client *c, uint32_t type, struct jwl_event *ev)
{
    uint64_t deadline = now() + 5 * NS_PER_S;
    do
        CHECK_ST(jwl_client_wait_event(c, deadline, ev), OK);
    while (ev->type != type);
    return true;
}

bool t_jwlc_blocking(void)
{
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    f.ping_at_bind = true;
    handle_t th;
    CHECK_ST(fake_thread_start(&f, &th), OK);
    struct jwl_client_config cfg = fake_config(&f);
    struct jwl_client *c = NULL;
    status_t st = jwl_client_connect(&cfg, now() + 5 * NS_PER_S, &c);
    bool ok = st == OK && jwl_client_info(c)->pings == 1 &&
              jwl_client_roundtrip(c, now() + 5 * NS_PER_S) == OK;
    struct jwl_window_config wc = { .width = 24, .height = 24 };
    struct jwl_window *w;
    struct jwl_event ev;
    struct jwl_frame fr;
    ok = ok && jwl_window_create(c, &wc, &w) == OK && wait_for(c, JWL_EV_CONFIGURE, &ev);
    ok = ok && jwl_window_begin(w, &fr) == OK && jwl_window_present(w, NULL, 0, true) == OK;
    ok = ok && wait_for(c, JWL_EV_FRAME, &ev) && ev.win == w;
    jwl_client_destroy(c);
    fake_thread_stop(&f, th);
    CHECK_ST(st, OK);
    CHECK(ok);
    CHECK(f.pongs == 1 && f.syncs >= 3);
    CHECK(fake_all_gone(&f, NULL, h0, b0));

    /* nobody answers: connect gives up at its deadline */
    fake_init(&f);
    f.refuse = 1000;
    cfg = fake_config(&f);
    uint64_t t0 = now();
    CHECK_ST(jwl_client_connect(&cfg, t0 + 200 * NS_PER_MS, &c), ERR_TIMED_OUT);
    CHECK(now() - t0 >= 200 * NS_PER_MS && f.connects == 0);
    cfg.no_reconnect = true;
    CHECK_ST(jwl_client_create(&cfg, &c), ERR_PEER_CLOSED);
    return fake_all_gone(&f, NULL, h0, b0);
}
