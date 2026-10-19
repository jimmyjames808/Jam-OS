/* dnstest: the resolver (/svc/dns) and the slow-peer rule from user
 * space (docs/HISTORY.md, M9: a service waiting on a slow peer delays only
 * that peer's requests). Run from the shell (`run dnstest`) on a boot
 * with QEMU's network card and tools/netpeer.py, whose DNS server never
 * answers slow.jam and answers fastN.jam with 10.9.0.N
 * (tools/dns-test.sh):
 *   slow        a thread of its own asks for slow.jam; the resolver gives
 *               up after its 10 s of tries: ERR_TIMED_OUT, not sooner than
 *               9 s
 *   fast        meanwhile 20 names, each a query of its own, each answered
 *               in under FAST_MS, and the slow lookup still waiting after
 *               the last one
 *   ping        meanwhile three pings to mac.jam (resolved, then echoed),
 *               each answered in under FAST_MS
 *   answers     NXDOMAIN, a CNAME, two addresses, an address as a name, a
 *               name that can't be one, the cache
 *   openers     the program's list names /svc/dns and /svc/net, so it has
 *               one opener of each (<os.h> "grants"): svc_open hands back
 *               the same channel, and svc.connect on it makes none
 *   room        five more threads ask for slow.jam, the resolver's six
 *               askers of one name for programs; a seventh is refused
 *               at first, and dns_lookup waits for room: it ends
 *               ERR_TIMED_OUT at its own deadline, not at once
 * One line per check that fails and a summary line; exit 0 if none did.
 * Every thread's lookups go on the program's one opener of /svc/dns.
 *
 * One busy asker holds up nobody else (docs/HISTORY.md, M9 item 2), with
 * a program for each (a program has one opener): `dnstest busy` keeps
 * its opener full of requests (written without waiting) for BUSY_S
 * seconds, and while it does `dnstest names` asks 5 names not yet asked,
 * each answered in under BUSY_MS (tools/shell-tests/dns.txt). */
#define CHECK_PROG "dnstest"
#define CHECK_CUR  cur
#include <check.h>
#include <dns.h>
#include <idl/dns.h>
#include <idl/svc.h>
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
#define BUSY_MS    1000u  /* a name asked while another asker floods (the flood shares the CPUs) */
#define BUSY_NAMES 5u
#define BUSY_S     20u    /* how long `dnstest busy` floods */

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

/* The crowd: CROWD more askers of slow.jam, the slow one's companions up
 * to the resolver's six askers of one name for programs; and the late
 * asker, the seventh, refused at first (ERR_NO_RESOURCES): dns_lookup
 * waits for room, which comes when the six time out, and then ends as
 * they did, ERR_TIMED_OUT (its own deadline), not at once. */
#define CROWD     5u
#define LATE_WAIT (14 * NS_PER_S)

static struct {
    uint64_t started, ended;   /* ns uptime; ended 0: still waiting */
    status_t st;
} late;

static void crowd_thread(void *arg)
{
    (void)arg;
    struct dns_answer a;
    (void)dns_lookup("slow.jam", now() + SLOW_WAIT, &a);
}

static void late_thread(void *arg)
{
    (void)arg;
    struct dns_answer a;
    late.started = now();
    late.st = dns_lookup("slow.jam", late.started + LATE_WAIT, &a);
    __atomic_store_n(&late.ended, now(), __ATOMIC_RELEASE);
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

static uint64_t koid_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.koid : 0;
}

/* svc_open of name twice: the channel svc_get keeps (ours, one a
 * program), and svc.connect on it makes no other. */
static bool one_opener(const char *name, handle_t kept)
{
    handle_t a = HANDLE_INVALID, b = HANDLE_INVALID, x = HANDLE_INVALID;
    CHECK(kept != HANDLE_INVALID);
    CHECK_ST(svc_open(name, &a), OK);
    CHECK_ST(svc_open(name, &b), OK);
    bool same = koid_of(a) == koid_of(kept) && koid_of(b) == koid_of(kept);
    status_t st = svc_connect_within(a, 5 * NS_PER_S, &x);
    jam_handle_close(a);
    jam_handle_close(b);
    CHECK(same);
    CHECK(st != OK && !x);   /* an opener's channel answers its own protocol alone */
    return true;
}

static bool t_openers(void)
{
    cur = "openers";
    CHECK(one_opener(SVC_DNS, dns_svc()));
    CHECK(one_opener(SVC_NET, net_svc()));
    printf("dnstest: one opener of /svc/dns and of /svc/net, which make no more\n");
    return true;
}

/* `dnstest busy`: the program's opener kept full of requests for a name
 * that can't be one (each answered at once), replies read and dropped,
 * for BUSY_S seconds. It must be served all along: no wait for room of
 * 2 * BUSY_MS or more. */
static bool busy(void)
{
    cur = "busy";
    handle_t ch = dns_svc();
    CHECK(ch != HANDLE_INVALID);
    uint32_t txid = 0x60000000u;
    uint64_t written = 0, t = now(), end = t + BUSY_S * NS_PER_S, last = t, longest = 0;
    while ((t = now()) < end) {
        if (dns_resolve_send(ch, ++txid, "a..b", 4, 1000) == OK) {
            longest = t - last > longest ? t - last : longest;
            last = t;
            if (++written == 256)   /* well past a turn's budget: the flood is on */
                printf("dnstest: busy: flooding\n");
            continue;
        }
        _Alignas(8) uint8_t rep[DNS_REP_MAX];
        struct idl_msg m;
        status_t st;
        while ((st = idl_reply_read(ch, rep, sizeof(rep), &m)) == OK || st == ERR_INTERNAL)
            idl_msg_drop(&m);   /* the channel was full: make room */
        CHECK_ST(st, ERR_SHOULD_WAIT);
    }
    printf("dnstest: busy: done: %lu requests in %u s, the longest wait for room %lu ms\n",
           (unsigned long)written, BUSY_S, (unsigned long)(longest / NS_PER_MS));
    CHECK(longest < 2 * (uint64_t)BUSY_MS * NS_PER_MS);
    return true;
}

/* `dnstest names`, while `dnstest busy` floods. */
static bool names(void)
{
    cur = "names";
    uint64_t worst = 0;
    for (unsigned i = 0; i < BUSY_NAMES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "fast%u.jam", 100 + i);   /* not cached: a query each */
        uint32_t addr = 0;
        uint64_t ms;
        CHECK_ST(timed(name, &addr, &ms), OK);
        CHECK_EQ(addr, NET_IPV4(10, 9, 0, 100 + i));
        if (ms >= BUSY_MS)
            FAIL("%s took %lu ms while another program floods", name, (unsigned long)ms);
        worst = ms > worst ? ms : worst;
    }
    printf("dnstest: names: %u names answered while another program floods, the slowest in "
           "%lu ms\n", BUSY_NAMES, (unsigned long)worst);
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

static bool t_room(handle_t th)
{
    cur = "room";
    signals_t seen;
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + LATE_WAIT + 5 * NS_PER_S, &seen),
             OK);
    uint64_t took = __atomic_load_n(&late.ended, __ATOMIC_ACQUIRE) - late.started;
    CHECK_ST(late.st, ERR_TIMED_OUT);
    if (took < SLOW_MIN)
        FAIL("the seventh asker of slow.jam ended after %lu ms", (unsigned long)(took / NS_PER_MS));
    printf("dnstest: room: the seventh asker of slow.jam waited for room, %s after %lu ms\n",
           status_str(late.st), (unsigned long)(took / NS_PER_MS));
    return true;
}

/* The slow lookup and its crowd, then the late asker (*lth). */
static bool setup(handle_t *th, handle_t *lth)
{
    static uint8_t stack[16384] __attribute__((aligned(64)));
    static uint8_t crowd_stacks[CROWD][16384] __attribute__((aligned(64)));
    static uint8_t late_stack[16384] __attribute__((aligned(64)));
    struct net_info i;
    CHECK(net_svc() != HANDLE_INVALID);
    CHECK_ST(net_wait_up(net_svc(), now() + 60 * NS_PER_S, &i), OK);
    CHECK(i.dns[0] != 0);
    CHECK_ST(svc_open(SVC_DNS, &slow.ch), OK);
    CHECK_ST(thread_spawn("slow", slow_thread, NULL, stack, sizeof(stack), th), OK);
    jam_sleep_until(now() + 300 * NS_PER_MS);   /* its query is out before the others start */
    CHECK(slow_waiting());
    for (unsigned k = 0; k < CROWD; k++) {
        handle_t ch;
        CHECK_ST(thread_spawn("crowd", crowd_thread, NULL, crowd_stacks[k],
                              sizeof(crowd_stacks[k]), &ch), OK);
        jam_handle_close(ch);
    }
    jam_sleep_until(now() + 300 * NS_PER_MS);   /* the six are the resolver's */
    CHECK_ST(thread_spawn("late", late_thread, NULL, late_stack, sizeof(late_stack), lth), OK);
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "busy"))
        return busy() ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "names"))
        return names() ? 0 : 1;
    handle_t th = HANDLE_INVALID, lth = HANDLE_INVALID;
    unsigned passed = 0, failed = 0;
    if (!setup(&th, &lth)) {
        printf("dnstest: 0 passed, 1 failed\n");
        return 1;
    }
    bool (*const tests[])(void) = { t_fast, t_ping, t_answers, t_openers };
    for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++)
        tests[k]() ? passed++ : failed++;
    t_slow(th) ? passed++ : failed++;
    t_room(lth) ? passed++ : failed++;
    printf("dnstest: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
