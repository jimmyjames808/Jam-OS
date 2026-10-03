/* mixer: its state VMO (<svcstate.h>): where the numbers live, the
 * commit, and requests read into the request slots and answered from
 * them.
 *
 * The mixer makes the VMO itself and maps it at SVCSTATE_ADDR; its own
 * area is struct mixer_saved (internal.h has the model). A commit copies
 * the working numbers over the saved copy that isn't current and then
 * moves the commit word with one released store: a process dies between
 * two instructions, never inside one, so the word names a whole copy
 * whatever moment it dies at. A request's answer commits the numbers
 * (with req_done naming the request) before the request slot's own
 * commit, so a saved copy never holds a request's effects without saying
 * which request it was.
 *
 * Reading into a slot keeps what the generated server did with a message
 * that doesn't fit: one longer than the channel's protocol takes, or
 * carrying handles, is answered ERR_INVALID_ARGS (with its txid, if it
 * has one) and dropped; one under 4 bytes is dropped unanswered. */
#include "internal.h"

/* The biggest reply of either protocol (svc.connect's is smaller). */
#define REP_CAP (AUDIO_REP_MAX > AUDIOCTL_REP_MAX ? AUDIO_REP_MAX : AUDIOCTL_REP_MAX)

_Static_assert(REQ_CAP_AUDIO >= REQ_CAP_CTL, "the slot holds the bigger request");

static const struct svcstate_layout layout = {
    .kind = STATE_KIND,
    .layout = STATE_LAYOUT,
    .req_cap = REQ_CAP_AUDIO,
    .rep_cap = REP_CAP,
    .user_size = sizeof(struct mixer_saved),
};

status_t state_init(struct mixer *m)
{
    handle_t vmo = HANDLE_INVALID;
    enum svcstate_start how;
    status_t st = svcstate_create(svcstate_size(&layout), &vmo);
    if (st == OK)
        st = svcstate_open(vmo, &layout, &m->state, &how);
    if (vmo)
        jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return st;
    m->saved = svcstate_user(&m->state);
    m->nums = &m->saved->work;
    return OK;
}

void state_commit(struct mixer *m)
{
    struct mixer_saved *v = m->saved;
    uint64_t next = v->commits + 1;
    memcpy(&v->copy[next & 1], &v->work, sizeof(v->work));
    __atomic_store_n(&v->commits, next, __ATOMIC_RELEASE);
}

/* The message that didn't fit the slot (too long, or too many handles):
 * its sizes, then dropped as the generated server drops one. */
static status_t drop_big(handle_t ch)
{
    uint32_t n = 0, nh = 0;
    status_t st = jam_channel_read(&(struct channel_read_args){
        .h = ch, .actual_bytes = (uint64_t)(uintptr_t)&n,
        .actual_handles = (uint64_t)(uintptr_t)&nh });
    return st == ERR_BUFFER_TOO_SMALL ? idl_drain(ch, n, nh) : st;
}

status_t req_take(struct mixer *m, handle_t ch, uint32_t key, uint32_t cap, unsigned *slot)
{
    unsigned i;
    status_t st = svcstate_take(&m->state, key, ch, &i);
    *slot = REQ_NONE;
    if (st == ERR_BUFFER_TOO_SMALL)
        return drop_big(ch);
    if (st == ERR_INVALID_ARGS)
        return OK;   /* under 4 bytes: taken, no txid to answer */
    if (st != OK)
        return st;
    uint32_t n = 0, nh = m->state.h->slot[i].nhandles;
    (void)svcstate_request(&m->state, i, &n);
    if (nh || n > cap) {
        idl_close_all(m->state.handles, nh);
        req_status(m, i, ch, ERR_INVALID_ARGS);
        return OK;
    }
    *slot = i;
    return OK;
}

void req_answer(struct mixer *m, unsigned slot, handle_t ch, uint32_t rn, handle_t *hs,
                uint32_t nh)
{
    m->nums->req_done = m->state.h->slot[slot].seq;
    state_commit(m);
    (void)svcstate_commit(&m->state, slot, rn);   /* rn fits: rep_cap is the biggest reply */
    if (!rn) {
        svcstate_sent(&m->state, slot);
        return;
    }
    if (svcstate_reply(&m->state, slot, ch, hs, nh) != OK)
        idl_close_all(hs, nh);   /* not sent: they're still ours */
}

void req_status(struct mixer *m, unsigned slot, handle_t ch, status_t st)
{
    const struct idl_req_hdr *q = svcstate_request(&m->state, slot, NULL);
    struct idl_rep_hdr *r = svcstate_reply_area(&m->state, slot);
    r->txid = q->txid;
    r->status = st;
    req_answer(m, slot, ch, sizeof(*r), NULL, 0);
}
