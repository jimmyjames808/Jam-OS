/* M4 milestone: a service loop the way M5 programs will write one.
 *
 * One server thread and one client per remaining CPU, each with its OWN
 * handle table, using only the handle-level sys_ API (the future system
 * calls). The server waits on a single port bound to every client's
 * channel plus a periodic timer. Each client first sends the server an
 * event handle, which the server signals (a handle crossing tables), then
 * makes thousands of channel_calls and checks every reply. At the end
 * every channel, port and binding must be gone. */
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/time.h>

#define CALLS_PER_CLIENT 2000
#define TIMER_KEY        0xffffffffull
#define TIMER_PERIOD_NS  20000000ull   /* 20 ms */
#define TEST_DEADLINE_NS 60000000000ull

enum { OP_HELLO = 1, OP_DOUBLE = 2 };

struct msg {
    uint32_t txid;   /* channel_call's transaction id */
    uint32_t op;
    uint64_t value;
};

struct client {
    uint32_t            index;
    struct handle_table table;
    handle_t            chan;
    volatile uint64_t   calls, bad, max_ns, total_ns;
    volatile bool       event_ok;
};

static struct handle_table server_table;
static handle_t server_port, server_timer;
static uint32_t n_clients;
static struct client *clients;
static volatile uint64_t served, timer_ticks, hellos, server_errors;

/* Move a handle from one table to another, as sending it would. */
static handle_t move_handle(struct handle_table *from, handle_t h, struct handle_table *to)
{
    struct khandle kh;
    KT_EQ(handle_take(from, h, &kh), OK);
    handle_t out;
    KT_EQ(handle_insert(to, &kh, &out), OK);
    return out;
}

static void arm_timer(void)
{
    KT_EQ(sys_timer_set(&server_table, server_timer, uptime_ns() + TIMER_PERIOD_NS), OK);
    KT_EQ(sys_port_bind(&server_table, server_port, server_timer, TIMER_KEY, SIG_SIGNALED,
                        PORT_BIND_ONCE), OK);
}

/* Drain one client channel: answer every queued request. Returns false once
 * the client has closed its end and nothing is left. */
static bool serve_channel(handle_t ch)
{
    for (;;) {
        struct msg m;
        handle_t hs[1];
        uint32_t nb = 0, nh = 0;
        status_t st = sys_channel_read(&server_table, ch, &m, sizeof(m), &nb, hs, 1, &nh);
        if (st == ERR_SHOULD_WAIT)
            return true;
        if (st == ERR_PEER_CLOSED)
            return false;
        if (st != OK || nb != sizeof(m)) {
            server_errors++;
            return true;
        }
        struct msg reply = { m.txid, m.op, 0 };
        if (m.op == OP_HELLO && nh == 1) {
            /* The client sent us its event: signal it, then drop our copy. */
            if (sys_event_signal(&server_table, hs[0], 0, SIG_SIGNALED) != OK)
                server_errors++;
            handle_close(&server_table, hs[0]);
            hellos++;
        } else if (m.op == OP_DOUBLE && nh == 0) {
            reply.value = m.value * 2 + 1;
        } else {
            server_errors++;
        }
        if (sys_channel_write(&server_table, ch, &reply, sizeof(reply), NULL, 0) != OK)
            server_errors++;
        served++;
    }
}

static void server_main(void *arg)
{
    handle_t *chans = arg;
    uint32_t open = n_clients;
    uint64_t deadline = uptime_ns() + TEST_DEADLINE_NS;
    arm_timer();
    while (open) {
        struct port_packet pkt;
        status_t st = sys_port_wait(&server_table, server_port, deadline, &pkt);
        if (st != OK) {
            server_errors++;
            break;
        }
        if (pkt.key == TIMER_KEY) {
            timer_ticks++;
            arm_timer();
            continue;
        }
        handle_t ch = chans[pkt.key];
        if (ch != HANDLE_INVALID && !serve_channel(ch)) {
            sys_port_unbind(&server_table, server_port, ch, pkt.key);
            handle_close(&server_table, ch);
            chans[pkt.key] = HANDLE_INVALID;
            open--;
        }
    }
    sys_timer_cancel(&server_table, server_timer);
    sys_port_unbind(&server_table, server_port, server_timer, TIMER_KEY);
}

static void client_main(void *arg)
{
    struct client *c = arg;
    struct handle_table *t = &c->table;
    uint64_t deadline = uptime_ns() + TEST_DEADLINE_NS;

    /* Hello: send a duplicate of our event, wait for the server to signal it. */
    handle_t ev, ev_dup;
    KT_EQ(sys_event_create(t, &ev), OK);
    KT_EQ(handle_duplicate(t, ev, RIGHT_SAME, &ev_dup), OK);
    struct msg m = { 0, OP_HELLO, 0 }, r;
    uint32_t rn = 0, rh = 0;
    if (sys_channel_call(t, c->chan, &m, sizeof(m), &ev_dup, 1, &r, sizeof(r), &rn, NULL, 0, &rh,
                         deadline) != OK)
        c->bad++;
    signals_t seen = 0;
    c->event_ok = sys_object_wait_one(t, ev, SIG_SIGNALED, deadline, &seen) == OK &&
                  (seen & SIG_SIGNALED);

    for (uint32_t k = 0; k < CALLS_PER_CLIENT; k++) {
        m = (struct msg){ 0, OP_DOUBLE, (uint64_t)c->index * 1000000 + k };
        uint64_t t0 = uptime_ns();
        status_t st = sys_channel_call(t, c->chan, &m, sizeof(m), NULL, 0, &r, sizeof(r), &rn,
                                       NULL, 0, &rh, deadline);
        uint64_t dt = uptime_ns() - t0;
        if (st != OK || rn != sizeof(r) || r.value != m.value * 2 + 1)
            c->bad++;
        c->calls++;
        c->total_ns += dt;
        if (dt > c->max_ns)
            c->max_ns = dt;
    }
    handle_table_destroy(t);   /* closes the channel: the server sees PEER_CLOSED */
}

KTEST(m4_milestone_service)
{
    uint64_t channels_before = channel_live_count();
    struct port_stats ps_before;
    port_get_stats(&ps_before);

    n_clients = cpu_count > 1 ? cpu_count - 1 : 1;
    served = timer_ticks = hellos = server_errors = 0;
    clients = kzalloc(sizeof(*clients) * n_clients);
    handle_t *server_chans = kzalloc(sizeof(handle_t) * n_clients);

    handle_table_init(&server_table);
    KT_EQ(sys_port_create(&server_table, &server_port), OK);
    KT_EQ(sys_timer_create(&server_table, &server_timer), OK);

    /* One channel per client: server end into the server's table, bound to
     * the port; client end into the client's own table. */
    struct handle_table boot;
    handle_table_init(&boot);
    for (uint32_t i = 0; i < n_clients; i++) {
        struct client *c = &clients[i];
        c->index = i;
        handle_table_init(&c->table);
        handle_t a, b;
        KT_EQ(sys_channel_create(&boot, &a, &b), OK);
        server_chans[i] = move_handle(&boot, a, &server_table);
        c->chan = move_handle(&boot, b, &c->table);
        KT_EQ(sys_port_bind(&server_table, server_port, server_chans[i], i,
                            SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT), OK);
    }
    handle_table_destroy(&boot);

    cpumask_t m;
    cpumask_one(&m, 0);
    uint64_t t0 = uptime_ns();
    struct thread *server = thread_create_on("m4-server", server_main, server_chans,
                                             PRIO_DEFAULT, &m);
    struct thread **ct = kzalloc(sizeof(*ct) * n_clients);
    for (uint32_t i = 0; i < n_clients; i++) {
        cpumask_one(&m, cpu_count > 1 ? i + 1 : 0);
        ct[i] = thread_create_on("m4-client", client_main, &clients[i], PRIO_DEFAULT, &m);
    }
    for (uint32_t i = 0; i < n_clients; i++)
        thread_join(ct[i]);
    thread_join(server);
    uint64_t elapsed = uptime_ns() - t0;

    uint64_t calls = 0, bad = 0, total_ns = 0, max_ns = 0, events_ok = 0;
    for (uint32_t i = 0; i < n_clients; i++) {
        calls += clients[i].calls;
        bad += clients[i].bad;
        total_ns += clients[i].total_ns;
        if (clients[i].max_ns > max_ns)
            max_ns = clients[i].max_ns;
        events_ok += clients[i].event_ok;
    }
    handle_table_destroy(&server_table);

    kprintf("m4: 1 server + %u clients on %u CPUs: %lu calls in %lu ms = %lu calls/s\n",
            n_clients, cpu_count, calls, elapsed / 1000000,
            elapsed ? (uint64_t)(calls * 1000000000ull / elapsed) : (uint64_t)0);
    kprintf("m4: call latency avg %lu ns, worst %lu us; %lu event handles delivered; "
            "%lu timer ticks\n", calls ? total_ns / calls : 0, max_ns / 1000, events_ok,
            timer_ticks);

    KT_EQ(bad, 0);
    KT_EQ(server_errors, 0);
    KT_EQ(calls, (uint64_t)n_clients * CALLS_PER_CLIENT);
    KT_EQ(served, (uint64_t)n_clients * (CALLS_PER_CLIENT + 1));
    KT_EQ(hellos, n_clients);
    KT_EQ(events_ok, n_clients);
    KT_ASSERT(elapsed < TIMER_PERIOD_NS || timer_ticks >= 1);

    /* Nothing left behind. */
    struct port_stats ps;
    port_get_stats(&ps);
    KT_EQ(channel_live_count(), channels_before);
    KT_EQ(ps.ports, ps_before.ports);
    KT_EQ(ps.bindings, ps_before.bindings);

    kfree(ct);
    kfree(server_chans);
    kfree(clients);
}
