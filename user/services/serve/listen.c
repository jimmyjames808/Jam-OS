/* serve: the listen worker. Asking netstack for a listener is a call that
 * waits for its answer (net.idl's tcp_listener, through libos's
 * net_tcp_listen, on /svc/net-listen's opener that libos keeps and opens
 * again after a netstack restart), so the serving loop doesn't make it:
 * this thread, which serves nobody, does. The loop writes a share's slot
 * on `asks`; the worker fills the share's lst and listen_st, stores `done`
 * last (release; the loop reads it with acquire, and touches lst only
 * after), and wakes the loop's wait set. While a share is S_LISTENING the
 * loop leaves its lst alone. */
#include "serve.h"

static handle_t ask_w, ask_r;   /* the loop writes slots on ask_w, the worker reads ask_r */
static uint8_t  stack[32 * 1024];

static void worker(void *arg)
{
    (void)arg;
    for (;;) {   /* one ask at a time, for as long as the loop lives */
        signals_t seen;
        uint32_t slot, n = 0, nh = 0;
        struct channel_read_args a = {
            .h = ask_r, .bytes_cap = sizeof(slot), .bytes = (uint64_t)(uintptr_t)&slot,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT) {
            (void)jam_object_wait_one(ask_r, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                                      &seen);
            continue;
        }
        if (st != OK)
            return;   /* the loop is gone */
        if (n != sizeof(slot) || slot >= SERVE_SHARES)
            continue;
        struct share *sh = &shares[slot];
        handle_t net = svc_get(SVC_NET_LISTEN);
        st = net ? net_wait_up(net, now() + 5 * NS_PER_S, NULL) : ERR_ACCESS_DENIED;
        if (st == ERR_TIMED_OUT)
            st = ERR_BAD_STATE;   /* no address yet: not a server that didn't answer */
        if (st == OK)
            st = net_tcp_listen(net, sh->port, BACKLOG, TX_RING, RX_RING, &sh->lst);
        sh->listen_st = st;
        __atomic_store_n(&sh->done, true, __ATOMIC_RELEASE);
        (void)netwait_wake(serve_w);   /* only out of memory fails: the next turn looks too */
    }
}

status_t listen_init(void)
{
    handle_t th;
    status_t st = jam_channel_create(&ask_w, &ask_r);
    if (st == OK)
        st = thread_spawn("serve-listen", worker, NULL, stack, sizeof(stack), &th);
    if (st == OK)
        jam_handle_close(th);
    return st;
}

status_t listen_ask(unsigned i)
{
    uint32_t slot = i;
    /* The worker reads the share's port after the slot (the channel orders them). */
    __atomic_store_n(&shares[i].done, false, __ATOMIC_RELAXED);
    return jam_channel_write(ask_w, &slot, sizeof(slot), NULL, 0);
}
