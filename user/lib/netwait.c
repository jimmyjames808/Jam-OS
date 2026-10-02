/* A wait set (<netwait.h>, which has the model): one port, entries in
 * slots, and a FIFO of the slots to look at on the next wait.
 *
 * Invariants:
 *   - an entry is in the pending FIFO at most once (`pending` says so), so
 *     the FIFO never holds more than the set's slots;
 *   - a removed entry's slot may stay in the FIFO, still `pending`: a look
 *     at a free slot takes it out, and an entry added to that slot in the
 *     meantime reuses the place (a new entry must be looked at anyway);
 *   - `armed_rx` / `armed_tx`: this set raised that `waits` flag in the
 *     socket's rings and has not lowered it; flags are lowered when the
 *     entry is reported ready, changed or removed, so a socket that is
 *     being read never makes netstack signal once per datagram;
 *   - `dead`: the socket's channel saw SIG_PEER_CLOSED; its rings are
 *     never touched again (netstack may shrink their VMO).
 * A key is (generation << 32) | (slot << 1) | which, which 0 for the
 * socket's `to_prog` or the handle, 1 for the socket's channel; KEY_WAKE
 * is netwait_wake's. */
#include <netwait.h>

#define KEY_WAKE  (1ull << 63)
#define SOCK_SIGS (SOCKRING_SIG_RX | SOCKRING_SIG_TX_ROOM | SOCKRING_SIG_STATE)
#define DEAD_BITS (NETWAIT_HUP | NETWAIT_ERROR)

enum entry_kind { ENTRY_FREE, ENTRY_SOCK, ENTRY_HANDLE };

struct entry {
    uint8_t  kind;            /* enum entry_kind */
    bool     pending;         /* its slot is in the FIFO */
    bool     armed_rx;        /* this set raised rx's consumer `waits` */
    bool     armed_tx;        /* ... tx's producer `waits` */
    bool     dead;            /* socket: netstack gone; never touch the rings again */
    uint16_t gen;             /* generation: in its id and keys; never 0 */
    status_t broken;          /* a call on its handles failed (reported as ERROR) */
    uint32_t interest;        /* NETWAIT_INTEREST bits */
    void    *user;            /* the caller's */
    union {
        struct netwait_sock   sock;     /* ENTRY_SOCK (tx_need resolved) */
        struct netwait_handle handle;   /* ENTRY_HANDLE */
    };
};

struct netwait {
    handle_t     port;        /* every binding is on it */
    uint32_t     max;         /* slots */
    uint32_t     used;        /* live entries */
    uint32_t     head, len;   /* the pending FIFO in fifo[]: oldest at head */
    uint32_t    *fifo;        /* max slot numbers */
    uint32_t     wake_queued; /* a KEY_WAKE packet is queued (atomic: any thread) */
    struct netwait_stats stats;
    struct entry e[];         /* max slots */
};

/* ---- ids, keys and the FIFO ------------------------------------------------------------ */

static uint64_t key_of(uint32_t slot, const struct entry *e, unsigned which)
{
    return (uint64_t)e->gen << 32 | (uint64_t)slot << 1 | which;
}

static struct entry *entry_of(struct netwait *w, uint32_t id, uint32_t *slot)
{
    uint32_t s = id & 0xffffu;
    if (s >= w->max || w->e[s].kind == ENTRY_FREE || w->e[s].gen != id >> 16)
        return NULL;
    *slot = s;
    return &w->e[s];
}

static void push(struct netwait *w, uint32_t slot)
{
    w->fifo[(w->head + w->len) % w->max] = slot;
    w->len++;
}

static uint32_t pop(struct netwait *w)
{
    uint32_t slot = w->fifo[w->head];
    w->head = (w->head + 1) % w->max;
    w->len--;
    return slot;
}

/* Look at it on the next wait. */
static void make_pending(struct netwait *w, uint32_t slot)
{
    if (w->e[slot].pending)
        return;
    w->e[slot].pending = true;
    push(w, slot);
}

/* ---- sockets ----------------------------------------------------------------------- */

/* The socket's readiness now, from its rings and status line. */
static uint32_t sock_ready(struct entry *e, status_t *err)
{
    if (e->dead || e->broken) {
        *err = e->dead ? ERR_PEER_CLOSED : e->broken;
        return DEAD_BITS;
    }
    struct sockring *r = e->sock.rings;
    struct sockring_status s;
    sockring_status_get(r, &s);
    uint32_t bits = 0;
    if (s.state == SOCKRING_STATE_CLOSED) {
        bits |= NETWAIT_HUP;
        if (s.error != OK) {
            bits |= NETWAIT_ERROR;
            *err = s.error;
        }
    }
    if (e->interest & NETWAIT_READ) {
        uint32_t n = sockring_ready(&r->rx);   /* notes the end when it first sees it */
        if (n || r->rx.ended)
            bits |= NETWAIT_READ;
        if (r->rx.ended)
            bits |= NETWAIT_RX_END;
    }
    if ((e->interest & NETWAIT_WRITE) && s.state == SOCKRING_STATE_OPEN && !r->tx.ended &&
        sockring_room(&r->tx) >= e->sock.tx_need)
        bits |= NETWAIT_WRITE;
    return bits;
}

/* Lower the flags this set raised. */
static void sock_disarm(struct entry *e)
{
    if (e->kind != ENTRY_SOCK || e->dead) {
        e->armed_rx = e->armed_tx = false;
        return;
    }
    if (e->armed_rx)
        sockring_awake(&e->sock.rings->rx);
    if (e->armed_tx)
        sockring_awake(&e->sock.rings->tx);
    e->armed_rx = e->armed_tx = false;
}

/* Not ready: clear the event's bits (so netstack's next signal is an edge
 * that queues a packet), raise the flags the interest needs, and look
 * again. Returns the readiness of that last look: 0, it sleeps. */
static uint32_t sock_arm(struct netwait *w, struct entry *e, status_t *err)
{
    struct sockring *r = e->sock.rings;
    status_t st = jam_event_signal(e->sock.to_prog, SOCK_SIGS, 0);
    if (st != OK) {
        e->broken = st;
        return sock_ready(e, err);
    }
    w->stats.arms++;
    if (e->interest & NETWAIT_READ)
        e->armed_rx = sockring_sleep(&r->rx, 1);
    if ((e->interest & NETWAIT_WRITE) && !r->tx.ended)
        e->armed_tx = sockring_sleep(&r->tx, e->sock.tx_need);
    return sock_ready(e, err);
}

/* ---- handles ----------------------------------------------------------------------- */

/* The signals a handle entry is bound for: those of its interest and HUP's. */
static signals_t handle_mask(const struct entry *e)
{
    const struct netwait_handle *h = &e->handle;
    return (e->interest & NETWAIT_READ ? h->read : 0) |
           (e->interest & NETWAIT_WRITE ? h->write : 0) | h->hup;
}

static uint32_t handle_ready(struct entry *e, status_t *err)
{
    const struct netwait_handle *h = &e->handle;
    signals_t mask = handle_mask(e), seen = 0;
    if (e->broken) {
        *err = e->broken;
        return DEAD_BITS;
    }
    if (!mask)
        return 0;
    status_t st = jam_object_wait_one(h->h, mask, 0, &seen);   /* a look: deadline past */
    if (st != OK && st != ERR_TIMED_OUT) {
        *err = st;
        return DEAD_BITS;
    }
    uint32_t bits = seen & h->hup ? NETWAIT_HUP : 0;
    if ((e->interest & NETWAIT_READ) && (seen & h->read))
        bits |= NETWAIT_READ;
    if ((e->interest & NETWAIT_WRITE) && (seen & h->write))
        bits |= NETWAIT_WRITE;
    return bits;
}

/* ---- adding and taking out --------------------------------------------------------- */

status_t netwait_create(uint32_t max_entries, struct netwait **out)
{
    if (!max_entries || max_entries > NETWAIT_MAX)
        return ERR_INVALID_ARGS;
    struct netwait *w = calloc(1, sizeof(*w) + max_entries * sizeof(struct entry));
    uint32_t *fifo = calloc(max_entries, sizeof(uint32_t));
    status_t st = w && fifo ? jam_port_create(&w->port) : ERR_NO_MEMORY;
    if (st != OK) {
        free(fifo);
        free(w);
        return st;
    }
    w->max = max_entries;
    w->fifo = fifo;
    for (uint32_t i = 0; i < max_entries; i++)
        w->e[i].gen = 1;
    *out = w;
    return OK;
}

void netwait_destroy(struct netwait *w)
{
    if (!w)
        return;
    for (uint32_t i = 0; i < w->max; i++)
        sock_disarm(&w->e[i]);
    jam_handle_close(w->port);   /* takes every binding with it */
    free(w->fifo);
    free(w);
}

/* A free slot, or ERR_NO_RESOURCES. */
static status_t slot_take(struct netwait *w, uint32_t *out)
{
    for (uint32_t i = 0; i < w->max; i++) {
        if (w->e[i].kind == ENTRY_FREE) {
            *out = i;
            return OK;
        }
    }
    return ERR_NO_RESOURCES;
}

/* The slot is filled in (kind, interest, user, sock or handle) and bound:
 * count it and look at it on the next wait. */
static void slot_commit(struct netwait *w, uint32_t slot, uint32_t *out_id)
{
    struct entry *e = &w->e[slot];
    e->broken = OK;
    e->dead = e->armed_rx = e->armed_tx = false;
    w->used++;
    make_pending(w, slot);
    *out_id = (uint32_t)e->gen << 16 | slot;
}

static bool rings_ok(const struct sockring *r)
{
    return r && r->page && r->tx.producer && !r->rx.producer &&
           (r->rx.framing == SOCKRING_DGRAM || r->rx.framing == SOCKRING_STREAM);
}

status_t netwait_add_sock(struct netwait *w, const struct netwait_sock *s, uint32_t interest,
                          void *user, uint32_t *out_id)
{
    if ((interest & ~NETWAIT_INTEREST) || !rings_ok(s->rings) || s->tx_need > s->rings->tx.size)
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < w->max; i++)
        if (w->e[i].kind == ENTRY_SOCK && w->e[i].sock.rings == s->rings)
            return ERR_BAD_STATE;
    uint32_t slot;
    status_t st = slot_take(w, &slot);
    if (st != OK)
        return st;
    struct entry *e = &w->e[slot];
    e->sock = *s;
    if (!e->sock.tx_need)
        e->sock.tx_need = s->rings->tx.framing == SOCKRING_DGRAM
                              ? sockring_dgram_bytes(SOCKRING_DGRAM_MAX) : 1;
    st = jam_port_bind(w->port, s->to_prog, key_of(slot, e, 0), SOCK_SIGS, PORT_BIND_PERSISTENT);
    if (st != OK)
        return st;
    /* PEER_CLOSED never goes away, so one packet is all it can ever say. */
    st = jam_port_bind(w->port, s->ch, key_of(slot, e, 1), SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st != OK) {
        (void)jam_port_unbind(w->port, s->to_prog, key_of(slot, e, 0));   /* just bound */
        return st;
    }
    e->kind = ENTRY_SOCK;
    e->interest = interest;
    e->user = user;
    slot_commit(w, slot, out_id);
    return OK;
}

status_t netwait_add_handle(struct netwait *w, const struct netwait_handle *h,
                            uint32_t interest, void *user, uint32_t *out_id)
{
    if ((interest & ~NETWAIT_INTEREST) || !(h->read | h->write | h->hup))
        return ERR_INVALID_ARGS;
    uint32_t slot;
    status_t st = slot_take(w, &slot);
    if (st != OK)
        return st;
    struct entry *e = &w->e[slot];
    e->handle = *h;
    e->interest = interest;
    signals_t mask = handle_mask(e);
    if (mask)   /* none (interest 0, no hup signals): nothing could ever fire */
        st = jam_port_bind(w->port, h->h, key_of(slot, e, 0), mask, PORT_BIND_PERSISTENT);
    if (st != OK)
        return st;
    e->kind = ENTRY_HANDLE;
    e->user = user;
    slot_commit(w, slot, out_id);
    return OK;
}

/* Undo an entry's bindings (a ONCE binding that fired is gone already). */
static void unbind(struct netwait *w, uint32_t slot, struct entry *e)
{
    if (e->kind == ENTRY_SOCK) {
        (void)jam_port_unbind(w->port, e->sock.to_prog, key_of(slot, e, 0));
        (void)jam_port_unbind(w->port, e->sock.ch, key_of(slot, e, 1));
    } else if (handle_mask(e)) {
        (void)jam_port_unbind(w->port, e->handle.h, key_of(slot, e, 0));
    }
}

status_t netwait_modify(struct netwait *w, uint32_t id, uint32_t interest)
{
    uint32_t slot;
    struct entry *e = entry_of(w, id, &slot);
    if (!e)
        return ERR_NOT_FOUND;
    if (interest & ~NETWAIT_INTEREST)
        return ERR_INVALID_ARGS;
    sock_disarm(e);
    if (e->kind == ENTRY_HANDLE) {
        /* Rebound for the new signals under the same key: a packet of the
         * old binding still queued only makes one look too many. */
        unbind(w, slot, e);
        e->interest = interest;
        signals_t mask = handle_mask(e);
        status_t st = mask ? jam_port_bind(w->port, e->handle.h, key_of(slot, e, 0), mask,
                                           PORT_BIND_PERSISTENT) : OK;
        if (st != OK)
            e->broken = st;   /* reported as ERROR until it is removed */
    }
    e->interest = interest;
    make_pending(w, slot);
    return OK;
}

status_t netwait_remove(struct netwait *w, uint32_t id)
{
    uint32_t slot;
    struct entry *e = entry_of(w, id, &slot);
    if (!e)
        return ERR_NOT_FOUND;
    sock_disarm(e);
    unbind(w, slot, e);
    e->kind = ENTRY_FREE;
    e->gen = e->gen == 0xffffu ? 1 : e->gen + 1;   /* its late packets no longer match */
    e->user = NULL;
    w->used--;
    return OK;   /* `pending` stays: its slot leaves the FIFO at the next look */
}

status_t netwait_touch(struct netwait *w, uint32_t id)
{
    uint32_t slot;
    if (!entry_of(w, id, &slot))
        return ERR_NOT_FOUND;
    make_pending(w, slot);
    return OK;
}

uint32_t netwait_count(const struct netwait *w)
{
    return w->used;
}

void netwait_get_stats(const struct netwait *w, struct netwait_stats *out)
{
    *out = w->stats;
}

/* ---- waiting ----------------------------------------------------------------------- */

/* One packet off the port: its entry becomes pending. */
static void take_packet(struct netwait *w, const struct port_packet *p, bool *woken)
{
    w->stats.packets++;
    if (p->key == KEY_WAKE) {
        __atomic_store_n(&w->wake_queued, 0, __ATOMIC_RELEASE);   /* netwait_wake's exchange */
        *woken = true;
        return;
    }
    uint32_t slot = (uint32_t)(p->key >> 1) & 0xffffu;
    struct entry *e = slot < w->max ? &w->e[slot] : NULL;
    if (!e || e->kind == ENTRY_FREE || e->gen != p->key >> 32 || p->type != PORT_PACKET_SIGNAL) {
        w->stats.stale++;
        return;
    }
    if ((p->key & 1) && (p->signal.observed & SIG_PEER_CLOSED)) {
        e->dead = true;   /* first: sock_disarm then forgets the flags without a touch */
        sock_disarm(e);
    }
    make_pending(w, slot);
}

/* Every packet queued now. Each binding owns one packet and there is one
 * wake, so the guard is never what ends the loop. */
static void take_queued(struct netwait *w, bool *woken)
{
    struct port_packet p;
    for (uint32_t guard = 0; guard < 2 * w->max + 1; guard++) {
        if (jam_port_wait(w->port, 0, &p) != OK)
            return;
        take_packet(w, &p, woken);
    }
}

/* Look at the pending entries (each once): the ready ones into out (and
 * back to the FIFO's tail), the rest armed. Returns how many are in out. */
static uint32_t look(struct netwait *w, struct netwait_ready *out, uint32_t max)
{
    uint32_t n = 0;
    for (uint32_t todo = w->len; todo && n < max; todo--) {
        uint32_t slot = pop(w);
        struct entry *e = &w->e[slot];
        e->pending = false;
        if (e->kind == ENTRY_FREE)
            continue;
        w->stats.looks++;
        status_t err = OK;
        bool sock = e->kind == ENTRY_SOCK;
        uint32_t bits = sock ? sock_ready(e, &err) : handle_ready(e, &err);
        if (!bits && sock)
            bits = sock_arm(w, e, &err);
        if (!bits)
            continue;   /* asleep: a packet brings it back */
        sock_disarm(e);
        make_pending(w, slot);   /* level: looked at again next time */
        out[n++] = (struct netwait_ready){ .id = (uint32_t)e->gen << 16 | slot, .ready = bits,
                                           .error = bits & NETWAIT_ERROR ? err : OK,
                                           .user = e->user };
    }
    return n;
}

status_t netwait_wait(struct netwait *w, uint64_t deadline, struct netwait_ready *out,
                      uint32_t max, uint32_t *out_n)
{
    if (!max)
        return ERR_INVALID_ARGS;
    w->stats.waits++;
    bool woken = false;
    for (;;) {   /* each turn either returns or sleeps until a packet or the deadline */
        take_queued(w, &woken);
        uint32_t n = look(w, out, max);
        if (n) {
            *out_n = n;
            return OK;
        }
        if (woken)
            return ERR_CANCELED;
        struct port_packet p;
        w->stats.blocks++;
        status_t st = jam_port_wait(w->port, deadline, &p);
        if (st != OK)
            return st;
        take_packet(w, &p, &woken);
    }
}

status_t netwait_wake(struct netwait *w)
{
    /* One wake packet at most: the port's user packets are bounded, and a
     * waiter needs only one. Cleared when the packet is taken. */
    if (__atomic_exchange_n(&w->wake_queued, 1, __ATOMIC_ACQ_REL))
        return OK;
    struct port_packet p = { .key = KEY_WAKE, .type = PORT_PACKET_USER };
    status_t st = jam_port_queue(w->port, &p);
    if (st != OK)
        __atomic_store_n(&w->wake_queued, 0, __ATOMIC_RELEASE);
    return st;
}
