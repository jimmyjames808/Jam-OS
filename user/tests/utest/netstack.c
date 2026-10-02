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
#include "netpkt.h"
#include "stack.h"
#include "utest.h"

#define CAP_MAX  40u           /* frames the fake edge keeps */

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

/* ---- the stack under test ---------------------------------------------------- */

static uint32_t heap_at_start;   /* lwIP's heap in use with nothing going on */

/* A fresh interface on the fake edge: link up, 10.2.21.5/24. */
static bool net_up(void)
{
    struct stack_edge e = { .tx = cap_tx };
    memcpy(e.mac, pkt_our_mac, 6);
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

/* The checks of netpkt.h on caught frame i. */
static bool frame_ok(unsigned i)
{
    return pkt_frame_ok(cap[i].f, cap[i].n);
}

static bool is_arp_reply(unsigned i, const uint8_t *to, uint32_t to_ip)
{
    return pkt_is_arp_reply(cap[i].f, cap[i].n, to, to_ip);
}

static bool is_ipv4(unsigned i, uint32_t dst, uint32_t proto, const uint8_t **pl, size_t *n)
{
    return pkt_is_ipv4(cap[i].f, cap[i].n, dst, proto, pl, n);
}

static bool is_echo_reply(unsigned i, const uint8_t *req, const uint8_t *to, uint32_t to_ip,
                          size_t data)
{
    return pkt_is_echo_reply(cap[i].f, cap[i].n, req, to, to_ip, data);
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
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));
    CHECK_EQ(ncap, 1);
    CHECK(is_arp_reply(0, pkt_peer_mac, PEER_IP));
    CHECK_EQ(cap[0].n, STACK_FRAME_MIN);
    ncap = 0;
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP + 1));   /* not ours */
    /* A reply nobody asked for. */
    input(f, pkt_arp(f, 2, pkt_peer_mac, PEER_IP, pkt_our_mac, OUR_IP));
    CHECK_EQ(ncap, 0);
    net_down();
    return true;
}

bool t_netstack_ping(void)
{
    static uint8_t f[STACK_FRAME_MAX], req[STACK_FRAME_MAX];
    CHECK(net_up());
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));   /* we learn the peer */
    ncap = 0;
    size_t n = pkt_echo(req, pkt_peer_mac, PEER_IP, OUR_IP, 1, 56);   /* `ping`'s default size */
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(is_echo_reply(0, req, pkt_peer_mac, PEER_IP, 56));
    ncap = 0;
    /* The biggest that fits. */
    n = pkt_echo(req, pkt_peer_mac, PEER_IP, OUR_IP, 2, STACK_MTU - 28);
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK_EQ(cap[0].n, STACK_FRAME_MAX);
    CHECK(is_echo_reply(0, req, pkt_peer_mac, PEER_IP, STACK_MTU - 28));
    /* A peer we don't know yet: ARP first, then the reply. */
    ncap = 0;
    n = pkt_echo(req, pkt_other_mac, OTHER_IP, OUR_IP, 3, 8);
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(frame_ok(0));
    CHECK_EQ(pkt_get16(cap[0].f + 12), ETH_ARP);
    CHECK(!memcmp(cap[0].f, pkt_bcast, 6));
    CHECK_EQ(pkt_get16(cap[0].f + 20), 1);
    CHECK_EQ(pkt_get32(cap[0].f + 38), OTHER_IP);
    input(f, pkt_arp(f, 2, pkt_other_mac, OTHER_IP, pkt_our_mac, OUR_IP));
    CHECK_EQ(ncap, 2);
    CHECK(is_echo_reply(1, req, pkt_other_mac, OTHER_IP, 8));
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
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));
    ncap = 0;
    struct stack_counts before, after;
    stack_get_counts(&before);
    size_t n = pkt_udp(f, 9999, 20);
    input(f, n);
    CHECK_EQ(ncap, 1);
    const uint8_t *icmp;
    size_t len;
    CHECK(is_ipv4(0, PEER_IP, 1, &icmp, &len));
    CHECK_EQ(icmp[0], 3);   /* destination unreachable */
    CHECK_EQ(icmp[1], 3);   /* port unreachable */
    CHECK_EQ(pkt_sum16(icmp, len), 0xffff);
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
    pkt_eth(f, pkt_bcast, pkt_peer_mac, ETH_IPV4);
    pkt_ipv4(f, 17, PEER_IP, 0x0a0215ffu, 8 + 20);
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
    pkt_put16(f + 24, 0);
    pkt_put16(f + 24, ~pkt_sum16(f + 14, 20) & 0xffff);
}

/* An IP header of 24 bytes: one word of NOP options moved in front of the
 * ICMP message. */
static void add_options(uint8_t *f, size_t *len)
{
    memmove(f + 38, f + 34, *len - 34);
    memset(f + 34, 1, 4);
    *len += 4;
    f[14] = 0x46;
    pkt_put16(f + 16, pkt_get16(f + 16) + 4);
    refix(f);
}

/* Frame `which` into f (an echo request of 32 bytes spoiled); its length. */
static size_t spoiled(uint8_t *f, enum bad which)
{
    size_t n = pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, which, 32);
    switch (which) {
    case BAD_RUNT:          return 13;
    case BAD_OVERSIZED:     return STACK_FRAME_MAX + 1;
    case BAD_VLAN_TAG:      pkt_put16(f + 12, 0x8100); break;
    case BAD_ETHERTYPE:     pkt_put16(f + 12, 0x88b5); break;
    case BAD_VERSION:       f[14] = 0x65; break;
    case BAD_IHL_SHORT:     f[14] = 0x44; break;
    case BAD_IHL_LONG:      f[14] = 0x4f; break;
    case BAD_TOTAL_LONG:    pkt_put16(f + 16, 1400); break;
    case BAD_TOTAL_SHORT:   pkt_put16(f + 16, 19); break;
    case BAD_IP_CHECKSUM:   f[24] ^= 0x01; break;
    case BAD_ICMP_CHECKSUM: f[36] ^= 0x01; break;
    case BAD_TRUNCATED:     return n - 10;
    case BAD_MORE_FRAGS:    f[20] = 0x20; refix(f); break;
    case BAD_FRAG_OFFSET:   f[21] = 0x10; refix(f); break;
    case BAD_OPTIONS:       add_options(f, &n); break;
    case BAD_ARP_HW:
        n = pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP);
        pkt_put16(f + 14, 6);   /* IEEE 802, not Ethernet */
        break;
    case BAD_TO_BROADCAST:  pkt_put32(f + 30, 0x0a0215ffu); refix(f); break;
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
    size_t n = pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP);
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
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));
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
    size_t n = pkt_echo(req, pkt_peer_mac, PEER_IP, OUR_IP, 99, 16);   /* still alive */
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(is_echo_reply(0, req, pkt_peer_mac, PEER_IP, 16));
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
            n = pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, i, r >> 8 & 63);
        else if (r % 3 == 1)
            n = pkt_arp(f, 1 + (r >> 4 & 1), pkt_peer_mac, PEER_IP + (r >> 5 & 3), pkt_our_mac,
                        OUR_IP);
        else
            n = pkt_udp(f, 1 + (r >> 6 & 0x3ff), r >> 16 & 63);
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
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));
    ncap = 0;
    size_t n = pkt_echo(req, pkt_peer_mac, PEER_IP, OUR_IP, 7, 16);
    input(req, n);
    CHECK_EQ(ncap, 1);
    CHECK(is_echo_reply(0, req, pkt_peer_mac, PEER_IP, 16));
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
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));
    input(f, pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, 1, 8));
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
    CHECK(!memcmp(cap[0].f, pkt_bcast, 6));
    CHECK_EQ(pkt_get16(cap[0].f + 12), ETH_ARP);
    CHECK_EQ(pkt_get32(cap[0].f + 28), OUR_IP);
    CHECK_EQ(pkt_get32(cap[0].f + 38), OUR_IP);
    ncap = 0;
    input(f, pkt_arp(f, 1, pkt_peer_mac, PEER_IP, NULL, OUR_IP));
    CHECK_EQ(ncap, 1);
    CHECK(is_arp_reply(0, pkt_peer_mac, PEER_IP));
    /* Link down: nothing goes out. */
    stack_set_link(false);
    ncap = 0;
    input(f, pkt_echo(f, pkt_peer_mac, PEER_IP, OUR_IP, 2, 8));
    CHECK_EQ(ncap, 0);
    net_down();
    return true;
}
