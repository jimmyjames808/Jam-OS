/* dnstest: the resolver (/svc/dns) and the slow-peer rule from user
 * space (docs/M9-PLAN.md: "a service waiting on a slow peer delays only
 * that peer's requests"). Run from the shell (`run dnstest`) on a boot
 * with QEMU's network card and tools/netpeer.py, whose DNS server never
 * answers slow.jam and answers fastN.jam with 10.9.0.N
 * (tools/dns-test.sh):
 *   slow        a thread of its own, on a /svc/dns channel of its own,
 *               asks for slow.jam; the resolver gives up after its 10 s
 *               of tries: ERR_TIMED_OUT, not sooner than 9 s
 *   fast        meanwhile 20 names, each a query of its own, each answered
 *               in under FAST_MS, and the slow lookup still waiting after
 *               the last one
 *   ping        meanwhile three pings to mac.jam (resolved, then echoed),
 *               each answered in under FAST_MS
 *   answers     NXDOMAIN, a CNAME, two addresses, an address as a name, a
 *               name that can't be one, the cache
 * One line per check that fails and a summary line; exit 0 if none did. */
#define CHECK_PROG "dnstest"
#define CHECK_CUR  cur
#include <check.h>
#include <dns.h>
#include <ipv4.h>
#include <net.h>
#include <netbytes.h>
#include <os.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc dns\n"
          "svc net\n");

#define FAST_MS    200u   /* an answer that isn't the slow name's comes this soon */
#define FAST_NAMES 20u
#define SLOW_MIN   (9 * NS_PER_S)    /* the resolver's tries take 10 s */
#define SLOW_WAIT  (15 * NS_PER_S)   /* the slow lookup's own deadline */

static const char *cur = "setup";

/* The slow lookup's thread: what it saw and when. */
static struct {
    handle_t ch;
    uint64_t started, ended;   /* ns uptime; ended 0: still waiting */
    status_t st;
} slow;

static void slow_thread(void *arg)
{
    (void)arg;
    struct dns_answer a;
    slow.started = now();
    slow.st = dns_lookup_on(slow.ch, "slow.jam", now() + SLOW_WAIT, &a);
    __atomic_store_n(&slow.ended, now(), __ATOMIC_RELEASE);   /* main reads it with ACQUIRE */
}

static bool slow_waiting(void)
{
    return !__atomic_load_n(&slow.ended, __ATOMIC_ACQUIRE);
}

/* One lookup of name, timed: its answer's first address. */
static status_t timed(const char *name, uint32_t *addr, uint64_t *ms)
{
    struct dns_answer a;
    uint64_t t = now();
    status_t st = dns_lookup(name, t + 5 * NS_PER_S, &a);
    *ms = (now() - t) / NS_PER_MS;
    if (st == OK)
        *addr = a.addr[0];
    return st;
}

static bool t_fast(void)
{
    cur = "fast";
    uint64_t worst = 0;
    for (unsigned i = 1; i <= FAST_NAMES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "fast%u.jam", i);
        uint32_t addr = 0;
        uint64_t ms;
        CHECK_ST(timed(name, &addr, &ms), OK);
        CHECK_EQ(addr, NET_IPV4(10, 9, 0, i));
        if (ms >= FAST_MS)
            FAIL("%s took %lu ms while slow.jam waits", name, (unsigned long)ms);
        worst = ms > worst ? ms : worst;
    }
    CHECK(slow_waiting());
    printf("dnstest: %u names answered while slow.jam waits, the slowest in %lu ms\n",
           FAST_NAMES, (unsigned long)worst);
    return true;
}

static bool t_ping(void)
{
    cur = "ping";
    uint32_t addr = 0, rtt = 0;
    uint64_t ms;
    CHECK_ST(timed("mac.jam", &addr, &ms), OK);
    CHECK_EQ(addr, NET_IPV4(10, 2, 21, 174));
    for (uint16_t seq = 1; seq <= 3; seq++) {
        uint64_t t = now();
        CHECK_ST(net_ping(net_svc(), addr, seq, 56, t + NS_PER_S, &rtt, NULL), OK);
        uint64_t took = (now() - t) / NS_PER_MS;
        if (took >= FAST_MS)
            FAIL("ping %u took %lu ms while slow.jam waits", seq, (unsigned long)took);
    }
    CHECK(slow_waiting());
    printf("dnstest: mac.jam pinged 3 times while slow.jam waits\n");
    return true;
}

static bool t_answers(void)
{
    cur = "answers";
    struct dns_answer a;
    uint64_t t = now() + 5 * NS_PER_S;
    CHECK_ST(dns_lookup("nothing.jam", t, &a), ERR_NOT_FOUND);
    CHECK_ST(dns_lookup("www.jam", t, &a), OK);   /* a CNAME to mac.jam */
    CHECK_EQ(a.addr[0], NET_IPV4(10, 2, 21, 174));
    CHECK_ST(dns_lookup("one.one.one.one", t, &a), OK);
    CHECK_EQ(a.n, 2);
    CHECK(a.ttl > 0 && a.ttl <= 300);
    CHECK_ST(dns_lookup("10.2.21.174", t, &a), OK);
    CHECK_EQ(a.addr[0], NET_IPV4(10, 2, 21, 174));
    CHECK_ST(dns_lookup("a..b", t, &a), ERR_INVALID_ARGS);
    uint32_t addr = 0;
    uint64_t ms;
    CHECK_ST(timed("fast1.jam", &addr, &ms), OK);   /* from the cache */
    CHECK_EQ(addr, NET_IPV4(10, 9, 0, 1));
    return true;
}

static bool t_slow(handle_t th)
{
    cur = "slow";
    signals_t seen;
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + SLOW_WAIT + 5 * NS_PER_S, &seen),
             OK);
    uint64_t took = slow.ended - slow.started;
    CHECK_ST(slow.st, ERR_TIMED_OUT);
    if (took < SLOW_MIN)
        FAIL("slow.jam gave up after %lu ms", (unsigned long)(took / NS_PER_MS));
    printf("dnstest: slow.jam: %s after %lu ms (the resolver's tries)\n", status_str(slow.st),
           (unsigned long)(took / NS_PER_MS));
    return true;
}

static bool setup(handle_t *th)
{
    static uint8_t stack[16384] __attribute__((aligned(64)));
    struct net_info i;
    CHECK(net_svc() != HANDLE_INVALID);
    CHECK_ST(net_wait_up(net_svc(), now() + 60 * NS_PER_S, &i), OK);
    CHECK(i.dns[0] != 0);
    CHECK_ST(svc_open(SVC_DNS, &slow.ch), OK);
    CHECK_ST(thread_spawn("slow", slow_thread, NULL, stack, sizeof(stack), th), OK);
    jam_nanosleep(now() + 300 * NS_PER_MS);   /* its query is out before the others start */
    CHECK(slow_waiting());
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t th = HANDLE_INVALID;
    unsigned passed = 0, failed = 0;
    if (!setup(&th)) {
        printf("dnstest: 0 passed, 1 failed\n");
        return 1;
    }
    bool (*const tests[])(void) = { t_fast, t_ping, t_answers };
    for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++)
        tests[k]() ? passed++ : failed++;
    t_slow(th) ? passed++ : failed++;
    printf("dnstest: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
