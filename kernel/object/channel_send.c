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
        if (c->txid == txid && !c->any)
            return c;
    }
    return NULL;
}

/* peer's lock held: the reader (channel_read_wait) that has waited on
 * peer longest, or NULL. */
static struct chan_waiter *find_reader_locked(struct channel *peer)
{
    for (struct list_node *n = peer->callers.next; n != &peer->callers; n = n->next) {
        struct chan_waiter *c = container_of(n, struct chan_waiter, node);
        if (c->any)
            return c;
    }
    return NULL;
}

/* peer's lock held, a message just queued on it: unlist and wake every
 * reader waiting there, so each reads the queue in order instead of
 * waiting for a later message (chan_waiter's rule). */
static void wake_readers_locked(struct channel *peer)
{
    for (struct list_node *n = peer->callers.next; n != &peer->callers;) {
        struct chan_waiter *c = container_of(n, struct chan_waiter, node);
        n = n->next;
        if (!c->any)
            continue;
        chan_waiter_unlist_locked(peer, c);
        thread_wake(c->thread);
    }
}

/* A slot message goes to waiter w: take w's free slot for ours, so each
 * thread still has one (we have none now: the message was built in it).
 * The peer's lock is held, so w (still listed) is not looking at w->slot. */
static void swap_slots_locked(struct chan_waiter *w)
{
    struct thread *me = current_thread();
    if (me->msg_slot)
        return;   /* we have one already: w keeps its own */
    me->msg_slot = w->slot;
    w->slot = NULL;
}

/* peer's lock held: give m to waiter w, take w off the list and wake it
 * (`sync`: wake-affine). The message is published last, with a release
 * store, after the wake: a waiter that finds it there returns without
 * taking the endpoint's lock again (thread_block_cancellable_unlocked), and
 * then its chan_waiter (on its stack) and maybe the thread itself are gone,
 * so nothing here may touch either after that store. A waiter woken
 * before it (by the wake, a deadline or a cancel) doesn't find it and takes
 * the lock, which we hold until it is there. */
static void hand_to_locked(struct channel *peer, struct chan_waiter *w, struct chan_msg *m,
                           bool sync)
{
    struct thread *t = w->thread;
    if (m->slot)
        swap_slots_locked(w);
    chan_waiter_unlist_locked(peer, w);
    if (sync)
        thread_wake_sync(t);
    else
        thread_wake(t);
    DBG_HOOK(DBG_CHANNEL_HANDED, t);
    __atomic_store_n(&w->reply, m, __ATOMIC_RELEASE);
}

/* peer's lock held (and the pair lock): give m to the caller waiting for
 * its txid; else, if nothing is queued, to the reader waiting longest, if
 * it fits; else queue it. *filled: the queue just became full. A slot
 * message is never queued: *must_queue instead (and nothing changes). */
static status_t hand_over_locked(struct channel *ch, struct channel *peer, struct chan_msg *m,
                                 bool *filled, bool *must_queue)
{
    uint32_t txid = msg_txid(m);
    struct chan_waiter *w = txid ? find_caller_locked(peer, txid) : NULL;
    if (w) {
        /* A reply to a channel_call. If no other request is queued for us,
         * we (the server) are most likely about to block for the next one,
         * so the caller may take our CPU (wake-affine; a guess: a server
         * that goes on to other work instead just delays it until the next
         * tick or steal). Our queue count is read without our lock: a stale
         * answer only changes the placement. */
        hand_to_locked(peer, w, m, !__atomic_load_n(&ch->nqueued, __ATOMIC_RELAXED));
        return OK;
    }
    w = peer->nqueued ? NULL : find_reader_locked(peer);
    if (w && m->nbytes <= w->bytes_cap && m->nhandles <= w->handles_cap) {
        /* A plain wake: wake-affine only for a writer that blocks next and
         * said so (channel_call's request, thread_set_wake_sync). */
        hand_to_locked(peer, w, m, false);
        return OK;
    }
    if (m->slot) {
        *must_queue = true;
        return OK;
    }
    if (peer->nqueued >= CHANNEL_MAX_QUEUED)
        return ERR_SHOULD_WAIT;
    list_add_tail(&peer->queue, &m->node);
    *filled = ++peer->nqueued == CHANNEL_MAX_QUEUED;
    if (w)
        wake_readers_locked(peer);   /* it didn't fit the reader: it reads it from the queue */
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
 * its txid. On success the message belongs to the peer, unless
 * *must_queue: a slot message nobody waits for, still the caller's, as it
 * is on failure. */
static status_t send_once(struct channel *ch, struct chan_msg *m, bool *must_queue)
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
        st = hand_over_locked(ch, peer, m, &filled, must_queue);
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

/* send_once, and if *mp is a slot message whose reader stopped waiting
 * (or waits for another txid), queue a charged copy of it in its place
 * (*mp becomes that copy). On failure *mp is still the caller's. */
static status_t send_msg(struct channel *ch, struct chan_msg **mp)
{
    bool must_queue = false;
    status_t st = send_once(ch, *mp, &must_queue);
    if (st != OK || !must_queue)
        return st;
    struct chan_msg *q;
    st = chan_msg_unslot(*mp, &q);
    if (st != OK)
        return st;
    *mp = q;
    return send_once(ch, q, &must_queue);   /* q is no slot: never must_queue */
}

status_t channel_write_from(struct channel *ch, const struct chan_bytes *b,
                            struct khandle *handles, uint32_t nhandles)
{
    struct chan_msg *m;
    status_t st = chan_peer_waits(ch) ? chan_msg_new_handed(b, 0, handles, nhandles, &m)
                                      : chan_msg_new(b, 0, handles, nhandles, &m);
    if (st != OK)
        return st;
    st = send_msg(ch, &m);
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

/* Send the request *mp with w listed on ch first, so even an instant
 * reply finds it, and our free slot on offer with it (see channel.c's
 * "Slots"; none if the request was built in it). On failure nothing was
 * sent, w is unlisted, the slot is ours again and *mp (send_msg may have
 * replaced it) is still the caller's. */
static status_t send_request(struct channel *ch, struct chan_waiter *w, struct chan_msg **mp)
{
    struct chan_msg *mine = chan_slot_take();
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    if (ch->closed) {
        spin_unlock_irqrestore(&ch->base.lock, f);
        chan_slot_keep(mine);
        return ERR_BAD_STATE;
    }
    w->slot = mine;
    chan_waiter_list_locked(ch, w);
    spin_unlock_irqrestore(&ch->base.lock, f);

    /* We block for the reply right after sending, so the server this wakes
     * (through whatever observer it waits with) may run on our CPU, handed
     * it straight (channel_call_with ends the hand-off). */
    thread_set_wake_sync(true);
    thread_set_handoff(true);
    status_t st = send_msg(ch, mp);
    thread_set_handoff(false);
    thread_set_wake_sync(false);
    if (st != OK) {
        f = spin_lock_irqsave(&ch->base.lock);
        if (w->node.next)
            chan_waiter_unlist_locked(ch, w);
        mine = w->slot;   /* NULL if a writer took it */
        w->slot = NULL;
        spin_unlock_irqrestore(&ch->base.lock, f);
        chan_slot_keep(mine);
        struct chan_msg *r = chan_handed(w);
        if (r) {   /* the peer guessed our txid before we even sent */
            chan_msg_drop(r);
            w->reply = NULL;
        }
    }
    return st;
}

/* wait_reply's loop, ch's lock held (*f its flags): OK once w has its
 * reply, else the error that ends the wait. *locked: whether the lock is
 * still held on return. A reply handed over while we slept is found
 * without taking the lock again (chan_handed: the writer published it
 * last, and had taken us off the list already). */
static status_t wait_reply_locked(struct channel *ch, struct chan_waiter *w, uint64_t *f,
                                  uint64_t deadline_ns, bool *locked)
{
    *locked = true;
    while (!chan_handed(w)) {
        if (ch->closed)
            return ERR_CANCELED;
        if (ch->peer_closed)
            return ERR_PEER_CLOSED;
        if (deadline_ns != DEADLINE_NEVER && uptime_ns() >= deadline_ns)
            return ERR_TIMED_OUT;
        status_t bs = thread_block_cancellable_unlocked(&ch->base.lock, *f, deadline_ns);
        if (chan_handed(w)) {
            *locked = false;
            return OK;
        }
        *f = spin_lock_irqsave(&ch->base.lock);
        if (bs != OK && !chan_handed(w))
            return ERR_CANCELED;
    }
    return OK;
}

/* Wait for w's reply. OK: it is in w->reply. Our free slot stays on offer
 * to a writer that hands us a reply built in its own; if the request was
 * built in ours, the slot its reader gave us in exchange is offered now.
 * Whatever is left of the offer comes back to us after. */
static status_t wait_reply(struct channel *ch, struct chan_waiter *w, uint64_t deadline_ns)
{
    struct chan_msg *mine = chan_slot_take();
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    if (!w->slot && !chan_handed(w)) {
        w->slot = mine;
        mine = NULL;
    }
    bool locked;
    status_t st = wait_reply_locked(ch, w, &f, deadline_ns, &locked);
    if (locked && w->node.next)   /* not answered: still listed */
        chan_waiter_unlist_locked(ch, w);
    struct chan_msg *offered = w->slot;   /* NULL if a writer took it */
    w->slot = NULL;
    if (locked)
        spin_unlock_irqrestore(&ch->base.lock, f);
    chan_slot_keep(mine);
    chan_slot_keep(offered);
    PATH_MARK(PATH_MK_REPLY);
    return chan_handed(w) ? OK : st;
}

status_t channel_call_with(struct channel *ch, struct chan_call *c, uint64_t deadline_ns)
{
    PATH_MARK(PATH_MK_CALL);
    const struct chan_bytes *q = &c->req;
    if ((!q->user && !q->addr) || q->len < 4 || (c->rep.len && !c->rep.user && !c->rep.addr) ||
        (c->rep_hcap && !c->rep_h))
        return ERR_INVALID_ARGS;
    struct chan_waiter w = { .txid = new_txid(), .thread = current_thread() };
    if (!q->user)
        memcpy((void *)(uintptr_t)q->addr, &w.txid, 4);   /* kernel callers read it back */
    struct chan_msg *m;
    status_t st = chan_peer_waits(ch) ? chan_msg_new_handed(q, w.txid, c->req_h, c->req_nh, &m)
                                      : chan_msg_new(q, w.txid, c->req_h, c->req_nh, &m);
    if (st != OK)
        return st;
    st = send_request(ch, &w, &m);
    if (st != OK) {
        sched_handoff_done();
        chan_msg_free(m);   /* its handle copies were never the message's */
        return st;
    }
    for (uint32_t i = 0; i < c->req_nh; i++)
        c->req_h[i].obj = NULL;   /* moved into the message */

    st = wait_reply(ch, &w, deadline_ns);
    sched_handoff_done();   /* a reply that was there at once: we never blocked */
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
