/* utest: several protocols in one .idl file (tools/genidl.py): idltest and
 * idltest_two (abi/idl/idltest.idl), one for each kind of channel. Each
 * has ordinals of its own, so a server of one refuses a request of the
 * other (ERR_NOT_SUPPORTED) instead of running one of its own methods. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/idltest.h>
#include <os.h>
#include "utest.h"

static status_t s_echo(void *ctx, uint32_t value, uint32_t *out_value)
{
    (*(unsigned *)ctx)++;
    *out_value = value + 1;
    return OK;
}

static const struct idltest_ops one = { .echo = s_echo };
static const struct idltest_two_ops two = { .echo = s_echo };

/* The next reply on c answers txid with status want (and value + 1). */
static bool echoed(handle_t c, uint32_t txid, bool second, status_t want, uint32_t value)
{
    _Alignas(8) uint8_t rep[IDLTEST_REP_MAX > IDLTEST_TWO_REP_MAX ? IDLTEST_REP_MAX
                                                                  : IDLTEST_TWO_REP_MAX];
    struct idl_msg m;
    uint32_t v = 0;
    CHECK_ST(idl_reply_read(c, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, txid);
    if (second)
        CHECK_ST(idltest_two_echo_result(rep, &m, &v), want);
    else
        CHECK_ST(idltest_echo_result(rep, &m, &v), want);
    if (want == OK)
        CHECK_EQ(v, value + 1);
    return true;
}

bool t_idl_wrong_protocol(void)
{
    handle_t c, sv;
    CHECK_ST(jam_channel_create(&c, &sv), OK);
    unsigned ran = 0;
    uint32_t last = 0;
    CHECK(IDLTEST_TWO_ECHO != IDLTEST_ECHO);
    CHECK_EQ(IDLTEST_TWO_ECHO >> 16, IDLTEST_TWO_PROTOCOL_ID);

    /* Each protocol's own request on its own server. */
    uint32_t t = idl_txid_next(&last);
    CHECK_ST(idltest_echo_send(c, t, 4), OK);
    CHECK_ST(idltest_serve_one(sv, &one, &ran), OK);
    if (!echoed(c, t, false, OK, 4))
        return false;
    t = idl_txid_next(&last);
    CHECK_ST(idltest_two_echo_send(c, t, 6), OK);
    CHECK_ST(idltest_two_serve_one(sv, &two, &ran), OK);
    if (!echoed(c, t, true, OK, 6))
        return false;
    CHECK_EQ(ran, 2);

    /* The other protocol's request, same method name and ordinal: refused
     * before any handler runs. */
    t = idl_txid_next(&last);
    CHECK_ST(idltest_two_echo_send(c, t, 8), OK);
    CHECK_ST(idltest_serve_one(sv, &one, &ran), OK);
    if (!echoed(c, t, true, ERR_NOT_SUPPORTED, 0))
        return false;
    t = idl_txid_next(&last);
    CHECK_ST(idltest_echo_send(c, t, 8), OK);
    CHECK_ST(idltest_two_serve_one(sv, &two, &ran), OK);
    if (!echoed(c, t, false, ERR_NOT_SUPPORTED, 0))
        return false;
    CHECK_EQ(ran, 2);

    CHECK_ST(jam_handle_close(c), OK);
    CHECK_ST(jam_handle_close(sv), OK);
    return true;
}
