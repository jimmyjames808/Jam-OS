/* Handle-level channel calls (the system calls' sys_* layer). The _from /
 * _into forms take the bytes as a struct chan_bytes, so a system call
 * passes its user buffer and the object layer copies straight between it
 * and the message; the plain forms are the same on kernel memory.
 * channel_reply_wait (a reply, then a wait on a channel or a port) is
 * here too, at the end: it shares the handle steps. */
#include <jam/channel.h>
#include <jam/panic.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/usercopy.h>

#define CHANNEL_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL)

static status_t get_channel(struct handle_table *t, handle_t h, rights_t need,
                            struct channel **out)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_CHANNEL, need, &obj, NULL);
    if (st == OK)
        *out = (struct channel *)obj;
    return st;
}

/* Put taken handles back under their old values, newest first. The slots were
 * reserved by handle_take, so handle_untake always succeeds and no handle is
 * lost; the release is a belt-and-braces fallback that should never run. */
static void untake_all(struct handle_table *t, const handle_t *hs, struct khandle *khs,
                       uint32_t n)
{
    while (n--) {
        handle_t v;
        if (handle_untake(t, hs[n], &khs[n], &v) != OK)
            khandle_release(&khs[n]);
    }
}

/* Release the reserved slots of handles a successful send consumed. */
static void commit_all(struct handle_table *t, const handle_t *hs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        handle_commit(t, hs[i]);
}

/* Take every handle in hs out of t (each needs RIGHT_TRANSFER), or none. */
static status_t take_all(struct handle_table *t, const handle_t *hs, struct khandle *khs,
                         uint32_t n)
{
    if (n > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    if (n && !hs)
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < n; i++) {
        status_t st = handle_take(t, hs[i], &khs[i]);
        if (st != OK) {
            untake_all(t, hs, khs, i);
            return st;
        }
    }
    return OK;
}

/* Put received khandles into slots reserved with handle_reserve (which makes
 * this unable to fail), and give back the reserved slots nobody needed. */
static void fill_reserved(struct handle_table *t, const handle_t *rsv, uint32_t nrsv,
                          struct khandle *khs, handle_t *hs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (handle_untake(t, rsv[i], &khs[i], &hs[i]) != OK)
            panic("channel: reserved handle slot %x vanished", rsv[i]);
    for (uint32_t i = n; i < nrsv; i++)
        handle_commit(t, rsv[i]);
}

status_t sys_channel_create(struct handle_table *t, handle_t *a, handle_t *b)
{
    struct channel *ca, *cb;
    status_t st = channel_create(&ca, &cb);
    if (st != OK)
        return st;
    struct khandle ka = khandle_from_new((struct kobject *)ca, CHANNEL_RIGHTS);
    struct khandle kb = khandle_from_new((struct kobject *)cb, CHANNEL_RIGHTS);
    st = handle_insert(t, &ka, a);
    if (st == OK) {
        st = handle_insert(t, &kb, b);
        if (st != OK)
            handle_close(t, *a);
    }
    khandle_release(&ka);   /* no-ops for the inserted ones */
    khandle_release(&kb);
    return st;
}

status_t sys_channel_write_from(struct handle_table *t, handle_t h, const struct chan_bytes *b,
                                const handle_t *handles, uint32_t nhandles)
{
    struct channel *ch;
    status_t st = get_channel(t, h, RIGHT_WRITE, &ch);
    if (st != OK)
        return st;
    struct khandle khs[CHANNEL_MAX_HANDLES];
    st = take_all(t, handles, khs, nhandles);
    if (st == OK) {
        st = channel_write_from(ch, b, khs, nhandles);
        if (st != OK)
            untake_all(t, handles, khs, nhandles);
        else
            commit_all(t, handles, nhandles);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}

status_t sys_channel_write(struct handle_table *t, handle_t h, const void *bytes, uint32_t nbytes,
                           const handle_t *handles, uint32_t nhandles)
{
    struct chan_bytes b = chan_kbytes(bytes, nbytes);
    return sys_channel_write_from(t, h, &b, handles, nhandles);
}

/* Write the taken handles khs with their new rights: each arrives with
 * rights[i], a subset of what it has (as handle_duplicate), or RIGHT_SAME.
 * Checked before anything changes, restored if the write fails. */
static status_t write_narrowed(struct channel *ch, const struct chan_bytes *b, struct khandle *khs,
                               const rights_t *rights, uint32_t nhandles)
{
    rights_t had[CHANNEL_MAX_HANDLES];
    status_t st = OK;
    for (uint32_t i = 0; i < nhandles; i++) {
        had[i] = khs[i].rights;
        if (rights[i] != RIGHT_SAME && (rights[i] & ~had[i]))
            st = ERR_INVALID_ARGS;
    }
    if (st != OK)
        return st;
    for (uint32_t i = 0; i < nhandles; i++)
        if (rights[i] != RIGHT_SAME)
            khs[i].rights = rights[i];
    st = channel_write_from(ch, b, khs, nhandles);
    if (st != OK)
        for (uint32_t i = 0; i < nhandles; i++)
            khs[i].rights = had[i];
    return st;
}

status_t sys_channel_write_rights_from(struct handle_table *t, handle_t h,
                                       const struct chan_bytes *b, const handle_t *handles,
                                       const rights_t *rights, uint32_t nhandles)
{
    if (nhandles && !rights)
        return ERR_INVALID_ARGS;
    struct channel *ch;
    status_t st = get_channel(t, h, RIGHT_WRITE, &ch);
    if (st != OK)
        return st;
    struct khandle khs[CHANNEL_MAX_HANDLES];
    st = take_all(t, handles, khs, nhandles);
    if (st == OK) {
        st = write_narrowed(ch, b, khs, rights, nhandles);
        if (st != OK)
            untake_all(t, handles, khs, nhandles);
        else
            commit_all(t, handles, nhandles);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}

status_t sys_channel_write_rights(struct handle_table *t, handle_t h, const void *bytes,
                                  uint32_t nbytes, const handle_t *handles,
                                  const rights_t *rights, uint32_t nhandles)
{
    struct chan_bytes b = chan_kbytes(bytes, nbytes);
    return sys_channel_write_rights_from(t, h, &b, handles, rights, nhandles);
}

status_t sys_channel_read_into(struct handle_table *t, handle_t h, const struct chan_bytes *b,
                               uint32_t *actual_bytes, handle_t *handles, uint32_t handles_cap,
                               uint32_t *actual_handles)
{
    if (handles_cap && !handles)
        return ERR_INVALID_ARGS;
    struct channel *ch;
    status_t st = get_channel(t, h, RIGHT_READ, &ch);
    if (st != OK)
        return st;
    struct khandle khs[CHANNEL_MAX_HANDLES];
    handle_t rsv[CHANNEL_MAX_HANDLES];
    uint32_t cap = handles_cap < CHANNEL_MAX_HANDLES ? handles_cap : CHANNEL_MAX_HANDLES;
    uint32_t nrsv = 0, nb = 0, nh = 0;
    /* Only reserve table slots for as many handles as the next message
     * carries: read with what is reserved; if the message needs more (and
     * the caller has room for them), reserve the rest and try again. The
     * message stays queued until its handles are guaranteed a slot. */
    for (;;) {
        st = channel_read_into(ch, b, &nb, khs, nrsv, &nh);
        if (st != ERR_BUFFER_TOO_SMALL || nb > b->len || nh > cap || nh <= nrsv)
            break;
        st = handle_reserve(t, nh - nrsv, &rsv[nrsv]);
        if (st != OK)
            break;   /* table full: the message stays queued */
        nrsv = nh;
    }
    if (actual_bytes)
        *actual_bytes = nb;
    if (actual_handles)
        *actual_handles = nh;
    fill_reserved(t, rsv, nrsv, khs, handles, st == OK ? nh : 0);
    kobject_unref((struct kobject *)ch);
    return st;
}

status_t sys_channel_read(struct handle_table *t, handle_t h, void *bytes, uint32_t bytes_cap,
                          uint32_t *actual_bytes, handle_t *handles, uint32_t handles_cap,
                          uint32_t *actual_handles)
{
    struct chan_bytes b = chan_kbytes(bytes, bytes_cap);
    return sys_channel_read_into(t, h, &b, actual_bytes, handles, handles_cap, actual_handles);
}

status_t sys_channel_call_from(struct handle_table *t, handle_t h, const struct chan_bytes *w,
                               const handle_t *wh, uint32_t whn, const struct chan_bytes *r,
                               uint32_t *ractual, handle_t *rh, uint32_t rhcap,
                               uint32_t *rhactual, uint64_t deadline_ns)
{
    if (rhcap && !rh)
        return ERR_INVALID_ARGS;
    struct channel *ch;
    status_t st = get_channel(t, h, RIGHT_READ | RIGHT_WRITE, &ch);
    if (st != OK)
        return st;
    struct khandle wkhs[CHANNEL_MAX_HANDLES], rkhs[CHANNEL_MAX_HANDLES];
    handle_t rsv[CHANNEL_MAX_HANDLES];
    uint32_t cap = rhcap < CHANNEL_MAX_HANDLES ? rhcap : CHANNEL_MAX_HANDLES;
    /* The reply is ours alone once it arrives, so its handles must have
     * slots before the request goes out: reserve room for all it may carry. */
    st = handle_reserve(t, cap, rsv);
    if (st != OK) {
        kobject_unref((struct kobject *)ch);
        return st;
    }
    st = take_all(t, wh, wkhs, whn);
    if (st != OK)
        fill_reserved(t, rsv, cap, rkhs, rh, 0);
    if (st == OK) {
        struct chan_call c = {
            .req = *w, .req_h = wkhs, .req_nh = whn, .rep = *r, .rep_h = rkhs, .rep_hcap = cap,
        };
        st = channel_call_with(ch, &c, deadline_ns);
        /* channel_call clears the request's khandles only once it is sent, so
         * a non-NULL first obj means the send failed: put them back. Otherwise
         * the send consumed them and their reserved slots are freed. */
        if (whn && wkhs[0].obj)
            untake_all(t, wh, wkhs, whn);
        else if (whn)
            commit_all(t, wh, whn);
        if (ractual && (st == OK || st == ERR_BUFFER_TOO_SMALL))
            *ractual = c.rep_nb;
        if (rhactual)
            *rhactual = st == OK || st == ERR_BUFFER_TOO_SMALL ? c.rep_nh : 0;
        fill_reserved(t, rsv, cap, rkhs, rh, st == OK ? c.rep_nh : 0);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}

status_t sys_channel_call(struct handle_table *t, handle_t h, void *wbytes, uint32_t wn,
                          const handle_t *wh, uint32_t whn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, handle_t *rh, uint32_t rhcap, uint32_t *rhactual,
                          uint64_t deadline_ns)
{
    struct chan_bytes w = chan_kbytes(wbytes, wn), r = chan_kbytes(rbytes, rcap);
    return sys_channel_call_from(t, h, &w, wh, whn, &r, ractual, rh, rhcap, rhactual,
                                 deadline_ns);
}

/* ---- channel_reply_wait --------------------------------------------------------- */

/* n bytes from v into b: kernel or user memory (b->len 0: nowhere). */
static status_t put_bytes(const struct chan_bytes *b, const void *v, uint32_t n)
{
    if (!b->len)
        return OK;
    if (b->user)
        return copy_to_user(b->addr, v, n) == OK ? OK : ERR_INVALID_ARGS;
    memcpy((void *)(uintptr_t)b->addr, v, n);
    return OK;
}

/* The reply's channel (NULL: no reply) and the wait's channel or port,
 * each with a reference: one lookup when they are the same handle. Types
 * are checked before rights, as handle_get does. */
static status_t rw_lookup(struct handle_table *t, const struct chan_reply_wait *rw,
                          struct channel **reply, struct kobject **wait)
{
    bool same = rw->h != HANDLE_INVALID && rw->h == rw->wait;
    rights_t need = same ? RIGHT_READ | RIGHT_WRITE : RIGHT_READ, have = 0;
    struct kobject *w;
    status_t st = handle_get(t, rw->wait, OBJ_NONE, 0, &w, &have);
    if (st != OK)
        return st;
    if ((w->type != OBJ_CHANNEL && w->type != OBJ_PORT) || (same && w->type != OBJ_CHANNEL))
        st = ERR_WRONG_TYPE;
    else if ((have & need) != need)
        st = ERR_ACCESS_DENIED;
    struct channel *r = NULL;
    if (st == OK && same) {
        kobject_ref(w);
        r = (struct channel *)w;
    } else if (st == OK && rw->h != HANDLE_INVALID) {
        st = get_channel(t, rw->h, RIGHT_WRITE, &r);
    }
    if (st != OK) {
        kobject_unref(w);
        return st;
    }
    *reply = r;
    *wait = w;
    return OK;
}

/* Only the fields of the form `wait` is in (rw->is_port). */
static bool rw_form_ok(const struct chan_reply_wait *rw)
{
    if (rw->is_port)
        return rw->want_packet && !rw->want_request;
    return !rw->want_packet && (!rw->req_hcap || rw->req_h);
}

/* The reply half: send the reply on ch (its handles leave t only if it
 * goes) as a wake that may hand this CPU to its caller (we wait right
 * after), then, once it went out, the mark. rw->reply_st gets its status.
 * OK to go on to the wait, else the error that ends the call. */
static status_t rw_reply(struct handle_table *t, struct channel *ch, struct chan_reply_wait *rw)
{
    struct khandle khs[CHANNEL_MAX_HANDLES];
    status_t st = take_all(t, rw->reply_h, khs, rw->reply_nh);
    if (st == OK) {
        thread_set_handoff(true);
        st = channel_write_from(ch, &rw->reply, khs, rw->reply_nh);
        thread_set_handoff(false);
        if (st != OK)
            untake_all(t, rw->reply_h, khs, rw->reply_nh);
        else
            commit_all(t, rw->reply_h, rw->reply_nh);
    }
    rw->replied = true;
    rw->reply_st = st;
    uint64_t one = 1;
    if (st == OK && put_bytes(&rw->mark, &one, sizeof(one)) != OK)
        return ERR_INVALID_ARGS;
    return st == ERR_PEER_CLOSED ? OK : st;   /* a caller that has gone stops nothing */
}

/* The wait half on a channel: its next message into rw, the handles into
 * t's reserved slots rsv (nrsv of them, all given back or filled). */
static status_t rw_read(struct handle_table *t, struct channel *ch, struct chan_reply_wait *rw,
                        const handle_t *rsv, uint32_t nrsv)
{
    struct khandle khs[CHANNEL_MAX_HANDLES];
    struct chan_read r = { .buf = rw->req, .h = khs, .hcap = nrsv };
    status_t st = channel_read_wait(ch, &r, rw->deadline_ns);
    rw->req_nb = r.nb;
    rw->req_nh = r.nh;
    fill_reserved(t, rsv, nrsv, khs, rw->req_h, st == OK ? r.nh : 0);
    return st;
}

status_t sys_channel_reply_wait(struct handle_table *t, struct chan_reply_wait *rw)
{
    if (rw->reply_nh > CHANNEL_MAX_HANDLES || (rw->reply_nh && !rw->reply_h))
        return ERR_INVALID_ARGS;
    struct channel *reply;
    struct kobject *wait;
    status_t st = rw_lookup(t, rw, &reply, &wait);
    if (st != OK)
        return st;
    rw->is_port = wait->type == OBJ_PORT;
    rw->replied = false;
    rw->req_nb = rw->req_nh = 0;
    /* The request's handles get table slots before the reply goes out, so
     * a full table fails here, with nothing sent. */
    uint32_t cap = rw->is_port ? 0 : rw->req_hcap < CHANNEL_MAX_HANDLES ? rw->req_hcap
                                                                         : CHANNEL_MAX_HANDLES;
    handle_t rsv[CHANNEL_MAX_HANDLES];
    st = rw_form_ok(rw) ? handle_reserve(t, cap, rsv) : ERR_INVALID_ARGS;
    if (st == OK) {
        st = reply ? rw_reply(t, reply, rw) : OK;
        if (st != OK)
            fill_reserved(t, rsv, cap, NULL, NULL, 0);
        else if (rw->is_port)
            st = port_wait(container_of(wait, struct port, base), rw->deadline_ns, &rw->pkt);
        else
            st = rw_read(t, (struct channel *)wait, rw, rsv, cap);
        sched_handoff_done();   /* a wait satisfied at once: queue the caller */
    }
    if (reply)
        kobject_unref((struct kobject *)reply);
    kobject_unref(wait);
    return st;
}
