/* sock_wait, the one way the layer sleeps on a socket (sock.h has the
 * model). It binds the socket's to_prog event (this side's bit and
 * SOCKRING_SIG_STATE) and its channel's end (netstack gone) on the
 * calling thread's own port, ONCE each, after raising this side's
 * `waits` flag and clearing the bits; then it tries again, and sleeps only
 * if that try still can't go on. So a signal netstack makes after the try
 * fires the binding, one made before it is seen by the try, and another
 * thread clearing the bits meanwhile hides nothing: every waiter has its
 * own binding and its own look. The flag is counted (sleepers), so it
 * stays up while any thread sleeps on that side. */
#include <errno.h>
#include <pthread.h>
#include "sock.h"

static __thread handle_t wait_port;   /* this thread's port for socket waits (0: not made) */
static __thread uint64_t last_key;    /* the last wait's key on it */
static pthread_once_t    port_once = PTHREAD_ONCE_INIT;
static pthread_key_t     port_key;    /* closes a thread's port at its exit */
static bool              port_key_ok;

static void port_free(void *p)
{
    jam_handle_close((handle_t)(uintptr_t)p);
    wait_port = HANDLE_INVALID;
}

static void port_key_make(void)
{
    port_key_ok = pthread_key_create(&port_key, port_free) == 0;
}

static handle_t thread_port(void)
{
    if (wait_port)
        return wait_port;
    pthread_once(&port_once, port_key_make);
    handle_t p = HANDLE_INVALID;
    if (!port_key_ok || jam_port_create(&p) != OK)
        return HANDLE_INVALID;
    if (pthread_setspecific(port_key, (void *)(uintptr_t)p) != 0) {
        jam_handle_close(p);
        return HANDLE_INVALID;
    }
    wait_port = p;
    return p;
}

uint64_t sock_deadline(uint64_t timeout)
{
    uint64_t t = now();
    return !timeout || timeout > DEADLINE_NEVER - t ? DEADLINE_NEVER : t + timeout;
}

static signals_t side_bits(enum sock_side side)
{
    return (side == SIDE_RX ? SOCKRING_SIG_RX : SOCKRING_SIG_TX_ROOM) | SOCKRING_SIG_STATE;
}

static lock_t *side_lock(struct sock *s, enum sock_side side)
{
    return side == SIDE_RX ? &s->rx_lock : &s->tx_lock;
}

/* This side's `waits` flag in the rings: rx's consumer line, tx's producer line. */
static uint32_t *side_flag(struct sock *s, enum sock_side side)
{
    return side == SIDE_RX ? &s->ns.r.rx.cons->waits : &s->ns.r.tx.prod->waits;
}

static bool live(const struct sock *s)
{
    uint32_t st = sock_state(s);
    return (st == SK_STREAM || st == SK_DGRAM) && s->ns.map;
}

/* What one sleep bound, to undo. */
struct sleep {
    handle_t to_prog;   /* the socket's event */
    handle_t ch;        /* its channel */
    uint64_t key;       /* both bindings' */
};

static void flag_down(struct sock *s, enum sock_side side)
{
    lock_take(side_lock(s, side));
    if (live(s) && s->sleepers[side] && --s->sleepers[side] == 0)
        __atomic_store_n(side_flag(s, side), 0, __ATOMIC_RELAXED);
    lock_give(side_lock(s, side));
}

/* Raise the flag, clear the bits, bind both: 0, or -errno (nothing left
 * bound). */
static long sleep_begin(struct sock *s, enum sock_side side, handle_t port, struct sleep *z)
{
    lock_take(side_lock(s, side));
    if (!live(s)) {
        lock_give(side_lock(s, side));
        return -ENOTCONN;   /* handed away meanwhile */
    }
    z->to_prog = s->ns.to_prog;
    z->ch = s->ns.ch;
    if (s->sleepers[side]++ == 0)
        __atomic_store_n(side_flag(s, side), 1, __ATOMIC_RELAXED);
    lock_give(side_lock(s, side));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the flag before the look: <sockring.h> */
    (void)jam_event_signal(z->to_prog, side_bits(side), 0);   /* ours to clear: can't fail */
    z->key = ++last_key;
    status_t st = jam_port_bind(port, z->to_prog, z->key, side_bits(side), PORT_BIND_ONCE);
    if (st == OK && (st = jam_port_bind(port, z->ch, z->key, SIG_PEER_CLOSED, PORT_BIND_ONCE)) !=
                        OK)
        (void)jam_port_unbind(port, z->to_prog, z->key);   /* just bound */
    if (st != OK)
        flag_down(s, side);
    return st == OK ? 0 : lx_err(st);
}

static void sleep_end(struct sock *s, enum sock_side side, handle_t port, const struct sleep *z)
{
    (void)jam_port_unbind(port, z->to_prog, z->key);   /* not found: it fired */
    (void)jam_port_unbind(port, z->ch, z->key);
    flag_down(s, side);
}

/* Sleep until a packet with key (an older wait's are skipped) or the deadline. */
static status_t wait_key(handle_t port, uint64_t key, uint64_t deadline)
{
    for (unsigned guard = 0; guard < 64; guard++) {   /* each binding leaves one packet at most */
        struct port_packet p;
        status_t st = jam_port_wait(port, deadline, &p);
        if (st != OK || p.key == key)
            return st;
    }
    return OK;   /* only old packets: the caller looks again */
}

long sock_wait(struct sock *s, enum sock_side side, sock_try_fn *try, void *arg, bool nonblock,
               uint64_t deadline, bool timed)
{
    long r = try(s, arg);
    if (r != -EAGAIN || nonblock)
        return r;
    handle_t port = thread_port();
    if (!port)
        return -ENOMEM;
    for (;;) {   /* each turn sleeps once, or ends */
        if (deadline != DEADLINE_NEVER && now() >= deadline)
            return -EAGAIN;   /* SO_RCVTIMEO's and SO_SNDTIMEO's answer */
        struct sleep z;
        r = sleep_begin(s, side, port, &z);
        if (r)
            return r;
        r = try(s, arg);
        status_t st = r == -EAGAIN ? wait_key(port, z.key, deadline) : OK;
        sleep_end(s, side, port, &z);
        if (r != -EAGAIN)
            return r;
        if (st == ERR_INTERRUPTED)
            return timed ? -LX_EINTR_FINAL : -EINTR;
        if (st != OK && st != ERR_TIMED_OUT)
            return lx_err(st);
    }
}
