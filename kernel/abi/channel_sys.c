/* Handle-level channel calls (system calls from M5). */
#include <jam/channel.h>
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

/* Put taken handles back under their old values, newest first. */
static void untake_all(struct handle_table *t, const handle_t *hs, struct khandle *khs,
                       uint32_t n)
{
    while (n--) {
        handle_t v;
        if (handle_untake(t, hs[n], &khs[n], &v) != OK)
            khandle_release(&khs[n]);   /* table full: nowhere to put it */
    }
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

/* Insert received khandles into t. If the table is full the message's
 * handles are lost (the ones inserted so far are closed again). */
static status_t insert_all(struct handle_table *t, struct khandle *khs, handle_t *hs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        status_t st = handle_insert(t, &khs[i], &hs[i]);
        if (st != OK) {
            for (uint32_t j = 0; j < i; j++)
                handle_close(t, hs[j]);
            for (uint32_t j = i; j < n; j++)
                khandle_release(&khs[j]);
            return st;
        }
    }
    return OK;
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
    uint32_t cap = handles_cap < CHANNEL_MAX_HANDLES ? handles_cap : CHANNEL_MAX_HANDLES;
    uint32_t nh = 0;
    st = channel_read(ch, bytes, bytes_cap, actual_bytes, khs, cap, &nh);
    if (actual_handles)
        *actual_handles = nh;
    if (st == OK)
        st = insert_all(t, khs, handles, nh);
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
    st = take_all(t, wh, wkhs, whn);
    if (st == OK) {
        uint32_t cap = rhcap < CHANNEL_MAX_HANDLES ? rhcap : CHANNEL_MAX_HANDLES;
        uint32_t nh = 0;
        st = channel_call(ch, wbytes, wn, wkhs, whn, rbytes, rcap, ractual, rkhs, cap, &nh,
                          deadline_ns);
        /* channel_call clears the request's khandles only once it is sent. */
        if (whn && wkhs[0].obj)
            untake_all(t, wh, wkhs, whn);
        if (rhactual)
            *rhactual = nh;
        if (st == OK)
            st = insert_all(t, rkhs, rh, nh);
    }
    kobject_unref((struct kobject *)ch);
    return st;
}
