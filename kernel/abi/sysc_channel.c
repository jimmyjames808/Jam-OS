/* System calls: channels. The rules all sysc_* follow are in sysc.h.
 *
 * Bytes go through a kernel buffer: on the stack for small messages (the
 * common case, and what the channel_call benchmark measures), from the
 * heap up to CHANNEL_MAX_BYTES otherwise. The txid channel_call stamps
 * goes into that kernel copy of the request, never into user memory.
 *
 * Received handles are inserted into the caller's table by the handle
 * layer before their values are copied out. If that copy (or the bytes')
 * fails, the message is already consumed, so its handles are closed again:
 * a bad buffer loses the message but never leaks a handle. */
#include <jam/channel.h>
#include <jam/mm.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include "sysc.h"

#define SMALL 512   /* bytes: messages up to this size never touch the heap */

/* A kernel buffer of n bytes: `small` if it fits, else from the heap. */
static void *buf_get(uint8_t *small, uint32_t n)
{
    return n <= SMALL ? small : kmalloc(n);
}

static void buf_put(uint8_t *small, void *b)
{
    if (b != small)
        kfree(b);
}

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
    uint8_t small[SMALL];
    void *kb = buf_get(small, nbytes);
    if (!kb)
        return ERR_NO_MEMORY;
    status_t st = copy_in(kb, bytes, nbytes);
    if (st == OK)
        st = sys_channel_write(t, h, kb, nbytes, hs, nhandles);
    buf_put(small, kb);
    return st;
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
    uint8_t small[SMALL];
    void *kb = buf_get(small, nbytes);
    if (!kb)
        return ERR_NO_MEMORY;
    status_t st = copy_in(kb, bytes, nbytes);
    if (st == OK)
        st = sys_channel_write_rights(t, h, kb, nbytes, hs, rs, nhandles);
    buf_put(small, kb);
    return st;
}

int64_t sysc_channel_read(const struct channel_read_args *a)
{
    SYSC_TABLE(t);
    if (a->reserved)
        return ERR_INVALID_ARGS;
    uint32_t bcap = a->bytes_cap < CHANNEL_MAX_BYTES ? a->bytes_cap : CHANNEL_MAX_BYTES;
    uint32_t hcap = a->handles_cap < CHANNEL_MAX_HANDLES ? a->handles_cap : CHANNEL_MAX_HANDLES;
    /* Try with the stack buffer first, whatever the caller's capacity: most
     * messages are small, and a reader with a big buffer shouldn't pay for
     * a big allocation per message. A message that only failed to fit our
     * try stays queued; allocate its size and read again (another reader
     * may take it meanwhile, so loop). */
    uint8_t small[SMALL];
    void *kb = small;
    uint32_t try_cap = bcap < SMALL ? bcap : SMALL;
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t nb = 0, nh = 0;
    status_t st;
    for (;;) {
        st = sys_channel_read(t, a->h, kb, try_cap, &nb, hs, hcap, &nh);
        if (st != ERR_BUFFER_TOO_SMALL || nb <= try_cap || nb > bcap || nh > hcap)
            break;
        buf_put(small, kb);
        kb = kmalloc(nb);
        try_cap = nb;
        if (!kb)
            return ERR_NO_MEMORY;
    }
    if (st == OK) {
        if (copy_out(a->bytes, kb, nb) != OK ||
            copy_out(a->handles, hs, nh * sizeof(handle_t)) != OK ||
            put_u32(a->actual_bytes, nb) != OK || put_u32(a->actual_handles, nh) != OK) {
            close_all(t, hs, nh);   /* the message is gone; its handles must not leak */
            st = ERR_INVALID_ARGS;
        }
    } else if (st == ERR_BUFFER_TOO_SMALL) {
        if (put_u32(a->actual_bytes, nb) != OK || put_u32(a->actual_handles, nh) != OK)
            st = ERR_INVALID_ARGS;   /* the message stays queued */
    }
    buf_put(small, kb);
    return st;
}

int64_t sysc_channel_call(const struct channel_call_args *a)
{
    SYSC_TABLE(t);
    if (a->reserved)
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
    uint8_t wsmall[SMALL], rsmall[SMALL];
    void *wb = buf_get(wsmall, a->wn);
    void *rb = buf_get(rsmall, rcap);
    status_t st = wb && rb ? OK : ERR_NO_MEMORY;
    if (st == OK)
        st = copy_from_user(wb, a->wbytes, a->wn);
    uint32_t nb = 0, nh = 0;
    if (st == OK)
        st = sys_channel_call(t, a->h, wb, a->wn, wh, a->whn, rb, rcap, &nb, rh, rhcap, &nh,
                              a->deadline_ns);
    if (st == OK) {
        if (copy_out(a->rbytes, rb, nb) != OK ||
            copy_out(a->rh, rh, nh * sizeof(handle_t)) != OK ||
            put_u32(a->ractual, nb) != OK || put_u32(a->rhactual, nh) != OK) {
            close_all(t, rh, nh);
            st = ERR_INVALID_ARGS;
        }
    } else if (st == ERR_BUFFER_TOO_SMALL) {
        if (put_u32(a->ractual, nb) != OK || put_u32(a->rhactual, nh) != OK)
            st = ERR_INVALID_ARGS;
    }
    if (wb)
        buf_put(wsmall, wb);
    if (rb)
        buf_put(rsmall, rb);
    return st;
}
