/* utest: the resolver's messages (user/services/dns/msg.c, linked in by
 * the Makefile). Names and IPv4 literals; the query, byte for byte;
 * replies as servers send them (one.one.one.one, a CNAME to github.com,
 * an NXDOMAIN with its SOA), parsed; answers that must not count (other
 * names, other classes, duplicates); and hostile replies: every
 * truncation, compression pointers to themselves, forward and in loops,
 * too many jumps, names too long, bad label types and bytes, counts the
 * bytes don't hold, records whose data runs past the end, and a fuzz
 * loop of mutated replies. Every hostile datagram ends at an unreadable
 * page (netfuzz.c). Also the server replies the resolver's tests
 * (dnsres.c) use. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netbytes.h>
#include <os.h>
#include "dns.h"
#include "nettest.h"
#include "utest.h"

#define BUF 1500

/* Replies captured-style: one.one.one.one (id 1a2b: 1.1.1.1 and 1.0.0.1,
 * TTL 300, the owners compressed to the question); www.github.com (id
 * 4c5d: a CNAME to github.com, TTL 3600, whose "com" points into the
 * question, then github.com's A record, TTL 60, its owner pointing at the
 * CNAME's data); nosuch.example (id 7e01: NXDOMAIN, the zone's SOA in
 * the authority section). */
static const uint8_t one_reply[65] = {
    0x1a, 0x2b, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x6f, 0x6e, 0x65, 0x03, 0x6f, 0x6e, 0x65, 0x03, 0x6f, 0x6e, 0x65,
    0x03, 0x6f, 0x6e, 0x65, 0x00, 0x00, 0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x04, 0x01, 0x01, 0x01,
    0x01, 0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00,
    0x04, 0x01, 0x00, 0x00, 0x01,
};
static const uint8_t github_reply[69] = {
    0x4c, 0x5d, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x77, 0x77, 0x77, 0x06, 0x67, 0x69, 0x74, 0x68, 0x75, 0x62, 0x03,
    0x63, 0x6f, 0x6d, 0x00, 0x00, 0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x05,
    0x00, 0x01, 0x00, 0x00, 0x0e, 0x10, 0x00, 0x09, 0x06, 0x67, 0x69, 0x74,
    0x68, 0x75, 0x62, 0xc0, 0x17, 0xc0, 0x2c, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x3c, 0x00, 0x04, 0x8c, 0x52, 0x70, 0x03,
};
static const uint8_t nx_reply[82] = {
    0x7e, 0x01, 0x81, 0x83, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x06, 0x6e, 0x6f, 0x73, 0x75, 0x63, 0x68, 0x07, 0x65, 0x78, 0x61, 0x6d,
    0x70, 0x6c, 0x65, 0x00, 0x00, 0x01, 0x00, 0x01, 0xc0, 0x13, 0x00, 0x06,
    0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x26, 0x02, 0x6e, 0x73, 0xc0,
    0x13, 0x0a, 0x68, 0x6f, 0x73, 0x74, 0x6d, 0x61, 0x73, 0x74, 0x65, 0x72,
    0xc0, 0x13, 0x78, 0xc3, 0xd5, 0xe9, 0x00, 0x00, 0x1c, 0x20, 0x00, 0x00,
    0x0e, 0x10, 0x00, 0x12, 0x75, 0x00, 0x00, 0x00, 0x01, 0x2c,
};

struct sample {
    const uint8_t *b;
    size_t         len;
    uint16_t       id;
    const char    *name;
};
static const struct sample samples[] = {
    { one_reply, sizeof(one_reply), 0x1a2b, "one.one.one.one" },
    { github_reply, sizeof(github_reply), 0x4c5d, "www.github.com" },
    { nx_reply, sizeof(nx_reply), 0x7e01, "nosuch.example" },
};
#define NSAMPLES (sizeof(samples) / sizeof(samples[0]))

/* ---- building replies ---------------------------------------------------- */

/* One record of a built reply, its names written out in full. */
struct rrspec {
    const char *owner;
    uint16_t    type;     /* DNS_TYPE_A or DNS_TYPE_CNAME */
    uint16_t    class;    /* 0: IN */
    uint32_t    ttl;
    uint32_t    addr;     /* A */
    const char *target;   /* CNAME */
};

static size_t put_name(uint8_t *b, size_t at, const char *name)
{
    size_t start = 0, n = strlen(name);
    for (size_t i = 0; i <= n && n; i++) {
        if (i < n && name[i] != '.')
            continue;
        b[at++] = (uint8_t)(i - start);
        memcpy(b + at, name + start, i - start);
        at += i - start;
        start = i + 1;
    }
    b[at++] = 0;
    return at;
}

/* A reply to (id, qname) with these flags and answer records: its length. */
static size_t build(uint8_t *b, uint16_t id, const char *qname, uint16_t flags,
                    const struct rrspec *rr, unsigned n)
{
    memset(b, 0, 12);
    net_put16(b, id);
    net_put16(b + 2, flags);
    net_put16(b + 4, 1);
    net_put16(b + 6, (uint16_t)n);
    size_t at = put_name(b, 12, qname);
    net_put16(b + at, DNS_TYPE_A);
    net_put16(b + at + 2, DNS_CLASS_IN);
    at += 4;
    for (unsigned i = 0; i < n; i++) {
        at = put_name(b, at, rr[i].owner);
        net_put16(b + at, rr[i].type);
        net_put16(b + at + 2, rr[i].class ? rr[i].class : DNS_CLASS_IN);
        net_put32(b + at + 4, rr[i].ttl);
        if (rr[i].type == DNS_TYPE_A) {
            net_put16(b + at + 8, 4);
            net_put32(b + at + 10, rr[i].addr);
            at += 14;
        } else {
            size_t end = put_name(b, at + 10, rr[i].target);
            net_put16(b + at + 8, (uint16_t)(end - at - 10));
            at = end;
        }
    }
    return at;
}
#define REPLY (DNS_QR | DNS_RD | 0x0080u)   /* RA */

size_t dns_srv_reply(uint8_t *buf, size_t cap, const uint8_t *query, size_t qlen,
                     const struct dns_srv *s)
{
    char qname[DNS_NAME_MAX + 1];
    size_t pos = 12;
    if (qlen < 12 || dns_read_name(query, qlen, &pos, qname) != OK || cap < BUF)
        return 0;
    struct rrspec rr[DNS_MAX_ADDRS + 4];
    unsigned n = 0;
    const char *owner = qname;
    if (s->cname) {
        rr[n++] = (struct rrspec){ qname, DNS_TYPE_CNAME, 0, s->ttl, 0, s->cname };
        owner = s->cname;
    }
    for (unsigned i = 0; i < s->n && i < DNS_MAX_ADDRS + 2; i++)
        rr[n++] = (struct rrspec){ owner, DNS_TYPE_A, 0, s->ttl, s->addr[i], NULL };
    uint16_t flags = (uint16_t)(REPLY | s->rcode | (s->tc ? DNS_TC : 0));
    return build(buf, net_get16(query), qname, flags, rr, n);
}

/* Parse len bytes of b against the guard page. */
static status_t parse(struct guard *g, const uint8_t *b, size_t len, uint16_t id,
                      const char *name, struct dns_result *r)
{
    return dns_parse_reply(guard_put(g, b, len), len, id, name, r);
}

/* ---- the tests --------------------------------------------------------------- */

bool t_dns_names(void)
{
    static const char *const good[] = { "one.one.one.one", "one.one.one.one.", "a", "_dns.x-y.Z9" };
    static const char *const bad[] = { "", ".", "a..b", ".a", "a b", "a/b", "a.b..", "a\x01" };
    size_t len;
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        CHECK(dns_name_ok(good[i], &len));
    CHECK(dns_name_ok("one.one.one.one.", &len) && len == 15);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!dns_name_ok(bad[i], &len));
    char n[300];
    memset(n, 'a', sizeof(n));
    n[63] = 0;
    CHECK(dns_name_ok(n, &len));   /* a label of 63 */
    n[63] = 'a';
    n[64] = 0;
    CHECK(!dns_name_ok(n, &len));  /* of 64 */
    n[64] = 'a';
    for (unsigned i = 63; i < 253; i += 64)
        n[i] = '.';
    n[253] = 0;
    CHECK(dns_name_ok(n, &len) && len == 253);
    n[253] = 'a';
    n[254] = 0;
    CHECK(!dns_name_ok(n, &len));
    CHECK(dns_name_eq("One.EXAMPLE", "one.example") && !dns_name_eq("one", "one.") &&
          !dns_name_eq("a", "ab") && !dns_name_eq("a-", "a_"));
    uint32_t a = 0;
    CHECK(dns_ipv4_literal("1.1.1.1", &a) && a == NET_IPV4(1, 1, 1, 1));
    CHECK(dns_ipv4_literal("10.2.21.174", &a) && a == NET_IPV4(10, 2, 21, 174));
    CHECK(dns_ipv4_literal("255.255.255.255", &a) && dns_ipv4_literal("0.0.0.0", &a));
    static const char *const notip[] = { "256.1.1.1", "1.1.1", "1.1.1.1.1", "01.1.1.1", "1.1.1.1x",
                                         "", "1..1.1", "1111.1.1.1", "one.one.one.one", "1.1.1." };
    for (unsigned i = 0; i < sizeof(notip) / sizeof(notip[0]); i++)
        CHECK(!dns_ipv4_literal(notip[i], &a));
    /* the query: the reply's header and question, with RD and no answers */
    uint8_t q[DNS_MSG_MAX];
    CHECK_EQ(dns_build_query(q, sizeof(q), 0x1a2b, "one.one.one.one."), 33);
    CHECK(net_get16(q) == 0x1a2b && net_get16(q + 2) == DNS_RD && net_get16(q + 4) == 1);
    CHECK(!net_get16(q + 6) && !net_get16(q + 8) && !net_get16(q + 10));
    CHECK(!memcmp(q + 12, one_reply + 12, 33 - 12));
    CHECK_EQ(dns_build_query(q, 32, 1, "one.one.one.one"), 0);
    CHECK_EQ(dns_build_query(q, sizeof(q), 1, "a..b"), 0);
    return true;
}

bool t_dns_parse_samples(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    struct dns_result r;
    CHECK_ST(parse(&g, one_reply, sizeof(one_reply), 0x1a2b, "ONE.one.One.one", &r), OK);
    CHECK(r.outcome == DNS_ADDRS && r.naddr == 2 && r.ttl == 300 && !r.cnames);
    CHECK(r.addr[0] == NET_IPV4(1, 1, 1, 1) && r.addr[1] == NET_IPV4(1, 0, 0, 1));
    CHECK_ST(parse(&g, github_reply, sizeof(github_reply), 0x4c5d, "www.github.com", &r), OK);
    CHECK(r.outcome == DNS_ADDRS && r.naddr == 1 && r.addr[0] == NET_IPV4(140, 82, 112, 3));
    CHECK(r.cnames == 1 && r.ttl == 60);
    CHECK_ST(parse(&g, nx_reply, sizeof(nx_reply), 0x7e01, "nosuch.example", &r), OK);
    CHECK_EQ(r.outcome, DNS_NO_NAME);
    /* not the reply to this query */
    uint8_t b[BUF];
    CHECK_ST(parse(&g, one_reply, sizeof(one_reply), 0x1a2c, "one.one.one.one", &r),
             ERR_NOT_FOUND);
    CHECK_ST(parse(&g, one_reply, sizeof(one_reply), 0x1a2b, "one.one.one.two", &r),
             ERR_NOT_FOUND);
    const unsigned at[] = { 2, 2, 5, 5, 30 };
    const uint8_t clear[] = { 0x80, 0x00, 0x01, 0x01, 0x01 }, set[] = { 0, 0x08, 0, 2, 0x1c };
    for (unsigned i = 0; i < 5; i++) {
        /* QR off; an opcode; no question; two questions; type AAAA */
        memcpy(b, one_reply, sizeof(one_reply));
        b[at[i]] = (uint8_t)((b[at[i]] & ~clear[i]) | set[i]);
        CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, "one.one.one.one", &r), ERR_NOT_FOUND);
    }
    guard_close(&g);
    return true;
}

bool t_dns_parse_answers(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint8_t b[BUF];
    struct dns_result r;
    const uint32_t a1 = NET_IPV4(1, 2, 3, 4), a2 = NET_IPV4(5, 6, 7, 8);
#define P(flags, ...)                                                                         \
    parse(&g, b,                                                                              \
          build(b, 7, "x.example", flags, (const struct rrspec[]){ __VA_ARGS__ },             \
                sizeof((const struct rrspec[]){ __VA_ARGS__ }) / sizeof(struct rrspec)), \
          7, "x.example", &r)
    /* duplicates count once; another name's and another class's records not at all */
    CHECK_ST(P(REPLY, { "x.example", DNS_TYPE_A, 0, 50, a1, NULL },
               { "evil.example", DNS_TYPE_A, 0, 50, NET_IPV4(6, 6, 6, 6), NULL },
               { "x.example", DNS_TYPE_A, 3, 50, NET_IPV4(6, 6, 6, 6), NULL },
               { "X.Example", DNS_TYPE_A, 0, 40, a1, NULL },
               { "x.example", DNS_TYPE_A, 0, 60, a2, NULL }), OK);
    CHECK(r.outcome == DNS_ADDRS && r.naddr == 2 && r.addr[0] == a1 && r.addr[1] == a2);
    CHECK_EQ(r.ttl, 40);
    CHECK_ST(P(REPLY, { "evil.example", DNS_TYPE_A, 0, 50, a1, NULL }), OK);
    CHECK_EQ(r.outcome, DNS_NO_DATA);
    /* at most DNS_MAX_ADDRS addresses */
    CHECK_ST(P(REPLY, { "x.example", DNS_TYPE_A, 0, 9, 1, NULL },
               { "x.example", DNS_TYPE_A, 0, 9, 2, NULL },
               { "x.example", DNS_TYPE_A, 0, 9, 3, NULL },
               { "x.example", DNS_TYPE_A, 0, 9, 4, NULL },
               { "x.example", DNS_TYPE_A, 0, 9, 5, NULL }), OK);
    CHECK(r.naddr == DNS_MAX_ADDRS && r.addr[3] == 4);
    /* a CNAME whose target isn't answered: follow it */
    CHECK_ST(P(REPLY, { "x.example", DNS_TYPE_CNAME, 0, 30, 0, "y.example" }), OK);
    CHECK(r.outcome == DNS_FOLLOW && r.cnames == 1 && !strcmp(r.next, "y.example") && r.ttl == 30);
    /* records in any order: the A record before the CNAME that leads to it */
    CHECK_ST(P(REPLY, { "y.example", DNS_TYPE_A, 0, 20, a2, NULL },
               { "x.example", DNS_TYPE_CNAME, 0, 30, 0, "y.example" }), OK);
    CHECK(r.outcome == DNS_ADDRS && r.addr[0] == a2 && r.cnames == 1 && r.ttl == 20);
    /* a CNAME loop in one reply: more than DNS_CNAME_MAX */
    CHECK_ST(P(REPLY, { "x.example", DNS_TYPE_CNAME, 0, 30, 0, "y.example" },
               { "y.example", DNS_TYPE_CNAME, 0, 30, 0, "x.example" }), OK);
    CHECK(r.outcome == DNS_FOLLOW && r.cnames > DNS_CNAME_MAX);
    /* rcodes, TC, TTLs */
    CHECK_ST(P(REPLY | DNS_RCODE_SERVFAIL, { "x.example", DNS_TYPE_A, 0, 9, a1, NULL }), OK);
    CHECK_EQ(r.outcome, DNS_SERVER_FAIL);
    CHECK_ST(P(REPLY | DNS_RCODE_REFUSED, { "x.example", DNS_TYPE_A, 0, 9, a1, NULL }), OK);
    CHECK_EQ(r.outcome, DNS_SERVER_FAIL);
    CHECK_ST(P(REPLY | DNS_TC, { "x.example", DNS_TYPE_A, 0, 9, a1, NULL }), OK);
    CHECK_EQ(r.outcome, DNS_TRUNCATED);
    CHECK_ST(P(REPLY, { "x.example", DNS_TYPE_A, 0, 0x80000001u, a1, NULL }), OK);
    CHECK(r.outcome == DNS_ADDRS && r.ttl == 0);
    CHECK_ST(P(REPLY, { "x.example", DNS_TYPE_A, 0, 0x7fffffffu, a1, NULL }), OK);
    CHECK_EQ(r.ttl, DNS_TTL_MAX);
#undef P
    /* TC with the records cut off mid-way: still TRUNCATED */
    size_t len = build(b, 7, "x.example", REPLY | DNS_TC,
                       (const struct rrspec[]){ { "x.example", DNS_TYPE_A, 0, 9, a1, NULL } }, 1);
    CHECK_ST(parse(&g, b, len - 3, 7, "x.example", &r), OK);
    CHECK_EQ(r.outcome, DNS_TRUNCATED);
    guard_close(&g);
    return true;
}

/* dns_read_name over hand-made bytes (at most 600), from pos. */
static status_t name_at(struct guard *g, const uint8_t *b, size_t len, size_t pos, char *out,
                        size_t *end)
{
    const uint8_t *m = guard_put(g, b, len);
    status_t st = dns_read_name(m, len, &pos, out);
    *end = pos;
    return st;
}

bool t_dns_names_hostile(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint8_t b[600];
    char out[DNS_NAME_MAX + 1];
    size_t end;
    /* "a", then a chain of pointers each to the one before it */
    b[0] = 1;
    b[1] = 'a';
    b[2] = 0;
    for (unsigned i = 0; i <= DNS_JUMPS_MAX; i++) {
        b[3 + 2 * i] = 0xc0;
        b[4 + 2 * i] = (uint8_t)(i ? 3 + 2 * (i - 1) : 0);
    }
    size_t at16 = 3 + 2 * (DNS_JUMPS_MAX - 1), at17 = at16 + 2;
    CHECK_ST(name_at(&g, b, at17 + 2, at16, out, &end), OK);   /* 16 jumps */
    CHECK(!strcmp(out, "a") && end == at16 + 2);
    CHECK_ST(name_at(&g, b, at17 + 2, at17, out, &end), ERR_INVALID_ARGS);   /* 17 */
    /* to itself; forward; into its own run of labels */
    static const uint8_t self[] = { 0xc0, 0x00 }, fwd[] = { 0xc0, 0x02, 0x01, 'a', 0x00 },
                         loop[] = { 0x01, 'a', 0xc0, 0x00 },
                         loop2[] = { 0x01, 'a', 0x00, 0x01, 'b', 0xc0, 0x03 };
    CHECK_ST(name_at(&g, self, 2, 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, fwd, sizeof(fwd), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, loop, sizeof(loop), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, loop2, sizeof(loop2), 3, out, &end), ERR_INVALID_ARGS);
    /* label types 01 and 10; a label or pointer past the end; no end at all */
    static const uint8_t t01[] = { 0x41, 'a', 0 }, t10[] = { 0x81, 'a', 0 },
                         past[] = { 0x05, 'a', 'b' }, half[] = { 0x01, 'a', 0xc0 },
                         open[] = { 0x01, 'a' };
    CHECK_ST(name_at(&g, t01, sizeof(t01), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, t10, sizeof(t10), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, past, sizeof(past), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, half, sizeof(half), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, open, sizeof(open), 0, out, &end), ERR_INVALID_ARGS);
    /* bytes a dotted name can't show */
    static const uint8_t dot[] = { 0x03, 'a', '.', 'b', 0 }, sp[] = { 0x03, 'a', ' ', 'b', 0 },
                         nul[] = { 0x03, 'a', 0, 'b', 0 }, del[] = { 0x01, 0x7f, 0 };
    CHECK_ST(name_at(&g, dot, sizeof(dot), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, sp, sizeof(sp), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, nul, sizeof(nul), 0, out, &end), ERR_INVALID_ARGS);
    CHECK_ST(name_at(&g, del, sizeof(del), 0, out, &end), ERR_INVALID_ARGS);
    /* the root; 253 characters; 255 */
    static const uint8_t root[] = { 0 };
    CHECK_ST(name_at(&g, root, 1, 0, out, &end), OK);
    CHECK(out[0] == 0 && end == 1);
    size_t at = 0;
    for (unsigned l = 0; l < 4; l++) {
        unsigned n = l == 3 ? 61 : 63;
        b[at++] = (uint8_t)n;
        memset(b + at, 'a' + l, n);
        at += n;
    }
    b[at++] = 0;
    CHECK_ST(name_at(&g, b, at, 0, out, &end), OK);
    CHECK(strlen(out) == 253 && end == at);
    b[3 * 64] = 63;   /* the last label 63 long: 255 characters */
    b[at - 1] = 'd';
    b[at++] = 'd';
    b[at++] = 0;
    CHECK_ST(name_at(&g, b, at, 0, out, &end), ERR_INVALID_ARGS);
    guard_close(&g);
    return true;
}

/* Every length each sample can be cut to; counts and lengths that lie. */
bool t_dns_parse_hostile(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    struct dns_result r;
    for (unsigned s = 0; s < NSAMPLES; s++)
        for (size_t len = 0; len < samples[s].len; len++)
            CHECK_ST(parse(&g, samples[s].b, len, samples[s].id, samples[s].name, &r),
                     ERR_INVALID_ARGS);
    uint8_t b[BUF];
    const char *name = "one.one.one.one";
    /* answer counts: more than are there; 65535; past DNS_RR_MAX in all; fewer (trailing bytes) */
    static const uint16_t an[] = { 3, 0xffff };
    for (unsigned i = 0; i < 2; i++) {
        memcpy(b, one_reply, sizeof(one_reply));
        net_put16(b + 6, an[i]);
        CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    }
    memcpy(b, one_reply, sizeof(one_reply));
    net_put16(b + 10, DNS_RR_MAX - 1);   /* 2 + 63 records claimed */
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    net_put16(b + 10, 1);                /* an authority record that isn't there */
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    net_put16(b + 10, 0);
    net_put16(b + 6, 1);                 /* the second answer is trailing bytes now */
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), OK);
    CHECK(r.outcome == DNS_ADDRS && r.naddr == 1);
    /* data lengths: an A record of 5 bytes; data past the end */
    memcpy(b, one_reply, sizeof(one_reply));
    b[44] = 5;
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    b[44] = 4;
    b[60] = 5;
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    /* a CNAME's data with a byte after its name; a CNAME name running past its data */
    memcpy(b, github_reply, sizeof(github_reply));
    b[43] = 10;
    CHECK_ST(parse(&g, b, sizeof(github_reply), 0x4c5d, "www.github.com", &r), ERR_INVALID_ARGS);
    b[43] = 8;
    CHECK_ST(parse(&g, b, sizeof(github_reply), 0x4c5d, "www.github.com", &r), ERR_INVALID_ARGS);
    /* the question's name pointing at itself; an answer's owner pointing forward */
    memcpy(b, one_reply, sizeof(one_reply));
    b[12] = 0xc0;
    b[13] = 0x0c;
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    memcpy(b, one_reply, sizeof(one_reply));
    b[34] = 0x31;   /* the first answer's owner -> offset 49, the second answer */
    CHECK_ST(parse(&g, b, sizeof(one_reply), 0x1a2b, name, &r), ERR_INVALID_ARGS);
    guard_close(&g);
    return true;
}

static bool result_sane(const struct dns_result *r)
{
    CHECK(r->outcome <= DNS_TRUNCATED && r->naddr <= DNS_MAX_ADDRS && r->ttl <= DNS_TTL_MAX);
    if (r->outcome == DNS_ADDRS) {
        CHECK(r->naddr >= 1);
        for (unsigned i = 0; i < r->naddr; i++)
            for (unsigned k = i + 1; k < r->naddr; k++)
                CHECK(r->addr[i] != r->addr[k]);
    }
    if (r->outcome == DNS_FOLLOW)
        CHECK(strnlen(r->next, DNS_NAME_MAX + 1) <= DNS_NAME_MAX);
    return true;
}

/* Mutated replies, and names read at random places in random bytes:
 * never a fault, a status the contract names, a sane result. */
bool t_dns_fuzz(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint64_t seed = 0xd15ea5e5u;
    uint8_t b[BUF];
    unsigned ok = 0;
    for (unsigned i = 0; i < NSAMPLES * FUZZ_ROUNDS; i++) {
        const struct sample *s = &samples[i % NSAMPLES];
        memcpy(b, s->b, s->len);
        size_t len = fuzz_mutate(&seed, b, s->len, sizeof(b));
        struct dns_result r;
        status_t st = parse(&g, b, len, s->id, s->name, &r);
        CHECK(st == OK || st == ERR_NOT_FOUND || st == ERR_INVALID_ARGS);
        if (st == OK && !result_sane(&r))
            return false;
        ok += st == OK;
    }
    CHECK(ok > 0);
    for (unsigned i = 0; i < FUZZ_ROUNDS; i++) {
        size_t len = 1 + fuzz_rand(&seed) % 300;
        for (size_t k = 0; k < len; k++)
            b[k] = (uint8_t)fuzz_rand(&seed);
        char out[DNS_NAME_MAX + 1];
        size_t end;
        status_t st = name_at(&g, b, len, fuzz_rand(&seed) % len, out, &end);
        CHECK(st == OK || st == ERR_INVALID_ARGS);
        CHECK(st != OK || (end <= len && strlen(out) <= DNS_NAME_MAX));
    }
    guard_close(&g);
    return true;
}
