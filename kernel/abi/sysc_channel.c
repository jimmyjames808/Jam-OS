/* System calls: channels. The rules all sysc_* follow are in sysc.h.
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

static status_t put_u32(uint64_t uaddr, uint32_t v)
{
    return uaddr ? copy_to_user(uaddr, &v, sizeof(v)) : OK;
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
    handle_t hs[CHANNEL_MAX_HANDLES];
    rights_t rs[CHANNEL_MAX_HANDLES];
    if (copy_in(hs, handles, nhandles * sizeof(handle_t)) != OK ||
        copy_in(rs, rights, nhandles * sizeof(rights_t)) != OK)
        return ERR_INVALID_ARGS;
    struct chan_bytes b = chan_ubytes(bytes, nbytes);
    return sys_channel_write_rights_from(t, h, &b, hs, rs, nhandles);
}

int64_t sysc_channel_read(const struct channel_read_args *a)
{
    SYSC_TABLE(t);
    if (a->reserved)
        return ERR_INVALID_ARGS;
    uint32_t bcap = a->bytes_cap < CHANNEL_MAX_BYTES ? a->bytes_cap : CHANNEL_MAX_BYTES;
    uint32_t hcap = a->handles_cap < CHANNEL_MAX_HANDLES ? a->handles_cap : CHANNEL_MAX_HANDLES;
    struct chan_bytes b = chan_ubytes(a->bytes, bcap);
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t nb = 0, nh = 0;
    status_t st = sys_channel_read_into(t, a->h, &b, &nb, hs, hcap, &nh);
    if (st == OK) {
        /* The bytes are in place already: the length goes last, so a
         * reader that finds it set finds the whole message (svcstate). */
        if (copy_out(a->handles, hs, nh * sizeof(handle_t)) != OK ||
            put_u32(a->actual_bytes, nb) != OK || put_u32(a->actual_handles, nh) != OK) {
            close_all(t, hs, nh);   /* the message is gone; its handles must not leak */
            st = ERR_INVALID_ARGS;
        }
    } else if (st == ERR_BUFFER_TOO_SMALL) {
        if (put_u32(a->actual_bytes, nb) != OK || put_u32(a->actual_handles, nh) != OK)
            st = ERR_INVALID_ARGS;   /* the message stays queued */
    }
    return st;
}

/* The call's deadline: deadline_ns itself, or with CHANNEL_CALL_TIMEOUT
 * that many ns from now (a sum past the clock's end is forever). */
static uint64_t call_deadline(const struct channel_call_args *a)
{
    uint64_t d;
    if (!(a->flags & CHANNEL_CALL_TIMEOUT))
        return a->deadline_ns;
    return __builtin_add_overflow(uptime_ns(), a->deadline_ns, &d) ? DEADLINE_NEVER : d;
}

int64_t sysc_channel_call(const struct channel_call_args *a)
{
    SYSC_TABLE(t);
    if (a->flags & ~CHANNEL_CALL_TIMEOUT)
        return ERR_INVALID_ARGS;
    if (a->wn < 4)
        return ERR_INVALID_ARGS;
    if (a->wn > CHANNEL_MAX_BYTES || a->whn > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    uint32_t rcap = a->rcap < CHANNEL_MAX_BYTES ? a->rcap : CHANNEL_MAX_BYTES;
    uint32_t rhcap = a->rhcap < CHANNEL_MAX_HANDLES ? a->rhcap : CHANNEL_MAX_HANDLES;

    handle_t wh[CHANNEL_MAX_HANDLES], rh[CHANNEL_MAX_HANDLES];
    if (copy_in(wh, a->wh, a->whn * sizeof(handle_t)) != OK)
        return ERR_INVALID_ARGS;
    struct chan_bytes w = chan_ubytes(a->wbytes, a->wn), r = chan_ubytes(a->rbytes, rcap);
    uint32_t nb = 0, nh = 0;
    status_t st = sys_channel_call_from(t, a->h, &w, wh, a->whn, &r, &nb, rh, rhcap, &nh,
                                        call_deadline(a));
    if (st == OK) {
        if (copy_out(a->rh, rh, nh * sizeof(handle_t)) != OK ||
            put_u32(a->ractual, nb) != OK || put_u32(a->rhactual, nh) != OK) {
            close_all(t, rh, nh);
            st = ERR_INVALID_ARGS;
        }
    } else if (st == ERR_BUFFER_TOO_SMALL) {
        if (put_u32(a->ractual, nb) != OK || put_u32(a->rhactual, nh) != OK)
            st = ERR_INVALID_ARGS;
    }
    return st;
}
