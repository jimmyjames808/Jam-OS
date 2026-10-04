/* utest: requests read into slots (tools/genidl.py's <proto>_take_slot,
 * <proto>_run_slot and the `idempotent` keyword), on the test protocol
 * `idltest` (abi/idl/idltest.idl), in one thread.
 *
 * A slot is memory the server chooses (struct idl_slot): take reads one
 * request into it, its length written last, and deals with what no method
 * can take as <proto>_serve_one does (too big or too many handles:
 * answered ERR_INVALID_ARGS; no txid: dropped); run builds the reply in the
 * slot, and runs the same request again with the same answer (what a
 * successor does with the request in progress); a request that carried
 * handles is refused without its handler seeing it. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/block.h>
#include <idl/idltest.h>
#include <idl/null.h>
#include <os.h>
#include "utest.h"

#define SLOT_HANDLES 2

/* A slot's memory, as a service's state would hold it. */
struct room {
    _Alignas(8) uint8_t q[IDLTEST_REQ_MAX + 16];
    _Alignas(8) uint8_t r[IDLTEST_REP_MAX];
    uint32_t n, nh;
    handle_t hs[SLOT_HANDLES];
};

static struct idl_slot slot_in(struct room *rm, uint32_t qcap)
{
    return (struct idl_slot){
        .q = rm->q, .qcap = qcap, .n = &rm->n, .nh = &rm->nh, .hs = rm->hs,
        .hcap = SLOT_HANDLES, .r = rm->r,
    };
}

static status_t s_echo(void *ctx, uint32_t value, uint32_t *out_value)
{
    (*(unsigned *)ctx)++;
    *out_value = value + 1;
    return OK;
}

static const struct idltest_ops ops = { .echo = s_echo };   /* the rest: ERR_NOT_SUPPORTED */

/* The next reply on c: its txid and status (an echo's value into *value). */
static bool reply_is(handle_t c, uint32_t txid, status_t want, uint32_t *value)
{
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg msg;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &msg), OK);
    CHECK_EQ(msg.txid, txid);
    CHECK_ST(idltest_echo_result(rep, &msg, value), want);
    return true;
}

/* A raw message on c: n bytes of an echo request (txid, value) with nh
 * new channel ends; their peers into peers[]. */
static bool raw(handle_t c, uint32_t txid, uint32_t n, unsigned nh, handle_t *peers)
{
    static uint8_t msg[sizeof(struct room) + 64];
    struct idltest_echo_req q = { txid, IDLTEST_ECHO, 5 };
    handle_t ends[4];
    memset(msg, 0, sizeof(msg));
    memcpy(msg, &q, sizeof(q) < n ? sizeof(q) : n);
    for (unsigned i = 0; i < nh; i++)
        CHECK_ST(jam_channel_create(&ends[i], &peers[i]), OK);
    CHECK_ST(jam_channel_write(c, msg, n, ends, nh), OK);
    return true;
}

/* Every peer sees its end closed: what the server took was closed. */
static bool closed(const handle_t *peers, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        signals_t seen = 0;
        CHECK_ST(jam_object_wait_one(peers[i], SIG_PEER_CLOSED, now() + NS_PER_S, &seen), OK);
        CHECK_ST(jam_handle_close(peers[i]), OK);
    }
    return true;
}

/* An echo taken, run twice (the same answer both times: a successor's
 * re-run), then answered from the slot. */
static bool take_and_run(handle_t c, handle_t sv, struct room *rm, unsigned *calls)
{
    struct idl_slot slot = slot_in(rm, sizeof(rm->q));
    rm->n = 99;
    CHECK_ST(idltest_take_slot(sv, &slot), ERR_SHOULD_WAIT);
    CHECK_EQ(rm->n, 0);   /* zeroed before the read */
    CHECK_ST(idltest_echo_send(c, 21, 41), OK);
    CHECK_ST(idltest_take_slot(sv, &slot), OK);
    CHECK(rm->n == sizeof(struct idltest_echo_req) && rm->nh == 0);
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 9;
    uint32_t rn = idltest_run_slot(sv, &slot, &ops, calls, rhs, &rhn);
    CHECK(rn == sizeof(struct idltest_echo_rep) && rhn == 0 && *calls == 1);
    uint8_t first[sizeof(rm->r)];
    memcpy(first, rm->r, rn);
    CHECK_EQ(idltest_run_slot(sv, &slot, &ops, calls, rhs, &rhn), rn);
    CHECK(*calls == 2 && !memcmp(first, rm->r, rn));
    CHECK_ST(jam_channel_write(sv, rm->r, rn, NULL, 0), OK);
    uint32_t v = 0;
    CHECK(reply_is(c, 21, OK, &v));
    CHECK_EQ(v, 42);
    return true;
}

/* What no method takes: refused or dropped as the generated server does. */
static bool refusals(handle_t c, handle_t sv, struct room *rm, unsigned *calls)
{
    struct idl_slot slot = slot_in(rm, sizeof(rm->q));
    handle_t peers[3], rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0, v = 0;
    /* A request with a handle: taken (the handle the taker's to close),
     * refused by run without its handler. */
    CHECK(raw(c, 22, sizeof(struct idltest_echo_req), 1, peers));
    CHECK_ST(idltest_take_slot(sv, &slot), OK);
    CHECK(rm->n == sizeof(struct idltest_echo_req) && rm->nh == 1);
    uint32_t rn = idltest_run_slot(sv, &slot, &ops, calls, rhs, &rhn);
    CHECK(rn == sizeof(struct idl_rep_hdr) && *calls == 2);
    idl_close_all(rm->hs, rm->nh);
    CHECK(closed(peers, 1));
    CHECK_ST(jam_channel_write(sv, rm->r, rn, NULL, 0), OK);
    CHECK(reply_is(c, 22, ERR_INVALID_ARGS, &v));
    /* Too big for the slot, then too many handles: answered at the take. */
    CHECK(raw(c, 23, sizeof(rm->q) + 8, 0, peers));
    CHECK_ST(idltest_take_slot(sv, &slot), OK);
    CHECK(rm->n == 0 && rm->nh == 0);
    CHECK(reply_is(c, 23, ERR_INVALID_ARGS, &v));
    CHECK(raw(c, 24, sizeof(struct idltest_echo_req), SLOT_HANDLES + 1, peers));
    CHECK_ST(idltest_take_slot(sv, &slot), OK);
    CHECK(rm->n == 0 && rm->nh == 0);
    CHECK(reply_is(c, 24, ERR_INVALID_ARGS, &v));
    CHECK(closed(peers, SLOT_HANDLES + 1));
    /* No txid: dropped, its handle closed, nothing answered. */
    CHECK(raw(c, 25, 3, 1, peers));
    CHECK_ST(idltest_take_slot(sv, &slot), OK);
    CHECK(rm->n == 0 && rm->nh == 0);
    CHECK(closed(peers, 1));
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg msg;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &msg), ERR_SHOULD_WAIT);
    return true;
}

bool t_idl_slot_take_run(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    static struct room rm;
    unsigned calls = 0;
    if (!take_and_run(c, sv, &rm, &calls) || !refusals(c, sv, &rm, &calls))
        return false;
    /* A slot too small for the protocol: refused, the request left queued. */
    struct idl_slot small = slot_in(&rm, IDLTEST_REQ_MAX - 1), slot = slot_in(&rm, sizeof(rm.q));
    CHECK_ST(idltest_echo_send(c, 26, 7), OK);
    CHECK_ST(idltest_take_slot(sv, &small), ERR_INVALID_ARGS);
    CHECK_ST(idltest_take_slot(sv, &slot), OK);
    CHECK_EQ(rm.n, sizeof(struct idltest_echo_req));
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(idltest_take_slot(sv, &slot), ERR_PEER_CLOSED);
    CHECK_EQ(rm.n, 0);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* <proto>_idempotent: the methods the .idl marks, by wire ordinal. */
bool t_idl_slot_idempotent(void)
{
    CHECK(idltest_idempotent(IDLTEST_ECHO));
    CHECK(!idltest_idempotent(IDLTEST_WAIT) && !idltest_idempotent(IDLTEST_RELEASE));
    CHECK(block_idempotent(BLOCK_INFO) && block_idempotent(BLOCK_READ));
    CHECK(block_idempotent(BLOCK_WRITE) && block_idempotent(BLOCK_SYNC));
    CHECK(!block_idempotent(BLOCK_MAP_BUFFER));
    CHECK(!null_idempotent(NULL_PING));   /* nothing marked */
    CHECK(!block_idempotent(IDLTEST_ECHO));   /* another protocol's ordinal */
    CHECK(!block_idempotent(BLOCK_PROTOCOL_ID << 16));   /* no method 0 */
    return true;
}
