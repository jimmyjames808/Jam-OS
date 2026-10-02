/* utest: wait sets (<netwait.h>, user/lib/netwait.c) over fake sockets
 * (fakesock.c): the calls' edges and limits, timeouts and wakes, a wait
 * that looks only at the entries that are ready; readiness through a
 * socket's life (connecting, open, ended, closed with and without an error,
 * netstack gone) and on plain handles; 64 sockets with random traffic,
 * interest changes and entries taken out and put back, every wait's report
 * checked against what the rings say (level-triggered: nothing missed,
 * nothing invented); and a fake netstack thread against the test blocking
 * in the set, which must never sleep through data. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netwait.h>
#include <os.h>
#include "fakesock.h"
#include "nettest.h"
#include "utest.h"

#define WAIT_LONG (5 * NS_PER_S)   /* a wait that should end long before this */

static struct fnet net;            /* 64 sockets: too big for a stack frame */
static struct netwait_ready got[NETWAIT_MAX];   /* what a wait says */

static uint64_t msg_bytes(void)
{
    struct job_info ji = { 0 };
    (void)info_of(own_job(), &ji);   /* our own job: a failure shows as a mismatch */
    return ji.used[JOB_LIMIT_MSG_BYTES];
}

/* A look now (the deadline past) says exactly want[] (and err for sockets
 * whose want has NETWAIT_ERROR) about the first n sockets. */
static bool report_is(struct netwait *w, uint32_t n, const uint32_t *want, const status_t *err)
{
    uint32_t bits[FAKE_SOCKS];
    status_t errs[FAKE_SOCKS], st;
    bool any = false;
    CHECK(fsock_gather(w, 0, bits, errs, &st));
    for (uint32_t i = 0; i < n; i++) {
        any |= want[i] != 0;
        if (bits[i] != want[i])
            FAIL("socket %u: reported %#x, want %#x", i, bits[i], want[i]);
        if ((want[i] & NETWAIT_ERROR) && errs[i] != err[i])
            FAIL("socket %u: error %s, want %s", i, status_str(errs[i]), status_str(err[i]));
    }
    CHECK_ST(st, any ? OK : ERR_TIMED_OUT);
    return true;
}

/* ---- the calls ----------------------------------------------------------------------------- */

static bool api_args(struct netwait *w)
{
    struct fsock *k0 = &net.sk[0];
    struct netwait_sock s = fsock_waitable(k0), bad = s;
    uint32_t id = 0, n = 0;
    CHECK_ST(netwait_add_sock(w, &s, 1u << 4, NULL, &id), ERR_INVALID_ARGS);
    bad.tx_need = FAKE_RING + 1;
    CHECK_ST(netwait_add_sock(w, &bad, NETWAIT_READ, NULL, &id), ERR_INVALID_ARGS);
    bad = s;
    bad.rings = NULL;
    CHECK_ST(netwait_add_sock(w, &bad, NETWAIT_READ, NULL, &id), ERR_INVALID_ARGS);
    bad.rings = &k0->s;   /* netstack's side of the rings, not the program's */
    CHECK_ST(netwait_add_sock(w, &bad, NETWAIT_READ, NULL, &id), ERR_INVALID_ARGS);
    for (uint32_t i = 0; i < net.n; i++)
        CHECK(fsock_add(w, &net.sk[i], NETWAIT_READ));
    CHECK_EQ(netwait_count(w), net.n);
    CHECK_ST(netwait_add_sock(w, &s, NETWAIT_READ, NULL, &id), ERR_BAD_STATE);   /* twice */
    struct netwait_handle h = { .h = k0->ch_prog };
    CHECK_ST(netwait_add_handle(w, &h, NETWAIT_READ, NULL, &id), ERR_INVALID_ARGS);
    CHECK_ST(netwait_wait(w, 0, got, 0, &n), ERR_INVALID_ARGS);
    CHECK_ST(netwait_modify(w, 0, NETWAIT_READ), ERR_NOT_FOUND);
    CHECK_ST(netwait_modify(w, k0->id, 1u << 3), ERR_INVALID_ARGS);
    struct fsock *last = &net.sk[net.n - 1];
    uint32_t old = last->id;
    CHECK(fsock_take_out(w, last));
    CHECK_ST(netwait_remove(w, old), ERR_NOT_FOUND);
    CHECK_ST(netwait_touch(w, old), ERR_NOT_FOUND);
    CHECK_ST(netwait_modify(w, old, 0), ERR_NOT_FOUND);
    CHECK(fsock_add(w, last, NETWAIT_READ));
    CHECK(last->id != old && last->id != 0);
    return true;
}

/* Every socket idle: a look, a wait with a deadline. Afterwards all are armed. */
static bool api_timeouts(struct netwait *w)
{
    uint32_t n = 0;
    uint64_t t0 = now();
    CHECK_ST(netwait_wait(w, 0, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    CHECK(now() - t0 < 500 * NS_PER_MS);
    t0 = now();
    CHECK_ST(netwait_wait(w, t0 + 30 * NS_PER_MS, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    CHECK(now() - t0 >= 30 * NS_PER_MS);
    return true;
}

/* With 64 entries asleep, a wait looks at the one that was signalled. */
static bool api_costs(struct netwait *w)
{
    struct netwait_stats a, b;
    struct fsock *k = &net.sk[17];
    uint32_t n = 0;
    netwait_get_stats(w, &a);
    CHECK_EQ(stack_rx(k, 1, UINT64_MAX), 1);
    CHECK_ST(netwait_wait(w, now() + WAIT_LONG, got, NETWAIT_MAX, &n), OK);
    CHECK(n == 1 && got[0].user == k && got[0].ready == NETWAIT_READ);
    netwait_get_stats(w, &b);
    CHECK_EQ(b.looks - a.looks, 1);
    CHECK_EQ(b.packets - a.packets, 1);
    CHECK_EQ(b.blocks - a.blocks, 0);
    CHECK_ST(netwait_wait(w, 0, got, NETWAIT_MAX, &n), OK);   /* level: still there */
    CHECK(n == 1 && got[0].user == k);
    CHECK_EQ(prog_read(k, 10), 1);
    CHECK_ST(netwait_wait(w, 0, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    netwait_get_stats(w, &a);
    CHECK_EQ(a.looks - b.looks, 2);   /* one entry, twice: not 64 */
    CHECK_EQ(a.arms - b.arms, 1);
    return true;
}

static struct netwait *helper_set;
static struct fsock *helper_sock;

/* After 20 ms: data on helper_sock, or (none) a netwait_wake. */
static void helper(void *arg)
{
    (void)arg;
    (void)jam_nanosleep(now() + 20 * NS_PER_MS);
    if (helper_sock)
        (void)stack_rx(helper_sock, 1, UINT64_MAX);
    else
        (void)netwait_wake(helper_set);
}

static bool with_helper(struct netwait *w, struct fsock *k, status_t want, uint32_t want_n)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    handle_t th = HANDLE_INVALID;
    uint32_t n = 0;
    helper_set = w;
    helper_sock = k;
    uint64_t t0 = now();
    CHECK_ST(thread_spawn("netwait-helper", helper, NULL, stack, sizeof(stack), &th), OK);
    status_t st = netwait_wait(w, k ? now() + WAIT_LONG : DEADLINE_NEVER, got, NETWAIT_MAX, &n);
    CHECK(wait_threads(&th, 1));
    CHECK_ST(st, want);
    CHECK(now() - t0 >= 15 * NS_PER_MS);   /* it slept until the helper came */
    CHECK(st != OK || (n == want_n && got[0].user == k));
    return true;
}

static bool api_blocking_and_wake(struct netwait *w)
{
    uint32_t n = 0;
    CHECK(with_helper(w, &net.sk[40], OK, 1));
    CHECK_EQ(prog_read(&net.sk[40], 10), 1);
    CHECK(with_helper(w, NULL, ERR_CANCELED, 0));
    for (unsigned i = 0; i < 100; i++)
        CHECK_ST(netwait_wake(w), OK);
    CHECK_ST(netwait_wait(w, DEADLINE_NEVER, got, NETWAIT_MAX, &n), ERR_CANCELED);
    CHECK_ST(netwait_wait(w, now() + 10 * NS_PER_MS, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    CHECK_EQ(stack_rx(&net.sk[3], 1, UINT64_MAX), 1);   /* ready and woken: ready wins */
    CHECK_ST(netwait_wake(w), OK);
    CHECK_ST(netwait_wait(w, DEADLINE_NEVER, got, NETWAIT_MAX, &n), OK);
    CHECK(n == 1 && got[0].user == &net.sk[3]);
    CHECK_EQ(prog_read(&net.sk[3], 10), 1);
    CHECK_ST(netwait_wait(w, now() + 10 * NS_PER_MS, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    return true;
}

/* Ten ready, four at a time: three waits report all ten. */
static bool api_small_out(struct netwait *w)
{
    uint32_t seen = 0, n = 0;
    for (uint32_t i = 0; i < 10; i++)
        CHECK_EQ(stack_rx(&net.sk[i], 1, UINT64_MAX), 1);
    for (unsigned round = 0; round < 3; round++) {
        CHECK_ST(netwait_wait(w, 0, got, 4, &n), OK);
        CHECK_EQ(n, 4);
        for (uint32_t j = 0; j < n; j++)
            seen |= 1u << ((struct fsock *)got[j].user)->idx;
    }
    CHECK_EQ(seen, 0x3ff);
    for (uint32_t i = 0; i < 10; i++)
        CHECK_EQ(prog_read(&net.sk[i], 10), 1);
    CHECK_ST(netwait_wait(w, 0, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    return true;
}

/* A packet for an entry taken out is ignored; put back, it is looked at. */
static bool api_remove(struct netwait *w)
{
    struct netwait_stats a, b;
    struct fsock *k = &net.sk[5];
    uint32_t n = 0;
    netwait_get_stats(w, &a);
    CHECK_EQ(stack_rx(k, 1, UINT64_MAX), 1);   /* armed: a packet is queued */
    CHECK(fsock_take_out(w, k));
    CHECK_ST(netwait_wait(w, 0, got, NETWAIT_MAX, &n), ERR_TIMED_OUT);
    netwait_get_stats(w, &b);
    CHECK_EQ(b.stale - a.stale, 1);
    CHECK(fsock_add(w, k, NETWAIT_READ));
    CHECK_ST(netwait_wait(w, 0, got, NETWAIT_MAX, &n), OK);
    CHECK(n == 1 && got[0].user == k && got[0].id == k->id);
    CHECK_EQ(prog_read(k, 10), 1);
    for (uint32_t i = 0; i < net.n; i++)
        CHECK(fsock_take_out(w, &net.sk[i]));
    CHECK_EQ(netwait_count(w), 0);
    return true;
}

/* NETWAIT_MAX entries, all ready at once; one more refused. */
static bool api_full(void)
{
    struct netwait *w = NULL;
    handle_t ev = HANDLE_INVALID;
    uint32_t id = 0, n = 0;
    bool ok = netwait_create(NETWAIT_MAX, &w) == OK && jam_event_create(&ev) == OK;
    struct netwait_handle h = { .h = ev, .read = 1u << 24 };
    for (uint32_t i = 0; ok && i < NETWAIT_MAX; i++)
        ok = netwait_add_handle(w, &h, NETWAIT_READ, NULL, &id) == OK;
    if (ok)
        ok = netwait_add_handle(w, &h, NETWAIT_READ, NULL, &id) == ERR_NO_RESOURCES &&
             jam_event_signal(ev, 0, 1u << 24) == OK &&
             netwait_wait(w, 0, got, NETWAIT_MAX, &n) == OK && n == NETWAIT_MAX &&
             jam_event_signal(ev, 1u << 24, 0) == OK &&
             netwait_wait(w, 0, got, NETWAIT_MAX, &n) == ERR_TIMED_OUT;
    netwait_destroy(w);
    if (ev)
        jam_handle_close(ev);
    CHECK(ok);
    return true;
}

bool t_netwait_api(void)
{
    struct netwait *w = NULL;
    uint64_t before = msg_bytes();
    CHECK_ST(netwait_create(0, &w), ERR_INVALID_ARGS);
    CHECK_ST(netwait_create(NETWAIT_MAX + 1, &w), ERR_INVALID_ARGS);
    bool ok = fnet_open(&net, FAKE_SOCKS) && netwait_create(FAKE_SOCKS, &w) == OK &&
              api_args(w) && api_timeouts(w) && api_costs(w) && api_blocking_and_wake(w) &&
              api_small_out(w) && api_remove(w);
    netwait_destroy(w);
    fnet_close(&net);
    CHECK(ok);
    CHECK(api_full());
    CHECK_EQ(msg_bytes(), before);   /* every binding went with its set */
    return true;
}

/* ---- a socket's life, and plain handles ------------------------------------------------- */

static bool states_sockets(struct netwait *w)
{
    struct fsock *sk = net.sk;
    uint32_t want[6] = { 0 };
    status_t err[6] = { 0 };
    stack_state(&sk[1], SOCKRING_STATE_CONNECTING, OK);
    CHECK(fsock_add(w, &sk[0], NETWAIT_READ) && fsock_add(w, &sk[1], NETWAIT_INTEREST) &&
          fsock_add(w, &sk[2], NETWAIT_READ) && fsock_add(w, &sk[3], NETWAIT_READ) &&
          fsock_add(w, &sk[5], 0));
    CHECK(report_is(w, 6, want, err));                /* connecting: not writable */
    stack_state(&sk[1], SOCKRING_STATE_OPEN, OK);
    want[1] = NETWAIT_WRITE;
    CHECK(report_is(w, 6, want, err) && report_is(w, 6, want, err));
    CHECK_ST(netwait_modify(w, sk[1].id, NETWAIT_READ), OK);
    want[1] = 0;
    CHECK(report_is(w, 6, want, err));
    CHECK_EQ(stack_rx(&sk[3], 5, UINT64_MAX), 5);
    stack_rx_end(&sk[3]);
    want[3] = NETWAIT_READ | NETWAIT_RX_END;
    CHECK(report_is(w, 6, want, err));
    CHECK_EQ(prog_read(&sk[3], 100), 5);
    CHECK(report_is(w, 6, want, err));                /* at the end: a read doesn't wait */
    stack_state(&sk[1], SOCKRING_STATE_CLOSED, ERR_TIMED_OUT);
    want[1] = NETWAIT_HUP | NETWAIT_ERROR;
    err[1] = ERR_TIMED_OUT;
    CHECK(report_is(w, 6, want, err) && report_is(w, 6, want, err));
    stack_state(&sk[5], SOCKRING_STATE_CLOSED, OK);   /* closed cleanly, no interest */
    want[5] = NETWAIT_HUP;
    CHECK(report_is(w, 6, want, err));
    stack_kill(&sk[2]);
    want[2] = NETWAIT_HUP | NETWAIT_ERROR;
    err[2] = ERR_PEER_CLOSED;
    CHECK(report_is(w, 6, want, err));
    CHECK_EQ(stack_rx(&sk[2], 3, UINT64_MAX), 3);     /* its rings are not looked at now */
    CHECK(report_is(w, 6, want, err));
    CHECK(fsock_take_out(w, &sk[1]) && fsock_take_out(w, &sk[2]) && fsock_take_out(w, &sk[3]) &&
          fsock_take_out(w, &sk[5]));
    want[1] = want[2] = want[3] = want[5] = 0;
    CHECK(report_is(w, 6, want, err));                /* socket 0 waited through it all */
    CHECK_EQ(stack_rx(&sk[0], 1, UINT64_MAX), 1);
    want[0] = NETWAIT_READ;
    CHECK(report_is(w, 6, want, err));
    return fsock_take_out(w, &sk[0]);
}

/* The one entry a wait reports now: its readiness, or 0 after a timeout. */
static uint32_t one(struct netwait *w)
{
    uint32_t n = 0;
    if (netwait_wait(w, 0, got, NETWAIT_MAX, &n) != OK)
        return 0;
    return n == 1 ? got[0].ready : ~0u;
}

static bool states_handles(struct netwait *w, handle_t a, handle_t *b, handle_t ev)
{
    struct netwait_handle ch = { .h = a, .read = SIG_READABLE, .hup = SIG_PEER_CLOSED };
    struct netwait_handle e = { .h = ev, .read = 1u << 24 };
    uint32_t ida = 0, ide = 0, len = 0;
    uint8_t byte = 0;
    CHECK_ST(netwait_add_handle(w, &ch, NETWAIT_READ, NULL, &ida), OK);
    CHECK_EQ(one(w), 0);
    CHECK_ST(jam_channel_write(*b, "x", 1, NULL, 0), OK);
    CHECK_EQ(one(w), NETWAIT_READ);
    CHECK_EQ(one(w), NETWAIT_READ);                    /* level: until it is read */
    struct channel_read_args r = { .h = a, .bytes_cap = 1, .bytes = (uint64_t)(uintptr_t)&byte,
                                   .actual_bytes = (uint64_t)(uintptr_t)&len };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK_EQ(one(w), 0);
    CHECK_ST(netwait_modify(w, ida, 0), OK);
    CHECK_ST(jam_channel_write(*b, "y", 1, NULL, 0), OK);
    CHECK_EQ(one(w), 0);                               /* no interest: not reported */
    CHECK_ST(netwait_modify(w, ida, NETWAIT_READ), OK);
    CHECK_EQ(one(w), NETWAIT_READ);
    jam_handle_close(*b);
    *b = HANDLE_INVALID;
    CHECK_EQ(one(w), NETWAIT_READ | NETWAIT_HUP);      /* the message is still there */
    CHECK_ST(netwait_remove(w, ida), OK);
    CHECK_ST(netwait_add_handle(w, &e, NETWAIT_READ, NULL, &ide), OK);
    CHECK_EQ(one(w), 0);
    CHECK_ST(jam_event_signal(ev, 0, 1u << 24), OK);
    CHECK_EQ(one(w), NETWAIT_READ);
    CHECK_ST(jam_event_signal(ev, 1u << 24, 0), OK);
    CHECK_EQ(one(w), 0);
    return netwait_remove(w, ide) == OK;
}

bool t_netwait_states(void)
{
    struct netwait *w = NULL;
    handle_t a = HANDLE_INVALID, b = HANDLE_INVALID, ev = HANDLE_INVALID;
    uint64_t before = msg_bytes();
    bool ok = fnet_open(&net, 6) && netwait_create(8, &w) == OK && states_sockets(w) &&
              jam_channel_create(&a, &b) == OK && jam_event_create(&ev) == OK &&
              states_handles(w, a, &b, ev);
    netwait_destroy(w);
    fnet_close(&net);
    if (a)
        jam_handle_close(a);
    if (b)
        jam_handle_close(b);
    if (ev)
        jam_handle_close(ev);
    CHECK(ok);
    CHECK_EQ(msg_bytes(), before);
    return true;
}
