/* utest: frames for the netstack tests (netpkt.c): built byte by byte,
 * and checked byte by byte (checksums included) as netstack sends them.
 * "Us" is OUR_IP at pkt_our_mac; the peers are made up. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OUR_IP   0x0a021505u   /* 10.2.21.5 */
#define PEER_IP  0x0a0215aeu   /* 10.2.21.174: the Mac */
#define OTHER_IP 0x0a0215c8u   /* 10.2.21.200: a peer we have no ARP entry for */
#define GW_IP    0x0a021501u   /* 10.2.21.1 */
#define MASK24   0xffffff00u

#define ETH_ARP  0x0806u
#define ETH_IPV4 0x0800u
#define PKT_FRAME_MIN 60u      /* what netstack sends: at least ... */
#define PKT_FRAME_MAX 1514u    /* ... and at most, untagged */

extern const uint8_t pkt_our_mac[6], pkt_peer_mac[6], pkt_other_mac[6], pkt_bcast[6];

/* Big-endian fields, and the Internet checksum's sum over n bytes
 * (0xffff when a block that carries its own checksum is intact). */
void     pkt_put16(uint8_t *p, uint32_t v);
void     pkt_put32(uint8_t *p, uint32_t v);
uint32_t pkt_get16(const uint8_t *p);
uint32_t pkt_get32(const uint8_t *p);
uint32_t pkt_sum16(const uint8_t *p, size_t n);

/* The Ethernet header. */
void   pkt_eth(uint8_t *f, const uint8_t *dst, const uint8_t *src, uint32_t type);
/* An ARP request (op 1, broadcast; tha unused) or reply (op 2) from
 * sha/spa about tpa: 42 bytes. */
size_t pkt_arp(uint8_t *f, uint32_t op, const uint8_t *sha, uint32_t spa, const uint8_t *tha,
               uint32_t tpa);
/* An IPv4 header (20 bytes, checksummed) at f + 14 for `len` payload bytes. */
void   pkt_ipv4(uint8_t *f, uint32_t proto, uint32_t src, uint32_t dst, size_t len);
/* An ICMP echo request from src_mac/src to dst at our MAC, `data` payload
 * bytes; its length. */
size_t pkt_echo(uint8_t *f, const uint8_t *src_mac, uint32_t src, uint32_t dst, uint32_t seq,
                size_t data);
/* A UDP datagram from the peer to our port `dport` (checksum 0: none). */
size_t pkt_udp(uint8_t *f, uint32_t dport, size_t data);
/* A UDP datagram from src:sport (src_mac) to dst:dport (our MAC, or the
 * broadcast MAC for 255.255.255.255) with len bytes of data, checksum 0. */
size_t pkt_udp_from(uint8_t *f, const uint8_t *src_mac, uint32_t src, uint32_t sport,
                    uint32_t dst, uint32_t dport, const void *data, size_t len);
/* The peer's echo reply to our echo request `req` (icmp_len bytes of ICMP
 * after a 20-byte IP header), to OUR_IP from the address req went to. */
size_t pkt_echo_reply_to(uint8_t *f, const uint8_t *req, size_t icmp_len);

/* Frame f (n bytes) as netstack must send it: untagged, 60..1514 bytes,
 * from our MAC. */
bool pkt_frame_ok(const uint8_t *f, size_t n);
/* ... an ARP reply telling `to` (mac, ip) that we are OUR_IP, zero-padded. */
bool pkt_is_arp_reply(const uint8_t *f, size_t n, const uint8_t *to, uint32_t to_ip);
/* ... a valid IPv4 packet from us to dst with protocol proto; *pl and *pn:
 * its payload. */
bool pkt_is_ipv4(const uint8_t *f, size_t n, uint32_t dst, uint32_t proto, const uint8_t **pl,
                 size_t *pn);
/* ... the reply to echo request `req` (as pkt_echo built it, data bytes). */
bool pkt_is_echo_reply(const uint8_t *f, size_t n, const uint8_t *req, const uint8_t *to,
                       uint32_t to_ip, size_t data);
