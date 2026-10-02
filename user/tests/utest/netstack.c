/* utest: netstack's core (user/services/netstack/stack.c, lwIP inside)
 * driven in-process over a fake edge: frames are built here byte by byte
 * and given to stack_input, and every frame lwIP sends is caught by the
 * edge's tx and checked byte by byte, checksums included. No lwIP header
 * is included: stack.h is the whole surface, as for the real loop.
 *
 * Covered: an ARP request for our address is answered (and one for
 * another is not); a ping from a known peer is answered, and one from an
 * unknown peer first sends an ARP request and is answered once the ARP
 * reply comes; a UDP datagram to a port nobody listens on gets an ICMP
 * port unreachable, rate-limited in a burst; malformed frames (runts,
 * oversized, a VLAN tag, bad IP header lengths and checksums, fragments,
 * IP options, bad ICMP checksums, short ARP) are dropped without an
 * answer, each counted where lwIP says; a mutation fuzz of thousands of
 * frames leaves no buffer or heap byte behind; after `clear` nothing is
 * answered. Every frame out is untagged and at least 60 bytes. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "stack.h"
#include "utest.h"

#define OUR_IP   0x0a021505u   /* 10.2.21.5 */
#define PEER_IP  0x0a0215aeu   /* 10.2.21.174: the Mac */
#define OTHER_IP 0x0a0215c8u   /* 10.2.21.200: a peer we have no ARP entry for */
#define GW_IP    0x0a021501u   /* 10.2.21.1 */
#define MASK24   0xffffff00u

#define ETH_ARP  0x0806u
#define ETH_IPV4 0x0800u
#define CAP_MAX  40u           /* frames the fake edge keeps */

static const uint8_t our_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t peer_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t other_mac[6] = { 0x02, 0x66, 0x77, 0x88, 0x99, 0xaa };
static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* What the edge was given since the last reset. */
static struct {
    uint8_t f[STACK_FRAME_MAX];
    size_t  n;
} cap[CAP_MAX];
static unsigned ncap;   /* frames caught (those past CAP_MAX refused) */

static status_t cap_tx(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (ncap >= CAP_MAX || len > STACK_FRAME_MAX)
        return ERR_NO_RESOURCES;
    memcpy(cap[ncap].f, frame, len);
    cap[ncap++].n = len;
    return OK;
}

/* ---- building frames ------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) << 16 | get16(p + 2);
}

/* The Internet checksum's sum over n bytes: 0xffff when a block that
 * carries its own checksum is intact. */
static uint32_t sum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        s += get16(p + i);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return s;
}

static void eth(uint8_t *f, const uint8_t *dst, const uint8_t *src, uint32_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    put16(f + 12, type);
}

/* An ARP request (op 1) or reply (op 2) from sha/spa to tha/tpa: 42 bytes. */
static size_t arp(uint8_t *f, uint32_t op, const uint8_t *sha, uint32_t spa, const uint8_t *tha,
                  uint32_t tpa)
{
    eth(f, op == 1 ? bcast : tha, sha, ETH_ARP);
    put16(f + 14, 1);          /* hardware: Ethernet */
    put16(f + 16, ETH_IPV4);   /* protocol: IPv4 */
    f[18] = 6;
    f[19] = 4;
    put16(f + 20, op);
    memcpy(f + 22, sha, 6);
    put32(f + 28, spa);
    memcpy(f + 32, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    put32(f + 38, tpa);
    return 42;
}

/* An IPv4 header (20 bytes, checksummed) at f + 14 for `len` payload bytes. */
static void ipv4(uint8_t *f, uint32_t proto, uint32_t src, uint32_t dst, size_t len)
{
    uint8_t *ip = f + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    put16(ip + 2, (uint32_t)(20 + len));
    put16(ip + 4, 0x4242);   /* id */
    ip[8] = 64;
    ip[9] = (uint8_t)proto;
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, ~sum16(ip, 20) & 0xffff);
}

/* An ICMP echo request from src_mac/src to us, `data` payload bytes. */
static size_t echo(uint8_t *f, const uint8_t *src_mac, uint32_t src, uint32_t dst, uint32_t seq,
                   size_t data)
{
    eth(f, our_mac, src_mac, ETH_IPV4);
    uint8_t *icmp = f + 34;
    icmp[0] = 8;
    icmp[1] = 0;
    put16(icmp + 2, 0);
    put16(icmp + 4, 0x1234);   /* id */
    put16(icmp + 6, seq);
    for (size_t i = 0; i < data; i++)
        icmp[8 + i] = (uint8_t)(i * 7 + seq);
    put16(icmp + 2, ~sum16(icmp, 8 + data) & 0xffff);
    ipv4(f, 1, src, dst, 8 + data);
    return 34 + 8 + data;
}

/* A UDP datagram from the peer to our port `dport` (checksum 0: none). */
static size_t udp(uint8_t *f, uint32_t dport, size_t data)
{
    eth(f, our_mac, peer_mac, ETH_IPV4);
    uint8_t *u = f + 34;
    put16(u, 40000);
    put16(u + 2, dport);
    put16(u + 4, (uint32_t)(8 + data));
    put16(u + 6, 0);
    memset(u + 8, 'u', data);
    ipv4(f, 17, PEER_IP, OUR_IP, 8 + data);
    return 34 + 8 + data;
}

/* ---- the stack under test ---------------------------------------------------- */

static uint32_t heap_at_start;   /* lwIP's heap in use with nothing going on */

/* A fresh interface on the fake edge: link up, 10.2.21.5/24. */
static bool net_up(void)
{
    struct stack_edge e = { .tx = cap_tx };
    memcpy(e.mac, our_mac, 6);
    stack_stop();
    CHECK_ST(stack_start(&e), OK);
    stack_set_link(true);
    struct stack_ipv4 ip = { OUR_IP, MASK24, GW_IP };
    stack_set_ipv4(&ip);
    struct stack_counts c;
    stack_get_counts(&c);
    heap_at_start = c.heap_used;
    ncap = 0;
    return true;
}

static void net_down(void)
{
    stack_clear();
    stack_stop();
}

/* Every frame out: untagged, 60 bytes at least, from our MAC. */
static bool frame_ok(unsigned i)
{
    const uint8_t *f = cap[i].f;
    CHECK(cap[i].n >= STACK_FRAME_MIN && cap[i].n <= STACK_FRAME_MAX);
    CHECK(get16(f + 12) == ETH_ARP || get16(f + 12) == ETH_IPV4);   /* never 0x8100 */
    CHECK(!memcmp(f + 6, our_mac, 6));
    return true;
}

/* Frame i is an ARP reply telling `to` (mac, ip) that we are OUR_IP. */
static bool is_arp_reply(unsigned i, const uint8_t *to, uint32_t to_ip)
{
    const uint8_t *f = cap[i].f;
    CHECK(frame_ok(i));
    CHECK_EQ(get16(f + 12), ETH_ARP);
    CHECK(!memcmp(f, to, 6));
    CHECK_EQ(get16(f + 20), 2);
    CHECK(!memcmp(f + 22, our_mac, 6));
    CHECK_EQ(get32(f + 28), OUR_IP);
    CHECK(!memcmp(f + 32, to, 6));
    CHECK_EQ(get32(f + 38), to_ip);
    for (size_t k = 42; k < cap[i].n; k++)
        CHECK_EQ(f[k], 0);   /* the padding is zeros */
    return true;
}

/* Frame i is a valid IPv4 packet from us to dst with protocol proto;
 * *icmp: its payload (n bytes). */
static bool is_ipv4(unsigned i, uint32_t dst, uint32_t proto, const uint8_t **pl, size_t *n)
{
    const uint8_t *f = cap[i].f, *ip = f + 14;
    CHECK(frame_ok(i));
    CHECK_EQ(get16(f + 12), ETH_IPV4);
    CHECK_EQ(ip[0], 0x45);
    CHECK_EQ(sum16(ip, 20), 0xffff);
    size_t total = get16(ip + 2);
    CHECK(total >= 20 && 14 + total <= cap[i].n);
    CHECK_EQ(ip[8], 64);   /* TTL */
    CHECK_EQ(ip[9], proto);
    CHECK_EQ(get32(ip + 12), OUR_IP);
    CHECK_EQ(get32(ip + 16), dst);
    CHECK_EQ(get16(ip + 6) & 0x3fff, 0);   /* not a fragment */
    *pl = ip + 20;
    *n = total - 20;
    return true;
}

/* Frame i answers echo request `req` (as built by echo(), data bytes). */
static bool is_echo_reply(unsigned i, const uint8_t *req, const uint8_t *to, uint32_t to_ip,
                          size_t data)
{
    const uint8_t *icmp;
    size_t n;
    CHECK(is_ipv4(i, to_ip, 1, &icmp, &n));
    CHECK(!memcmp(cap[i].f, to, 6));
    CHECK_EQ(n, 8 + data);
    CHECK_EQ(icmp[0], 0);
    CHECK_EQ(icmp[1], 0);
    CHECK_EQ(sum16(icmp, n), 0xffff);
    CHECK(!memcmp(icmp + 4, req + 34 + 4, 4 + data));   /* id, seq and data echoed */
    return true;
}

static void input(const uint8_t *f, size_t n)
{
    stack_input(f, n);
}

/* ---- the tests ---------------------------------------------------------------- */

bool t_netstack_arp(void)
{
    static uint8_t f[64];
    CHECK(net_up());
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));
    CHECK_EQ(ncap, 1);
    CHECK(is_arp_reply(0, peer_mac, PEER_IP));
    CHECK_EQ(cap[0].n, STACK_FRAME_MIN);
    ncap = 0;
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP + 1));   /* not ours */
    input(f, arp(f, 2, peer_mac, PEER_IP, our_mac, OUR_IP));    /* a reply nobody asked for */
    CHECK_EQ(ncap, 0);
    net_down();
    return true;
}

bool t_netstack_ping(void)
{
    static uint8_t f[STACK_FRAME_MAX], req[STACK_FRAME_MAX];
    CHECK(net_up());
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));   /* we learn the peer */
    ncap = 0;
    size_t n = echo(req, peer_mac, PEER_IP, OUR_IP, 1, 56);   /* `ping`'s default size */
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(is_echo_reply(0, req, peer_mac, PEER_IP, 56));
    ncap = 0;
    n = echo(req, peer_mac, PEER_IP, OUR_IP, 2, STACK_MTU - 28);   /* the biggest that fits */
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK_EQ(cap[0].n, STACK_FRAME_MAX);
    CHECK(is_echo_reply(0, req, peer_mac, PEER_IP, STACK_MTU - 28));
    /* A peer we don't know yet: ARP first, then the reply. */
    ncap = 0;
    n = echo(req, other_mac, OTHER_IP, OUR_IP, 3, 8);
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(frame_ok(0));
    CHECK_EQ(get16(cap[0].f + 12), ETH_ARP);
    CHECK(!memcmp(cap[0].f, bcast, 6));
    CHECK_EQ(get16(cap[0].f + 20), 1);
    CHECK_EQ(get32(cap[0].f + 38), OTHER_IP);
    input(f, arp(f, 2, other_mac, OTHER_IP, our_mac, OUR_IP));
    CHECK_EQ(ncap, 2);
    CHECK(is_echo_reply(1, req, other_mac, OTHER_IP, 8));
    struct stack_counts c;
    stack_get_counts(&c);
    CHECK(c.echo_replies >= 3);
    net_down();
    return true;
}

bool t_netstack_udp_unreachable(void)
{
    static uint8_t f[STACK_FRAME_MAX];
    CHECK(net_up());
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));
    ncap = 0;
    struct stack_counts before, after;
    stack_get_counts(&before);
    size_t n = udp(f, 9999, 20);
    input(f, n);
    CHECK_EQ(ncap, 1);
    const uint8_t *icmp;
    size_t len;
    CHECK(is_ipv4(0, PEER_IP, 1, &icmp, &len));
    CHECK_EQ(icmp[0], 3);   /* destination unreachable */
    CHECK_EQ(icmp[1], 3);   /* port unreachable */
    CHECK_EQ(sum16(icmp, len), 0xffff);
    CHECK_EQ(len, 8u + 20 + 8);   /* the datagram's IP header and its first 8 bytes */
    CHECK(!memcmp(icmp + 8, f + 14, 28));
    /* A burst: at most a second's worth of answers. */
    ncap = 0;
    for (int i = 0; i < 30; i++)
        input(f, n);
    stack_get_counts(&after);
    CHECK(ncap <= STACK_ICMP_ERR_PER_S + 1);
    CHECK(after.icmp_limited - before.icmp_limited >= 30 - (STACK_ICMP_ERR_PER_S + 1) - 1);
    CHECK_EQ(after.icmp_errors - before.icmp_errors, 1 + ncap);
    /* No answer to a broadcast datagram. */
    ncap = 0;
    eth(f, bcast, peer_mac, ETH_IPV4);
    ipv4(f, 17, PEER_IP, 0x0a0215ffu, 8 + 20);
    input(f, n);
    CHECK_EQ(ncap, 0);
    net_down();
    return true;
}

/* Malformed frames, each an echo request (or an ARP request) spoiled one
 * way. */
enum bad {
    BAD_RUNT, BAD_OVERSIZED, BAD_VLAN_TAG, BAD_ETHERTYPE, BAD_VERSION, BAD_IHL_SHORT,
    BAD_IHL_LONG, BAD_TOTAL_LONG, BAD_TOTAL_SHORT, BAD_IP_CHECKSUM, BAD_ICMP_CHECKSUM,
    BAD_TRUNCATED, BAD_MORE_FRAGS, BAD_FRAG_OFFSET, BAD_OPTIONS, BAD_ARP_HW, BAD_TO_BROADCAST,
    BAD_COUNT
};

static const char *const bad_names[BAD_COUNT] = {
    "runt", "oversized", "VLAN-tagged", "unknown EtherType", "IP version 6", "IHL 4", "IHL 15",
    "total length past the frame", "total length under 20", "bad IP checksum",
    "bad ICMP checksum", "truncated", "more-fragments", "fragment offset", "IP options",
    "ARP not for Ethernet", "ping to the broadcast address",
};

/* The IPv4 header checksum again, after a change to the header. */
static void refix(uint8_t *f)
{
    put16(f + 24, 0);
    put16(f + 24, ~sum16(f + 14, 20) & 0xffff);
}

/* An IP header of 24 bytes: one word of NOP options moved in front of the
 * ICMP message. */
static void add_options(uint8_t *f, size_t *len)
{
    memmove(f + 38, f + 34, *len - 34);
    memset(f + 34, 1, 4);
    *len += 4;
    f[14] = 0x46;
    put16(f + 16, get16(f + 16) + 4);
    refix(f);
}

/* Frame `which` into f (an echo request of 32 bytes spoiled); its length. */
static size_t spoiled(uint8_t *f, enum bad which)
{
    size_t n = echo(f, peer_mac, PEER_IP, OUR_IP, which, 32);
    switch (which) {
    case BAD_RUNT:          return 13;
    case BAD_OVERSIZED:     return STACK_FRAME_MAX + 1;
    case BAD_VLAN_TAG:      put16(f + 12, 0x8100); break;
    case BAD_ETHERTYPE:     put16(f + 12, 0x88b5); break;
    case BAD_VERSION:       f[14] = 0x65; break;
    case BAD_IHL_SHORT:     f[14] = 0x44; break;
    case BAD_IHL_LONG:      f[14] = 0x4f; break;
    case BAD_TOTAL_LONG:    put16(f + 16, 1400); break;
    case BAD_TOTAL_SHORT:   put16(f + 16, 19); break;
    case BAD_IP_CHECKSUM:   f[24] ^= 0x01; break;
    case BAD_ICMP_CHECKSUM: f[36] ^= 0x01; break;
    case BAD_TRUNCATED:     return n - 10;
    case BAD_MORE_FRAGS:    f[20] = 0x20; refix(f); break;
    case BAD_FRAG_OFFSET:   f[21] = 0x10; refix(f); break;
    case BAD_OPTIONS:       add_options(f, &n); break;
    case BAD_ARP_HW:        n = arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP); put16(f + 14, 6); break;
    case BAD_TO_BROADCAST:  put32(f + 30, 0x0a0215ffu); refix(f); break;
    case BAD_COUNT:         break;
    }
    return n;
}

/* Everything lwIP or the edge dropped, added up. */
static uint64_t drops(void)
{
    struct stack_counts c;
    stack_get_counts(&c);
    return c.rx_refused + c.link_dropped + c.arp_dropped + c.ip_dropped + c.icmp_dropped +
           c.udp_dropped + c.bad_checksums;   /* lwIP counts a bad ICMP checksum only there */
}

/* An ARP request cut short (24 bytes: 10 of its 28) right after a whole
 * one: lwIP's ARP input reads the 28 bytes without checking the frame has
 * them, so unless netstack pads a short frame with zeros it would read the
 * rest of the last frame from the buffer and answer it. */
static bool short_arp_after_whole(uint8_t *f)
{
    size_t n = arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP);
    ncap = 0;
    input(f, n);
    CHECK_EQ(ncap, 1);
    ncap = 0;
    input(f, 24);   /* padded, it asks who has 0.0.0.0: nobody answers that */
    if (ncap)
        FAIL("a short ARP request was answered");
    return true;
}

bool t_netstack_malformed(void)
{
    static uint8_t f[STACK_FRAME_MAX + 64], req[STACK_FRAME_MAX];
    CHECK(net_up());
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));
    for (unsigned i = 0; i < BAD_COUNT; i++) {
        ncap = 0;
        size_t n = spoiled(f, i);
        uint64_t before = drops();
        input(f, n);
        if (ncap)
            FAIL("a %s frame was answered", bad_names[i]);
        if (drops() == before && i != BAD_TO_BROADCAST)   /* lwIP doesn't count that one */
            FAIL("a %s frame was dropped but not counted", bad_names[i]);
    }
    CHECK(short_arp_after_whole(f));
    ncap = 0;
    size_t n = echo(req, peer_mac, PEER_IP, OUR_IP, 99, 16);   /* still alive */
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(is_echo_reply(0, req, peer_mac, PEER_IP, 16));
    struct stack_counts c;
    stack_get_counts(&c);
    CHECK_EQ(c.rx_buffers_used, 0);
    CHECK_EQ(c.heap_used, heap_at_start);
    net_down();
    return true;
}


static uint32_t xorshift(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* Thousands of frames: valid pings, ARP and UDP with a few bytes changed
 * at random, cut short or grown. Whatever lwIP makes of them, it doesn't
 * crash, and once their ARP waits are cleared it holds no buffer and no
 * heap byte more than before. */
bool t_netstack_fuzz(void)
{
    static uint8_t f[STACK_FRAME_MAX], req[STACK_FRAME_MAX];
    CHECK(net_up());
    uint32_t seed = 0x6a616d21;
    for (unsigned i = 0; i < 4000; i++) {
        size_t n;
        uint32_t r = xorshift(&seed);
        if (r % 3 == 0)
            n = echo(f, peer_mac, PEER_IP, OUR_IP, i, r >> 8 & 63);
        else if (r % 3 == 1)
            n = arp(f, 1 + (r >> 4 & 1), peer_mac, PEER_IP + (r >> 5 & 3), our_mac, OUR_IP);
        else
            n = udp(f, 1 + (r >> 6 & 0x3ff), r >> 16 & 63);
        for (unsigned k = 0, flips = 1 + (r >> 24 & 7); k < flips; k++)
            f[xorshift(&seed) % n] ^= (uint8_t)(1u << (xorshift(&seed) & 7));
        if (r >> 28 == 0)
            n = xorshift(&seed) % (n + 1);
        else if (r >> 28 == 1)
            n = n + xorshift(&seed) % (STACK_FRAME_MAX - n + 1);
        ncap = 0;
        input(f, n);
    }
    stack_clear();   /* drops the packets waiting on ARP */
    struct stack_counts c;
    stack_get_counts(&c);
    CHECK_EQ(c.rx_buffers_used, 0);
    CHECK_EQ(c.heap_used, heap_at_start);
    struct stack_ipv4 ip = { OUR_IP, MASK24, GW_IP };
    stack_set_ipv4(&ip);
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));
    ncap = 0;
    size_t n = echo(req, peer_mac, PEER_IP, OUR_IP, 7, 16);
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(is_echo_reply(0, req, peer_mac, PEER_IP, 16));
    net_down();
    return true;
}

/* After clear: no ARP answer, no echo answer; set again: answers again. */
bool t_netstack_cleared(void)
{
    static uint8_t f[STACK_FRAME_MAX];
    CHECK(net_up());
    stack_clear();
    struct stack_state s;
    stack_get(&s);
    CHECK_EQ(s.ip.address, 0);
    CHECK(s.link);
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));
    input(f, echo(f, peer_mac, PEER_IP, OUR_IP, 1, 8));
    CHECK_EQ(ncap, 0);
    struct stack_ipv4 ip = { OUR_IP, MASK24, 0 };
    stack_set_ipv4(&ip);
    stack_get(&s);
    CHECK_EQ(s.ip.address, OUR_IP);
    CHECK_EQ(s.ip.mask, MASK24);
    CHECK_EQ(s.ip.gateway, 0);
    /* A new address is announced: one gratuitous ARP (a broadcast request
     * for our own address, from it). */
    CHECK_EQ(ncap, 1);
    CHECK(frame_ok(0));
    CHECK(!memcmp(cap[0].f, bcast, 6));
    CHECK_EQ(get16(cap[0].f + 12), ETH_ARP);
    CHECK_EQ(get32(cap[0].f + 28), OUR_IP);
    CHECK_EQ(get32(cap[0].f + 38), OUR_IP);
    ncap = 0;
    input(f, arp(f, 1, peer_mac, PEER_IP, NULL, OUR_IP));
    CHECK_EQ(ncap, 1);
    CHECK(is_arp_reply(0, peer_mac, PEER_IP));
    /* Link down: nothing goes out. */
    stack_set_link(false);
    ncap = 0;
    input(f, echo(f, peer_mac, PEER_IP, OUR_IP, 2, 8));
    CHECK_EQ(ncap, 0);
    net_down();
    return true;
}
