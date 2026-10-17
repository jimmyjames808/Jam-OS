/* utest port_order: the port behaviour that file watching's "a close
 * before the next request" rule leans on (docs/INOTIFY-PLAN.md 2.3,
 * <fswatch.h>'s struct fsw_order), pinned without a kernel change. A
 * client closes a file's channel F, then writes a request on another
 * channel C bound to the same port. By the time C's packet can be taken,
 * F's is on the port too: queued in the close's own system call, or
 * already there from before (an earlier packet keeps its place, the close
 * folded into it). So a service that takes every packet on its port
 * before it serves C's request sees the close first. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define KEY_F 1
#define KEY_C 2
#define BOTH  (SIG_READABLE | SIG_PEER_CLOSED)

struct pair {
    handle_t port;
    handle_t f, fc;   /* F: the service's end, the client's */
    handle_t c, cc;   /* C: the same */
};

static bool make(struct pair *p, uint32_t flags)
{
    CHECK_ST(jam_port_create(&p->port), OK);
    CHECK_ST(jam_channel_create(&p->f, &p->fc), OK);
    CHECK_ST(jam_channel_create(&p->c, &p->cc), OK);
    CHECK_ST(jam_port_bind(p->port, p->f, KEY_F, BOTH, flags), OK);
    CHECK_ST(jam_port_bind(p->port, p->c, KEY_C, BOTH, flags), OK);
    return true;
}

static void unmake(struct pair *p)
{
    jam_handle_close(p->f);
    jam_handle_close(p->c);
    jam_handle_close(p->cc);
    if (p->fc != HANDLE_INVALID)
        jam_handle_close(p->fc);
    jam_handle_close(p->port);
}

/* Take every packet on the port now (no waiting) into got: how many. */
static int drain(handle_t port, struct port_packet *got, int max)
{
    int n = 0;
    while (n < max && jam_port_wait(port, 0, &got[n]) == OK)
        n++;
    return n;
}

static bool write1(handle_t ch, char b)
{
    CHECK_ST(jam_channel_write(ch, &b, 1, NULL, 0), OK);
    return true;
}

/* The client's two steps, from another thread. */
static void close_then_write(void *arg)
{
    struct pair *p = arg;
    jam_handle_close(p->fc);
    (void)jam_channel_write(p->cc, "x", 1, NULL, 0);
}

bool t_port_order(void)
{
    struct port_packet got[8];
    struct pair p;

    /* Nothing queued before: F's packet (the close) first, then C's. */
    for (int persistent = 0; persistent < 2; persistent++) {
        CHECK(make(&p, persistent ? PORT_BIND_PERSISTENT : PORT_BIND_ONCE));
        jam_handle_close(p.fc);
        p.fc = HANDLE_INVALID;
        CHECK(write1(p.cc, 'x'));
        CHECK_EQ(drain(p.port, got, 8), 2);
        CHECK(got[0].key == KEY_F && (got[0].signal.observed & SIG_PEER_CLOSED));
        CHECK(got[1].key == KEY_C && (got[1].signal.observed & SIG_READABLE));
        unmake(&p);
    }

    /* C's packet queued already (its first request unread): a close and a
     * second request on C put no new packet before F's, and F's is there. */
    CHECK(make(&p, PORT_BIND_PERSISTENT));
    CHECK(write1(p.cc, '1'));
    jam_handle_close(p.fc);
    p.fc = HANDLE_INVALID;
    CHECK(write1(p.cc, '2'));
    CHECK_EQ(drain(p.port, got, 8), 2);
    CHECK(got[0].key == KEY_C && got[1].key == KEY_F);
    CHECK(got[1].signal.observed & SIG_PEER_CLOSED);
    unmake(&p);

    /* F's packet queued already (a request on F unread): the close folds
     * into it, which keeps its place before C's. Its signals may still say
     * only readable (no new edge); the channel says closed. */
    for (int persistent = 0; persistent < 2; persistent++) {
        CHECK(make(&p, persistent ? PORT_BIND_PERSISTENT : PORT_BIND_ONCE));
        CHECK(write1(p.fc, 'r'));
        jam_handle_close(p.fc);
        p.fc = HANDLE_INVALID;
        CHECK(write1(p.cc, 'x'));
        CHECK_EQ(drain(p.port, got, 8), 2);
        CHECK(got[0].key == KEY_F && got[1].key == KEY_C);
        CHECK_ST(jam_object_wait_one(p.f, SIG_PEER_CLOSED, 0, NULL), OK);
        unmake(&p);
    }

    /* A ONCE binding made again (a service's re-arm after one request)
     * while the close is up fires at once: the close is on the port before
     * the service's next look. */
    CHECK(make(&p, PORT_BIND_ONCE));
    CHECK(write1(p.fc, 'r'));
    CHECK_EQ(drain(p.port, got, 8), 1);
    jam_handle_close(p.fc);
    p.fc = HANDLE_INVALID;
    char b;
    struct channel_read_args rd = { .h = p.f, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)&b };
    CHECK_ST(jam_channel_read(&rd), OK);
    CHECK_ST(jam_port_bind(p.port, p.f, KEY_F, BOTH, PORT_BIND_ONCE), OK);
    CHECK_EQ(drain(p.port, got, 8), 1);
    CHECK(got[0].key == KEY_F && (got[0].signal.observed & SIG_PEER_CLOSED));
    unmake(&p);

    /* From another thread, many times: once C's packet is taken, F's has
     * been or is there to drain. */
    static uint8_t stack[16384];
    for (int i = 0; i < 300; i++) {
        handle_t t;
        bool seen_f = false;
        CHECK(make(&p, i & 1 ? PORT_BIND_PERSISTENT : PORT_BIND_ONCE));
        CHECK_ST(thread_spawn("order", close_then_write, &p, stack, sizeof(stack), &t), OK);
        for (;;) {
            struct port_packet one;
            CHECK_ST(jam_port_wait(p.port, now() + 10 * NS_PER_S, &one), OK);
            if (one.key == KEY_F) {
                seen_f = true;
                continue;
            }
            CHECK_EQ(one.key, KEY_C);
            break;
        }
        int k = drain(p.port, got, 8);
        CHECK(seen_f || (k == 1 && got[0].key == KEY_F));
        CHECK_ST(jam_object_wait_one(t, SIG_TERMINATED, now() + 10 * NS_PER_S, NULL), OK);
        jam_handle_close(t);
        p.fc = HANDLE_INVALID;   /* the thread closed it */
        unmake(&p);
    }
    return true;
}
