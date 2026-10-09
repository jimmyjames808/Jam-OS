/* utest: events in the IDL (tools/genidl.py's `event` methods), on the test
 * protocol idltest_ev (abi/idl/idltest.idl): one-way messages with txid 0,
 * never answered, both ways in one protocol (note and give to the server,
 * ring and gift to the client), carrying arrays and handles, beside a call.
 *
 * To the server: the handler runs with the event's fields and owns its
 * handles, nothing is answered; an event that breaks its format (a bad
 * str, a handle too few or too many), goes the wrong way or has no handler
 * is dropped, its handles closed; one sent with a txid is refused
 * (ERR_INVALID_ARGS, its sender may be waiting); a call with txid 0 is
 * dropped (a reply with txid 0 would look like an event). Through
 * <proto>_serve_one, <proto>_serve and slots alike. To the client:
 * <proto>_read_event and <proto>_event_dispatch run the handlers; a
 * blocking call's reply is taken past an event queued before it. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/idltest.h>
#include <os.h>
#include "utest.h"

#define CALL_WAIT (5 * NS_PER_S)

struct ev_server {
    uint32_t notes, gifts;
    uint32_t key;          /* the last event's */
    char     text[17];     /* the last note's, NUL-terminated */
    handle_t obj;          /* the last give's handle, kept */
    handle_t sv;           /* the server's channel, for an event from inside count */
};

static void s_note(void *ctx, uint32_t key, const char *text, uint32_t text_len)
{
    struct ev_server *s = ctx;
    s->notes++;
    s->key = key;
    memcpy(s->text, text, text_len);
    s->text[text_len] = 0;
}

static void s_give(void *ctx, uint32_t key, handle_t obj)
{
    struct ev_server *s = ctx;
    s->gifts++;
    s->key = key;
    if (s->obj != HANDLE_INVALID)
        jam_handle_close(s->obj);
    s->obj = obj;
}

static status_t s_count(void *ctx, uint32_t *out_notes, uint32_t *out_gifts)
{
    struct ev_server *s = ctx;
    *out_notes = s->notes;
    *out_gifts = s->gifts;
    /* An event to the client queued before this call's reply. */
    if (s->sv != HANDLE_INVALID)
        (void)idltest_ev_ring(s->sv, 99, (const uint8_t *)"ab", 2);
    return OK;
}

static const struct idltest_ev_ops ops = { .count = s_count, .note = s_note, .give = s_give };

/* Is h a VMO of `size` bytes (ours)? */
static bool vmo_of(handle_t h, uint64_t size)
{
    uint64_t got = 0;
    CHECK_ST(jam_vmo_get_size(h, &got), OK);
    CHECK(got >= size);
    return true;
}

/* Has the peer of our end `peer` been closed (the other end's handle)? */
static bool closed(handle_t peer)
{
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(peer, SIG_PEER_CLOSED, now() + CALL_WAIT, &seen), OK);
    CHECK_ST(jam_handle_close(peer), OK);
    return true;
}

/* Nothing was answered on c. */
static bool no_reply(handle_t c)
{
    _Alignas(8) uint8_t rep[IDLTEST_EV_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_SHOULD_WAIT);
    return true;
}

/* A raw message on c (n bytes at q, txid as set there) with nh new
 * channel ends, whose peers go to peers[]. */
static bool raw(handle_t c, const void *q, uint32_t n, unsigned nh, handle_t *peers)
{
    handle_t hs[3];
    for (unsigned i = 0; i < nh; i++)
        CHECK_ST(jam_channel_create(&hs[i], &peers[i]), OK);
    CHECK_ST(jam_channel_write(c, q, n, hs, nh), OK);
    return true;
}

bool t_idl_events_to_server(void)
{
    handle_t c, sv, peers[3];
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct ev_server s = { .obj = HANDLE_INVALID, .sv = HANDLE_INVALID };

    /* Events: run, nothing answered. */
    CHECK_ST(idltest_ev_note(c, 1, "hello", 5), OK);
    handle_t vmo;
    CHECK_ST(jam_vmo_create(8192, 0, &vmo), OK);
    CHECK_ST(idltest_ev_give(c, 2, vmo), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    CHECK(s.notes == 1 && s.key == 1 && strcmp(s.text, "hello") == 0);
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    CHECK(s.gifts == 1 && s.key == 2);
    if (!vmo_of(s.obj, 8192) || !no_reply(c))
        return false;
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), ERR_SHOULD_WAIT);

    /* The sender refuses a bad field, its handle closed all the same. */
    CHECK_ST(idltest_ev_note(c, 3, "seventeen bytes!!", 17), ERR_INVALID_ARGS);
    CHECK_ST(idltest_ev_note(c, 3, "\xc0\x80", 2), ERR_INVALID_ARGS);
    handle_t x, y;
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_ev_give(HANDLE_INVALID, 4, x), ERR_BAD_HANDLE);   /* the write fails */
    if (!closed(y))
        return false;
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), ERR_SHOULD_WAIT);

    /* Dropped, handles closed, nothing answered, no handler run: a give
     * with no handle, with two, a note with a handle, a note that isn't a
     * str, a give one byte short, an event of the other way (ring), an
     * unknown ordinal, a call (count) with txid 0. */
    struct idltest_ev_give_msg g = { 0, IDLTEST_EV_GIVE, 5 };
    if (!raw(c, &g, sizeof(g), 0, peers) || !raw(c, &g, sizeof(g), 2, peers))
        return false;
    for (unsigned i = 0; i < 2; i++)
        CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    if (!closed(peers[0]) || !closed(peers[1]))
        return false;
    struct { struct idltest_ev_note_msg h; uint16_t n; char t[2]; } __attribute__((packed))
        nt = { { 0, IDLTEST_EV_NOTE, 6 }, 2, { 'o', 'k' } };
    if (!raw(c, &nt, sizeof(nt), 1, peers))
        return false;
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    if (!closed(peers[0]))
        return false;
    nt.t[1] = (char)0xff;
    if (!raw(c, &nt, sizeof(nt), 0, peers) || !raw(c, &g, sizeof(g) - 1, 1, peers))
        return false;
    struct idltest_ev_ring_msg r = { 0, IDLTEST_EV_RING, 7 };
    struct idl_req_hdr unknown = { 0, (IDLTEST_EV_PROTOCOL_ID << 16) | 77 };
    struct idltest_ev_count_req cnt = { 0, IDLTEST_EV_COUNT };
    CHECK_ST(jam_channel_write(c, &r, sizeof(r), NULL, 0), OK);
    CHECK_ST(jam_channel_write(c, &unknown, sizeof(unknown), NULL, 0), OK);
    CHECK_ST(jam_channel_write(c, &cnt, sizeof(cnt), NULL, 0), OK);
    for (unsigned i = 0; i < 5; i++)
        CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    if (!closed(peers[0]) || !no_reply(c))
        return false;
    CHECK(s.notes == 1 && s.gifts == 1);

    /* An event with a txid: refused (its sender may be waiting), its
     * handles closed, no handler run. A call with a handle: refused too. */
    g.txid = 21;
    if (!raw(c, &g, sizeof(g), 1, peers))
        return false;
    cnt.txid = 22;
    if (!raw(c, &cnt, sizeof(cnt), 1, peers + 1))
        return false;
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    if (!closed(peers[0]) || !closed(peers[1]))
        return false;
    _Alignas(8) uint8_t rep[IDLTEST_EV_REP_MAX];
    struct idl_msg m;
    for (uint32_t t = 21; t <= 22; t++) {
        CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
        CHECK(m.txid == t && m.n == sizeof(struct idl_rep_hdr));
        CHECK_ST(((struct idl_rep_hdr *)rep)->status, ERR_INVALID_ARGS);
    }
    CHECK(s.gifts == 1);

    /* No handler: dropped, its handle closed. */
    static const struct idltest_ev_ops none = { 0 };
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_ev_give(c, 8, x), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &none, &s), OK);
    if (!closed(y) || !no_reply(c))
        return false;

    /* Through slots: run_slot gives the handle to the handler. */
    struct {
        _Alignas(8) uint8_t q[IDLTEST_EV_REQ_MAX];
        _Alignas(8) uint8_t r[IDLTEST_EV_REP_MAX];
        uint32_t n, nh;
        handle_t hs[2];
    } rm;
    struct idl_slot slot = { .q = rm.q, .qcap = sizeof(rm.q), .n = &rm.n, .nh = &rm.nh,
                             .hs = rm.hs, .hcap = 2, .r = rm.r };
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_ev_give(c, 9, x), OK);
    CHECK_ST(idltest_ev_take_slot(sv, &slot, NULL), OK);
    CHECK_EQ(rm.nh, 1);
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    CHECK_EQ(idltest_ev_run_slot(sv, &slot, &ops, &s, rhs, &rhn), 0);
    CHECK(rm.nh == 0 && s.gifts == 2 && s.key == 9 && s.obj != vmo);
    CHECK_ST(jam_handle_close(s.obj), OK);   /* the handler's: y's peer */
    if (!closed(y))
        return false;
    s.obj = HANDLE_INVALID;

    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- to the client --------------------------------------------------------- */

struct ev_client {
    unsigned rings, gifts;
    uint32_t key;
    uint8_t  data[8];
    uint32_t len;
    handle_t a, b;
};

static void c_ring(void *ctx, uint32_t key, const uint8_t *data, uint32_t data_len)
{
    struct ev_client *k = ctx;
    k->rings++;
    k->key = key;
    memcpy(k->data, data, data_len);
    k->len = data_len;
}

static void c_gift(void *ctx, uint32_t key, handle_t a, handle_t b)
{
    struct ev_client *k = ctx;
    k->gifts++;
    k->key = key;
    k->a = a;
    k->b = b;
}

static const struct idltest_ev_events events = { .ring = c_ring, .gift = c_gift };

struct ev_thread {
    handle_t         sv;
    struct ev_server s;
    status_t         st;
};

static void ev_serve(void *arg)
{
    struct ev_thread *t = arg;
    t->st = idltest_ev_serve(t->sv, &ops, &t->s);
}

bool t_idl_events_to_client(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct ev_client k = { 0 };

    /* Events from the server, read by the client. */
    CHECK_ST(idltest_ev_ring(sv, 5, (const uint8_t *)"\1\2\3", 3), OK);
    handle_t x, y, vmo;
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(jam_vmo_create(4096, 0, &vmo), OK);
    CHECK_ST(idltest_ev_gift(sv, 6, vmo, x), OK);
    CHECK_ST(idltest_ev_read_event(c, &events, &k), OK);
    CHECK(k.rings == 1 && k.key == 5 && k.len == 3 && k.data[2] == 3);
    CHECK_ST(idltest_ev_read_event(c, &events, &k), OK);
    CHECK(k.gifts == 1 && k.key == 6);
    if (!vmo_of(k.a, 4096))
        return false;
    CHECK_ST(jam_handle_close(k.a), OK);
    CHECK_ST(jam_handle_close(k.b), OK);
    if (!closed(y))
        return false;
    CHECK_ST(idltest_ev_read_event(c, &events, &k), ERR_SHOULD_WAIT);

    /* Refused at the client, handles closed: no handler, a count past N, a
     * handle too few, a reply that is no event. */
    static const struct idltest_ev_events none = { 0 };
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(jam_vmo_create(4096, 0, &vmo), OK);
    CHECK_ST(idltest_ev_gift(sv, 7, vmo, x), OK);
    CHECK_ST(idltest_ev_read_event(c, &none, &k), ERR_NOT_SUPPORTED);
    if (!closed(y))
        return false;
    CHECK_ST(idltest_ev_ring(sv, 8, (const uint8_t *)"123456789", 9), ERR_INVALID_ARGS);
    struct { struct idltest_ev_ring_msg h; uint16_t n; uint8_t d[8]; } __attribute__((packed))
        rg = { { 0, IDLTEST_EV_RING, 8 }, 9, { 0 } };   /* a count of 9 */
    CHECK_ST(jam_channel_write(sv, &rg, sizeof(rg), NULL, 0), OK);
    CHECK_ST(idltest_ev_read_event(c, &events, &k), ERR_INVALID_ARGS);
    struct idltest_ev_gift_msg gm = { 0, IDLTEST_EV_GIFT, 9 };
    handle_t peer;
    CHECK_ST(jam_channel_create(&x, &peer), OK);
    CHECK_ST(jam_channel_write(sv, &gm, sizeof(gm), &x, 1), OK);
    CHECK_ST(idltest_ev_read_event(c, &events, &k), ERR_INVALID_ARGS);
    if (!closed(peer))
        return false;
    struct idl_rep_hdr stray = { 55, OK };
    CHECK_ST(jam_channel_write(sv, &stray, sizeof(stray), NULL, 0), OK);
    CHECK_ST(idltest_ev_read_event(c, &events, &k), ERR_INTERNAL);
    CHECK(k.rings == 1 && k.gifts == 1);

    /* Replies and events on one channel: an asynchronous client tells them
     * apart by txid (0: an event). */
    struct ev_server s = { .obj = HANDLE_INVALID, .sv = sv };
    uint32_t last = 0, t = idl_txid_next(&last), notes = 9, gifts = 9;
    CHECK_ST(idltest_ev_note(c, 1, "a", 1), OK);
    CHECK_ST(idltest_ev_count_send(c, t), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);
    CHECK_ST(idltest_ev_serve_one(sv, &ops, &s), OK);   /* count: a ring, then the reply */
    _Alignas(8) uint8_t rep[IDLTEST_EV_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, 0);
    CHECK_ST(idltest_ev_event_dispatch(&events, &k, rep, &m), OK);
    CHECK(k.rings == 2 && k.key == 99 && k.len == 2);
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t);
    CHECK_ST(idltest_ev_count_result(rep, &m, &notes, &gifts), OK);
    CHECK(notes == 1 && gifts == 0);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);

    /* A blocking call takes its reply past the event queued before it;
     * the event is read after. Events with a handle through
     * idltest_ev_serve (its loop reads handles). */
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct ev_thread srv;
    handle_t th;
    srv = (struct ev_thread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    srv.s = (struct ev_server){ .obj = HANDLE_INVALID, .sv = srv.sv };
    CHECK_ST(thread_spawn("idl-ev", ev_serve, &srv, stack, sizeof(stack), &th), OK);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(idltest_ev_give(c, 10, x), OK);
    CHECK_ST(idltest_ev_note(c, 11, "b", 1), OK);
    CHECK_ST(idltest_ev_count_within(c, CALL_WAIT, &notes, &gifts), OK);
    CHECK(notes == 1 && gifts == 1);
    k.rings = 0;
    CHECK_ST(idltest_ev_read_event(c, &events, &k), OK);   /* count's ring */
    CHECK(k.rings == 1 && k.key == 99);
    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK(srv.s.obj != HANDLE_INVALID);
    CHECK_ST(jam_handle_close(srv.s.obj), OK);   /* the server's handler kept x */
    if (!closed(y))
        return false;
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}
