/* utest: libjwl's transport refusing malformed batches (<jwl.h>): each
 * written raw into a compositor connection's channel, refused with the
 * right wl_display.error, which is read back raw from the client's end
 * (the JWL1 header, then the error event), with every handle the batch
 * brought closed; and a client refusing a compositor that breaks the
 * transport (it sends nothing back). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <os.h>
#include "jwltest.h"
#include "utest.h"

static uint8_t raw[JWL_BATCH_MAX + 64];

/* A batch header at the start of w. */
static void header(struct jt_wr *w, uint32_t magic, uint32_t acked, uint32_t nfds, uint32_t res)
{
    w->n = 0;
    jt_w32(w, magic);
    jt_w32(w, acked);
    jt_w32(w, nfds);
    jt_w32(w, res);
}

static handle_t vmo(void)
{
    handle_t h = HANDLE_INVALID;
    (void)jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &h);   /* HANDLE_INVALID fails the write */
    return h;
}

/* The compositor reads raw[0..n) with nh new VMOs: after `good` messages
 * it must be dead with (object, code), having written exactly that error
 * to the client. */
static bool refused(size_t n, unsigned nh, unsigned good, uint32_t object, uint32_t code,
                    const char *what)
{
    handle_t a, b, hs[20];
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct jwl_conn *sv;
    struct jwl_conn_config cfg = { .ch = b, .side = JWL_SERVER, .display = &jt_display,
                                   .known = jt_known, .nknown = JT_NKNOWN };
    CHECK_ST(jwl_conn_create(&cfg, &sv), OK);
    for (unsigned i = 0; i < nh; i++)
        hs[i] = vmo();
    CHECK_ST(jam_channel_write(a, raw, (uint32_t)n, hs, nh), OK);
    struct jwl_msg m;
    status_t st;
    unsigned got = 0;
    while ((st = jwl_conn_next(sv, &m)) == OK) {
        jwl_msg_close_handles(&m);
        got++;
    }
    if (st != ERR_INVALID_ARGS || got != good)
        FAIL("%s: %s after %u messages", what, status_str(st), got);
    if (sv->error.object != object || sv->error.code != code)
        FAIL("%s: code %u on @%u (\"%s\")", what, sv->error.code, sv->error.object, sv->error.text);
    uint8_t back[512];
    uint32_t nb = 0, nhb = 0;
    struct channel_read_args r = { .h = a, .bytes_cap = sizeof(back),
                                   .bytes = (uint64_t)(uintptr_t)back,
                                   .actual_bytes = (uint64_t)(uintptr_t)&nb,
                                   .actual_handles = (uint64_t)(uintptr_t)&nhb };
    CHECK_ST(jam_channel_read(&r), OK);
    uint32_t w[8];
    memcpy(w, back, sizeof(w));
    CHECK(nb >= 40 && nhb == 0 && w[0] == JWL_MAGIC && w[1] == 0 && w[2] == 0 && w[3] == 0);
    CHECK(w[4] == JWL_DISPLAY_ID && (w[5] & 0xffff) == 0 && w[5] >> 16 == nb - 16);
    CHECK(w[6] == object && w[7] == code);
    jwl_conn_destroy(sv);
    jam_handle_close(a);
    return true;
}

#define REFUSED(n, nh, good, object, code, what)                     \
    do {                                                              \
        if (!refused(n, nh, good, object, code, what))                \
            return false;                                             \
    } while (0)

bool t_jwl_bad_batches(void)
{
    uint64_t h0, b0;
    struct job_info ji;
    CHECK_ST(jam_job_get_info(own_job(), &ji), OK);
    h0 = ji.used[JOB_LIMIT_HANDLES];
    b0 = ji.used[JOB_LIMIT_MSG_BYTES];
    struct jt_wr w = { .b = raw };
    const uint32_t M = JWL_DISPLAY_ID, BAD = JWL_ERROR_INVALID_METHOD;
    header(&w, 0x314c574b, 0, 0, 0);
    REFUSED(w.n, 0, 0, M, BAD, "a bad magic");
    header(&w, JWL_MAGIC, 0, 0, 1);
    REFUSED(w.n, 0, 0, M, BAD, "the reserved word set");
    header(&w, JWL_MAGIC, 0, 1, 0);
    REFUSED(w.n, 0, 0, M, BAD, "nfds 1, no handle");
    header(&w, JWL_MAGIC, 0, 0, 0);
    REFUSED(w.n, 1, 0, M, BAD, "nfds 0, a handle");
    REFUSED(8, 0, 0, M, BAD, "half a header");
    REFUSED(0, 0, 0, M, BAD, "an empty channel message");
    header(&w, JWL_MAGIC, 1, 0, 0);
    REFUSED(w.n, 0, 0, M, BAD, "acknowledging a batch never sent");
    header(&w, JWL_MAGIC, 0, 0, 0);
    memset(raw + w.n, 0, JWL_BATCH_MAX + 4 - w.n);
    REFUSED(JWL_BATCH_MAX + 4, 0, 0, M, BAD, "a batch over 16 KiB");
    header(&w, JWL_MAGIC, 0, 17, 0);
    REFUSED(w.n, 17, 0, M, BAD, "17 handles");
    /* a good message, then a handle nobody uses */
    header(&w, JWL_MAGIC, 0, 1, 0);
    jt_begin(&w, JWL_DISPLAY_ID, 1);
    jt_w32(&w, 2);
    jt_end(&w);
    REFUSED(w.n, 1, 1, M, BAD, "a handle no message takes");
    /* a good message, then a bad one: the bad one's own error */
    jt_begin(&w, 9, 0);
    jt_end(&w);
    w.b[8] = 0;   /* nfds 0 now */
    REFUSED(w.n, 0, 1, M, JWL_ERROR_INVALID_OBJECT, "a message to nothing");
    /* half a message at the end */
    header(&w, JWL_MAGIC, 0, 0, 0);
    jt_begin(&w, JWL_DISPLAY_ID, 1);
    jt_w32(&w, 2);
    jt_end(&w);
    REFUSED(w.n - 4, 0, 0, M, BAD, "a message cut off");
    CHECK_ST(jam_job_get_info(own_job(), &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_HANDLES], h0);   /* every refused batch's handles closed */
    CHECK_EQ(ji.used[JOB_LIMIT_MSG_BYTES], b0);
    return true;
}

/* A client and a compositor that breaks the transport: the client stops,
 * keeps the reason, and sends nothing. Empty batches are fine both ways. */
bool t_jwl_bad_compositor(void)
{
    struct jt_wr w = { .b = raw };
    struct { uint32_t acked, nfds, nh; const char *what; } cases[] = {
        { 1, 0, 0, "a compositor acknowledging" },
        { 0, 2, 1, "nfds 2, one handle" },
    };
    for (unsigned i = 0; i < 3; i++) {
        handle_t a, b;
        CHECK_ST(jam_channel_create(&a, &b), OK);
        struct jwl_conn *cl;
        struct jwl_conn_config cfg = { .ch = a, .side = JWL_CLIENT, .display = &jt_display };
        CHECK_ST(jwl_conn_create(&cfg, &cl), OK);
        header(&w, JWL_MAGIC, 0, 0, 0);
        CHECK_ST(jam_channel_write(b, raw, (uint32_t)w.n, NULL, 0), OK);   /* empty: fine */
        if (i < 2) {
            header(&w, JWL_MAGIC, cases[i].acked, cases[i].nfds, 0);
            handle_t h = cases[i].nh ? vmo() : HANDLE_INVALID;
            CHECK_ST(jam_channel_write(b, raw, (uint32_t)w.n, &h, cases[i].nh), OK);
        } else {
            jt_begin(&w, JWL_DISPLAY_ID, 1);   /* delete_id of an id never made */
            jt_w32(&w, 9);
            jt_end(&w);
            CHECK_ST(jam_channel_write(b, raw, (uint32_t)w.n, NULL, 0), OK);
        }
        struct jwl_msg m;
        CHECK_ST(jwl_conn_next(cl, &m), ERR_INVALID_ARGS);
        CHECK_EQ(cl->nread, i < 2 ? 1 : 2);   /* a refused header isn't counted read */
        CHECK_ST(jwl_conn_flush(cl), ERR_INVALID_ARGS);
        CHECK_EQ(cl->stats.batches_out, 0);
        uint32_t nb, nh;
        struct channel_read_args r = { .h = b, .bytes_cap = sizeof(raw),
                                       .bytes = (uint64_t)(uintptr_t)raw,
                                       .actual_bytes = (uint64_t)(uintptr_t)&nb,
                                       .actual_handles = (uint64_t)(uintptr_t)&nh };
        CHECK_ST(jam_channel_read(&r), ERR_SHOULD_WAIT);
        jwl_conn_destroy(cl);
        jam_handle_close(b);
    }
    /* the tables a connection is made with are checked */
    static const struct jwl_message bad_events[] = { { "error", "ou", NULL },
                                                     { "delete_id", "u", NULL } };
    struct jwl_interface bad = jt_display;
    bad.events = bad_events;
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct jwl_conn *c;
    struct jwl_conn_config cfg = { .ch = a, .side = JWL_SERVER, .display = &bad };
    CHECK_ST(jwl_conn_create(&cfg, &c), ERR_INVALID_ARGS);
    uint64_t size;
    CHECK_ST(jam_vmo_get_size(a, &size), ERR_BAD_HANDLE);   /* consumed */
    jam_handle_close(b);
    return true;
}
