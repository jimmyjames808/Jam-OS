/* System calls: channels (channel_reply_wait at the end). The rules all
 * sysc_* follow are in sysc.h.
 *
 * Message bytes are not copied here: the object layer copies them once,
 * straight from the caller's buffer into the message and straight from
 * the message into the reader's buffer (struct chan_bytes). The txid
 * channel_call stamps goes into the message, never into user memory.
 *
 * Received handles are inserted into the caller's table by the handle
 * layer before their values are copied out. If that copy fails, the
 * message is already consumed, so its handles are closed again; if the
 * bytes' copy fails, the object layer releases them. Either way a bad
 * buffer loses the message but never leaks a handle. */
#include <jam/channel.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include <jam/time.h>
#include "sysc.h"

static void close_all(struct handle_table *t, const handle_t *hs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        handle_close(t, hs[i]);
}

/* A word a call returns to the caller: the user address it goes to (0:
 * none wanted) and its value. */
struct out_word {
    uint64_t addr;
    uint32_t v;
};

/* Write n out words, in order. Words whose addresses follow each other
 * (a caller that keeps its sizes in one struct, in the order the call
 * names them) go in one copy: each copy costs its own trip into user
 * memory, whatever its length. ERR_INVALID_ARGS on a fault. */
static status_t put_words(const struct out_word *w, unsigned n)
{
    uint32_t run[3];
    unsigned i = 0;
    while (i < n) {
        if (!w[i].addr) {
            i++;
            continue;
        }
        uint64_t start = w[i].addr;
        unsigned k = 0;
        do
            run[k++] = w[i++].v;
        while (i < n && k < 3 && w[i].addr && w[i].addr == start + 4 * k);
        if (copy_to_user(start, run, 4 * k) != OK)
            return ERR_INVALID_ARGS;
    }
    return OK;
}

/* A message's sizes: actual bytes, then actual handles. */
static status_t put_sizes(uint64_t ub, uint32_t nb, uint64_t uh, uint32_t nh)
{
    const struct out_word w[2] = { { ub, nb }, { uh, nh } };
    return put_words(w, 2);
}

/* A buffer of n bytes or entries (n > 0) at user address 0. Refused with
 * ERR_INVALID_ARGS before anything happens: found only at the copy, it
 * would come after a message left its queue (lost, its handles closed),
 * or after channel_reply_wait's reply went out. */
static bool no_buffer(uint64_t addr, uint32_t n)
{
    return n && !addr;
}

int64_t sysc_channel_create(uint64_t a, uint64_t b)
{
    SYSC_TABLE(t);
    handle_t ha, hb;
    status_t st = sys_channel_create(t, &ha, &hb);
    if (st != OK)
        return st;
    if (copy_to_user(a, &ha, sizeof(ha)) != OK || copy_to_user(b, &hb, sizeof(hb)) != OK) {
        handle_close(t, ha);
        handle_close(t, hb);
        return ERR_INVALID_ARGS;
    }
    return OK;
}

int64_t sysc_channel_write(handle_t h, uint64_t bytes, uint32_t nbytes, uint64_t handles,
                           uint32_t nhandles)
{
    SYSC_TABLE(t);
    if (nbytes > CHANNEL_MAX_BYTES || nhandles > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    if (no_buffer(bytes, nbytes) || no_buffer(handles, nhandles))
        return ERR_INVALID_ARGS;
    handle_t hs[CHANNEL_MAX_HANDLES];
    if (copy_in(hs, handles, nhandles * sizeof(handle_t)) != OK)
        return ERR_INVALID_ARGS;
    struct chan_bytes b = chan_ubytes(bytes, nbytes);
    return sys_channel_write_from(t, h, &b, hs, nhandles);
}

int64_t sysc_channel_write_rights(handle_t h, uint64_t bytes, uint32_t nbytes, uint64_t handles,
                                  uint64_t rights, uint32_t nhandles)
{
    SYSC_TABLE(t);
    if (nbytes > CHANNEL_MAX_BYTES || nhandles > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    if (no_buffer(bytes, nbytes) || no_buffer(handles, nhandles) || no_buffer(rights, nhandles))
        return ERR_INVALID_ARGS;
    handle_t hs[CHANNEL_MAX_HANDLES];
    rights_t rs[CHANNEL_MAX_HANDLES];
    if (copy_in(hs, handles, nhandles * sizeof(handle_t)) != OK ||
        copy_in(rs, rights, nhandles * sizeof(rights_t)) != OK)
        return ERR_INVALID_ARGS;
    struct chan_bytes b = chan_ubytes(bytes, nbytes);
    return sys_channel_write_rights_from(t, h, &b, hs, rs, nhandles);
}

int64_t sysc_channel_set_flags(handle_t h, uint32_t flags)
{
    SYSC_TABLE(t);
    return sys_channel_set_flags(t, h, flags);
}

int64_t sysc_channel_read(const struct channel_read_args *a)
{
    SYSC_TABLE(t);
    if (a->reserved || no_buffer(a->rd_bytes, a->rd_cap) || no_buffer(a->rd_handles, a->rd_hcap))
        return ERR_INVALID_ARGS;
    uint32_t bcap = a->rd_cap < CHANNEL_MAX_BYTES ? a->rd_cap : CHANNEL_MAX_BYTES;
    uint32_t hcap = a->rd_hcap < CHANNEL_MAX_HANDLES ? a->rd_hcap : CHANNEL_MAX_HANDLES;
    struct chan_bytes b = chan_ubytes(a->rd_bytes, bcap);
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t nb = 0, nh = 0;
    status_t st = sys_channel_read_into(t, a->h, &b, &nb, hs, hcap, &nh);
    if (st == OK) {
        /* The bytes are in place already: the length goes last, so a
         * reader that finds it set finds the whole message (svcstate). */
        if (copy_out(a->rd_handles, hs, nh * sizeof(handle_t)) != OK ||
            put_sizes(a->rd_actual, nb, a->rd_hactual, nh) != OK) {
            close_all(t, hs, nh);   /* the message is gone; its handles must not leak */
            st = ERR_INVALID_ARGS;
        }
    } else if (st == ERR_BUFFER_TOO_SMALL) {
        if (put_sizes(a->rd_actual, nb, a->rd_hactual, nh) != OK)
            st = ERR_INVALID_ARGS;   /* the message stays queued */
    }
    return st;
}

/* A call's deadline: d itself, or with a timeout flag that many ns from
 * now (a sum past the clock's end is forever). */
static uint64_t deadline_of(uint64_t d, bool timeout)
{
    uint64_t sum;
    if (!timeout)
        return d;
    return __builtin_add_overflow(uptime_ns(), d, &sum) ? DEADLINE_NEVER : sum;
}

int64_t sysc_channel_call(const struct channel_call_args *a)
{
    SYSC_TABLE(t);
    if (a->flags & ~CHANNEL_CALL_TIMEOUT)
        return ERR_INVALID_ARGS;
    if (a->wr_len < 4 || no_buffer(a->wr_bytes, a->wr_len) ||
        no_buffer(a->wr_handles, a->wr_nhandles) || no_buffer(a->rd_bytes, a->rd_cap) ||
        no_buffer(a->rd_handles, a->rd_hcap))
        return ERR_INVALID_ARGS;
    if (a->wr_len > CHANNEL_MAX_BYTES || a->wr_nhandles > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    uint32_t rcap = a->rd_cap < CHANNEL_MAX_BYTES ? a->rd_cap : CHANNEL_MAX_BYTES;
    uint32_t rhcap = a->rd_hcap < CHANNEL_MAX_HANDLES ? a->rd_hcap : CHANNEL_MAX_HANDLES;

    handle_t wh[CHANNEL_MAX_HANDLES], rh[CHANNEL_MAX_HANDLES];
    if (copy_in(wh, a->wr_handles, a->wr_nhandles * sizeof(handle_t)) != OK)
        return ERR_INVALID_ARGS;
    struct chan_bytes w = chan_ubytes(a->wr_bytes, a->wr_len), r = chan_ubytes(a->rd_bytes, rcap);
    uint32_t nb = 0, nh = 0;
    status_t st = sys_channel_call_from(t, a->h, &w, wh, a->wr_nhandles, &r, &nb, rh, rhcap, &nh,
                                        deadline_of(a->deadline_ns,
                                                    a->flags & CHANNEL_CALL_TIMEOUT));
    if (st == OK) {
        if (copy_out(a->rd_handles, rh, nh * sizeof(handle_t)) != OK ||
            put_sizes(a->rd_actual, nb, a->rd_hactual, nh) != OK) {
            close_all(t, rh, nh);
            st = ERR_INVALID_ARGS;
        }
    } else if (st == ERR_BUFFER_TOO_SMALL) {
        if (put_sizes(a->rd_actual, nb, a->rd_hactual, nh) != OK)
            st = ERR_INVALID_ARGS;
    }
    return st;
}

/* channel_reply_wait's results: the reply's status, then for a channel
 * end the request's sizes as channel_read's (after its bytes and handles:
 * the length goes last, as sysc_channel_read), all three in one copy when
 * the caller keeps them in a row; for a port, the packet. */
static status_t reply_wait_out(struct handle_table *t, const struct channel_reply_wait_args *a,
                               const struct chan_reply_wait *rw, status_t st)
{
    bool sizes = !rw->is_port && (st == OK || st == ERR_BUFFER_TOO_SMALL);
    uint32_t nh = st == OK && !rw->is_port ? rw->req_nh : 0;   /* handles in t now */
    status_t cst = OK;
    if (st == OK && rw->is_port)
        cst = copy_to_user(a->packet, &rw->pkt, sizeof(rw->pkt));
    else
        cst = copy_out(a->rd_handles, rw->req_h, nh * sizeof(handle_t));
    const struct out_word w[3] = {
        { rw->replied ? a->reply_status : 0, (uint32_t)rw->reply_st },
        { sizes ? a->rd_actual : 0, rw->req_nb },
        { sizes ? a->rd_hactual : 0, rw->req_nh },
    };
    if (cst != OK || put_words(w, 3) != OK) {
        /* What left its queue is lost (as sysc_channel_read's, and a
         * packet as port_wait's), but its handles must not leak. */
        close_all(t, rw->req_h, nh);
        return ERR_INVALID_ARGS;
    }
    return st;
}

/* No reply: then none of its fields either (fail closed). */
static bool reply_fields_ok(const struct channel_reply_wait_args *a)
{
    return a->h != HANDLE_INVALID ||
           !(a->wr_len || a->wr_bytes || a->wr_handles || a->wr_nhandles || a->mark ||
             a->reply_status);
}

int64_t sysc_channel_reply_wait(const struct channel_reply_wait_args *a)
{
    SYSC_TABLE(t);
    if (a->reserved || (a->flags & ~CHANNEL_REPLY_WAIT_TIMEOUT) || !reply_fields_ok(a))
        return ERR_INVALID_ARGS;
    if (no_buffer(a->wr_bytes, a->wr_len) || no_buffer(a->wr_handles, a->wr_nhandles) ||
        no_buffer(a->rd_bytes, a->rd_cap) || no_buffer(a->rd_handles, a->rd_hcap))
        return ERR_INVALID_ARGS;
    if (a->wr_len > CHANNEL_MAX_BYTES || a->wr_nhandles > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    handle_t rh[CHANNEL_MAX_HANDLES], hs[CHANNEL_MAX_HANDLES];
    if (copy_in(rh, a->wr_handles, a->wr_nhandles * sizeof(handle_t)) != OK)
        return ERR_INVALID_ARGS;
    struct chan_reply_wait rw = {
        .h = a->h, .reply = chan_ubytes(a->wr_bytes, a->wr_len), .reply_h = rh,
        .reply_nh = a->wr_nhandles,
        .wait = a->wait,
        .mark = chan_ubytes(a->mark, a->mark ? sizeof(uint64_t) : 0),
        .req = chan_ubytes(a->rd_bytes, a->rd_cap < CHANNEL_MAX_BYTES ? a->rd_cap
                                                                       : CHANNEL_MAX_BYTES),
        .req_h = hs,
        .req_hcap = a->rd_hcap < CHANNEL_MAX_HANDLES ? a->rd_hcap : CHANNEL_MAX_HANDLES,
        .want_request = a->rd_bytes || a->rd_cap || a->rd_handles || a->rd_hcap ||
                        a->rd_actual || a->rd_hactual,
        .want_packet = a->packet != 0,
        .deadline_ns = deadline_of(a->deadline_ns, a->flags & CHANNEL_REPLY_WAIT_TIMEOUT),
    };
    status_t st = sys_channel_reply_wait(t, &rw);
    return reply_wait_out(t, a, &rw, st);
}
