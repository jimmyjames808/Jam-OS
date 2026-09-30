/* Handle-level channel calls (the system calls' sys_* layer). */
#include <jam/channel.h>
#include <jam/panic.h>
#include <jam/sys.h>

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

status_t sys_channel_write(struct handle_table *t, handle_t h, const void *bytes, uint32_t nbytes,
                           const handle_t *handles, uint32_t nhandles)
{
    struct channel *ch;
    status_t st = get_channel(t, h, RIGHT_WRITE, &ch);
    if (st != OK)
        return st;
    struct khandle khs[CHANNEL_MAX_HANDLES];
    st = take_all(t, handles, khs, nhandles);
    if (st == OK) {
        st = channel_write(ch, bytes, nbytes, khs, nhandles);
        if (st != OK)
            untake_all(t, handles, khs, nhandles);
        else
            commit_all(t, handles, nhandles);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}

/* Write the taken handles khs with their new rights: each arrives with
 * rights[i], a subset of what it has (as handle_duplicate), or RIGHT_SAME.
 * Checked before anything changes, restored if the write fails. */
static status_t write_narrowed(struct channel *ch, const void *bytes, uint32_t nbytes,
                               struct khandle *khs, const rights_t *rights, uint32_t nhandles)
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
    st = channel_write(ch, bytes, nbytes, khs, nhandles);
    if (st != OK)
        for (uint32_t i = 0; i < nhandles; i++)
            khs[i].rights = had[i];
    return st;
}

status_t sys_channel_write_rights(struct handle_table *t, handle_t h, const void *bytes,
                                  uint32_t nbytes, const handle_t *handles,
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
        st = write_narrowed(ch, bytes, nbytes, khs, rights, nhandles);
        if (st != OK)
            untake_all(t, handles, khs, nhandles);
        else
            commit_all(t, handles, nhandles);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}

status_t sys_channel_read(struct handle_table *t, handle_t h, void *bytes, uint32_t bytes_cap,
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
        st = channel_read(ch, bytes, bytes_cap, &nb, khs, nrsv, &nh);
        if (st != ERR_BUFFER_TOO_SMALL || nb > bytes_cap || nh > cap || nh <= nrsv)
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

status_t sys_channel_call(struct handle_table *t, handle_t h, void *wbytes, uint32_t wn,
                          const handle_t *wh, uint32_t whn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, handle_t *rh, uint32_t rhcap, uint32_t *rhactual,
                          uint64_t deadline_ns)
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
        uint32_t nh = 0;
        st = channel_call(ch, wbytes, wn, wkhs, whn, rbytes, rcap, ractual, rkhs, cap, &nh,
                          deadline_ns);
        /* channel_call clears the request's khandles only once it is sent, so
         * a non-NULL first obj means the send failed: put them back. Otherwise
         * the send consumed them and their reserved slots are freed. */
        if (whn && wkhs[0].obj)
            untake_all(t, wh, wkhs, whn);
        else if (whn)
            commit_all(t, wh, whn);
        if (rhactual)
            *rhactual = nh;
        fill_reserved(t, rsv, cap, rkhs, rh, st == OK ? nh : 0);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}
