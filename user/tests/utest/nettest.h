/* utest: what the DHCP and DNS tests share (netfuzz.c: the guard page
 * and the mutations; dhcp.c: a DHCP server's replies and the sample
 * OFFER; dns.c: a DNS server's replies). */
#pragma once

#include <os.h>

#define FUZZ_ROUNDS 4000u   /* mutated datagrams per sample in a fuzz test */

/* Two pages, the second with no access: guard_put copies len bytes (at
 * most a page) to end exactly where the second page begins, so a read
 * one byte past them faults. */
struct guard {
    uint8_t *base;
};
bool     guard_open(struct guard *g);
void     guard_close(struct guard *g);
uint8_t *guard_put(struct guard *g, const void *bytes, size_t len);
/* 32 random bits from the generator state *s (seeded by the test). */
uint32_t fuzz_rand(uint64_t *s);
/* 1 to 4 random mutations of buf (len bytes, room for cap): the new length. */
size_t   fuzz_mutate(uint64_t *s, uint8_t *buf, size_t len, size_t cap);

/* dhcp.c: the hardware address the DHCP tests use (locally administered). */
extern const uint8_t test_mac[6];
#define TEST_SERVER NET_IPV4(10, 2, 21, 1)
#define TEST_ADDR   NET_IPV4(10, 2, 21, 67)
/* A server's reply, for the client's tests: each option left out when 0. */
struct srv {
    uint8_t  type;     /* DHCP_OFFER, DHCP_ACK, DHCP_NAK */
    uint32_t xid, yiaddr, server, mask, router, dns, lease, t1, t2;
};
size_t   srv_build(uint8_t *buf, size_t cap, const struct srv *s);
/* The sample OFFER (or, with type DHCP_ACK, the same as an ACK), as a
 * VLAN 21 router sends it: 305 bytes into buf (at least 305). */
size_t   sample_offer(uint8_t *buf, uint8_t type);
#define SAMPLE_XID 0x3903f326u
/* The option `code` in a message the client built (len bytes): its code
 * byte, or NULL. The test's own walk, not msg.c's. */
const uint8_t *test_dhcp_opt(const uint8_t *b, size_t len, uint8_t code);

/* dns.c: the reply to a query (query, qlen bytes, as the resolver sent
 * it), with these answers for the name asked: n A records with ttl, or
 * a CNAME to `cname` (then n A records for it, when n > 0). rcode and
 * tc go in the header. Its length, or 0 if cap is too small. */
struct dns_srv {
    const uint32_t *addr;
    unsigned        n;
    uint32_t        ttl;
    const char     *cname;   /* NULL: none */
    uint8_t         rcode;
    bool            tc;
};
size_t   dns_srv_reply(uint8_t *buf, size_t cap, const uint8_t *query, size_t qlen,
                       const struct dns_srv *s);
