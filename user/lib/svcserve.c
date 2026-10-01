/* svc_serve_request (<os.h> "services"): one request on a service's channel,
 * where the svc protocol's connect (abi/idl/svc.idl) is answered next to
 * the service's own protocol. The two are told apart by the protocol id
 * in the request's ordinal, as every generated dispatch does; a request
 * of neither gets the service's dispatch, which refuses it. */
#include <idl/svc.h>
#include <os.h>

#define MSG_MAX 8192   /* an IDL message's most bytes (tools/genidl.py) */

/* svc_dispatch's ops: connect, with the caller's function and context. */
struct conn {
    status_t (*connect)(void *ctx, handle_t *out);
    void     *ctx;
};

static status_t on_connect(void *ctx, handle_t *out)
{
    const struct conn *c = ctx;
    return c->connect ? c->connect(c->ctx, out) : ERR_NOT_SUPPORTED;
}

static const struct svc_ops svc_ops = { .connect = on_connect };

status_t svc_serve_request(handle_t ch, svc_dispatch_fn dispatch,
                           status_t (*connect)(void *, handle_t *), void *ctx)
{
    _Alignas(8) uint8_t q[MSG_MAX], r[MSG_MAX];   /* 16 KiB of the stack */
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ch, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *h = (const struct idl_req_hdr *)q;
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0, rn;
    if (n >= sizeof(*h) && h->ordinal >> 16 == SVC_PROTOCOL_ID) {
        struct conn c = { connect, ctx };
        rn = svc_dispatch(&svc_ops, &c, q, n, r, rhs, &rhn);
    } else {
        rn = dispatch(ctx, q, n, r, rhs, &rhn);
    }
    if (!rn || drv_channel_write(ch, r, rn, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* not sent: still ours */
    return OK;
}
