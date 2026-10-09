/* utest: arrays of varying length in the IDL (tools/genidl.py's u8[<=N] and
 * str[<=N]), on the test protocol idltest (abi/idl/idltest.idl): a u16
 * count and the elements on the wire, after the message's fixed part.
 *
 * Calls carry them both ways; the generated client refuses an array past
 * its N and a str that isn't UTF-8 or holds a NUL before sending, and the
 * generated server refuses the same in a request (and a count past the
 * message's end, bytes left over) before any handler sees it; a handler
 * that breaks its results' rules, or a reply_<method> given bad ones,
 * answers ERR_INTERNAL; a client refuses a reply that breaks them. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/idltest.h>
#include <os.h>
#include "utest.h"

#define CALL_WAIT (5 * NS_PER_S)

struct arr_server {
    unsigned       ran;    /* handlers run */
    struct idl_txn kept;   /* later_text's request */
    bool           have;
};

static status_t s_bytes(void *ctx, uint32_t tag, const uint8_t *data, uint32_t data_len,
                        uint8_t *out_data, uint32_t *out_data_len, uint32_t *out_tag)
{
    struct arr_server *s = ctx;
    s->ran++;
    for (uint32_t i = 0; i < data_len; i++)
        out_data[i] = data[data_len - 1 - i];
    *out_data_len = data_len;
    *out_tag = tag + 1;
    return OK;
}

static status_t s_greet(void *ctx, const char *name, uint32_t name_len, const char *word,
                        uint32_t word_len, char *out_text, uint32_t *out_text_len)
{
    struct arr_server *s = ctx;
    s->ran++;
    uint32_t n = 0;
    for (uint32_t i = 0; i < word_len; i++)
        out_text[n++] = word[i];
    out_text[n++] = ',';
    out_text[n++] = ' ';
    for (uint32_t i = 0; i < name_len; i++)
        out_text[n++] = name[i];
    *out_text_len = n;
    return OK;
}

static status_t s_later_text(void *ctx, struct idl_txn txn, uint32_t key, char *out_name,
                             uint32_t *out_name_len, uint8_t *out_extra, uint32_t *out_extra_len)
{
    struct arr_server *s = ctx;
    (void)key, (void)out_name, (void)out_name_len, (void)out_extra, (void)out_extra_len;
    s->ran++;
    s->kept = txn;
    s->have = true;
    return IDL_LATER;
}

static status_t s_broken(void *ctx, uint32_t how, char *out_text, uint32_t *out_text_len,
                         uint8_t *out_data, uint32_t *out_data_len)
{
    struct arr_server *s = ctx;
    s->ran++;
    memcpy(out_text, "ok", 2);
    *out_text_len = 2;
    *out_data_len = 4;
    memset(out_data, 7, 4);
    if (how == 1)
        *out_text_len = 9;           /* past str[<=8] */
    else if (how == 2)
        out_text[0] = (char)0xc0;    /* an overlong form */
    else if (how == 3)
        *out_data_len = 5;           /* past u8[<=4] */
    else if (how == 4)
        out_text[1] = 0;             /* a NUL */
    return OK;
}

static const struct idltest_ops ops = {
    .bytes = s_bytes, .greet = s_greet, .later_text = s_later_text, .broken = s_broken,
};

struct arr_thread {
    handle_t          sv;
    struct arr_server s;
    status_t          st;
};

static void arr_serve(void *arg)
{
    struct arr_thread *t = arg;
    t->st = idltest_serve(t->sv, &ops, &t->s);
}

/* Write the request q (n bytes, txid set here) on c, serve it on sv, and
 * read its reply's status (*st). */
static bool raw(handle_t c, handle_t sv, struct arr_server *s, void *q, uint32_t n, status_t *st)
{
    static uint32_t last;
    uint32_t txid = idl_txid_next(&last);
    memcpy(q, &txid, 4);
    CHECK_ST(jam_channel_write(c, q, n, NULL, 0), OK);
    CHECK_ST(idltest_serve_one(sv, &ops, s), OK);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, txid);
    CHECK(m.n >= sizeof(struct idl_rep_hdr));
    *st = ((struct idl_rep_hdr *)rep)->status;
    return true;
}

/* A greet request by hand: name (nlen bytes, count `count`) then word. */
static uint32_t greet_req(uint8_t *q, const char *name, uint32_t nlen, uint16_t count,
                          const char *word, uint16_t wlen)
{
    struct idltest_greet_req h = { 0, IDLTEST_GREET };
    uint32_t o = sizeof(h);
    memcpy(q, &h, o);
    memcpy(q + o, &count, 2);
    memcpy(q + o + 2, name, nlen);
    o += 2 + nlen;
    memcpy(q + o, &wlen, 2);
    memcpy(q + o + 2, word, wlen);
    return o + 2 + wlen;
}

bool t_idl_var_arrays(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    static struct arr_thread srv;
    handle_t c, th;
    srv = (struct arr_thread){ .st = ERR_INTERNAL };
    CHECK_ST(jam_channel_create(&c, &srv.sv), OK);
    CHECK_ST(thread_spawn("idl-arr", arr_serve, &srv, stack, sizeof(stack), &th), OK);

    /* u8[<=64]: empty, one, full; reversed back. */
    uint8_t in[65], out[64];
    for (unsigned i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)(i + 1);
    uint32_t len = 99, tag = 0;
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 5, in, 0, out, &len, &tag), OK);
    CHECK(len == 0 && tag == 6);
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 1, NULL, 0, out, &len, &tag), OK);
    CHECK_EQ(len, 0);
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 7, in, 1, out, &len, &tag), OK);
    CHECK(len == 1 && out[0] == 1 && tag == 8);
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 7, in, 64, out, &len, &tag), OK);
    CHECK_EQ(len, 64);
    for (unsigned i = 0; i < 64; i++)
        CHECK_EQ(out[i], in[63 - i]);
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 7, in, 64, NULL, NULL, NULL), OK);

    /* str[<=N]: the result NUL-terminated, multi-byte UTF-8 kept. */
    char text[33];
    CHECK_ST(idltest_greet_within(c, CALL_WAIT, "Ana", 3, "hi", 2, text, &len), OK);
    CHECK(len == 7 && strcmp(text, "hi, Ana") == 0);
    const char *jose = "Jos\xc3\xa9 \xf0\x9f\x8e\xb5";   /* "José" and a U+1F3B5 */
    CHECK_ST(idltest_greet_within(c, CALL_WAIT, jose, (uint32_t)strlen(jose), "", 0, text, &len),
             OK);
    CHECK(len == strlen(jose) + 2 && strcmp(text + 2, jose) == 0);
    CHECK_ST(idltest_greet_within(c, CALL_WAIT, "", 0, "", 0, text, &len), OK);
    CHECK(len == 2 && strcmp(text, ", ") == 0);
    unsigned ran = srv.s.ran;

    /* The client refuses before sending: past N, not UTF-8, a NUL. */
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 7, in, 65, out, &len, &tag), ERR_INVALID_ARGS);
    CHECK_ST(idltest_bytes_within(c, CALL_WAIT, 7, NULL, 3, out, &len, &tag), ERR_INVALID_ARGS);
    CHECK_ST(idltest_greet_within(c, CALL_WAIT, "seventeen bytes!!", 17, "w", 1, text, &len),
             ERR_INVALID_ARGS);
    CHECK_ST(idltest_greet_within(c, CALL_WAIT, "a\xc0\x80", 3, "w", 1, text, &len),
             ERR_INVALID_ARGS);
    CHECK_ST(idltest_greet_within(c, CALL_WAIT, "a\0b", 3, "w", 1, text, &len), ERR_INVALID_ARGS);
    CHECK_EQ(srv.s.ran, ran);

    /* A handler breaking its results' rules: ERR_INTERNAL. */
    char t8[9];
    uint8_t d4[4];
    uint32_t dlen = 0;
    CHECK_ST(idltest_broken_within(c, CALL_WAIT, 0, t8, &len, d4, &dlen), OK);
    CHECK(len == 2 && strcmp(t8, "ok") == 0 && dlen == 4 && d4[3] == 7);
    for (uint32_t how = 1; how <= 4; how++)
        CHECK_ST(idltest_broken_within(c, CALL_WAIT, how, t8, &len, d4, &dlen), ERR_INTERNAL);

    CHECK_ST(jam_handle_close(c), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(srv.st, OK);
    CHECK_ST(jam_handle_close(srv.sv), OK);
    return true;
}

bool t_idl_var_refused(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct arr_server s = { 0 };
    _Alignas(8) uint8_t q[128];
    status_t st;

    /* By hand, what a good client sends: answered. */
    uint32_t n = greet_req(q, "Ana", 3, 3, "hi", 2);
    if (!raw(c, sv, &s, q, n, &st))
        return false;
    CHECK_ST(st, OK);
    CHECK_EQ(s.ran, 1);

    /* Refused before the handler runs: a count past N (17 of str[<=16]),
     * past the message's end, a byte left over, a count cut short, a NUL,
     * bad UTF-8 (an overlong form, a surrogate, a byte past F4, a sequence
     * cut short). */
    static const char seventeen[] = "abcdefghijklmnopq";
    n = greet_req(q, seventeen, 17, 17, "hi", 2);
    if (!raw(c, sv, &s, q, n, &st))
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    n = greet_req(q, "Ana", 3, 3, "hi", 2);
    if (!raw(c, sv, &s, q, n - 1, &st))   /* the word's last byte missing */
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    q[n] = 'x';
    if (!raw(c, sv, &s, q, n + 1, &st))   /* a byte after the last array */
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    if (!raw(c, sv, &s, q, sizeof(struct idltest_greet_req) + 1, &st))   /* half a count */
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    if (!raw(c, sv, &s, q, sizeof(struct idltest_greet_req), &st))       /* no count */
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    static const char *bad[] = { "A\0a", "A\xc0\x80", "A\xed\xa0\x80", "A\xf5\x80", "A\xe2\x82" };
    static const uint32_t bad_len[] = { 3, 3, 4, 3, 3 };
    for (unsigned i = 0; i < 5; i++) {
        n = greet_req(q, bad[i], bad_len[i], (uint16_t)bad_len[i], "hi", 2);
        if (!raw(c, sv, &s, q, n, &st))
            return false;
        CHECK_ST(st, ERR_INVALID_ARGS);
        n = greet_req(q, "ok", 2, 2, bad[i], (uint16_t)bad_len[i]);
        if (!raw(c, sv, &s, q, n, &st))
            return false;
        CHECK_ST(st, ERR_INVALID_ARGS);
    }
    CHECK_EQ(s.ran, 1);

    /* Answered later through reply_<method>: varying results; bad ones
     * become ERR_INTERNAL. */
    uint32_t last = 100, t = idl_txid_next(&last);
    CHECK_ST(idltest_later_text_send(c, t, 1), OK);
    CHECK_ST(idltest_serve_one(sv, &ops, &s), OK);
    CHECK(s.have);
    static const uint8_t extra[4] = { 9, 8, 7, 6 };
    CHECK_ST(idltest_reply_later_text(s.kept, OK, "Zo\xc3\xab", 4, extra, 2), OK);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    char name[17];
    uint8_t got[4];
    uint32_t nlen = 0, elen = 0;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t);
    CHECK_ST(idltest_later_text_result(rep, &m, name, &nlen, got, &elen), OK);
    CHECK(nlen == 4 && strcmp(name, "Zo\xc3\xab") == 0 && elen == 2 && got[1] == 8);
    struct idl_txn txn = s.kept;
    CHECK_ST(idltest_reply_later_text(txn, OK, "seventeen bytes!!", 17, extra, 0), OK);
    CHECK_ST(idltest_reply_later_text(txn, OK, "ok", 2, extra, 5), OK);
    CHECK_ST(idltest_reply_later_text(txn, OK, "\xff", 1, extra, 0), OK);
    for (unsigned i = 0; i < 3; i++) {
        CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
        CHECK_EQ(m.n, sizeof(struct idl_rep_hdr));
        CHECK_ST(idltest_later_text_result(rep, &m, name, &nlen, got, &elen), ERR_INTERNAL);
    }

    /* A client refuses a reply breaking the rules (a forged one): a str
     * that isn't UTF-8, a count past N, bytes left over. */
    struct { uint32_t txid; int32_t status; uint16_t c1; char s[3]; uint16_t c2; uint8_t b[1]; }
        __attribute__((packed)) f = { 5, OK, 3, { 'a', (char)0xc1, 'b' }, 1, { 1 } };
    m = (struct idl_msg){ .txid = 5, .n = sizeof(f) };
    CHECK_ST(idltest_later_text_result(&f, &m, name, &nlen, got, &elen), ERR_INTERNAL);
    f.s[1] = 'x';
    CHECK_ST(idltest_later_text_result(&f, &m, name, &nlen, got, &elen), OK);
    f.c2 = 5;
    CHECK_ST(idltest_later_text_result(&f, &m, name, &nlen, got, &elen), ERR_INTERNAL);
    f.c2 = 1;
    m.n = sizeof(f) - 1;
    CHECK_ST(idltest_later_text_result(&f, &m, name, &nlen, got, &elen), ERR_INTERNAL);
    m.n = sizeof(f);
    f.c2 = 0;   /* the array's byte is now left over */
    CHECK_ST(idltest_later_text_result(&f, &m, name, &nlen, got, &elen), ERR_INTERNAL);

    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), ERR_SHOULD_WAIT);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}

/* ---- arrays of wider scalars (u16[<=N] ... i64[<=N]) ------------------------- */

static status_t s_sums(void *ctx, uint8_t tag, const uint16_t *a, uint32_t a_len,
                       const uint32_t *b, uint32_t b_len, const uint64_t *c, uint32_t c_len,
                       uint64_t *out_total, uint8_t *out_tag, uint32_t *out_b,
                       uint32_t *out_b_len, int64_t *out_c, uint32_t *out_c_len)
{
    struct arr_server *s = ctx;
    s->ran++;
    if ((uintptr_t)a % 2 || (uintptr_t)b % 4 || (uintptr_t)c % 8 || (uintptr_t)out_b % 4 ||
        (uintptr_t)out_c % 8)
        return ERR_BAD_STATE;
    uint64_t t = 0;
    for (uint32_t i = 0; i < a_len; i++)
        t += a[i];
    for (uint32_t i = 0; i < b_len; i++) {
        t += b[i];
        out_b[i] = b[i];
    }
    for (uint32_t i = 0; i < c_len; i++) {
        t += c[i];
        out_c[i] = -(int64_t)c[i];
    }
    *out_total = t;
    *out_tag = tag;
    *out_b_len = b_len;
    *out_c_len = c_len;
    return OK;
}

static const struct idltest_ops sum_ops = { .sums = s_sums };

/* One sums call through _send, serve_one and _result. */
static bool sums(handle_t c, handle_t sv, struct arr_server *s, uint8_t tag, const uint16_t *a,
                 uint32_t na, const uint32_t *b, uint32_t nb, const uint64_t *cc, uint32_t nc,
                 status_t want)
{
    static uint32_t last;
    uint32_t t = idl_txid_next(&last), nb2 = 9, nc2 = 9, b2[4];
    int64_t c2[2];
    uint64_t total = 0, sum = 0;
    uint8_t tag2 = 0;
    CHECK_ST(idltest_sums_send(c, t, tag, a, na, b, nb, cc, nc), OK);
    CHECK_ST(idltest_serve_one(sv, &sum_ops, s), OK);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, t);
    CHECK_ST(idltest_sums_result(rep, &m, &total, &tag2, b2, &nb2, c2, &nc2), want);
    if (want != OK)
        return true;
    for (uint32_t i = 0; i < na; i++)
        sum += a[i];
    for (uint32_t i = 0; i < nb; i++)
        sum += b[i];
    for (uint32_t i = 0; i < nc; i++)
        sum += cc[i];
    CHECK(total == sum && tag2 == tag && nb2 == nb && nc2 == nc);
    for (uint32_t i = 0; i < nb; i++)
        CHECK_EQ(b2[i], b[i]);
    for (uint32_t i = 0; i < nc; i++)
        CHECK(c2[i] == -(int64_t)cc[i]);
    return true;
}

bool t_idl_scalar_arrays(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    struct arr_server s = { 0 };
    static const uint16_t a[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const uint32_t b[5] = { 100000, 200000, 300000, 400000, 500000 };
    static const uint64_t cc[3] = { 1ull << 40, 3, 5 };

    /* Every length from none to full, each array aligned to its element
     * (the handler checks; the results are moved into place after it). */
    for (uint32_t na = 0; na <= 8; na += 3)
        for (uint32_t nb = 0; nb <= 4; nb++)
            for (uint32_t nc = 0; nc <= 2; nc++)
                if (!sums(c, sv, &s, (uint8_t)(na + nb + nc), a, na, b, nb, cc, nc, OK))
                    return false;
    unsigned ran = s.ran;

    /* Past N: the client refuses. */
    uint32_t last = 50;
    CHECK_ST(idltest_sums_send(c, idl_txid_next(&last), 0, a, 9, b, 0, cc, 0), ERR_INVALID_ARGS);
    CHECK_ST(idltest_sums_send(c, idl_txid_next(&last), 0, a, 0, b, 5, cc, 0), ERR_INVALID_ARGS);
    CHECK_ST(idltest_sums_send(c, idl_txid_next(&last), 0, a, 0, b, 0, cc, 3), ERR_INVALID_ARGS);

    /* By hand: tag, a (1: count at 9, a pad byte at 11, the element at 12),
     * b (none: count at 14, pad 16), c (none: count at 16, pad to 24). */
    _Alignas(8) uint8_t q[32] = { 0 };
    struct idltest_sums_req h = { 0, IDLTEST_SUMS, 7 };
    memcpy(q, &h, sizeof(h));
    uint16_t one = 1, none = 0, elem = 42;
    memcpy(q + 9, &one, 2);
    memcpy(q + 12, &elem, 2);
    memcpy(q + 14, &none, 2);
    memcpy(q + 16, &none, 2);
    status_t st;
    if (!raw(c, sv, &s, q, 24, &st))
        return false;
    CHECK_ST(st, ERR_NOT_SUPPORTED);   /* well-formed: the server has no sums */
    struct arr_server s2 = { 0 };
    uint32_t txid = 77;
    memcpy(q, &txid, 4);
    CHECK_ST(jam_channel_write(c, q, 24, NULL, 0), OK);
    CHECK_ST(idltest_serve_one(sv, &sum_ops, &s2), OK);
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX];
    struct idl_msg m;
    uint64_t total = 0;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_ST(idltest_sums_result(rep, &m, &total, NULL, NULL, NULL, NULL, NULL), OK);
    CHECK_EQ(total, 42);
    /* A padding byte not 0, a count past N, the last padding cut short. */
    q[11] = 1;
    if (!raw(c, sv, &s2, q, 24, &st))
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    q[11] = 0;
    q[23] = 1;
    if (!raw(c, sv, &s2, q, 24, &st))
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    q[23] = 0;
    uint16_t nine = 9;
    memcpy(q + 9, &nine, 2);
    if (!raw(c, sv, &s2, q, 24, &st))
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    memcpy(q + 9, &one, 2);
    if (!raw(c, sv, &s2, q, 20, &st))
        return false;
    CHECK_ST(st, ERR_INVALID_ARGS);
    CHECK_EQ(s2.ran, 1);
    CHECK_EQ(s.ran, ran);

    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}
