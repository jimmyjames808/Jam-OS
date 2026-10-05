/* mixer: its state VMO (<svcstate.h>): where the numbers live, the
 * commit, the checks a successor makes before it trusts them, requests
 * read into the request slots and answered from them, and the keep
 * channel's two calls.
 *
 * init makes the VMO and hands it over (SR_STATE); a mixer started
 * without one (by a test) makes its own. Either way it is mapped at
 * SVCSTATE_ADDR; the mixer's own area is struct mixer_saved (internal.h
 * has the model). A commit copies the working numbers over the saved copy
 * that isn't current and then moves the commit word with one released
 * store: a process dies between two instructions, never inside one, so
 * the word names a whole copy whatever moment it dies at. A request's
 * answer commits the numbers (with req_done naming the request) before the
 * request slot's own commit, so a saved copy never holds a request's
 * effects without saying which request it was.
 *
 * What a successor finds was written by a process that may have been
 * misbehaving when it died: svcstate_open checks the header and the
 * slots, state_check every count and index of the numbers; any failure
 * and the mixer starts fresh (a line in the log), as it did before it
 * kept a state.
 *
 * Reading into a slot keeps what the generated server did with a message
 * that doesn't fit: one longer than the channel's protocol takes, or
 * carrying handles, is answered ERR_INVALID_ARGS (with its txid, if it
 * has one) and dropped; one under 4 bytes is dropped unanswered.
 *
 * A reply waits (m->reply) for the loop's next system call that can carry
 * it: the next take, which reads the next request in the same call
 * (channel_reply_wait), or the port wait (main.c). A synchronous request
 * then costs three system calls: the port wait, the take, and the take
 * that finds the channel empty and sends the reply. The kernel marks the
 * reply out in its slot in that call, so a successor answers it again
 * only if it never went. */
#include <keep.h>
#include "internal.h"

/* The biggest reply of either protocol (svc.connect's is smaller). */
#define REP_CAP (AUDIO_REP_MAX > AUDIOCTL_REP_MAX ? AUDIO_REP_MAX : AUDIOCTL_REP_MAX)

_Static_assert(REQ_CAP_AUDIO >= REQ_CAP_CTL, "the slot holds the bigger request");
_Static_assert(MIXER_MAX_STREAMS <= KEEP_CLIENT - KEEP_STREAM, "stream keep slots");
_Static_assert(MIXER_CLIENTS <= KEEP_OUT - KEEP_CLIENT, "opener keep slots");

static const struct svcstate_layout layout = {
    .kind = STATE_KIND,
    .layout = STATE_LAYOUT,
    .req_cap = REQ_CAP_AUDIO,
    .rep_cap = REP_CAP,
    .user_size = sizeof(struct mixer_saved),
};

/* The adopted state's numbers: the last commit, checked, as the working
 * copy. false: none, or one that failed a check (said). */
static bool take_saved(struct mixer_saved *v)
{
    const char *why = NULL;
    if (!v->commits)
        why = "nothing was ever committed";
    else
        why = state_check(&v->copy[v->commits & 1]);
    for (unsigned i = 0; !why && i < DEVICE_QUEUE; i++)
        if (v->dev[i].busy > 1)
            why = "a device request in no known state";
    if (why) {
        printf("mixer: the saved numbers are refused (%s): starting fresh\n", why);
        return false;
    }
    memcpy(&v->work, &v->copy[v->commits & 1], sizeof(v->work));
    return true;
}

status_t state_init(struct mixer *m, bool *adopted)
{
    handle_t vmo = startup_handle(SR_STATE);
    bool given = vmo != HANDLE_INVALID;
    enum svcstate_start how = SVCSTATE_FRESH;
    status_t st = given ? OK : svcstate_create(svcstate_size(&layout), &vmo);
    if (st == OK)
        st = svcstate_open(vmo, &layout, &m->state, &how);
    if (!given && vmo)
        jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return st;
    m->saved = svcstate_user(&m->state);
    m->nums = &m->saved->work;
    *adopted = how == SVCSTATE_ADOPTED && take_saved(m->saved);
    if (!*adopted)   /* a refused state's bytes are whatever it left */
        memset(m->saved, 0, sizeof(*m->saved));
    return OK;
}

void state_commit(struct mixer *m)
{
    struct mixer_saved *v = m->saved;
    uint64_t next = v->commits + 1;
    memcpy(&v->copy[next & 1], &v->work, sizeof(v->work));
    __atomic_store_n(&v->commits, next, __ATOMIC_RELEASE);
}

/* ---- the checks ------------------------------------------------------------------ */

static bool name_ok(const char *name, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if (!name[i])
            return true;
    return false;
}

static const char *check_stream(const struct stream *s)
{
    if (!s->used)
        return NULL;
    if (s->nhist > HIST)
        return "a stream's history too long";
    if (s->owner > MIXER_CLIENTS)
        return "a stream of no opener";
    if (s->volume < MIX_VOLUME_MIN || s->volume > MIX_VOLUME_MAX || s->gain > MIX_UNITY)
        return "a stream's volume out of range";
    if (!name_ok(s->name, sizeof(s->name)))
        return "a stream's name unterminated";
    for (unsigned k = 0; k < s->nhist; k++)
        if (s->hist[k].n > PERIOD_MAX)
            return "a stream's history entry too big";
    return NULL;
}

static const char *check_out(const struct out *o)
{
    if (!o->frames)
        return NULL;   /* never opened */
    if (!((o->bits == 16 && o->frame_bytes == 4) ||
          (o->bits >= 20 && o->bits <= 32 && o->frame_bytes == 8)))
        return "an output format it never uses";
    if (!o->period || o->period > PERIOD_MAX || o->frames % o->period ||
        o->frames < (OUT_LEAD + 1) * o->period || (uint64_t)o->frames * o->frame_bytes > UINT32_MAX)
        return "an output ring it can't use";
    if (!(o->lim.gain >= 0.0f && o->lim.gain <= 1.0f))   /* NaN fails too */
        return "the limiter's gain out of range";
    for (unsigned i = 0; i < 2 * MIX_LOOKAHEAD; i++) {
        int32_t v = o->lim.held[i];
        if (v > MIX_FULL * MIX_MAX_INPUTS || v < MIX_FLOOR * MIX_MAX_INPUTS)
            return "the limiter holds samples out of range";
    }
    return NULL;
}

const char *state_check(const struct mixer_state *n)
{
    if (n->master < MIX_VOLUME_MIN || n->master > MIX_VOLUME_MAX || n->master_gain > MIX_UNITY)
        return "the master volume out of range";
    if (n->made_kind > MADE_CLIENT ||
        n->made_index >= (n->made_kind == MADE_CLIENT ? MIXER_CLIENTS : MIXER_MAX_STREAMS))
        return "a made object out of range";
    for (unsigned i = 0; i < MIXER_MAX_STREAMS; i++) {
        const char *why = check_stream(&n->s[i]);
        if (why)
            return why;
    }
    return check_out(&n->out);
}

/* ---- the keeper ------------------------------------------------------------------- */

status_t keep_slot_put(struct mixer *m, uint32_t slot, const handle_t *hs, unsigned n)
{
    if (!m->keep)
        return OK;
    status_t st = keep_put(m->keep, slot, hs, n);
    if (st != OK)
        printf("mixer: the keeper can't keep slot %u (%s): refused\n", slot, status_str(st));
    return st;
}

void keep_slot_drop(struct mixer *m, uint32_t slot)
{
    if (!m->keep)
        return;
    status_t st = keep_drop(m->keep, slot);
    if (st != OK)   /* it holds the slot until it gives the mixer up: a client's end stays */
        printf("mixer: the keeper can't be told to drop slot %u (%s)\n", slot, status_str(st));
}

/* ---- requests in slots ------------------------------------------------------------ */

status_t req_take(struct mixer *m, handle_t ch, uint32_t key, bool ctl, unsigned *slot)
{
    unsigned i;
    struct idl_slot is;
    svcstate_prepare(&m->state, key, &i, &is);
    /* The protocol's generated take, which sends the reply waiting first: a
     * message too long for the slot is answered ERR_INVALID_ARGS there, one
     * under 4 bytes (no txid) dropped. */
    status_t st = ctl ? audioctl_take_slot(ch, &is, &m->reply)
                      : audio_take_slot(ch, &is, &m->reply);
    *slot = REQ_NONE;
    if (!svcstate_taken(&m->state, i) || st != OK)
        return st;
    uint32_t n = *is.n, nh = *is.nh;
    if (nh || n > (ctl ? REQ_CAP_CTL : REQ_CAP_AUDIO)) {
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
    (void)idl_reply_flush(&m->reply);   /* one waits at a time: a take sent the last one */
    svcstate_answer(&m->state, slot, ch, hs, nh, &m->reply);
}

status_t req_budget_spent(struct mixer *m)
{
    if (m->reply.ch == HANDLE_INVALID)
        return OK;   /* nothing waits: more may be queued */
    return idl_reply_flush(&m->reply) ? OK : ERR_SHOULD_WAIT;
}

void req_status(struct mixer *m, unsigned slot, handle_t ch, status_t st)
{
    const struct idl_req_hdr *q = svcstate_request(&m->state, slot, NULL);
    struct idl_rep_hdr *r = svcstate_reply_area(&m->state, slot);
    r->txid = q->txid;
    r->status = st;
    req_answer(m, slot, ch, sizeof(*r), NULL, 0);
}
