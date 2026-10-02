/* utest: the resolver's queries in flight and its cache
 * (user/services/dns/resolver.c and cache.c, linked in by the Makefile),
 * over a scripted clock and a scripted edge that records the queries and
 * answers. Covered: a query answered, then from the cache until its TTL
 * runs out; replies that don't count (another server, port, id); retries
 * over the servers at 1, 2, 3 and 4 s, then a time-out; a late reply to
 * an earlier try; the slow-peer rule (a name that is never answered
 * holds up nobody else); CNAMEs, in one reply and over several, and
 * their limit; NXDOMAIN, SERVFAIL, TC; askers joining a query, cancelled
 * askers, a full table; ports the edge says are taken; the cache's
 * bounds; mutated replies; and the fair shares (ordinary askers' names
 * in flight and askers of one name, each asker's class kept through
 * cancels). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netbytes.h>
#include <os.h>
#include "dns.h"
#include "nettest.h"
#include "utest.h"

#define S       NS_PER_S
#define MS      NS_PER_MS
#define RING    64u
#define SERVER1 NET_IPV4(10, 2, 21, 1)
#define SERVER2 NET_IPV4(1, 1, 1, 1)

struct sent {
    uint16_t port;
    uint32_t server;
    uint8_t  msg[DNS_MSG_MAX];
    size_t   len;
};

struct answer {
    uint64_t cookie;
    status_t st;
    uint32_t addr[DNS_MAX_ADDRS];
    unsigned n;
    uint32_t ttl;
};

/* What the resolver did, through the scripted edge (the last RING of each kept). */
struct script {
    struct sent   sent[RING];
    unsigned      nsent;
    uint16_t      released[RING];
    unsigned      nreleased;
    struct answer ans[RING];
    unsigned      nans;
    unsigned      taken;   /* the next sends fail with ERR_ALREADY_BOUND */
    uint64_t      rng;
};

static struct script ds;
static struct dns_resolver res;

static status_t io_send(void *ctx, uint16_t port, uint32_t server, const void *msg, size_t len)
{
    struct script *s = ctx;
    if (s->taken) {
        s->taken--;
        return ERR_ALREADY_BOUND;
    }
    struct sent *e = &s->sent[s->nsent++ % RING];
    *e = (struct sent){ .port = port, .server = server, .len = len };
    memcpy(e->msg, msg, len < DNS_MSG_MAX ? len : DNS_MSG_MAX);
    return OK;
}

static void io_release(void *ctx, uint16_t port)
{
    struct script *s = ctx;
    s->released[s->nreleased++ % RING] = port;
}

static void io_answer(void *ctx, uint64_t cookie, status_t st, const uint32_t *addr, unsigned n,
                      uint32_t ttl_s)
{
    struct script *s = ctx;
    struct answer *a = &s->ans[s->nans++ % RING];
    *a = (struct answer){ .cookie = cookie, .st = st, .n = n, .ttl = ttl_s };
    if (n)
        memcpy(a->addr, addr, (n < DNS_MAX_ADDRS ? n : DNS_MAX_ADDRS) * sizeof(uint32_t));
}

static uint32_t io_random(void *ctx)
{
    return fuzz_rand(&((struct script *)ctx)->rng);
}

static const struct dns_io dio = { &ds, io_send, io_release, io_answer, io_random };

static void dreset(unsigned nservers)
{
    static const uint32_t servers[] = { SERVER1, SERVER2 };
    memset(&ds, 0, sizeof(ds));
    ds.rng = 0xd1ce5eedu;
    dns_init(&res, &dio);
    dns_set_servers(&res, servers, nservers);
}

static const struct sent *last(void)
{
    return &ds.sent[(ds.nsent - 1) % RING];
}

static const struct answer *last_ans(void)
{
    return &ds.ans[(ds.nans - 1) % RING];
}

/* The answer for cookie among the last RING, or NULL. */
static const struct answer *answer_for(uint64_t cookie)
{
    for (unsigned i = ds.nans > RING ? ds.nans - RING : 0; i < ds.nans; i++)
        if (ds.ans[i % RING].cookie == cookie)
            return &ds.ans[i % RING];
    return NULL;
}

/* The datagram d, as it arrives: from `from`'s server, port 53, to its port. */
static void arrive(uint64_t now, const struct sent *to, const uint8_t *b, size_t len)
{
    struct dns_datagram d = { .port = to->port, .src = to->server, .src_port = DNS_PORT,
                              .msg = b, .len = len };
    dns_input(&res, now, &d);
}

/* The server's reply to query q. */
static void answer(uint64_t now, const struct sent *q, const struct dns_srv *s)
{
    uint8_t b[1500];
    size_t len = dns_srv_reply(b, sizeof(b), q->msg, q->len, s);
    arrive(now, q, b, len);
}

static const uint32_t one[] = { NET_IPV4(1, 1, 1, 1), NET_IPV4(1, 0, 0, 1) };
static const struct dns_srv one_srv = { one, 2, 300, NULL, 0, false };

bool t_dnsres_basic(void)
{
    dreset(1);
    CHECK_EQ(dns_deadline(&res), DEADLINE_NEVER);
    CHECK_ST(dns_ask(&res, 0, "One.One.One.One.", 1), OK);
    CHECK(ds.nsent == 1 && !ds.nans && last()->server == SERVER1 && last()->port >= DNS_PORT_MIN);
    char name[DNS_NAME_MAX + 1];
    size_t pos = 12;
    CHECK_ST(dns_read_name(last()->msg, last()->len, &pos, name), OK);
    CHECK(!strcmp(name, "One.One.One.One"));
    CHECK_EQ(dns_deadline(&res), 1 * S);
    /* replies that don't count: another server, source port, local port, id; garbage */
    uint8_t b[1500];
    size_t len = dns_srv_reply(b, sizeof(b), last()->msg, last()->len, &one_srv);
    struct dns_datagram d = { last()->port, SERVER2, DNS_PORT, b, len };
    dns_input(&res, S / 10, &d);
    d = (struct dns_datagram){ last()->port, SERVER1, 5353, b, len };
    dns_input(&res, S / 10, &d);
    d = (struct dns_datagram){ (uint16_t)(last()->port + 1), SERVER1, DNS_PORT, b, len };
    dns_input(&res, S / 10, &d);
    b[1] ^= 1;
    arrive(S / 10, last(), b, len);
    b[1] ^= 1;
    arrive(S / 10, last(), b, 5);
    CHECK(!ds.nans && res.stats.foreign == 4 && res.stats.malformed == 1);
    arrive(S / 10, last(), b, len);
    CHECK(ds.nans == 1 && last_ans()->cookie == 1 && last_ans()->st == OK);
    CHECK(last_ans()->n == 2 && last_ans()->addr[0] == one[0] && last_ans()->addr[1] == one[1]);
    CHECK(last_ans()->ttl == 300 && ds.nreleased == 1 && ds.released[0] == last()->port);
    CHECK_EQ(dns_deadline(&res), DEADLINE_NEVER);
    arrive(S / 10, last(), b, len);   /* the same reply again: no query to answer */
    CHECK(ds.nans == 1 && res.stats.foreign == 5);
    /* from the cache, in any case, until the TTL runs out */
    CHECK_ST(dns_ask(&res, 100 * S, "one.ONE.one.one", 2), OK);
    CHECK(ds.nans == 2 && last_ans()->n == 2 && last_ans()->ttl == 201 && ds.nsent == 1);
    CHECK_ST(dns_ask(&res, 300 * S + S / 10 - 1, "one.one.one.one", 3), OK);
    CHECK(ds.nans == 3 && last_ans()->ttl == 1 && ds.nsent == 1);
    CHECK_ST(dns_ask(&res, 300 * S + S / 10, "one.one.one.one", 4), OK);
    CHECK(ds.nans == 3 && ds.nsent == 2);
    /* an IPv4 literal needs no server; a bad name gets no answer */
    CHECK_ST(dns_ask(&res, 400 * S, "10.2.21.174", 5), OK);
    CHECK(ds.nans == 4 && last_ans()->n == 1 && last_ans()->addr[0] == NET_IPV4(10, 2, 21, 174));
    CHECK_ST(dns_ask(&res, 400 * S, "a..b", 6), ERR_INVALID_ARGS);
    CHECK_ST(dns_ask(&res, 400 * S, "", 6), ERR_INVALID_ARGS);
    CHECK(ds.nans == 4 && ds.nsent == 2);
    return true;
}

bool t_dnsres_retries(void)
{
    dreset(2);
    CHECK_ST(dns_ask(&res, 0, "slow.example", 1), OK);
    const struct sent first = *last();
    static const uint64_t at[] = { 1 * S, 3 * S, 6 * S };   /* waits of 1, 2, 3 s */
    for (unsigned i = 0; i < 3; i++) {
        CHECK_EQ(dns_deadline(&res), at[i]);
        dns_tick(&res, at[i] - 1);
        CHECK_EQ(ds.nsent, i + 1);
        dns_tick(&res, at[i]);
        CHECK(ds.nsent == i + 2 && last()->port == first.port);
        CHECK(last()->server == (i % 2 ? SERVER1 : SERVER2));        /* in turn */
        CHECK(net_get16(last()->msg) == net_get16(first.msg));       /* the same id */
    }
    CHECK_EQ(dns_deadline(&res), 10 * S);   /* the 4th try's 4 s */
    dns_tick(&res, 10 * S);
    CHECK(ds.nans == 1 && last_ans()->st == ERR_TIMED_OUT && ds.nsent == 4);
    CHECK(ds.nreleased == 1 && ds.released[0] == first.port && res.stats.timeouts == 1);
    CHECK_EQ(dns_deadline(&res), DEADLINE_NEVER);
    /* a late reply to the first try, after the second went out, counts */
    CHECK_ST(dns_ask(&res, 20 * S, "late.example", 2), OK);
    const struct sent q = *last();
    dns_tick(&res, 21 * S);
    CHECK_EQ(ds.nsent, 6);
    answer(22 * S, &q, &one_srv);
    CHECK(ds.nans == 2 && last_ans()->cookie == 2 && last_ans()->st == OK);
    return true;
}

/* A name whose server never answers delays nobody else: 20 other names
 * asked meanwhile are each answered the moment their reply comes. */
bool t_dnsres_slow_peer(void)
{
    dreset(1);
    CHECK_ST(dns_ask(&res, 0, "slow.example", 1000), OK);
    char name[32];
    for (unsigned i = 0; i < 20; i++) {
        uint64_t t = (uint64_t)i * 10 * MS;
        snprintf(name, sizeof(name), "fast%u.example", i);
        CHECK_ST(dns_ask(&res, t, name, i), OK);
        answer(t, last(), &one_srv);
        const struct answer *a = answer_for(i);
        CHECK(a && a->st == OK && a->n == 2 && ds.nans == i + 1);
        CHECK_EQ(dns_deadline(&res), 1 * S);   /* only the slow one is waiting */
    }
    /* five at once, answered in reverse order: each reply finds its own query */
    struct sent q[5];
    for (unsigned i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "many%u.example", i);
        CHECK_ST(dns_ask(&res, 300 * MS, name, 100 + i), OK);
        q[i] = *last();
    }
    for (unsigned i = 5; i-- > 0;) {
        uint32_t addr = NET_IPV4(10, 0, 0, i + 1);
        struct dns_srv s = { &addr, 1, 60, NULL, 0, false };
        answer(400 * MS, &q[i], &s);
        const struct answer *a = answer_for(100 + i);
        CHECK(a && a->st == OK && a->addr[0] == addr);
    }
    CHECK(!answer_for(1000));
    for (unsigned guard = 0; guard < 10 && !answer_for(1000); guard++)
        dns_tick(&res, dns_deadline(&res));
    CHECK(answer_for(1000) && answer_for(1000)->st == ERR_TIMED_OUT);
    CHECK_EQ(ds.nans, 26);
    return true;
}

bool t_dnsres_cname(void)
{
    dreset(1);
    const uint32_t gh = NET_IPV4(140, 82, 112, 3);
    struct dns_srv s = { &gh, 1, 60, "github.com", 0, false };
    CHECK_ST(dns_ask(&res, 0, "www.github.com", 1), OK);
    answer(0, last(), &s);
    CHECK(ds.nans == 1 && last_ans()->st == OK && last_ans()->addr[0] == gh);
    CHECK_ST(dns_ask(&res, S, "www.github.com", 2), OK);   /* cached under the name asked */
    CHECK(ds.nans == 2 && ds.nsent == 1);
    /* a CNAME alone: the target is asked for, with a new id and port */
    s = (struct dns_srv){ NULL, 0, 30, "target.example", 0, false };
    CHECK_ST(dns_ask(&res, 2 * S, "alias.example", 3), OK);
    const struct sent q1 = *last();
    answer(2 * S, &q1, &s);
    CHECK(ds.nans == 2 && ds.nsent == 3 && ds.nreleased == 2 && ds.released[1] == q1.port);
    char name[DNS_NAME_MAX + 1];
    size_t pos = 12;
    CHECK_ST(dns_read_name(last()->msg, last()->len, &pos, name), OK);
    CHECK(!strcmp(name, "target.example") && last()->port != q1.port);
    s = (struct dns_srv){ &gh, 1, 300, NULL, 0, false };
    answer(2 * S, last(), &s);
    CHECK(ds.nans == 3 && last_ans()->cookie == 3 && last_ans()->addr[0] == gh);
    CHECK_EQ(last_ans()->ttl, 30);   /* the chain's smallest */
    /* CNAMEs over replies, one each: the 9th is one too many */
    CHECK_ST(dns_ask(&res, 3 * S, "loop0.example", 4), OK);
    unsigned sends = 0;
    char target[32];
    for (unsigned i = 1; i <= DNS_CNAME_MAX + 1 && !answer_for(4); i++) {
        snprintf(target, sizeof(target), "loop%u.example", i);
        s = (struct dns_srv){ NULL, 0, 30, target, 0, false };
        answer(3 * S, last(), &s);
        sends++;
    }
    CHECK(answer_for(4) && answer_for(4)->st == ERR_OUT_OF_RANGE && sends == DNS_CNAME_MAX + 1);
    /* a target that isn't a name we can ask for */
    s = (struct dns_srv){ NULL, 0, 30, "*.example", 0, false };
    CHECK_ST(dns_ask(&res, 4 * S, "wild.example", 5), OK);
    answer(4 * S, last(), &s);
    CHECK(answer_for(5) && answer_for(5)->st == ERR_NOT_FOUND);
    CHECK_EQ(dns_deadline(&res), DEADLINE_NEVER);
    return true;
}

/* Resolve name for cookie and give the query's reply s: the status answered. */
static status_t one_reply(const char *name, uint64_t cookie, const struct dns_srv *s)
{
    if (dns_ask(&res, 0, name, cookie) != OK)
        return ERR_INTERNAL;
    answer(0, last(), s);
    const struct answer *a = answer_for(cookie);
    return a ? a->st : ERR_SHOULD_WAIT;
}

bool t_dnsres_failures(void)
{
    dreset(2);
    struct dns_srv s = { NULL, 0, 0, NULL, DNS_RCODE_NXDOMAIN, false };
    CHECK_ST(one_reply("nx.example", 1, &s), ERR_NOT_FOUND);
    s.rcode = 0;
    CHECK_ST(one_reply("nodata.example", 2, &s), ERR_NOT_FOUND);
    s = (struct dns_srv){ one, 2, 60, NULL, 0, true };
    CHECK_ST(one_reply("big.example", 3, &s), ERR_NOT_SUPPORTED);
    /* SERVFAIL: the next server at once, DNS_TRIES in all, then ERR_IO */
    s = (struct dns_srv){ NULL, 0, 0, NULL, DNS_RCODE_SERVFAIL, false };
    CHECK_ST(one_reply("fail.example", 4, &s), ERR_SHOULD_WAIT);
    CHECK(last()->server == SERVER2);
    for (unsigned i = 1; i < DNS_TRIES; i++)
        answer(0, last(), &s);
    CHECK(answer_for(4) && answer_for(4)->st == ERR_IO);
    /* askers of a name in flight share its query */
    unsigned n = ds.nsent;
    for (unsigned i = 0; i < DNS_MAX_WAITERS; i++)
        CHECK_ST(dns_ask(&res, 0, "shared.example", 10 + i), OK);
    CHECK_ST(dns_ask(&res, 0, "SHARED.example", 99), ERR_NO_RESOURCES);
    CHECK_EQ(ds.nsent, n + 1);
    dns_cancel(&res, 10);
    answer(0, last(), &one_srv);
    CHECK(!answer_for(10) && !answer_for(99));
    for (unsigned i = 1; i < DNS_MAX_WAITERS; i++)
        CHECK(answer_for(10 + i) && answer_for(10 + i)->st == OK);
    /* the last asker gone: the query ends, its port goes, a late reply is foreign */
    CHECK_ST(dns_ask(&res, 0, "gone.example", 20), OK);
    const struct sent q = *last();
    unsigned rel = ds.nreleased;
    dns_cancel(&res, 20);
    CHECK(ds.nreleased == rel + 1 && dns_deadline(&res) == DEADLINE_NEVER);
    answer(0, &q, &one_srv);
    CHECK(!answer_for(20));
    /* a full table */
    char name[32];
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++) {
        snprintf(name, sizeof(name), "n%u.example", i);
        CHECK_ST(dns_ask(&res, 0, name, 1000 + i), OK);
    }
    CHECK_ST(dns_ask(&res, 0, "one.more.example", 2000), ERR_NO_RESOURCES);
    /* the servers taken away mid-query: each query ends at its next try */
    dns_set_servers(&res, NULL, 0);
    dns_tick(&res, 1 * S);
    CHECK(answer_for(1000) && answer_for(1000)->st == ERR_BAD_STATE);
    CHECK_EQ(dns_deadline(&res), DEADLINE_NEVER);
    CHECK_ST(dns_ask(&res, 2 * S, "x.example", 3000), ERR_BAD_STATE);
    static const uint32_t bad[] = { 0, 0xffffffffu, NET_IPV4(127, 0, 0, 1) };
    dns_set_servers(&res, bad, 3);
    CHECK_ST(dns_ask(&res, 2 * S, "x.example", 3000), ERR_BAD_STATE);
    return true;
}

bool t_dnsres_ports(void)
{
    dreset(1);
    ds.taken = 2;   /* two ports taken: the third is used */
    CHECK_ST(dns_ask(&res, 0, "p.example", 1), OK);
    CHECK(ds.nsent == 1 && res.stats.sent == 1 && !res.stats.send_failed);
    answer(0, last(), &one_srv);
    CHECK(answer_for(1) && answer_for(1)->st == OK);
    ds.taken = DNS_PORT_TRIES;   /* every port tried taken: a lost send, retried */
    CHECK_ST(dns_ask(&res, 0, "q.example", 2), OK);
    CHECK(ds.nsent == 1 && res.stats.send_failed == 1);
    dns_tick(&res, 1 * S);
    CHECK_EQ(ds.nsent, 2);
    answer(S, last(), &one_srv);
    CHECK(answer_for(2) && answer_for(2)->st == OK);
    /* ports and ids differ from query to query */
    unsigned same_port = 0, same_id = 0;
    char name[32];
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++) {
        snprintf(name, sizeof(name), "r%u.example", i);
        CHECK_ST(dns_ask(&res, 2 * S, name, 10 + i), OK);
        CHECK(last()->port >= DNS_PORT_MIN);
        if (i) {
            const struct sent *p = &ds.sent[(ds.nsent - 2) % RING];
            same_port += p->port == last()->port;
            same_id += net_get16(p->msg) == net_get16(last()->msg);
        }
    }
    CHECK(same_port == 0 && same_id < 3);
    return true;
}

bool t_dnsres_cache(void)
{
    static struct dns_cache c;
    dns_cache_flush(&c);
    char name[32];
    uint32_t a = NET_IPV4(10, 0, 0, 1), got[DNS_MAX_ADDRS], ttl;
    uint8_t n;
    for (unsigned i = 0; i < DNS_CACHE_SIZE + 8; i++) {
        snprintf(name, sizeof(name), "c%u.example", i);
        dns_cache_put(&c, 0, name, &a, 1, 100 + i);
    }
    /* full: the ones that expire first made room */
    for (unsigned i = 0; i < DNS_CACHE_SIZE + 8; i++) {
        snprintf(name, sizeof(name), "c%u.example", i);
        status_t want = i < 8 ? ERR_NOT_FOUND : OK;
        CHECK_ST(dns_cache_get(&c, 0, name, got, &n, &ttl), want);
    }
    CHECK_ST(dns_cache_get(&c, 120 * S, "C20.EXAMPLE", got, &n, &ttl), ERR_NOT_FOUND);
    CHECK_ST(dns_cache_get(&c, 120 * S, "c21.example", got, &n, &ttl), OK);
    CHECK(n == 1 && got[0] == a && ttl == 1);
    /* the same name replaced, not added; nothing kept for TTL 0 or no addresses */
    uint32_t two[2] = { 1, 2 };
    dns_cache_put(&c, 0, "C9.example", two, 2, 50);
    CHECK_ST(dns_cache_get(&c, 0, "c9.example", got, &n, &ttl), OK);
    CHECK(n == 2 && ttl == 50);
    dns_cache_put(&c, 0, "zero.example", &a, 1, 0);
    dns_cache_put(&c, 0, "none.example", &a, 0, 60);
    dns_cache_put(&c, 0, "many.example", &a, DNS_MAX_ADDRS + 1, 60);
    CHECK_ST(dns_cache_get(&c, 0, "zero.example", got, &n, &ttl), ERR_NOT_FOUND);
    CHECK_ST(dns_cache_get(&c, 0, "none.example", got, &n, &ttl), ERR_NOT_FOUND);
    CHECK_ST(dns_cache_get(&c, 0, "many.example", got, &n, &ttl), ERR_NOT_FOUND);
    dns_cache_put(&c, 0, "long.example", &a, 1, 0xfffffff0u);
    CHECK_ST(dns_cache_get(&c, 0, "long.example", got, &n, &ttl), OK);
    CHECK_EQ(ttl, DNS_TTL_MAX);
    /* a new server list empties the resolver's cache; the same list keeps it */
    dreset(1);
    CHECK_ST(one_reply("one.example", 1, &one_srv), OK);
    static const uint32_t same[] = { SERVER1 }, other[] = { SERVER2 };
    dns_set_servers(&res, same, 1);
    CHECK_ST(dns_ask(&res, 0, "one.example", 2), OK);
    CHECK(ds.nsent == 1 && answer_for(2));
    dns_set_servers(&res, other, 1);
    CHECK_ST(dns_ask(&res, 0, "one.example", 3), OK);
    CHECK(ds.nsent == 2 && !answer_for(3));
    return true;
}

/* Mutated replies to a query in flight: never a fault; an answer, if
 * one comes, is an error or sane addresses. */
bool t_dnsres_hostile(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    dreset(1);
    uint64_t seed = 0x5105eedu;
    uint8_t b[1500];
    char name[32];
    const uint32_t addr = NET_IPV4(9, 9, 9, 9);
    struct dns_srv s = { &addr, 1, 60, NULL, 0, false };
    for (unsigned i = 0; i < FUZZ_ROUNDS; i++) {
        snprintf(name, sizeof(name), "f%u.example", i);
        CHECK_ST(dns_ask(&res, 0, name, i), OK);
        s.cname = i % 3 == 0 ? "t.example" : NULL;
        size_t len = dns_srv_reply(b, sizeof(b), last()->msg, last()->len, &s);
        len = fuzz_mutate(&seed, b, len, sizeof(b));
        unsigned before = ds.nans;
        arrive(0, last(), guard_put(&g, b, len), len);
        if (ds.nans != before) {
            const struct answer *a = last_ans();
            CHECK(a->cookie == i && (a->st != OK || (a->n >= 1 && a->n <= DNS_MAX_ADDRS)));
        }
        dns_cancel(&res, i);   /* whatever is still waiting */
    }
    CHECK_EQ(dns_deadline(&res), DEADLINE_NEVER);
    guard_close(&g);
    return true;
}

/* The query for name (in flight), or NULL. */
static const struct dns_query *query_of(const char *name)
{
    for (unsigned i = 0; i < DNS_MAX_QUERIES; i++)
        if (res.q[i].used && !strcmp(res.q[i].name, name))
            return &res.q[i];
    return NULL;
}

/* One name's askers, each class: the ordinary share, then the totals; a
 * cancelled asker takes its class with it, whichever asker moves into its
 * place. n0 starts with 2 ordinary askers. */
static bool waiter_shares(void)
{
    for (unsigned k = 0; k < DNS_PROG_WAITERS - 2; k++)
        CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 170 + k, false), OK);
    CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 180, false), ERR_NO_RESOURCES);
    CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 210, true), OK);
    CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 211, true), OK);
    CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 212, true), ERR_NO_RESOURCES);   /* DNS_MAX_WAITERS */
    const struct dns_query *q = query_of("n0.jam");
    CHECK(q && q->nwait == DNS_MAX_WAITERS && __builtin_popcount(q->sys) == 2);
    for (unsigned k = 0; k < DNS_PROG_WAITERS - 2; k++)
        dns_cancel(&res, 170 + k);
    CHECK(q->nwait == 4 && __builtin_popcount(q->sys) == 2);
    for (unsigned k = 0; k < q->nwait; k++)
        CHECK_EQ((q->sys >> k) & 1u, q->cookies[k] >= 200);
    for (unsigned k = 0; k < DNS_PROG_WAITERS - 2; k++)
        CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 170 + k, false), OK);
    CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 180, false), ERR_NO_RESOURCES);
    return true;
}

/* The fair shares (<dns.h>'s DNS_PROG_*): ordinary askers get
 * DNS_PROG_QUERIES names in flight and DNS_PROG_WAITERS askers of one
 * name; system askers the rest, up to the totals. Cookies 100 and up are
 * ordinary askers, 200 and up system ones. */
bool t_dnsres_shares(void)
{
    char name[16];
    dreset(1);
    for (unsigned k = 0; k < DNS_PROG_QUERIES; k++) {
        snprintf(name, sizeof(name), "n%u.jam", k);
        CHECK_ST(dns_ask_as(&res, 0, name, 100 + k, false), OK);
    }
    CHECK_ST(dns_ask_as(&res, 0, "o0.jam", 150, false), ERR_NO_RESOURCES);   /* the share */
    for (unsigned k = 0; k < DNS_MAX_QUERIES - DNS_PROG_QUERIES; k++) {
        snprintf(name, sizeof(name), "s%u.jam", k);
        CHECK_ST(dns_ask_as(&res, 0, name, 200 + k, true), OK);   /* the reserve */
    }
    CHECK_ST(dns_ask_as(&res, 0, "s9.jam", 250, true), ERR_NO_RESOURCES);   /* the total */
    CHECK_ST(dns_ask_as(&res, 0, "n0.jam", 160, false), OK);   /* joining: no new name */
    CHECK(waiter_shares());
    /* A name only ordinary askers wait for counts against their share,
     * whoever asked first: s1 once its system asker goes, n0 once its
     * system askers go (until then it was the system's). */
    CHECK_ST(dns_ask_as(&res, 0, "s1.jam", 152, false), OK);
    dns_cancel(&res, 201);
    dns_cancel(&res, 210);
    dns_cancel(&res, 211);
    dns_cancel(&res, 101);
    dns_cancel(&res, 102);   /* n1 and n2 end: 11 names ordinary askers hold, 14 in all */
    CHECK(!query_of("n1.jam") && !query_of("n2.jam"));
    CHECK_ST(dns_ask_as(&res, 0, "o1.jam", 153, false), OK);
    CHECK_ST(dns_ask_as(&res, 0, "o2.jam", 154, false), ERR_NO_RESOURCES);
    CHECK_ST(dns_ask_as(&res, 0, "s9.jam", 250, true), OK);
    CHECK_ST(dns_ask(&res, 0, "s10.jam", 251), ERR_NO_RESOURCES);   /* dns_ask: a system asker */
    return true;
}
