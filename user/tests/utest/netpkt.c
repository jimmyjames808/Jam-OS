/* utest: Ethernet, ARP, IPv4, ICMP and UDP frames built and checked byte
 * by byte, for the netstack tests (netstack.c: the core over a fake edge;
 * netdrv.c: bin/netstack over a fake driver's rings). netpkt.h says what
 * each one makes or checks. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "netpkt.h"
#include "utest.h"

const uint8_t pkt_our_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
const uint8_t pkt_peer_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
const uint8_t pkt_other_mac[6] = { 0x02, 0x66, 0x77, 0x88, 0x99, 0xaa };
const uint8_t pkt_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ---- building ------------------------------------------------------------ */

void pkt_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void pkt_put32(uint8_t *p, uint32_t v)
{
    pkt_put16(p, v >> 16);
    pkt_put16(p + 2, v);
}

uint32_t pkt_get16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

uint32_t pkt_get32(const uint8_t *p)
{
    return pkt_get16(p) << 16 | pkt_get16(p + 2);
}

/* The Internet checksum's sum over n bytes: 0xffff when a block that
 * carries its own checksum is intact. */
uint32_t pkt_sum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        s += pkt_get16(p + i);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return s;
}

void pkt_eth(uint8_t *f, const uint8_t *dst, const uint8_t *src, uint32_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    pkt_put16(f + 12, type);
}

/* An ARP request (op 1) or reply (op 2) from sha/spa to tha/tpa: 42 bytes. */
size_t pkt_arp(uint8_t *f, uint32_t op, const uint8_t *sha, uint32_t spa, const uint8_t *tha,
               uint32_t tpa)
{
    pkt_eth(f, op == 1 ? pkt_bcast : tha, sha, ETH_ARP);
    pkt_put16(f + 14, 1);          /* hardware: Ethernet */
    pkt_put16(f + 16, ETH_IPV4);   /* protocol: IPv4 */
    f[18] = 6;
    f[19] = 4;
    pkt_put16(f + 20, op);
    memcpy(f + 22, sha, 6);
    pkt_put32(f + 28, spa);
    memcpy(f + 32, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tha, 6);
    pkt_put32(f + 38, tpa);
    return 42;
}

/* An IPv4 header (20 bytes, checksummed) at f + 14 for `len` payload bytes. */
void pkt_ipv4(uint8_t *f, uint32_t proto, uint32_t src, uint32_t dst, size_t len)
{
    uint8_t *ip = f + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    pkt_put16(ip + 2, (uint32_t)(20 + len));
    pkt_put16(ip + 4, 0x4242);   /* id */
    ip[8] = 64;
    ip[9] = (uint8_t)proto;
    pkt_put32(ip + 12, src);
    pkt_put32(ip + 16, dst);
    pkt_put16(ip + 10, ~pkt_sum16(ip, 20) & 0xffff);
}

/* An ICMP echo request from src_mac/src to us, `data` payload bytes. */
size_t pkt_echo(uint8_t *f, const uint8_t *src_mac, uint32_t src, uint32_t dst, uint32_t seq,
                size_t data)
{
    pkt_eth(f, pkt_our_mac, src_mac, ETH_IPV4);
    uint8_t *icmp = f + 34;
    icmp[0] = 8;
    icmp[1] = 0;
    pkt_put16(icmp + 2, 0);
    pkt_put16(icmp + 4, 0x1234);   /* id */
    pkt_put16(icmp + 6, seq);
    for (size_t i = 0; i < data; i++)
        icmp[8 + i] = (uint8_t)(i * 7 + seq);
    pkt_put16(icmp + 2, ~pkt_sum16(icmp, 8 + data) & 0xffff);
    pkt_ipv4(f, 1, src, dst, 8 + data);
    return 34 + 8 + data;
}

/* A UDP datagram from the peer to our port `dport` (checksum 0: none). */
size_t pkt_udp(uint8_t *f, uint32_t dport, size_t data)
{
    pkt_eth(f, pkt_our_mac, pkt_peer_mac, ETH_IPV4);
    uint8_t *u = f + 34;
    pkt_put16(u, 40000);
    pkt_put16(u + 2, dport);
    pkt_put16(u + 4, (uint32_t)(8 + data));
    pkt_put16(u + 6, 0);
    memset(u + 8, 'u', data);
    pkt_ipv4(f, 17, PEER_IP, OUR_IP, 8 + data);
    return 34 + 8 + data;
}

size_t pkt_udp_from(uint8_t *f, const uint8_t *src_mac, uint32_t src, uint32_t sport,
                    uint32_t dst, uint32_t dport, const void *data, size_t len)
{
    pkt_eth(f, dst == 0xffffffffu ? pkt_bcast : pkt_our_mac, src_mac, ETH_IPV4);
    uint8_t *u = f + 34;
    pkt_put16(u, sport);
    pkt_put16(u + 2, dport);
    pkt_put16(u + 4, (uint32_t)(8 + len));
    pkt_put16(u + 6, 0);
    memcpy(u + 8, data, len);
    pkt_ipv4(f, 17, src, dst, 8 + len);
    return 34 + 8 + len;
}

size_t pkt_echo_reply_to(uint8_t *f, const uint8_t *req, size_t icmp_len)
{
    pkt_eth(f, pkt_our_mac, pkt_peer_mac, ETH_IPV4);
    memmove(f + 34, req + 34, icmp_len);   /* f may be req */
    f[34] = 0;
    pkt_put16(f + 36, 0);
    pkt_put16(f + 36, ~pkt_sum16(f + 34, icmp_len) & 0xffff);
    pkt_ipv4(f, 1, pkt_get32(req + 30), OUR_IP, icmp_len);
    return 34 + icmp_len;
}

/* ---- checks ---------------------------------------------------------------- */

bool pkt_frame_ok(const uint8_t *f, size_t n)
{
    CHECK(n >= PKT_FRAME_MIN && n <= PKT_FRAME_MAX);
    CHECK(pkt_get16(f + 12) == ETH_ARP || pkt_get16(f + 12) == ETH_IPV4);   /* never 0x8100 */
    CHECK(!memcmp(f + 6, pkt_our_mac, 6));
    return true;
}

bool pkt_is_arp_reply(const uint8_t *f, size_t n, const uint8_t *to, uint32_t to_ip)
{
    CHECK(pkt_frame_ok(f, n));
    CHECK_EQ(pkt_get16(f + 12), ETH_ARP);
    CHECK(!memcmp(f, to, 6));
    CHECK_EQ(pkt_get16(f + 20), 2);
    CHECK(!memcmp(f + 22, pkt_our_mac, 6));
    CHECK_EQ(pkt_get32(f + 28), OUR_IP);
    CHECK(!memcmp(f + 32, to, 6));
    CHECK_EQ(pkt_get32(f + 38), to_ip);
    for (size_t k = 42; k < n; k++)
        CHECK_EQ(f[k], 0);   /* the padding is zeros */
    return true;
}

bool pkt_is_ipv4(const uint8_t *f, size_t n, uint32_t dst, uint32_t proto, const uint8_t **pl,
                 size_t *pn)
{
    const uint8_t *ip = f + 14;
    CHECK(pkt_frame_ok(f, n));
    CHECK_EQ(pkt_get16(f + 12), ETH_IPV4);
    CHECK_EQ(ip[0], 0x45);
    CHECK_EQ(pkt_sum16(ip, 20), 0xffff);
    size_t total = pkt_get16(ip + 2);
    CHECK(total >= 20 && 14 + total <= n);
    CHECK_EQ(ip[8], 64);   /* TTL */
    CHECK_EQ(ip[9], proto);
    CHECK_EQ(pkt_get32(ip + 12), OUR_IP);
    CHECK_EQ(pkt_get32(ip + 16), dst);
    CHECK_EQ(pkt_get16(ip + 6) & 0x3fff, 0);   /* not a fragment */
    *pl = ip + 20;
    *pn = total - 20;
    return true;
}

bool pkt_is_echo_reply(const uint8_t *f, size_t n, const uint8_t *req, const uint8_t *to,
                       uint32_t to_ip, size_t data)
{
    const uint8_t *icmp;
    size_t len;
    CHECK(pkt_is_ipv4(f, n, to_ip, 1, &icmp, &len));
    CHECK(!memcmp(f, to, 6));
    CHECK_EQ(len, 8 + data);
    CHECK_EQ(icmp[0], 0);
    CHECK_EQ(icmp[1], 0);
    CHECK_EQ(pkt_sum16(icmp, len), 0xffff);
    CHECK(!memcmp(icmp + 4, req + 34 + 4, 4 + data));   /* id, seq and data echoed */
    return true;
}
