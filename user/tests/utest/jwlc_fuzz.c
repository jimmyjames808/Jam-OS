/* utest: libjwl's client side against a compositor that sends anything
 * (M12.1, review 2b's hostile tests), over the fake compositor
 * (jwlcfake.h).
 *
 * t_jwlc_fuzz_events: a ready client with a window it has drawn into, its
 * keyboard focused, gets thousands of well-formed events with hostile
 * values: every event of every object it has (but wl_display's), each
 * argument by its letter: words at their ends (0, -1, INT32_MIN,
 * INT32_MAX) or random, strings of random bytes, arrays of random words
 * (configure's states), objects of the right type or null, new objects,
 * handles of every kind (a VMO of random bytes or with a keymap's first
 * line, a channel end, an event). Between bursts the program does what a
 * program does (takes its events, draws when it may) and now and then the
 * compositor dies. The client must never crash, stay usable, and give
 * back every handle and message byte when it is destroyed.
 *
 * t_jwlc_fuzz_batches: random and mutated batches, written raw onto the
 * client's connection: each is taken or makes the client drop the
 * connection and connect again; it never crashes, and a dead client (a
 * batch that happens to be a valid wl_display.error) is made again.
 *
 * JWLC_FUZZ_SCALE multiplies the counts. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <jwl_client.h>
#include <keymap.h>
#include <os.h>
#include "jwlcfake.h"
#include "jwltest.h"
#include "utest.h"

#ifndef JWLC_FUZZ_SCALE
#define JWLC_FUZZ_SCALE 1
#endif

#define BURSTS    (200 * JWLC_FUZZ_SCALE)
#define PER_BURST 12
#define BATCHES   (60 * JWLC_FUZZ_SCALE)
#define MAX_DRAW  1024   /* the program draws at sizes up to this (a pool commits its pages) */

/* ---- hostile values ------------------------------------------------------------------- */

static uint32_t word(uint32_t *seed)
{
    uint32_t x = jt_rng(seed);
    switch (x % 10) {
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return 0x80000000u;
    case 3: return 0x7fffffffu;
    case 4: return (x >> 8) % 300;                /* a plausible count or size */
    case 5: return ((x >> 8) % 4000) << 8;        /* a plausible fixed coordinate */
    case 6: return (uint32_t)-(int32_t)((x >> 8) % 4000);
    default: return jt_rng(seed);
    }
}

/* A live object of the server's map: of iface (any, if NULL), with events
 * if events; 0 if none. */
static uint32_t pick(const struct jwl_map *m, const struct jwl_interface *iface, bool events,
                     uint32_t *seed)
{
    uint32_t ids[96];
    unsigned n = 0;
    for (unsigned r = 0; r < 2; r++)
        for (uint32_t i = 0; i < m->r[r].n && n < 96; i++) {
            const struct jwl_object *o = &m->r[r].v[i];
            uint32_t id = r ? JWL_SERVER_ID_BASE + i : i + 1;
            if (o->state != JWL_LIVE || o->pending || id == JWL_DISPLAY_ID)
                continue;
            if ((iface && !jwl_interface_same(o->iface, iface)) || (events && !o->iface->nevents))
                continue;
            ids[n++] = id;
        }
    return n ? ids[jt_rng(seed) % n] : 0;
}

/* A handle of some kind for an fd argument; HANDLE_INVALID if none could
 * be made. */
static handle_t any_handle(uint32_t *seed)
{
    handle_t h = HANDLE_INVALID, other;
    switch (jt_rng(seed) % 4) {
    case 0:
    case 1: {   /* a VMO: random bytes, or a keymap's first line and then random ones */
        uint8_t b[160];
        for (unsigned i = 0; i < sizeof(b); i++)
            b[i] = (uint8_t)jt_rng(seed);
        if (jt_rng(seed) & 1) {
            const char *nl = strchr(keymap_us.xkb, '\n');
            size_t n = nl ? (size_t)(nl - keymap_us.xkb) + 1 : 0;
            memcpy(b, keymap_us.xkb, n < sizeof(b) ? n : sizeof(b));
        }
        if (jam_vmo_create(PAGE_SIZE * (1 + jt_rng(seed) % 2), 0, &h) == OK)
            (void)jam_vmo_write(h, 0, b, sizeof(b));
        break;
    }
    case 2:
        if (jam_channel_create(&h, &other) == OK)
            jam_handle_close(other);
        break;
    default:
        (void)jam_event_create(&h);
    }
    return h;
}

static char strs[JWL_ARGS_MAX][200];
static uint32_t arrs[JWL_ARGS_MAX][24];

/* One event with hostile values on a random object of the client's: OK if
 * it went (a refusal by our own jwl_send, e.g. no object of a type it
 * names, is just skipped). */
static void one_event(struct fake *f, uint32_t *seed, unsigned *sent)
{
    struct jwl_map *m = &f->conn->map;
    uint32_t id = pick(m, NULL, true, seed);
    if (!id)
        return;
    const struct jwl_object *o = jwl_map_get(m, id);
    const struct jwl_interface *iface = o->iface;
    uint32_t op = jt_rng(seed) % iface->nevents;
    const struct jwl_message *msg = &iface->events[op];
    struct jwl_sig sig;
    if (jwl_sig_parse(msg->signature, &sig) != OK || sig.since > o->version)
        return;
    union jwl_arg a[JWL_ARGS_MAX];
    uint32_t made[JWL_ARGS_MAX];
    unsigned nmade = 0;
    memset(a, 0, sizeof(a));
    for (unsigned i = 0; i < sig.n; i++) {
        const struct jwl_interface *t = msg->types ? msg->types[i] : NULL;
        switch (sig.type[i]) {
        case 's': {
            unsigned len = jt_rng(seed) % sizeof(strs[i]);
            for (unsigned k = 0; k < len; k++)
                strs[i][k] = (char)(1 + jt_rng(seed) % 255);
            strs[i][len] = 0;
            a[i].s = sig.nullable[i] && jt_rng(seed) % 4 == 0 ? NULL : strs[i];
            break;
        }
        case 'a': {
            unsigned len = jt_rng(seed) % 24;
            for (unsigned k = 0; k < len; k++)
                arrs[i][k] = jt_rng(seed) & 1 ? jt_rng(seed) % 40 : word(seed);
            a[i].a = (struct jwl_array){ len ? arrs[i] : NULL, len * 4 };
            break;
        }
        case 'o':
            a[i].o = sig.nullable[i] && jt_rng(seed) % 3 == 0 ? 0 : pick(m, t, false, seed);
            break;
        case 'n':
            if (t && jwl_conn_make(f->conn, t, o->version, NULL, &a[i].n) == OK)
                made[nmade++] = a[i].n;
            break;
        case 'h':
            a[i].h = any_handle(seed);
            break;
        default:
            a[i].u = word(seed);
        }
    }
    status_t st = jwl_send(f->conn, id, op, msg, a);   /* consumes the handles either way */
    for (unsigned i = 0; i < nmade; i++) {
        struct jwl_object *n = jwl_map_get(m, made[i]);
        if (st != OK && n && n->pending)
            (void)jwl_conn_delete(f->conn, made[i]);   /* refused: no gap in our ids */
    }
    *sent += st == OK;
}

/* What a program does between bursts: takes its events, and draws when it
 * may at a size it can afford. */
static void program_turn(struct jwl_client *c, struct jwl_window *w, unsigned *events)
{
    struct jwl_event ev;
    while (jwl_client_next_event(c, &ev) == OK)
        (*events)++;
    int32_t ww, wh;
    jwl_window_size(w, &ww, &wh);
    struct jwl_frame fr;
    if (jwl_client_status(c) != OK || ww > MAX_DRAW || wh > MAX_DRAW ||
        jwl_window_begin(w, &fr) != OK)
        return;
    fake_fill(&fr, 0xff102030);
    struct jwl_rect r = { 1, 1, ww / 2, wh / 2 };
    (void)jwl_window_present(w, &r, 1, true);
}

/* A client made and destroyed before the counting starts: the first
 * client of a run leaves a handle libos keeps (TESTING.md: jwlc_setup
 * sees the same when `utest only` runs it first). */
static void warm_up(void)
{
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    if (c)
        jwl_client_destroy(c);
    fake_serve(&f);
    fake_free(&f);
}

bool t_jwlc_fuzz_events(void)
{
    warm_up();
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    struct jwl_window_config wc = { .width = 64, .height = 48, .resizable = true };
    struct jwl_window *w;
    struct fake_surface *s;
    CHECK(fake_window(&f, c, &wc, 0, &w, &s));
    unsigned slot;
    CHECK(fake_draw(w, 0xff405060, NULL, &slot));
    CHECK_ST(fake_kb_enter(&f, s), OK);
    CHECK_ST(fake_ptr_enter(&f, s, 3, 4), OK);
    CHECK(fake_pump(&f, c));
    uint32_t seed = 0x2b5e1d3u;
    unsigned sent = 0, events = 0, drops = 0;
    for (unsigned b = 0; b < BURSTS; b++) {
        if (b % 67 == 66) {   /* the compositor dies; the client connects again */
            fake_drop(&f);
            drops++;
            uint64_t until = now() + 2 * NS_PER_S;
            while (jwl_client_status(c) != OK && now() < until) {
                (void)jam_sleep_until(jwl_client_deadline(c) < until ? jwl_client_deadline(c)
                                                                     : until);
                (void)fake_pump(&f, c);
            }
            CHECK_ST(jwl_client_status(c), OK);
            continue;
        }
        if (!f.conn)
            (void)fake_pump(&f, c);
        for (unsigned i = 0; i < PER_BURST && f.conn; i++)
            one_event(&f, &seed, &sent);
        if (f.conn)
            (void)jwl_conn_flush(f.conn);
        (void)fake_pump(&f, c);
        program_turn(c, w, &events);
        CHECK(jwl_client_status(c) != ERR_INVALID_ARGS);   /* never dead: no error was sent */
    }
    if (sent < BURSTS * PER_BURST / 4)
        FAIL("only %u of %u events went", sent, BURSTS * PER_BURST);
    printf("utest: jwlc_fuzz_events: %u hostile events, %u program events, %u compositor "
           "deaths\n", sent, events, drops);
    f.unexpected[0] = 0;   /* answers to nonsense may be anything */
    return fake_all_gone(&f, c, h0, b0);
}

/* ---- raw batches ------------------------------------------------------------------------ */

/* Random messages behind a header, or a header that is wrong, or none. */
static size_t garbage(uint8_t *b, size_t cap, uint32_t *seed, unsigned *nh)
{
    struct jt_wr w = { .b = b };
    uint32_t x = jt_rng(seed);
    *nh = 0;
    if (x % 5) {
        *nh = x % 7 == 0 ? 1 + (x >> 8) % 3 : 0;
        jt_w32(&w, x % 11 ? JWL_MAGIC : jt_rng(seed));
        jt_w32(&w, x % 13 ? *nh : *nh + 1);
        jt_w32(&w, x % 17 ? 0 : 1);
        jt_w32(&w, 0);
    }
    size_t want = jt_rng(seed) % 600;
    while (w.n + 8 <= want && w.n + 64 < cap) {
        uint32_t y = jt_rng(seed);
        uint32_t id = y % 3 ? 1 + y % 40 : word(seed);
        jt_begin(&w, id, (uint16_t)((y >> 8) % 8));
        for (unsigned i = 0, n = (y >> 16) % 10; i < n; i++)
            jt_w32(&w, word(seed));
        if (y >> 29)
            jt_end(&w);   /* else the size word is whatever the opcode made it */
    }
    return w.n;
}

bool t_jwlc_fuzz_batches(void)
{
    warm_up();
    uint64_t h0, b0;
    fake_held(&h0, &b0);
    struct fake f;
    fake_init(&f);
    struct jwl_client *c = fake_ready(&f);
    CHECK(c);
    static uint8_t buf[1024];
    uint32_t seed = 0x51ed27u;
    unsigned lost = 0, remade = 0;
    for (unsigned i = 0; i < BATCHES; i++) {
        if (jwl_client_status(c) != OK || !f.conn) {   /* back to a working connection */
            uint64_t until = now() + 3 * NS_PER_S;
            while (jwl_client_status(c) == ERR_SHOULD_WAIT && now() < until) {
                uint64_t d = jwl_client_deadline(c);
                (void)jam_sleep_until(d < until ? d : until);
                (void)fake_pump(&f, c);
            }
            if (jwl_client_status(c) != OK) {   /* dead for good: a valid error came */
                f.unexpected[0] = 0;
                jwl_client_destroy(c);
                fake_drop(&f);
                c = fake_ready(&f);
                CHECK(c);
                remade++;
            }
        }
        unsigned nh;
        size_t n = garbage(buf, sizeof(buf), &seed, &nh);
        handle_t hs[4];
        for (unsigned k = 0; k < nh; k++)
            hs[k] = any_handle(&seed);
        uint32_t gen = jwl_client_info(c)->generation;
        CHECK_ST(jam_channel_write(f.conn->ch, buf, (uint32_t)n, hs, nh), OK);
        (void)jwl_client_dispatch(c);
        lost += jwl_client_info(c)->generation != gen || jwl_client_status(c) != OK;
        (void)fake_pump(&f, c);
    }
    printf("utest: jwlc_fuzz_batches: %u raw batches: %u connections lost, %u clients made "
           "again\n", BATCHES, lost, remade);
    f.unexpected[0] = 0;
    return fake_all_gone(&f, c, h0, b0);
}
