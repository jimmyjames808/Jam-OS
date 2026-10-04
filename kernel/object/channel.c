/* Channels.
 *
 * The two endpoints share a struct chan_pair whose lock guards the two
 * endpoint pointers. An endpoint leaves the pair when it closes, so while
 * the pair lock is held, any endpoint still in it is alive: that is how a
 * writer reaches its peer without holding a reference on it, and why the
 * endpoints need no references on each other (which would be a cycle).
 *
 * Lock order: "channel carry" (only for a message that carries an
 * endpoint: see check_carried) -> "channel pair" -> "channel" (one
 * endpoint's object lock) -> whatever observers take. Only one endpoint
 * lock is ever held at a time.
 * Nothing that can close a channel (khandle_release, kobject_unref) runs
 * under either lock: dropped messages are freed after unlocking, because
 * their handles may be the last ones to another channel.
 *
 * Each endpoint's object lock guards its message queue, its list of
 * channel_call waiters and its closed / peer_closed flags. */
#include <jam/channel.h>
#include <jam/dbghook.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>

struct chan_pair {
    spinlock_t        lock;    /* "channel pair": guards both endpoints' queues and state */
    struct channel   *ep[2];   /* NULL once that endpoint has closed */
    uint32_t          refs;    /* one per endpoint not yet destroyed */
};

/* One queued message. The khandles follow the header, then the bytes. */
struct chan_msg {
    struct list_node node;      /* on the receiver's queue */
    uint32_t         nbytes;    /* message bytes */
    uint32_t         nhandles;  /* khandles carried */
    struct job      *job;       /* the sender's job, charged `charge` bytes (a reference) */
    uint64_t         charge;    /* bytes charged to job */
};

static inline struct khandle *msg_handles(struct chan_msg *m)
{
    return (struct khandle *)(m + 1);
}

static inline uint8_t *msg_bytes(struct chan_msg *m)
{
    return (uint8_t *)(msg_handles(m) + m->nhandles);
}

/* A thread inside channel_call, waiting on its own endpoint for the reply
 * carrying txid. The writer that delivers the reply unlinks it. */
struct chan_waiter {
    struct list_node node;     /* on the endpoint's callers list */
    uint32_t         txid;     /* the reply it waits for */
    struct thread   *thread;   /* the caller */
    struct chan_msg *reply;    /* set by the writer that delivers it */
};

struct channel {
    struct kobject    base;          /* OBJ_CHANNEL */
    struct chan_pair *pair;          /* shared with the peer */
    uint32_t          side;          /* our index in pair->ep */
    struct list_node  queue;         /* chan_msg, oldest first */
    uint32_t          nqueued;       /* messages on queue (at most CHANNEL_MAX_QUEUED) */
    struct list_node  callers;       /* chan_waiter */
    bool              closed;        /* we left the pair */
    bool              peer_closed;   /* the peer left the pair */
};

static uint64_t live_endpoints;
static uint32_t next_txid;
/* Held by a send that carries a channel endpoint from its cycle check to
 * its queueing (check_carried). */
static spinlock_t carry_lock = SPINLOCK_INIT("channel carry");

uint64_t channel_live_count(void)
{
    return __atomic_load_n(&live_endpoints, __ATOMIC_RELAXED);
}

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

/* ---- messages ------------------------------------------------------------- */

/* Every message (its whole allocation, handles included, plus
 * JOB_OBJECT_BYTES per handle for the object it may keep alive) is charged
 * to the SENDER's job (JOB_LIMIT_MSG_BYTES) from creation until it is freed, so a
 * process can't pin kernel memory by filling a peer's queues beyond its
 * job's limit; the charge follows the message, not the queue it sits on.
 * Messages made by kernel threads have no job. */

/* Free a message's memory and credit its sender's job. Its handles are
 * the caller's business. No locks held (the job reference may be the last). */
static void msg_free(struct chan_msg *m)
{
    job_uncharge(m->job, JOB_LIMIT_MSG_BYTES, m->charge);
    job_unref(m->job);
    kfree(m);
}

/* Copy bytes and handles into a new message. The handles are copied, not
 * taken: the caller clears its own array once the message is delivered. */
static status_t msg_new(const void *bytes, uint32_t nbytes, const struct khandle *handles,
                        uint32_t nhandles, struct chan_msg **out)
{
    if (nbytes > CHANNEL_MAX_BYTES || nhandles > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    if ((nbytes && !bytes) || (nhandles && !handles))
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < nhandles; i++)
        if (!handles[i].obj)
            return ERR_INVALID_ARGS;
    uint64_t size = sizeof(struct chan_msg) + nhandles * sizeof(struct khandle) + nbytes;
    /* Each handle also pays for the object it names: once the sender closes
     * its own handles, the message may be all that keeps it alive. */
    uint64_t charge = size + (uint64_t)nhandles * JOB_OBJECT_BYTES;
    struct job *job = job_current();
    status_t st = job_charge(job, JOB_LIMIT_MSG_BYTES, charge);
    if (st != OK)
        return st;
    struct chan_msg *m = kmalloc(size);
    if (!m) {
        job_uncharge(job, JOB_LIMIT_MSG_BYTES, charge);
        return ERR_NO_MEMORY;
    }
    job_ref(job);
    m->job = job;
    m->charge = charge;
    m->node.next = m->node.prev = NULL;
    m->nbytes = nbytes;
    m->nhandles = nhandles;
    if (nhandles)
        memcpy(msg_handles(m), handles, nhandles * sizeof(struct khandle));
    if (nbytes) {
        PATH_COUNT(PATH_KCOPY);
        PATH_ADD(PATH_KCOPY_B, nbytes);
        memcpy(msg_bytes(m), bytes, nbytes);
    }
    PATH_MARK(PATH_MK_MSG_MADE);
    *out = m;
    return OK;
}

/* Free a message and every handle it still owns. No locks held. */
static void msg_drop(struct chan_msg *m)
{
    for (uint32_t i = 0; i < m->nhandles; i++)
        khandle_release(&msg_handles(m)[i]);
    msg_free(m);
}

/* Hand a message's contents to a reader, which now owns the handles. */
static void msg_deliver_to(struct chan_msg *m, void *bytes, struct khandle *handles)
{
    if (m->nbytes) {
        PATH_COUNT(PATH_KCOPY);
        PATH_ADD(PATH_KCOPY_B, m->nbytes);
        memcpy(bytes, msg_bytes(m), m->nbytes);
    }
    if (m->nhandles)
        memcpy(handles, msg_handles(m), m->nhandles * sizeof(struct khandle));
    msg_free(m);
}

/* ---- closing ---------------------------------------------------------------- */

/* With ch->base.lock held: kick every channel_call waiting on ch so it
 * re-checks closed / peer_closed. Waiters unlink themselves. */
static void wake_callers_locked(const struct channel *ch)
{
    for (struct list_node *n = ch->callers.next; n != &ch->callers; n = n->next)
        thread_wake(container_of(n, struct chan_waiter, node)->thread);
}

/* Leave the pair: tell the peer, then empty our own queue. Idempotent. */
static void channel_close(struct channel *ch)
{
    struct chan_pair *pair = ch->pair;
    uint64_t f = spin_lock_irqsave(&pair->lock);
    if (pair->ep[ch->side] != ch) {
        spin_unlock_irqrestore(&pair->lock, f);
        return;
    }
    pair->ep[ch->side] = NULL;
    struct channel *peer = pair->ep[!ch->side];
    if (peer) {
        spin_lock(&peer->base.lock);
        peer->peer_closed = true;
        wake_callers_locked(peer);
        kobject_signal_locked(&peer->base, SIG_WRITABLE, SIG_PEER_CLOSED);
        spin_unlock(&peer->base.lock);
    }
    spin_unlock_irqrestore(&pair->lock, f);

    /* No writer can reach our queue any more: it had to find us in the
     * pair. Take the queued messages out and free them unlocked. */
    struct list_node dead;
    list_init(&dead);
    f = spin_lock_irqsave(&ch->base.lock);
    ch->closed = true;
    if (!list_empty(&ch->queue)) {
        dead.next = ch->queue.next;
        dead.prev = ch->queue.prev;
        dead.next->prev = &dead;
        dead.prev->next = &dead;
        list_init(&ch->queue);
    }
    ch->nqueued = 0;
    wake_callers_locked(ch);
    kobject_signal_locked(&ch->base, SIG_READABLE | SIG_WRITABLE, 0);
    spin_unlock_irqrestore(&ch->base.lock, f);

    while (!list_empty(&dead)) {
        struct chan_msg *m = list_first(&dead, struct chan_msg, node);
        list_del(&m->node);
        msg_drop(m);
    }
}

static void channel_on_zero_handles(struct kobject *obj)
{
    channel_close((struct channel *)obj);
}

static void channel_destroy(struct kobject *obj)
{
    struct channel *ch = (struct channel *)obj;
    channel_close(ch);
    ASSERT(list_empty(&ch->callers));
    if (__atomic_sub_fetch(&ch->pair->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(ch->pair);
    __atomic_sub_fetch(&live_endpoints, 1, __ATOMIC_RELAXED);
    kfree(ch);
}

static const struct kobject_ops channel_ops = {
    .name = "channel",
    .destroy = channel_destroy,
    .on_zero_handles = channel_on_zero_handles,
};

status_t channel_create(struct channel **a, struct channel **b)
{
    struct chan_pair *pair = kzalloc(sizeof(*pair));
    struct channel *ep[2] = { kzalloc(sizeof(struct channel)), kzalloc(sizeof(struct channel)) };
    if (!pair || !ep[0] || !ep[1]) {
        kfree(pair);
        kfree(ep[0]);
        kfree(ep[1]);
        return ERR_NO_MEMORY;
    }
    spin_init(&pair->lock, "channel pair");
    pair->refs = 2;
    for (int i = 0; i < 2; i++) {
        kobject_init(&ep[i]->base, OBJ_CHANNEL, &channel_ops, "channel", SIG_WRITABLE);
        ep[i]->pair = pair;
        ep[i]->side = i;
        list_init(&ep[i]->queue);
        list_init(&ep[i]->callers);
        pair->ep[i] = ep[i];
    }
    __atomic_add_fetch(&live_endpoints, 2, __ATOMIC_RELAXED);
    *a = ep[0];
    *b = ep[1];
    return OK;
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

status_t channel_write(struct channel *ch, const void *bytes, uint32_t nbytes,
                       struct khandle *handles, uint32_t nhandles)
{
    struct chan_msg *m;
    status_t st = msg_new(bytes, nbytes, handles, nhandles, &m);
    if (st != OK)
        return st;
    st = send_msg(ch, m);
    if (st != OK) {
        msg_free(m);   /* its handle copies were never the message's */
        return st;
    }
    for (uint32_t i = 0; i < nhandles; i++)
        handles[i].obj = NULL;   /* moved into the message */
    return OK;
}

/* ---- reading ------------------------------------------------------------------ */

/* A read just made room in ch's full queue: the peer (the writer) may be
 * writable again. Recomputed from scratch under the pair lock, like the
 * writer's clear in send_msg, so whichever of the two runs last leaves the
 * right answer. One "channel" lock at a time, as everywhere. */
static void refresh_writable(struct channel *ch)
{
    struct chan_pair *pair = ch->pair;
    uint64_t f = spin_lock_irqsave(&pair->lock);
    struct channel *writer = pair->ep[!ch->side];
    if (writer && pair->ep[ch->side] == ch) {
        spin_lock(&ch->base.lock);
        bool full = ch->nqueued >= CHANNEL_MAX_QUEUED;
        spin_unlock(&ch->base.lock);
        spin_lock(&writer->base.lock);
        kobject_signal_locked(&writer->base, full ? SIG_WRITABLE : 0, full ? 0 : SIG_WRITABLE);
        spin_unlock(&writer->base.lock);
    }
    spin_unlock_irqrestore(&pair->lock, f);
}

status_t channel_read(struct channel *ch, void *bytes, uint32_t bytes_cap, uint32_t *actual_bytes,
                      struct khandle *handles, uint32_t handles_cap, uint32_t *actual_handles)
{
    if ((bytes_cap && !bytes) || (handles_cap && !handles))
        return ERR_INVALID_ARGS;
    struct chan_msg *m = NULL;
    uint32_t nb = 0, nh = 0;
    status_t st = OK;
    bool was_full = false;
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    if (ch->closed) {
        st = ERR_BAD_STATE;
    } else if (list_empty(&ch->queue)) {
        st = ch->peer_closed ? ERR_PEER_CLOSED : ERR_SHOULD_WAIT;
    } else {
        m = list_first(&ch->queue, struct chan_msg, node);
        nb = m->nbytes;
        nh = m->nhandles;
        if (nb > bytes_cap || nh > handles_cap) {
            st = ERR_BUFFER_TOO_SMALL;   /* stays queued */
            m = NULL;
        } else {
            list_del(&m->node);
            was_full = ch->nqueued == CHANNEL_MAX_QUEUED;
            if (--ch->nqueued == 0)
                kobject_signal_locked(&ch->base, SIG_READABLE, 0);
        }
    }
    spin_unlock_irqrestore(&ch->base.lock, f);
    if (actual_bytes)
        *actual_bytes = nb;
    if (actual_handles)
        *actual_handles = nh;
    if (was_full)
        refresh_writable(ch);
    if (m)
        msg_deliver_to(m, bytes, handles);
    if (st == ERR_SHOULD_WAIT)
        PATH_COUNT(PATH_EMPTY_READ);
    PATH_MARK_ARG(PATH_MK_READ, st == ERR_SHOULD_WAIT);
    return st;
}

void channel_queued(struct channel *ch, uint32_t *msgs, uint64_t *charged)
{
    uint64_t sum = 0;
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    for (struct list_node *n = ch->queue.next; n != &ch->queue; n = n->next)
        sum += container_of(n, struct chan_msg, node)->charge;
    *msgs = ch->nqueued;
    spin_unlock_irqrestore(&ch->base.lock, f);
    *charged = sum;
}

/* ---- call ---------------------------------------------------------------------- */

status_t channel_call(struct channel *ch, void *wbytes, uint32_t wn, struct khandle *wh,
                      uint32_t whn, void *rbytes, uint32_t rcap, uint32_t *ractual,
                      struct khandle *rh, uint32_t rhcap, uint32_t *rhactual,
                      uint64_t deadline_ns)
{
    PATH_MARK(PATH_MK_CALL);
    if (!wbytes || wn < 4 || (rcap && !rbytes) || (rhcap && !rh))
        return ERR_INVALID_ARGS;
    struct chan_waiter w = { .txid = new_txid(), .thread = current_thread(), .reply = NULL };
    memcpy(wbytes, &w.txid, 4);
    struct chan_msg *m;
    status_t st = msg_new(wbytes, wn, wh, whn, &m);
    if (st != OK)
        return st;

    /* Wait on our endpoint before the request goes out, so even an instant
     * reply finds us. */
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    if (ch->closed) {
        spin_unlock_irqrestore(&ch->base.lock, f);
        msg_free(m);
        return ERR_BAD_STATE;
    }
    list_add_tail(&ch->callers, &w.node);
    spin_unlock_irqrestore(&ch->base.lock, f);

    /* We block for the reply right after sending, so the server this wakes
     * (through whatever observer it waits with) may run on our CPU. */
    thread_set_wake_sync(true);
    st = send_msg(ch, m);
    thread_set_wake_sync(false);
    if (st != OK) {
        f = spin_lock_irqsave(&ch->base.lock);
        if (w.node.next)
            list_del(&w.node);
        spin_unlock_irqrestore(&ch->base.lock, f);
        msg_free(m);
        if (w.reply)   /* the peer guessed our txid before we even sent */
            msg_drop(w.reply);
        return st;
    }
    for (uint32_t i = 0; i < whn; i++)
        wh[i].obj = NULL;

    f = spin_lock_irqsave(&ch->base.lock);
    while (!w.reply) {
        if (ch->closed) {
            st = ERR_CANCELED;
            break;
        }
        if (ch->peer_closed) {
            st = ERR_PEER_CLOSED;
            break;
        }
        if (deadline_ns != DEADLINE_NEVER && uptime_ns() >= deadline_ns) {
            st = ERR_TIMED_OUT;
            break;
        }
        if (thread_block_cancellable(&ch->base.lock, &f, deadline_ns) != OK && !w.reply) {
            st = ERR_CANCELED;
            break;
        }
    }
    if (w.node.next)   /* not answered: still listed */
        list_del(&w.node);
    spin_unlock_irqrestore(&ch->base.lock, f);
    PATH_MARK(PATH_MK_REPLY);

    struct chan_msg *r = w.reply;
    if (!r)
        return st;
    if (ractual)
        *ractual = r->nbytes;
    if (rhactual)
        *rhactual = r->nhandles;
    if (r->nbytes > rcap || r->nhandles > rhcap) {
        msg_drop(r);
        return ERR_BUFFER_TOO_SMALL;
    }
    msg_deliver_to(r, rbytes, rh);
    return OK;
}
