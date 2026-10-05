/* Channels: the endpoints, their messages, closing and reading. Writing
 * and channel_call are in channel_send.c; what both files share is in
 * channel_internal.h.
 *
 * The two endpoints share a struct chan_pair whose lock guards the two
 * endpoint pointers. An endpoint leaves the pair when it closes, so while
 * the pair lock is held, any endpoint still in it is alive: that is how a
 * writer reaches its peer without holding a reference on it, and why the
 * endpoints need no references on each other (which would be a cycle).
 *
 * Lock order: "channel carry" (only for a message that carries an
 * endpoint: see check_carried in channel_send.c) -> "channel pair" ->
 * "channel" (one endpoint's object lock) -> whatever observers take. Only
 * one endpoint lock is ever held at a time.
 * Nothing that can close a channel (khandle_release, kobject_unref) runs
 * under either lock: dropped messages are freed after unlocking, because
 * their handles may be the last ones to another channel.
 *
 * Each endpoint's object lock guards its message queue, its list of
 * waiters (channel_call callers and channel_read_wait readers, both
 * struct chan_waiter) and its closed / peer_closed flags. */
#include <jam/atomic.h>
#include <jam/channel.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/usercopy.h>

#include "channel_internal.h"

static uint64_t live_endpoints;

uint64_t channel_live_count(void)
{
    return __atomic_load_n(&live_endpoints, __ATOMIC_RELAXED);
}

/* ---- messages ------------------------------------------------------------- */

/* Every message that can be queued (its whole allocation, handles
 * included, plus JOB_OBJECT_BYTES per handle for the object it may keep
 * alive) is charged to the SENDER's job (JOB_LIMIT_MSG_BYTES) before it is
 * allocated and until it is freed, so a process can't pin kernel memory
 * by filling a peer's queues beyond its job's limit; the charge follows
 * the message, not the queue it sits on. Messages made by kernel threads
 * have no job.
 *
 * Slots. A small message for a reader that is already waiting for it (a
 * reply to a channel_call, or any message for a channel_read_wait reader:
 * a request to a server waiting in channel_reply_wait) is built in its
 * writer's slot instead: one
 * free CHAN_SLOT_SIZE buffer per thread (struct thread's msg_slot), kept
 * from one message to the next. Handing it over swaps slots: the writer
 * takes the waiting reader's free slot (chan_waiter.slot), and the reader
 * keeps the message's once it has read it. So a ping-pong pair allocates
 * nothing after its first round trips, and a slot is never charged:
 * nothing piles up, as a slot message is never queued (one that finds
 * nobody waiting after all is copied into a charged message first:
 * chan_msg_unslot) and each thread holds at most one free slot, like its
 * FPU area. */

bool channel_slots = true;

static uint64_t msg_size(uint32_t nbytes, uint32_t nhandles)
{
    return sizeof(struct chan_msg) + nhandles * sizeof(struct khandle) + nbytes;
}

/* A message of `size` bytes for nhandles handles, allocated and charged:
 * only its header is set. */
static status_t msg_alloc(uint64_t size, uint32_t nhandles, struct chan_msg **out)
{
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
    m->slot = false;
    m->node.next = m->node.prev = NULL;
    *out = m;
    return OK;
}

/* The current thread's slot as a message (a new one if it has none):
 * only its header is set. NULL when out of memory. */
static struct chan_msg *slot_alloc(void)
{
    struct chan_msg *m = chan_slot_take();
    if (!m)
        m = kmalloc(CHAN_SLOT_SIZE);
    if (!m)
        return NULL;
    m->job = NULL;
    m->charge = 0;
    m->slot = true;
    m->node.next = m->node.prev = NULL;
    return m;
}

void chan_msg_free(struct chan_msg *m)
{
    if (m->slot) {
        chan_slot_keep(m);
        return;
    }
    job_uncharge(m->job, JOB_LIMIT_MSG_BYTES, m->charge);
    job_unref(m->job);
    kfree(m);
}

/* Copy n bytes from b, at offset off, to dst. */
static status_t bytes_in(void *dst, const struct chan_bytes *b, uint32_t off, uint32_t n)
{
    if (!n)
        return OK;
    if (b->user)
        return copy_from_user(dst, b->addr + off, n);
    PATH_COUNT(PATH_KCOPY);
    PATH_ADD(PATH_KCOPY_B, n);
    memcpy(dst, (const uint8_t *)(uintptr_t)b->addr + off, n);
    return OK;
}

/* Copy n bytes from src to b. */
static status_t bytes_out(const struct chan_bytes *b, const void *src, uint32_t n)
{
    if (!n)
        return OK;
    if (b->user)
        return copy_to_user(b->addr, src, n);
    PATH_COUNT(PATH_KCOPY);
    PATH_ADD(PATH_KCOPY_B, n);
    memcpy((void *)(uintptr_t)b->addr, src, n);
    return OK;
}

/* chan_msg_new's work: in a slot if `handed` allows and it fits. */
static status_t msg_make(const struct chan_bytes *b, uint32_t txid, const struct khandle *handles,
                         uint32_t nhandles, bool handed, struct chan_msg **out)
{
    uint32_t nbytes = b->len;
    if (nbytes > CHANNEL_MAX_BYTES || nhandles > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    if ((nbytes && !b->user && !b->addr) || (nhandles && !handles) || (txid && nbytes < 4))
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < nhandles; i++)
        if (!handles[i].obj)
            return ERR_INVALID_ARGS;
    uint64_t size = msg_size(nbytes, nhandles);
    struct chan_msg *m = NULL;
    if (handed && size <= CHAN_SLOT_SIZE && __atomic_load_n(&channel_slots, __ATOMIC_RELAXED)) {
        m = slot_alloc();
        if (!m)
            return ERR_NO_MEMORY;
    } else {
        status_t st = msg_alloc(size, nhandles, &m);
        if (st != OK)
            return st;
    }
    m->nbytes = nbytes;
    m->nhandles = nhandles;
    /* No lock is held: a copy from user memory may fault and sleep. */
    if (bytes_in(msg_bytes(m), b, 0, nbytes) != OK) {
        chan_msg_free(m);
        return ERR_INVALID_ARGS;
    }
    /* Over whatever the writer had there, which is never looked at. */
    if (txid)
        memcpy(msg_bytes(m), &txid, 4);
    if (nhandles)
        memcpy(msg_handles(m), handles, nhandles * sizeof(struct khandle));
    PATH_MARK(PATH_MK_MSG_MADE);
    *out = m;
    return OK;
}

status_t chan_msg_new(const struct chan_bytes *b, uint32_t txid, const struct khandle *handles,
                      uint32_t nhandles, struct chan_msg **out)
{
    return msg_make(b, txid, handles, nhandles, false, out);
}

status_t chan_msg_new_handed(const struct chan_bytes *b, uint32_t txid,
                             const struct khandle *handles, uint32_t nhandles,
                             struct chan_msg **out)
{
    return msg_make(b, txid, handles, nhandles, true, out);
}

status_t chan_msg_unslot(struct chan_msg *m, struct chan_msg **out)
{
    struct chan_msg *q;
    status_t st = msg_alloc(msg_size(m->nbytes, m->nhandles), m->nhandles, &q);
    if (st != OK)
        return st;
    q->nbytes = m->nbytes;
    q->nhandles = m->nhandles;
    /* The handles and the bytes are one run after the header. */
    PATH_COUNT(PATH_KCOPY);
    PATH_ADD(PATH_KCOPY_B, m->nbytes);
    memcpy(msg_handles(q), msg_handles(m), m->nhandles * sizeof(struct khandle) + m->nbytes);
    chan_slot_keep(m);
    *out = q;
    return OK;
}

/* ---- slots and waiters -------------------------------------------------------- */

struct chan_msg *chan_slot_take(void)
{
    struct thread *t = current_thread();
    struct chan_msg *m = t->msg_slot;
    t->msg_slot = NULL;
    return m;
}

void chan_slot_keep(struct chan_msg *m)
{
    struct thread *t = current_thread();
    if (!m)
        return;
    if (!t->msg_slot)
        t->msg_slot = m;
    else
        kfree(m);
}

void chan_waiter_list_locked(struct channel *ch, struct chan_waiter *w)
{
    list_add_tail(&ch->callers, &w->node);
    COUNTER_ADD(&ch->pair->waiting[ch->side], 1);
}

void chan_waiter_unlist_locked(struct channel *ch, struct chan_waiter *w)
{
    list_del(&w->node);
    COUNTER_SUB(&ch->pair->waiting[ch->side], 1);
}

bool chan_peer_waits(const struct channel *ch)
{
    return __atomic_load_n(&ch->pair->waiting[!ch->side], __ATOMIC_RELAXED) != 0;
}

void chan_msg_drop(struct chan_msg *m)
{
    for (uint32_t i = 0; i < m->nhandles; i++)
        khandle_release(&msg_handles(m)[i]);
    chan_msg_free(m);
}

status_t chan_msg_deliver(struct chan_msg *m, const struct chan_bytes *b, struct khandle *handles)
{
    if (bytes_out(b, msg_bytes(m), m->nbytes) != OK) {
        chan_msg_drop(m);
        return ERR_INVALID_ARGS;
    }
    if (m->nhandles)
        memcpy(handles, msg_handles(m), m->nhandles * sizeof(struct khandle));
    chan_msg_free(m);
    return OK;
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
    __atomic_store_n(&ch->nqueued, 0, __ATOMIC_RELAXED);
    wake_callers_locked(ch);
    kobject_signal_locked(&ch->base, SIG_READABLE | SIG_WRITABLE, 0);
    spin_unlock_irqrestore(&ch->base.lock, f);

    while (!list_empty(&dead)) {
        struct chan_msg *m = list_first(&dead, struct chan_msg, node);
        list_del(&m->node);
        chan_msg_drop(m);
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
        bool full = chan_nqueued(ch) >= CHANNEL_MAX_QUEUED;
        spin_unlock(&ch->base.lock);
        spin_lock(&writer->base.lock);
        kobject_signal_locked(&writer->base, full ? SIG_WRITABLE : 0, full ? 0 : SIG_WRITABLE);
        spin_unlock(&writer->base.lock);
    }
    spin_unlock_irqrestore(&pair->lock, f);
}

/* ch's lock held, its queue not empty: take the oldest message for a
 * reader with room for bytes_cap bytes and handles_cap handles. *nb, *nh:
 * its sizes. If it doesn't fit: ERR_BUFFER_TOO_SMALL, and it stays queued.
 * *was_full: the queue was full before (the writer may be writable again:
 * refresh_writable, once unlocked). */
static status_t take_first_locked(struct channel *ch, uint32_t bytes_cap, uint32_t handles_cap,
                                  struct chan_msg **out, uint32_t *nb, uint32_t *nh,
                                  bool *was_full)
{
    struct chan_msg *m = list_first(&ch->queue, struct chan_msg, node);
    *nb = m->nbytes;
    *nh = m->nhandles;
    if (m->nbytes > bytes_cap || m->nhandles > handles_cap)
        return ERR_BUFFER_TOO_SMALL;
    list_del(&m->node);
    *was_full = chan_nqueued(ch) == CHANNEL_MAX_QUEUED;
    COUNTER_SUB(&ch->nqueued, 1);
    if (chan_nqueued(ch) == 0)
        kobject_signal_locked(&ch->base, SIG_READABLE, 0);
    *out = m;
    return OK;
}

status_t channel_read_into(struct channel *ch, const struct chan_bytes *b, uint32_t *actual_bytes,
                           struct khandle *handles, uint32_t handles_cap,
                           uint32_t *actual_handles)
{
    uint32_t bytes_cap = b->len;
    if ((bytes_cap && !b->user && !b->addr) || (handles_cap && !handles))
        return ERR_INVALID_ARGS;
    struct chan_msg *m = NULL;
    uint32_t nb = 0, nh = 0;
    status_t st = OK;
    bool was_full = false;
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    if (ch->closed)
        st = ERR_BAD_STATE;
    else if (list_empty(&ch->queue))
        st = ch->peer_closed ? ERR_PEER_CLOSED : ERR_SHOULD_WAIT;
    else
        st = take_first_locked(ch, bytes_cap, handles_cap, &m, &nb, &nh, &was_full);
    spin_unlock_irqrestore(&ch->base.lock, f);
    if (actual_bytes)
        *actual_bytes = nb;
    if (actual_handles)
        *actual_handles = nh;
    if (was_full)
        refresh_writable(ch);
    if (m)
        st = chan_msg_deliver(m, b, handles);   /* off the queue: no lock held */
    if (st == ERR_SHOULD_WAIT)
        PATH_COUNT(PATH_EMPTY_READ);
    PATH_MARK_ARG(PATH_MK_READ, st == ERR_SHOULD_WAIT);
    return st;
}

status_t channel_read(struct channel *ch, void *bytes, uint32_t bytes_cap, uint32_t *actual_bytes,
                      struct khandle *handles, uint32_t handles_cap, uint32_t *actual_handles)
{
    struct chan_bytes b = chan_kbytes(bytes, bytes_cap);
    return channel_read_into(ch, &b, actual_bytes, handles, handles_cap, actual_handles);
}

/* channel_read_wait's state through its wait. */
struct read_wait {
    struct chan_waiter w;          /* us: listed on the endpoint while we sleep */
    uint64_t           f;          /* the endpoint lock's saved flags */
    bool               locked;     /* the endpoint lock is held */
    bool               was_full;   /* we took a message off a full queue (refresh_writable) */
};

/* channel_read_wait's wait, ch's lock held: OK once rw->w has a message
 * for r (handed over by a writer, or taken off the queue here), else the
 * error that ends the wait. A handed message wins over every error: it is
 * no longer queued anywhere, so it must reach the reader. One handed over
 * while we slept is found without taking the lock again (chan_handed),
 * and then the lock is not held on return. */
static status_t read_wait_locked(struct channel *ch, struct read_wait *rw, struct chan_read *r,
                                 uint64_t deadline_ns)
{
    struct chan_waiter *w = &rw->w;
    for (;;) {
        if (chan_handed(w))
            return OK;
        if (ch->closed)
            return ERR_BAD_STATE;
        if (!list_empty(&ch->queue))
            return take_first_locked(ch, r->buf.len, r->hcap, &w->reply, &r->nb, &r->nh,
                                     &rw->was_full);
        if (ch->peer_closed)
            return ERR_PEER_CLOSED;
        if (deadline_ns != DEADLINE_NEVER && uptime_ns() >= deadline_ns)
            return ERR_TIMED_OUT;
        if (!w->node.next)
            chan_waiter_list_locked(ch, w);   /* the queue is empty: see chan_waiter */
        status_t bs = thread_block_cancellable_unlocked(&ch->base.lock, rw->f, deadline_ns);
        if (chan_handed(w)) {
            rw->locked = false;
            return OK;
        }
        rw->f = spin_lock_irqsave(&ch->base.lock);
        if (bs != OK && !chan_handed(w))
            return ERR_CANCELED;
    }
}

status_t channel_read_wait(struct channel *ch, struct chan_read *r, uint64_t deadline_ns)
{
    if ((r->buf.len && !r->buf.user && !r->buf.addr) || (r->hcap && !r->h))
        return ERR_INVALID_ARGS;
    PATH_MARK(PATH_MK_WAIT_IN);
    /* Our free slot is on offer while we are listed (channel_send.c's
     * hand_over_locked takes it for a message built in its writer's). */
    struct read_wait rw = {
        .w = { .thread = current_thread(), .any = true, .bytes_cap = r->buf.len,
               .handles_cap = r->hcap, .slot = chan_slot_take() },
        .locked = true,
    };
    r->nb = r->nh = 0;
    rw.f = spin_lock_irqsave(&ch->base.lock);
    status_t st = read_wait_locked(ch, &rw, r, deadline_ns);
    if (rw.locked && rw.w.node.next)
        chan_waiter_unlist_locked(ch, &rw.w);
    struct chan_msg *offered = rw.w.slot;   /* NULL if a writer took it */
    if (rw.locked)
        spin_unlock_irqrestore(&ch->base.lock, rw.f);
    chan_slot_keep(offered);
    if (rw.was_full)
        refresh_writable(ch);
    struct chan_msg *m = rw.w.reply;
    if (m) {
        r->nb = m->nbytes;
        r->nh = m->nhandles;
        st = chan_msg_deliver(m, &r->buf, r->h);   /* no lock held */
    }
    PATH_MARK(PATH_MK_WAIT_OUT);
    return st;
}

void channel_queued(struct channel *ch, uint32_t *msgs, uint64_t *charged)
{
    uint64_t sum = 0;
    uint64_t f = spin_lock_irqsave(&ch->base.lock);
    for (struct list_node *n = ch->queue.next; n != &ch->queue; n = n->next)
        sum += container_of(n, struct chan_msg, node)->charge;
    *msgs = chan_nqueued(ch);
    spin_unlock_irqrestore(&ch->base.lock, f);
    *charged = sum;
}
