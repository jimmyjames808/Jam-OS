/* utest: the DHCP client's state machine (user/services/dhcp/client.c,
 * linked in by the Makefile), over a scripted clock and a scripted edge
 * that records what the client sends and does. Covered: a lease from
 * DISCOVER to BOUND (with and without the ARP probe), renewing at T1,
 * rebinding at T2 and losing the address at the lease's end; the
 * retransmit times with their backoff and jitter; NAKs while requesting
 * and in the middle of a renewal; replies that don't count (another xid,
 * another server, another address, the wrong state); a probe conflict
 * (DECLINE, then a 10 s wait); INIT-REBOOT; the lease's times; RELEASE;
 * and mutated replies fed to a client waiting for an OFFER. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netbytes.h>
#include <os.h>
#include "dhcp.h"
#include "nettest.h"
#include "utest.h"

#define S        NS_PER_S
#define SENT_MAX 64u
#define OTHER    NET_IPV4(10, 2, 21, 2)   /* another DHCP server */

/* What the client did, through the scripted edge. */
struct script {
    uint32_t to[SENT_MAX];
    uint8_t  msg[SENT_MAX][DHCP_MSG_BUILT];
    unsigned nsent;                 /* every send; the last SENT_MAX kept */
    unsigned nbound, nunbound;
    struct dhcp_lease lease;        /* the last bound */
    enum dhcp_why why;              /* the last unbound */
    unsigned nprobe;
    uint32_t probed;
    uint64_t rng;
    bool     fail_send;
};

static struct script sc;
static struct dhcp_client c;

static status_t io_send(void *ctx, uint32_t to, const void *msg, size_t len)
{
    struct script *s = ctx;
    if (s->fail_send || len != DHCP_MSG_BUILT)
        return ERR_IO;
    s->to[s->nsent % SENT_MAX] = to;
    memcpy(s->msg[s->nsent % SENT_MAX], msg, len);
    s->nsent++;
    return OK;
}

static void io_bound(void *ctx, const struct dhcp_lease *l)
{
    struct script *s = ctx;
    s->nbound++;
    s->lease = *l;
}

static void io_unbound(void *ctx, enum dhcp_why why)
{
    struct script *s = ctx;
    s->nunbound++;
    s->why = why;
}

static void io_probe(void *ctx, uint32_t addr)
{
    struct script *s = ctx;
    s->nprobe++;
    s->probed = addr;
}

static uint32_t io_random(void *ctx)
{
    return fuzz_rand(&((struct script *)ctx)->rng);
}

static const struct dhcp_io with_probe = { &sc, io_send, io_bound, io_unbound, io_probe,
                                           io_random };
static const struct dhcp_io no_probe = { &sc, io_send, io_bound, io_unbound, NULL, io_random };

static void reset(const struct dhcp_io *io)
{
    memset(&sc, 0, sizeof(sc));
    sc.rng = 0x0dc9c11e47u;
    dhcp_init(&c, io, test_mac);
}

/* The last message sent: its bytes, its option 53, its xid. */
static const uint8_t *last(void)
{
    return sc.msg[(sc.nsent - 1) % SENT_MAX];
}

static uint32_t last_to(void)
{
    return sc.to[(sc.nsent - 1) % SENT_MAX];
}

static uint8_t last_type(void)
{
    const uint8_t *o = test_dhcp_opt(last(), DHCP_MSG_BUILT, 53);
    return o ? o[2] : 0;
}

static uint32_t last_xid(void)
{
    return net_get32(last() + DHCP_OFF_XID);
}

/* The option's 4-byte value in the last message, 0 if it isn't there. */
static uint32_t last_opt(uint8_t code)
{
    const uint8_t *o = test_dhcp_opt(last(), DHCP_MSG_BUILT, code);
    return o && o[1] == 4 ? net_get32(o + 2) : 0;
}

/* A reply from server to the client's last message, with the VLAN 21
 * router's other options. */
static void reply_from(uint64_t now, uint8_t type, uint32_t server, uint32_t yiaddr,
                       uint32_t lease)
{
    uint8_t b[DHCP_MSG_BUILT];
    struct srv s = { .type = type, .xid = last_xid(), .yiaddr = type == DHCP_NAK ? 0 : yiaddr,
                     .server = server, .mask = NET_IPV4(255, 255, 255, 0),
                     .router = TEST_SERVER, .dns = TEST_SERVER, .lease = lease };
    if (type == DHCP_NAK)
        s.mask = s.router = s.dns = s.lease = 0;
    size_t len = srv_build(b, sizeof(b), &s);
    dhcp_input(&c, now, b, len);
}

static void reply(uint64_t now, uint8_t type)
{
    reply_from(now, type, TEST_SERVER, TEST_ADDR, 86400);
}

/* From STOPPED to BOUND (no probe) at time t0: the REQUEST went at t0. */
static bool get_lease(uint64_t t0, uint32_t lease)
{
    dhcp_start(&c, t0, 0);
    reply_from(t0, DHCP_OFFER, TEST_SERVER, TEST_ADDR, lease);
    reply_from(t0, DHCP_ACK, TEST_SERVER, TEST_ADDR, lease);
    CHECK(c.state == DHCP_BOUND);
    return true;
}

/* DISCOVER, OFFER, REQUEST, ACK, the probe, BOUND; then renewing at T1,
 * no answers, rebinding at T2, the lease's end. */
bool t_dhcpc_lease_cycle(void)
{
    reset(&with_probe);
    CHECK(c.state == DHCP_STOPPED && dhcp_deadline(&c) == DEADLINE_NEVER);
    dhcp_start(&c, 0, 0);
    CHECK(sc.nsent == 1 && last_type() == DHCP_DISCOVER && last_to() == DHCP_BROADCAST);
    CHECK_EQ(net_get16(last() + DHCP_OFF_FLAGS), DHCP_FLAG_BROADCAST);
    uint32_t xid = last_xid();
    CHECK(dhcp_deadline(&c) >= 3 * S && dhcp_deadline(&c) <= 5 * S);
    reply(S / 2, DHCP_OFFER);
    CHECK(c.state == DHCP_REQUESTING && sc.nsent == 2 && last_type() == DHCP_REQUEST);
    CHECK(last_xid() == xid && last_to() == DHCP_BROADCAST);
    CHECK(last_opt(50) == TEST_ADDR && last_opt(54) == TEST_SERVER);
    reply(S / 2 + S / 10, DHCP_ACK);
    CHECK(c.state == DHCP_PROBING && sc.nprobe == 1 && sc.probed == TEST_ADDR && !sc.nbound);
    dhcp_probe_done(&c, S, TEST_ADDR, false);
    CHECK(c.state == DHCP_BOUND && sc.nbound == 1);
    CHECK(sc.lease.addr == TEST_ADDR && sc.lease.mask == 0xffffff00u &&
          sc.lease.router == TEST_SERVER && sc.lease.server == TEST_SERVER);
    CHECK(sc.lease.ndns == 1 && sc.lease.dns[0] == TEST_SERVER);
    CHECK(sc.lease.lease_s == 86400 && sc.lease.t1_s == 43200 && sc.lease.t2_s == 75600);
    uint64_t t1 = S / 2 + 43200 * S;   /* the times count from the REQUEST */
    CHECK_EQ(dhcp_deadline(&c), t1);
    dhcp_tick(&c, t1 - 1);
    CHECK_EQ(sc.nsent, 2);
    /* T1: renew, unicast to the server, with our address and a new xid */
    dhcp_tick(&c, t1);
    CHECK(c.state == DHCP_RENEWING && sc.nsent == 3 && last_type() == DHCP_REQUEST);
    CHECK(last_to() == TEST_SERVER && last_xid() != xid);
    CHECK(net_get32(last() + DHCP_OFF_CIADDR) == TEST_ADDR && !last_opt(50) && !last_opt(54));
    CHECK_EQ(net_get16(last() + DHCP_OFF_FLAGS), 0);
    reply(t1 + S, DHCP_ACK);
    CHECK(c.state == DHCP_BOUND && sc.nbound == 1);   /* nothing changed: not told again */
    uint64_t t1b = t1 + 43200 * S, t2b = t1 + 75600 * S, endb = t1 + 86400 * S;
    CHECK_EQ(dhcp_deadline(&c), t1b);
    /* from here no server answers */
    unsigned renews = 0, rebinds = 0;
    for (unsigned guard = 0; guard < 100 && c.state != DHCP_SELECTING; guard++) {
        uint64_t now = dhcp_deadline(&c);
        dhcp_tick(&c, now);
        if (c.state == DHCP_RENEWING) {
            CHECK(now >= t1b && now < t2b && last_to() == TEST_SERVER);
            renews++;
        } else if (c.state == DHCP_REBINDING) {
            CHECK(now >= t2b && now < endb && last_to() == DHCP_BROADCAST);
            CHECK_EQ(last_opt(54), 0);
            rebinds++;
        } else {
            CHECK_EQ(now, endb);
        }
    }
    /* renewing: at T1, then halfway to T2 (16200 s), and so on; rebinding likewise */
    CHECK(renews >= 2 && rebinds >= 2);
    CHECK(c.state == DHCP_SELECTING && last_type() == DHCP_DISCOVER);
    CHECK(sc.nunbound == 1 && sc.why == DHCP_WHY_EXPIRED);
    return true;
}

/* An interval between deadlines: ms +- the jitter. */
static bool interval(uint64_t from, uint64_t to, uint32_t ms)
{
    uint64_t lo = (uint64_t)(ms - DHCP_JITTER_MS) * NS_PER_MS,
             hi = (uint64_t)(ms + DHCP_JITTER_MS) * NS_PER_MS;
    CHECK(to - from >= lo && to - from <= hi);
    return true;
}

bool t_dhcpc_retransmit(void)
{
    reset(&no_probe);
    dhcp_start(&c, 0, 0);
    uint32_t xid = last_xid();
    static const uint32_t discover_ms[] = { 4000, 8000, 16000, 32000, 64000, 64000, 64000 };
    uint64_t at = 0;
    for (unsigned i = 0; i < sizeof(discover_ms) / sizeof(discover_ms[0]); i++) {
        uint64_t d = dhcp_deadline(&c);
        if (!interval(at, d, discover_ms[i]))
            return false;
        unsigned n = sc.nsent;
        dhcp_tick(&c, d - 1);
        CHECK_EQ(sc.nsent, n);   /* not yet */
        dhcp_tick(&c, d);
        CHECK(sc.nsent == n + 1 && last_type() == DHCP_DISCOVER && last_xid() == xid);
        at = d;
    }
    /* REQUESTING: DHCP_REQUEST_TRIES sends at 4, 8, 16 s, then a new DISCOVER */
    reply(at, DHCP_OFFER);
    CHECK(c.state == DHCP_REQUESTING && last_type() == DHCP_REQUEST);
    static const uint32_t request_ms[] = { 4000, 8000, 16000 };
    for (unsigned i = 0; i < 3; i++) {
        uint64_t d = dhcp_deadline(&c);
        if (!interval(at, d, request_ms[i]))
            return false;
        dhcp_tick(&c, d);
        CHECK(last_type() == DHCP_REQUEST && last_xid() == xid && c.state == DHCP_REQUESTING);
        at = d;
    }
    uint64_t d = dhcp_deadline(&c);
    if (!interval(at, d, 32000))
        return false;
    dhcp_tick(&c, d);
    CHECK(c.state == DHCP_SELECTING && last_type() == DHCP_DISCOVER && last_xid() != xid);
    /* a failed send is counted and retried like a lost one */
    sc.fail_send = true;
    unsigned n = sc.nsent;
    dhcp_tick(&c, dhcp_deadline(&c));
    CHECK(sc.nsent == n && c.stats.send_failed == 1 && dhcp_deadline(&c) != DEADLINE_NEVER);
    return true;
}

bool t_dhcpc_nak(void)
{
    /* while requesting: wait 2 s, then 4 s, before the next DISCOVER */
    reset(&no_probe);
    dhcp_start(&c, 0, 0);
    reply(S, DHCP_OFFER);
    reply_from(S, DHCP_NAK, OTHER, 0, 0);
    CHECK(c.state == DHCP_REQUESTING && c.stats.ignored == 1);   /* not the server we asked */
    reply(S, DHCP_NAK);
    CHECK(c.state == DHCP_INIT && dhcp_deadline(&c) == 3 * S && !sc.nunbound);
    unsigned n = sc.nsent;
    dhcp_tick(&c, 3 * S);
    CHECK(sc.nsent == n + 1 && last_type() == DHCP_DISCOVER && c.state == DHCP_SELECTING);
    reply(4 * S, DHCP_OFFER);
    reply(4 * S, DHCP_NAK);
    CHECK(c.state == DHCP_INIT && dhcp_deadline(&c) == 8 * S);
    /* in the middle of a renewal: the address goes at once, DISCOVER at once */
    reset(&no_probe);
    if (!get_lease(0, 3600))
        return false;
    dhcp_tick(&c, 1800 * S);
    CHECK(c.state == DHCP_RENEWING);
    reply_from(1801 * S, DHCP_NAK, OTHER, 0, 0);
    CHECK(c.state == DHCP_RENEWING && !sc.nunbound);
    reply(1801 * S, DHCP_NAK);
    CHECK(sc.nunbound == 1 && sc.why == DHCP_WHY_NAK);
    CHECK(c.state == DHCP_SELECTING && last_type() == DHCP_DISCOVER);
    /* while rebinding: from any server */
    reset(&no_probe);
    if (!get_lease(0, 3600))
        return false;
    dhcp_tick(&c, 3150 * S);
    CHECK(c.state == DHCP_REBINDING);
    reply_from(3151 * S, DHCP_NAK, OTHER, 0, 0);
    CHECK(sc.nunbound == 1 && sc.why == DHCP_WHY_NAK && c.state == DHCP_SELECTING);
    return true;
}

/* Replies that must not count: they change nothing. */
bool t_dhcpc_wrong_replies(void)
{
    reset(&no_probe);
    dhcp_start(&c, 0, 0);
    uint8_t b[DHCP_MSG_BUILT];
    struct srv s = { .type = DHCP_OFFER, .xid = last_xid() + 1, .yiaddr = TEST_ADDR,
                     .server = TEST_SERVER };
    dhcp_input(&c, S, b, srv_build(b, sizeof(b), &s));
    CHECK(c.state == DHCP_SELECTING && c.stats.foreign == 1 && sc.nsent == 1);
    reply(S, DHCP_ACK);
    reply(S, DHCP_NAK);
    CHECK(c.state == DHCP_SELECTING && c.stats.ignored == 2);
    dhcp_input(&c, S, b, 100);
    CHECK(c.state == DHCP_SELECTING && c.stats.malformed == 1 && sc.nsent == 1);
    reply(S, DHCP_OFFER);
    CHECK(c.state == DHCP_REQUESTING && sc.nsent == 2);
    reply_from(S, DHCP_OFFER, OTHER, NET_IPV4(10, 2, 21, 99), 3600);
    reply_from(S, DHCP_ACK, OTHER, TEST_ADDR, 3600);
    reply_from(S, DHCP_ACK, TEST_SERVER, NET_IPV4(10, 2, 21, 99), 3600);
    CHECK(c.state == DHCP_REQUESTING && c.stats.ignored == 5 && sc.nsent == 2);
    /* a retransmit, then the late ACK to the first REQUEST: same xid, counts */
    dhcp_tick(&c, dhcp_deadline(&c));
    CHECK_EQ(sc.nsent, 3);
    reply(dhcp_deadline(&c) - S, DHCP_ACK);
    CHECK(c.state == DHCP_BOUND && sc.nbound == 1);
    CHECK_EQ(dhcp_deadline(&c), S + 43200 * S);   /* T1 from the first REQUEST, the earliest */
    reply(dhcp_deadline(&c) - S, DHCP_ACK);
    CHECK(c.state == DHCP_BOUND && sc.nbound == 1 && c.stats.ignored == 6);
    return true;
}

bool t_dhcpc_probe(void)
{
    reset(&with_probe);
    dhcp_start(&c, 0, 0);
    reply(S, DHCP_OFFER);
    reply(S, DHCP_ACK);
    CHECK(c.state == DHCP_PROBING);
    dhcp_probe_done(&c, 2 * S, NET_IPV4(10, 2, 21, 99), true);
    CHECK(c.state == DHCP_PROBING);   /* another address: not this probe */
    dhcp_probe_done(&c, 2 * S, TEST_ADDR, true);
    CHECK(c.state == DHCP_INIT && last_type() == DHCP_DECLINE && last_to() == DHCP_BROADCAST);
    CHECK(last_opt(50) == TEST_ADDR && last_opt(54) == TEST_SERVER && !sc.nbound);
    CHECK_EQ(dhcp_deadline(&c), 12 * S);
    dhcp_tick(&c, 12 * S);
    CHECK(c.state == DHCP_SELECTING && last_type() == DHCP_DISCOVER);
    /* the edge never answers: the address is taken as free */
    reply(13 * S, DHCP_OFFER);
    reply(13 * S, DHCP_ACK);
    CHECK(c.state == DHCP_PROBING && dhcp_deadline(&c) == 23 * S);
    dhcp_tick(&c, 23 * S);
    CHECK(c.state == DHCP_BOUND && sc.nbound == 1);
    dhcp_probe_done(&c, 24 * S, TEST_ADDR, true);   /* too late: ignored */
    CHECK(c.state == DHCP_BOUND && sc.nunbound == 0);
    return true;
}

bool t_dhcpc_reboot(void)
{
    reset(&no_probe);
    dhcp_start(&c, 0, TEST_ADDR);
    CHECK(c.state == DHCP_REBOOTING && last_type() == DHCP_REQUEST && last_to() == DHCP_BROADCAST);
    CHECK(last_opt(50) == TEST_ADDR && !last_opt(54) && !net_get32(last() + DHCP_OFF_CIADDR));
    reply_from(S, DHCP_ACK, OTHER, TEST_ADDR, 3600);   /* any server may answer */
    CHECK(c.state == DHCP_BOUND && sc.nbound == 1 && sc.lease.server == OTHER);
    /* starting again while bound clears the address first */
    dhcp_start(&c, 2 * S, TEST_ADDR);
    CHECK(sc.nunbound == 1 && sc.why == DHCP_WHY_STOPPED && c.state == DHCP_REBOOTING);
    reply(3 * S, DHCP_NAK);
    CHECK(c.state == DHCP_SELECTING && last_type() == DHCP_DISCOVER);
    /* silence: DHCP_REQUEST_TRIES REQUESTs, then DISCOVER */
    dhcp_start(&c, 10 * S, TEST_ADDR);
    for (unsigned i = 1; i < DHCP_REQUEST_TRIES; i++) {
        dhcp_tick(&c, dhcp_deadline(&c));
        CHECK(c.state == DHCP_REBOOTING && last_type() == DHCP_REQUEST);
    }
    dhcp_tick(&c, dhcp_deadline(&c));
    CHECK(c.state == DHCP_SELECTING && last_type() == DHCP_DISCOVER);
    /* not an address to ask for again: straight to DISCOVER */
    dhcp_start(&c, 100 * S, NET_IPV4(127, 0, 0, 1));
    CHECK(c.state == DHCP_SELECTING);
    return true;
}

/* One ACK with these times: the lease the edge was given. */
static bool lease_times(uint32_t lease, uint32_t t1, uint32_t t2, struct dhcp_lease *out)
{
    reset(&no_probe);
    dhcp_start(&c, 0, 0);
    reply(0, DHCP_OFFER);
    uint8_t b[DHCP_MSG_BUILT];
    struct srv s = { .type = DHCP_ACK, .xid = last_xid(), .yiaddr = TEST_ADDR,
                     .server = TEST_SERVER, .lease = lease, .t1 = t1, .t2 = t2 };
    dhcp_input(&c, 0, b, srv_build(b, sizeof(b), &s));
    CHECK(c.state == DHCP_BOUND);
    *out = sc.lease;
    return true;
}

bool t_dhcpc_times(void)
{
    struct dhcp_lease l;
    CHECK(lease_times(3600, 0, 0, &l) && l.t1_s == 1800 && l.t2_s == 3150);
    CHECK(lease_times(3600, 1000, 3000, &l) && l.t1_s == 1000 && l.t2_s == 3000);
    CHECK(lease_times(3600, 3000, 1000, &l) && l.t1_s == 1800 && l.t2_s == 3150);
    CHECK(lease_times(3600, 1000, 3600, &l) && l.t1_s == 1800 && l.t2_s == 3150);
    CHECK(lease_times(1, 0, 0, &l) && l.lease_s == DHCP_MIN_LEASE_S && l.t1_s == 5);
    CHECK(lease_times(DHCP_INFINITE, 0, 0, &l) && l.lease_s == DHCP_INFINITE);
    CHECK_EQ(dhcp_deadline(&c), DEADLINE_NEVER);
    dhcp_tick(&c, 1000000 * S);
    CHECK(c.state == DHCP_BOUND && sc.nsent == 2);
    /* a renewal that changes the DNS server: the edge is told again */
    reset(&no_probe);
    if (!get_lease(0, 3600))
        return false;
    dhcp_tick(&c, 1800 * S);
    uint8_t b[DHCP_MSG_BUILT];
    struct srv s = { .type = DHCP_ACK, .xid = last_xid(), .yiaddr = TEST_ADDR,
                     .server = TEST_SERVER, .mask = 0xffffff00u, .router = TEST_SERVER,
                     .dns = NET_IPV4(1, 1, 1, 1), .lease = 3600 };
    dhcp_input(&c, 1800 * S, b, srv_build(b, sizeof(b), &s));
    CHECK(c.state == DHCP_BOUND && sc.nbound == 2 && sc.lease.dns[0] == NET_IPV4(1, 1, 1, 1));
    CHECK_EQ(dhcp_deadline(&c), 3600 * S);   /* T1 from the renewal's REQUEST */
    return true;
}

bool t_dhcpc_stop(void)
{
    reset(&no_probe);
    dhcp_stop(&c, 0, true);   /* no lease: nothing to release */
    CHECK(sc.nsent == 0 && !sc.nunbound && c.state == DHCP_STOPPED);
    if (!get_lease(0, 3600))
        return false;
    unsigned n = sc.nsent;
    dhcp_stop(&c, S, true);
    CHECK(sc.nsent == n + 1 && last_type() == DHCP_RELEASE && last_to() == TEST_SERVER);
    CHECK(net_get32(last() + DHCP_OFF_CIADDR) == TEST_ADDR && last_opt(54) == TEST_SERVER);
    CHECK(sc.nunbound == 1 && sc.why == DHCP_WHY_STOPPED && c.state == DHCP_STOPPED);
    CHECK_EQ(dhcp_deadline(&c), DEADLINE_NEVER);
    reply(2 * S, DHCP_ACK);
    dhcp_tick(&c, 1000000 * S);
    CHECK(sc.nsent == n + 1 && c.state == DHCP_STOPPED && sc.nbound == 1);
    /* stopping without a release still clears the address */
    reset(&no_probe);
    if (!get_lease(0, 3600))
        return false;
    dhcp_stop(&c, S, false);
    CHECK(sc.nsent == 2 && sc.nunbound == 1);
    CHECK(!strcmp(dhcp_state_name(DHCP_REBINDING), "rebinding"));
    return true;
}

/* Mutated OFFERs and ACKs (for the client's own xid) fed to a client
 * waiting for them: never a fault; a state the reply may lead to; an
 * address bound only from a reply that says it. */
bool t_dhcpc_hostile(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint64_t seed = 0xbadd0c5u;
    uint8_t b[1500];
    for (unsigned i = 0; i < FUZZ_ROUNDS; i++) {
        reset(&no_probe);
        dhcp_start(&c, 0, 0);
        bool ack = i & 1;
        if (ack)
            reply(0, DHCP_OFFER);   /* now waiting for the ACK */
        struct srv s = { .type = ack ? DHCP_ACK : DHCP_OFFER, .xid = last_xid(),
                         .yiaddr = TEST_ADDR, .server = TEST_SERVER, .mask = 0xffffff00u,
                         .router = TEST_SERVER, .dns = TEST_SERVER, .lease = 3600 };
        size_t len = fuzz_mutate(&seed, b, srv_build(b, sizeof(b), &s), sizeof(b));
        dhcp_input(&c, S, guard_put(&g, b, len), len);
        if (!ack) {
            CHECK(c.state == DHCP_SELECTING || c.state == DHCP_REQUESTING);
            CHECK(c.state == DHCP_SELECTING || last_opt(50) == c.offer.yiaddr);
            continue;
        }
        CHECK(c.state == DHCP_REQUESTING || c.state == DHCP_BOUND || c.state == DHCP_INIT);
        CHECK(c.state != DHCP_BOUND || (sc.nbound == 1 && sc.lease.addr == TEST_ADDR));
    }
    guard_close(&g);
    return true;
}
