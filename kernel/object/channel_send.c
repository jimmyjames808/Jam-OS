/* Channels: writing and channel_call. A write queues a message on the
 * peer endpoint, or hands it straight to the channel_call there waiting
 * for its txid. The model and the lock order are in channel.c's header. */
#include <jam/channel.h>
#include <jam/dbghook.h>
#include <jam/pathstat.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>

#include "channel_internal.h"

static uint32_t next_txid;
/* Held by a send that carries a channel endpoint from its cycle check to
 * its queueing (check_carried). */
static spinlock_t carry_lock = SPINLOCK_INIT("channel carry");

/* Globally unique (until it wraps), so two endpoints calling each other
 * can't mistake one another's requests for replies. Never 0. */
static uint32_t new_txid(void)
{
    uint32_t t;
    do
        t = __atomic_add_fetch(&next_txid, 1, __ATOMIC_RELAXED);
    while (t == 0);
    return t;
}

static uint32_t msg_txid(struct chan_msg *m)
{
    uint32_t t = 0;
    if (m->nbytes >= 4)
        memcpy(&t, msg_bytes(m), 4);
    return t;
}

/* ---- writing ------------------------------------------------------------------ */

/* With no lock held on entry: does ep's message queue currently hold any
 * channel endpoint? Used to break reference cycles (see send_msg). */
static bool queue_holds_channel(struct channel *ep)
{
    bool found = false;
    uint64_t f = spin_lock_irqsave(&ep->base.lock);
    for (struct list_node *n = ep->queue.next; n != &ep->queue && !found; n = n->next) {
        struct chan_msg *qm = container_of(n, struct chan_msg, node);
        for (uint32_t i = 0; i < qm->nhandles; i++)
            if (msg_handles(qm)[i].obj->type == OBJ_CHANNEL) {
                found = true;
                break;
            }
    }
    spin_unlock_irqrestore(&ep->base.lock, f);
    return found;
}

/* ERR_NOT_SUPPORTED if m carries something that can't be sent on ch, else
 * OK. Called with pair->lock held. */
static status_t check_carried(struct channel *ch, struct channel *peer, struct chan_msg *m)
{
    for (uint32_t i = 0; i < m->nhandles; i++) {
        struct kobject *o = msg_handles(m)[i].obj;
        if (o == &ch->base || o == &peer->base)
            return ERR_NOT_SUPPORTED;
        /* Refuse to queue a channel endpoint whose own queue already holds a
         * channel endpoint. Every reference cycle among channels has to be
         * closed by an edge that carries such an endpoint (the one whose
         * queue already holds the previous member of the cycle), so rejecting
         * exactly these edges makes cycles -- direct or indirect, of any
         * length -- impossible, while sending an endpoint that only has plain
         * messages, or non-channel handles, queued still works.
         * The check reads o's queue, which another pair's sender may be
         * filling right now: two sends that each carry one endpoint into
         * the other's queue would each see that queue still empty and close
         * a 2-cycle together. So every send that carries an endpoint holds
         * carry_lock from this check until its message is queued (test:
         * channel_cycle_check_races_refused); queueing anything else adds
         * no endpoint to any queue.
         * We take o's object lock here having only pair->lock held ("channel
         * pair" -> "channel"), and drop it before locking the peer, so no two
         * "channel" locks are ever held at once. */
        if (o->type == OBJ_CHANNEL && queue_holds_channel((struct channel *)o))
            return ERR_NOT_SUPPORTED;
    }
    return OK;
}

/* peer's lock held: the channel_call waiting on peer for txid, or NULL. */
static struct chan_waiter *find_caller_locked(struct channel *peer, uint32_t txid)
{
    for (struct list_node *n = peer->callers.next; n != &peer->callers; n = n->next) {
        struct chan_waiter *c = container_of(n, struct chan_waiter, node);
        if (c->txid == txid)
            return c;
    }
    return NULL;
}

/* peer's lock held (and the pair lock): give m to the caller waiting for
 * its txid, or queue it. *filled: the queue just became full. */
static status_t hand_over_locked(struct channel *ch, struct channel *peer, struct chan_msg *m,
                                 bool *filled)
{
    uint32_t txid = msg_txid(m);
    struct chan_waiter *w = txid ? find_caller_locked(peer, txid) : NULL;
    if (w) {
        w->reply = m;
        list_del(&w->node);
        /* A reply to a channel_call. If no other request is queued for us,
         * we (the server) are most likely about to block for the next one,
         * so the caller may take our CPU (wake-affine; a guess: a server
         * that goes on to other work instead just delays it until the next
         * tick or steal). Our queue count is read without our lock: a stale
         * answer only changes the placement. */
        if (!__atomic_load_n(&ch->nqueued, __ATOMIC_RELAXED))
            thread_wake_sync(w->thread);
        else
            thread_wake(w->thread);
        return OK;
    }
    if (peer->nqueued >= CHANNEL_MAX_QUEUED)
        return ERR_SHOULD_WAIT;
    list_add_tail(&peer->queue, &m->node);
    *filled = ++peer->nqueued == CHANNEL_MAX_QUEUED;
    kobject_signal_locked(&peer->base, 0, SIG_READABLE);
    return OK;
}

static bool carries_channel(struct chan_msg *m)
{
    for (uint32_t i = 0; i < m->nhandles; i++)
        if (msg_handles(m)[i].obj->type == OBJ_CHANNEL)
            return true;
    return false;
}

/* Queue m on ch's peer, or hand it to the channel_call there waiting for
 * its txid. On success the message belongs to the peer; on failure it is
 * still the caller's. */
static status_t send_msg(struct channel *ch, struct chan_msg *m)
{
    struct chan_pair *pair = ch->pair;
    status_t st;
    bool filled = false;
    bool carry = carries_channel(m);
    uint64_t cf = carry ? spin_lock_irqsave(&carry_lock) : 0;
    uint64_t f = spin_lock_irqsave(&pair->lock);
    struct channel *peer = pair->ep[!ch->side];
    if (pair->ep[ch->side] != ch)
        st = ERR_BAD_STATE;   /* we closed (a racing handle_close) */
    else if (!peer)
        st = ERR_PEER_CLOSED;
    else
        st = check_carried(ch, peer, m);
    if (st == OK) {
        DBG_HOOK(DBG_CHANNEL_CARRIED, ch);
        spin_lock(&peer->base.lock);
        st = hand_over_locked(ch, peer, m, &filled);
        spin_unlock(&peer->base.lock);
    }
    if (filled) {
        /* The peer's queue is full: we are not writable until a read makes
         * room. Still under the pair lock, which orders this against the
         * reader's refresh_writable. */
        spin_lock(&ch->base.lock);
        kobject_signal_locked(&ch->base, SIG_WRITABLE, 0);
        spin_unlock(&ch->base.lock);
    }
    spin_unlock_irqrestore(&pair->lock, f);
    if (carry)
        spin_unlock_irqrestore(&carry_lock, cf);
    PATH_MARK(PATH_MK_SENT);
    return st;
}

status_t channel_write_from(struct channel *ch, const struct chan_bytes *b,
                            struct khandle *handles, uint32_t nhandles)
{
    struct chan_msg *m;
    status_t st = chan_msg_new(b, 0, handles, nhandles, &m);
    if (st != OK)
        return st;
    st = send_msg(ch, m);
    if (st != OK) {
        chan_msg_free(m);   /* its handle copies were never the message's */
        return st;
    }
    for (uint32_t i = 0; i < nhandles; i++)
        handles[i].obj = NULL;   /* moved into the message */
    return OK;
}

status_t channel_write(struct channel *ch, const void *bytes, uint32_t nbytes,
                       struct khandle *handles, uint32_t nhandles)
{
    struct chan_bytes b = chan_kbytes(bytes, nbytes);
    return channel_write_from(ch, &b, handles, nhandles);
}

/* ---- call ---------------------------------------------------------------------- */

/* Send the request with w listed on ch first, so even an instant reply
 * finds it. On failure nothing was sent and w is unlisted. */
static status_t send_request(struct channel *ch, struct chan_waiter *w, struct chan_msg *m)
{
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    if (ch->closed) {
        spin_unlock_irqrestore(&ch->base.lock, f);
        return ERR_BAD_STATE;
    }
    list_add_tail(&ch->callers, &w->node);
    spin_unlock_irqrestore(&ch->base.lock, f);

    /* We block for the reply right after sending, so the server this wakes
     * (through whatever observer it waits with) may run on our CPU. */
    thread_set_wake_sync(true);
    status_t st = send_msg(ch, m);
    thread_set_wake_sync(false);
    if (st != OK) {
        f = spin_lock_irqsave(&ch->base.lock);
        if (w->node.next)
            list_del(&w->node);
        spin_unlock_irqrestore(&ch->base.lock, f);
        if (w->reply) {   /* the peer guessed our txid before we even sent */
            chan_msg_drop(w->reply);
            w->reply = NULL;
        }
    }
    return st;
}

/* Wait for w's reply. OK: it is in w->reply. */
static status_t wait_reply(struct channel *ch, struct chan_waiter *w, uint64_t deadline_ns)
{
    status_t st = OK;
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    while (!w->reply) {
        if (ch->closed) {
            st = ERR_CANCELED;
            break;
        }
        if (ch->peer_closed) {
            st = ERR_PEER_CLOSED;
            break;
        }
        if (uptime_ns() >= deadline_ns) {
            st = ERR_TIMED_OUT;
            break;
        }
        if (thread_block_cancellable(&ch->base.lock, &f, deadline_ns) != OK && !w->reply) {
            st = ERR_CANCELED;
            break;
        }
    }
    if (w->node.next)   /* not answered: still listed */
        list_del(&w->node);
    spin_unlock_irqrestore(&ch->base.lock, f);
    PATH_MARK(PATH_MK_REPLY);
    return w->reply ? OK : st;
}

status_t channel_call_with(struct channel *ch, struct chan_call *c, uint64_t deadline_ns)
{
    PATH_MARK(PATH_MK_CALL);
    const struct chan_bytes *q = &c->req;
    if ((!q->user && !q->addr) || q->len < 4 || (c->rep.len && !c->rep.user && !c->rep.addr) ||
        (c->rep_hcap && !c->rep_h))
        return ERR_INVALID_ARGS;
    struct chan_waiter w = { .txid = new_txid(), .thread = current_thread(), .reply = NULL };
    if (!q->user)
        memcpy((void *)(uintptr_t)q->addr, &w.txid, 4);   /* kernel callers read it back */
    struct chan_msg *m;
    status_t st = chan_msg_new(q, w.txid, c->req_h, c->req_nh, &m);
    if (st != OK)
        return st;
    st = send_request(ch, &w, m);
    if (st != OK) {
        chan_msg_free(m);   /* its handle copies were never the message's */
        return st;
    }
    for (uint32_t i = 0; i < c->req_nh; i++)
        c->req_h[i].obj = NULL;   /* moved into the message */

    st = wait_reply(ch, &w, deadline_ns);
    struct chan_msg *r = w.reply;
    if (st != OK)
        return st;
    c->rep_nb = r->nbytes;
    c->rep_nh = r->nhandles;
    if (r->nbytes > c->rep.len || r->nhandles > c->rep_hcap) {
        chan_msg_drop(r);
        return ERR_BUFFER_TOO_SMALL;
    }
    return chan_msg_deliver(r, &c->rep, c->rep_h);   /* no lock held */
}

status_t channel_call(struct channel *ch, void *wbytes, uint32_t wn, struct khandle *wh,
                      uint32_t whn, void *rbytes, uint32_t rcap, uint32_t *ractual,
                      struct khandle *rh, uint32_t rhcap, uint32_t *rhactual,
                      uint64_t deadline_ns)
{
    struct chan_call c = {
        .req = chan_kbytes(wbytes, wn), .req_h = wh, .req_nh = whn,
        .rep = chan_kbytes(rbytes, rcap), .rep_h = rh, .rep_hcap = rhcap,
    };
    status_t st = channel_call_with(ch, &c, deadline_ns);
    if (st == OK || st == ERR_BUFFER_TOO_SMALL) {
        if (ractual)
            *ractual = c.rep_nb;
        if (rhactual)
            *rhactual = c.rep_nh;
    }
    return st;
}
