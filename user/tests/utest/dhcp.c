/* utest: the DHCP client's messages (user/services/dhcp/msg.c, linked in
 * by the Makefile). What the client builds, byte for byte; a server's
 * OFFER as a VLAN 21 router sends it, parsed; and hostile replies: every
 * truncation of it, options running past their field, wrong lengths,
 * repeated and split options, option overload, addresses that can't be
 * ours, and a fuzz loop of mutated replies. Every hostile datagram is
 * placed to end at an unreadable page (netfuzz.c), so a read past its
 * end faults instead of passing. Also the servers' replies the state
 * machine's tests (dhcpc.c) use. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netbytes.h>
#include <os.h>
#include "dhcp.h"
#include "nettest.h"
#include "utest.h"

#define BUF 1500

const uint8_t test_mac[6] = { 0x02, 0x00, 0x00, 0x4a, 0x4d, 0x01 };

/* The sample OFFER: its fixed part up to the hardware address (the rest
 * of the 236 bytes are zero), and its options after the magic cookie:
 * 53 (OFFER), 54, 51 (24 h), 58, 59, 1 (/24), 28 (broadcast address, not
 * read), 3, 6, 15 (domain "localdomain", not read), END. */
static const uint8_t offer_head[34] = {
    0x02, 0x01, 0x06, 0x00, 0x39, 0x03, 0xf3, 0x26, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0a, 0x02, 0x15, 0x43, 0x0a, 0x02, 0x15, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x4a, 0x4d, 0x01,
};
static const uint8_t offer_opts[65] = {
    0x35, 0x01, 0x02, 0x36, 0x04, 0x0a, 0x02, 0x15, 0x01, 0x33, 0x04, 0x00,
    0x01, 0x51, 0x80, 0x3a, 0x04, 0x00, 0x00, 0xa8, 0xc0, 0x3b, 0x04, 0x00,
    0x01, 0x27, 0x50, 0x01, 0x04, 0xff, 0xff, 0xff, 0x00, 0x1c, 0x04, 0x0a,
    0x02, 0x15, 0xff, 0x03, 0x04, 0x0a, 0x02, 0x15, 0x01, 0x06, 0x04, 0x0a,
    0x02, 0x15, 0x01, 0x0f, 0x0b, 0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x64, 0x6f,
    0x6d, 0x61, 0x69, 0x6e, 0xff,
};
/* Where each whole option of the sample ends: a cut there is a field
 * that ends without END. */
static const size_t offer_ends[] = { 243, 249, 255, 261, 267, 273, 279, 285, 291, 304, 305 };

size_t sample_offer(uint8_t *buf, uint8_t type)
{
    memset(buf, 0, DHCP_OFF_OPTIONS + sizeof(offer_opts));
    memcpy(buf, offer_head, sizeof(offer_head));
    net_put32(buf + DHCP_OFF_COOKIE, DHCP_COOKIE);
    memcpy(buf + DHCP_OFF_OPTIONS, offer_opts, sizeof(offer_opts));
    buf[DHCP_OFF_OPTIONS + 2] = type;
    return DHCP_OFF_OPTIONS + sizeof(offer_opts);
}

static size_t put_opt(uint8_t *b, size_t at, uint8_t code, uint32_t v)
{
    b[at] = code;
    b[at + 1] = 4;
    net_put32(b + at + 2, v);
    return at + 6;
}

size_t srv_build(uint8_t *buf, size_t cap, const struct srv *s)
{
    if (cap < DHCP_MSG_BUILT)
        return 0;
    memset(buf, 0, DHCP_MSG_BUILT);
    buf[DHCP_OFF_OP] = BOOTREPLY;
    buf[DHCP_OFF_HTYPE] = HTYPE_ETHER;
    buf[DHCP_OFF_HLEN] = 6;
    net_put32(buf + DHCP_OFF_XID, s->xid);
    net_put32(buf + DHCP_OFF_YIADDR, s->yiaddr);
    memcpy(buf + DHCP_OFF_CHADDR, test_mac, 6);
    net_put32(buf + DHCP_OFF_COOKIE, DHCP_COOKIE);
    size_t at = DHCP_OFF_OPTIONS;
    buf[at++] = 53;
    buf[at++] = 1;
    buf[at++] = s->type;
    const uint8_t codes[] = { 54, 51, 58, 59, 1, 3, 6 };
    const uint32_t vals[] = { s->server, s->lease, s->t1, s->t2, s->mask, s->router, s->dns };
    for (unsigned i = 0; i < sizeof(codes); i++)
        if (vals[i])
            at = put_opt(buf, at, codes[i], vals[i]);
    buf[at] = 255;   /* 243 + 7 * 6 + 1 = 286 <= 300 */
    return DHCP_MSG_BUILT;
}

/* The sample's fixed part and cookie, then n option bytes: the length. */
static size_t with_opts(uint8_t *b, const uint8_t *opts, size_t n)
{
    memset(b, 0, BUF);
    sample_offer(b, DHCP_OFFER);
    memset(b + DHCP_OFF_OPTIONS, 0, BUF - DHCP_OFF_OPTIONS);
    memcpy(b + DHCP_OFF_OPTIONS, opts, n);
    return DHCP_OFF_OPTIONS + n;
}

/* Parse len bytes of b placed against the guard page. */
static status_t parse(struct guard *g, const uint8_t *b, size_t len, struct dhcp_reply *r)
{
    return dhcp_parse(guard_put(g, b, len), len, test_mac, SAMPLE_XID, r);
}

const uint8_t *test_dhcp_opt(const uint8_t *b, size_t len, uint8_t code)
{
    for (size_t i = DHCP_OFF_OPTIONS; i + 1 < len && b[i] != 255; i += b[i] ? 2u + b[i + 1] : 1u)
        if (b[i] == code)
            return b + i;
    return NULL;
}

bool t_dhcp_build(void)
{
    static const uint8_t want[] = {
        53, 1, 1,  61, 7, 1, 0x02, 0x00, 0x00, 0x4a, 0x4d, 0x01,  57, 2, 0x05, 0xdc,
        12, 5, 'j', 'a', 'm', 'o', 's',  55, 7, 1, 3, 6, 51, 54, 58, 59,  255,
    };
    uint8_t b[400];
    struct dhcp_out m = { .type = DHCP_DISCOVER, .xid = 0x01020304, .secs = 3, .broadcast = true };
    CHECK_EQ(dhcp_build(b, sizeof(b), test_mac, &m), DHCP_MSG_BUILT);
    CHECK(b[0] == BOOTREQUEST && b[1] == 1 && b[2] == 6 && b[3] == 0);
    CHECK_EQ(net_get32(b + 4), 0x01020304);
    CHECK_EQ(net_get16(b + 8), 3);
    CHECK_EQ(net_get16(b + 10), DHCP_FLAG_BROADCAST);
    for (unsigned i = 12; i < 28; i++)
        CHECK_EQ(b[i], 0);   /* ciaddr, yiaddr, siaddr, giaddr */
    CHECK(!memcmp(b + 28, test_mac, 6));
    for (unsigned i = 34; i < DHCP_OFF_COOKIE; i++)
        CHECK_EQ(b[i], 0);   /* the rest of chaddr, sname, file */
    CHECK_EQ(net_get32(b + DHCP_OFF_COOKIE), DHCP_COOKIE);
    CHECK(!memcmp(b + DHCP_OFF_OPTIONS, want, sizeof(want)));
    for (size_t i = DHCP_OFF_OPTIONS + sizeof(want); i < DHCP_MSG_BUILT; i++)
        CHECK_EQ(b[i], 0);
    /* the REQUEST for an offer: options 50 and 54, broadcast */
    m = (struct dhcp_out){ .type = DHCP_REQUEST, .xid = 9, .broadcast = true,
                           .requested = TEST_ADDR, .server = TEST_SERVER };
    CHECK_EQ(dhcp_build(b, sizeof(b), test_mac, &m), DHCP_MSG_BUILT);
    const uint8_t *o = test_dhcp_opt(b, DHCP_MSG_BUILT, 50);
    CHECK(o && o[1] == 4 && net_get32(o + 2) == TEST_ADDR);
    o = test_dhcp_opt(b, DHCP_MSG_BUILT, 54);
    CHECK(o && o[1] == 4 && net_get32(o + 2) == TEST_SERVER);
    CHECK((o = test_dhcp_opt(b, DHCP_MSG_BUILT, 53)) && o[2] == DHCP_REQUEST);
    /* renewing: our address in ciaddr, no 50 or 54, no broadcast flag */
    m = (struct dhcp_out){ .type = DHCP_REQUEST, .xid = 9, .ciaddr = TEST_ADDR };
    CHECK_EQ(dhcp_build(b, sizeof(b), test_mac, &m), DHCP_MSG_BUILT);
    CHECK_EQ(net_get32(b + DHCP_OFF_CIADDR), TEST_ADDR);
    CHECK_EQ(net_get16(b + DHCP_OFF_FLAGS), 0);
    CHECK(!test_dhcp_opt(b, DHCP_MSG_BUILT, 50) && !test_dhcp_opt(b, DHCP_MSG_BUILT, 54));
    /* RELEASE: 54, no parameter list or host name */
    m = (struct dhcp_out){ .type = DHCP_RELEASE, .ciaddr = TEST_ADDR, .server = TEST_SERVER };
    CHECK_EQ(dhcp_build(b, sizeof(b), test_mac, &m), DHCP_MSG_BUILT);
    CHECK((o = test_dhcp_opt(b, DHCP_MSG_BUILT, 53)) && o[2] == DHCP_RELEASE);
    CHECK(test_dhcp_opt(b, DHCP_MSG_BUILT, 54) && !test_dhcp_opt(b, DHCP_MSG_BUILT, 55) &&
          !test_dhcp_opt(b, DHCP_MSG_BUILT, 12));
    /* not a client's message; too small a buffer */
    m.type = DHCP_OFFER;
    CHECK_EQ(dhcp_build(b, sizeof(b), test_mac, &m), 0);
    m.type = DHCP_DECLINE;
    CHECK_EQ(dhcp_build(b, DHCP_MSG_BUILT - 1, test_mac, &m), 0);
    /* the client's own message is not a reply */
    struct dhcp_reply r;
    CHECK_EQ(dhcp_build(b, sizeof(b), test_mac, &m), DHCP_MSG_BUILT);
    CHECK_ST(dhcp_parse(b, DHCP_MSG_BUILT, test_mac, 0, &r), ERR_NOT_FOUND);
    return true;
}

bool t_dhcp_parse_sample(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint8_t b[BUF];
    struct dhcp_reply r;
    size_t len = sample_offer(b, DHCP_OFFER);
    CHECK_ST(parse(&g, b, len, &r), OK);
    CHECK(r.type == DHCP_OFFER && r.yiaddr == TEST_ADDR && r.server == TEST_SERVER);
    CHECK(r.mask == NET_IPV4(255, 255, 255, 0) && r.router == TEST_SERVER);
    CHECK(r.ndns == 1 && r.dns[0] == TEST_SERVER);
    CHECK(r.lease_s == 86400 && r.t1_s == 43200 && r.t2_s == 75600);
    len = sample_offer(b, DHCP_ACK);
    CHECK_ST(parse(&g, b, len, &r), OK);
    CHECK(r.type == DHCP_ACK && r.yiaddr == TEST_ADDR);
    /* not ours: another xid, hardware address, a request, another hardware type */
    CHECK_ST(dhcp_parse(b, len, test_mac, SAMPLE_XID + 1, &r), ERR_NOT_FOUND);
    uint8_t other[6] = { 0x02, 0, 0, 0x4a, 0x4d, 0x02 };
    CHECK_ST(dhcp_parse(b, len, other, SAMPLE_XID, &r), ERR_NOT_FOUND);
    const unsigned at[] = { DHCP_OFF_OP, DHCP_OFF_HTYPE, DHCP_OFF_HLEN };
    const uint8_t bad[] = { BOOTREQUEST, 6, 16 };
    for (unsigned i = 0; i < 3; i++) {
        sample_offer(b, DHCP_ACK);
        b[at[i]] = bad[i];
        CHECK_ST(parse(&g, b, len, &r), ERR_NOT_FOUND);
    }
    /* another message type; none (BOOTP); a broken magic cookie */
    CHECK_ST(parse(&g, b, sample_offer(b, DHCP_REQUEST), &r), ERR_NOT_SUPPORTED);
    CHECK_ST(parse(&g, b, sample_offer(b, 200), &r), ERR_NOT_SUPPORTED);
    sample_offer(b, DHCP_ACK);
    memset(b + DHCP_OFF_OPTIONS, 0, 3);   /* option 53 turned into pads */
    CHECK_ST(parse(&g, b, len, &r), ERR_NOT_SUPPORTED);
    sample_offer(b, DHCP_ACK);
    b[DHCP_OFF_COOKIE + 3] ^= 1;
    CHECK_ST(parse(&g, b, len, &r), ERR_INVALID_ARGS);
    guard_close(&g);
    return true;
}

static bool is_end(size_t len)
{
    for (unsigned i = 0; i < sizeof(offer_ends) / sizeof(offer_ends[0]); i++)
        if (offer_ends[i] == len)
            return true;
    return false;
}

/* Every length the sample can be cut to, each against the guard page. */
bool t_dhcp_parse_truncated(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint8_t b[BUF];
    struct dhcp_reply r;
    size_t full = sample_offer(b, DHCP_OFFER);
    for (size_t len = 0; len <= full; len++) {
        status_t st = parse(&g, b, len, &r);
        if (len < DHCP_OFF_OPTIONS)
            CHECK_ST(st, ERR_INVALID_ARGS);
        else if (len == DHCP_OFF_OPTIONS)
            CHECK_ST(st, ERR_NOT_SUPPORTED);   /* no options at all: no type */
        else if (len == 243)
            CHECK_ST(st, ERR_INVALID_ARGS);    /* a type, no server id */
        else if (is_end(len))
            CHECK(st == OK && r.yiaddr == TEST_ADDR && r.server == TEST_SERVER);
        else
            CHECK_ST(st, ERR_INVALID_ARGS);    /* an option cut short */
    }
    /* cut before option 1: the class's mask (10/8) */
    CHECK_ST(parse(&g, b, 255, &r), OK);
    CHECK(r.mask == 0xff000000u && r.lease_s == 86400 && !r.t1_s && !r.router);
    guard_close(&g);
    return true;
}

/* Option bytes after the sample's fixed part: the parse's status. */
static status_t opts(struct guard *g, const uint8_t *o, size_t n, struct dhcp_reply *r)
{
    uint8_t b[BUF];
    return parse(g, b, with_opts(b, o, n), r);
}
#define OPTS(...) \
    opts(&g, (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }), &r)
#define TYPE(t)  53, 1, t
#define SERVER   54, 4, 10, 2, 21, 1
#define LEASE    51, 4, 0, 1, 0x51, 0x80
#define MASK24   1, 4, 255, 255, 255, 0

bool t_dhcp_parse_options(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    struct dhcp_reply r;
    /* lengths: past the end, no length byte, a wrong fixed length */
    CHECK_ST(OPTS(TYPE(2), SERVER, 3, 10, 10, 2, 21, 1), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(2), SERVER, 3), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(53, 2, 2, 2, SERVER, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(5), SERVER, 51, 2, 1, 0, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(2), SERVER, 3, 6, 10, 2, 21, 1, 10, 2, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(2), SERVER, 6, 0, 255), ERR_INVALID_ARGS);
    /* pads and END only; what follows END is not read */
    CHECK_ST(OPTS(0, 0, 0, 255), ERR_NOT_SUPPORTED);
    CHECK_ST(OPTS(TYPE(2), SERVER, 255, 51, 200, 7), OK);
    /* a repeated fixed option is one long one (RFC 3396): 8 bytes, refused */
    CHECK_ST(OPTS(TYPE(5), SERVER, LEASE, LEASE, 255), ERR_INVALID_ARGS);
    /* a DNS list split in the middle of an address, joined */
    CHECK_ST(OPTS(TYPE(2), SERVER, 6, 6, 10, 2, 21, 1, 1, 1, 6, 2, 1, 1, 255), OK);
    CHECK(r.ndns == 2 && r.dns[0] == TEST_SERVER && r.dns[1] == NET_IPV4(1, 1, 1, 1));
    /* the first router on the subnet; DNS servers that aren't unicast dropped; at most 3 */
    CHECK_ST(OPTS(TYPE(2), SERVER, MASK24, 3, 12, 10, 2, 22, 1, 10, 2, 21, 67, 10, 2, 21, 254,
                  6, 16, 0, 0, 0, 0, 255, 255, 255, 255, 224, 0, 0, 1, 9, 9, 9, 9, 255), OK);
    CHECK(r.router == NET_IPV4(10, 2, 21, 254) && r.ndns == 1 && r.dns[0] == NET_IPV4(9, 9, 9, 9));
    CHECK_ST(OPTS(TYPE(2), SERVER, 6, 16, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 255), OK);
    CHECK(r.ndns == 3 && r.dns[2] == NET_IPV4(3, 3, 3, 3));
    CHECK_ST(OPTS(TYPE(2), SERVER, MASK24, 3, 4, 10, 2, 21, 255, 255), OK);
    CHECK_EQ(r.router, 0);   /* the subnet's broadcast address */
    /* masks: not contiguous, /31, /32; /16 is fine */
    CHECK_ST(OPTS(TYPE(2), SERVER, 1, 4, 255, 0, 255, 0, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(2), SERVER, 1, 4, 255, 255, 255, 254, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(2), SERVER, 1, 4, 255, 255, 255, 255, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(2), SERVER, 1, 4, 255, 255, 0, 0, 255), OK);
    CHECK_EQ(r.mask, 0xffff0000u);
    /* required: a server id in every reply, a lease time in an ACK */
    CHECK_ST(OPTS(TYPE(2), LEASE, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(6), 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(5), SERVER, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(5), SERVER, 51, 4, 0, 0, 0, 0, 255), ERR_INVALID_ARGS);
    CHECK_ST(OPTS(TYPE(6), SERVER, 255), OK);
    CHECK(r.type == DHCP_NAK && r.server == TEST_SERVER && !r.yiaddr);
    guard_close(&g);
    return true;
}

/* The address offered, against a /24: never one that can't be a host's. */
bool t_dhcp_parse_addresses(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    static const uint8_t o[] = { TYPE(2), SERVER, MASK24, 255 };
    const uint32_t bad[] = { 0, NET_IPV4(10, 2, 21, 0), NET_IPV4(10, 2, 21, 255),
                             NET_IPV4(127, 0, 0, 1), NET_IPV4(224, 0, 0, 5),
                             NET_IPV4(240, 1, 2, 3), DHCP_BROADCAST, NET_IPV4(0, 1, 2, 3) };
    uint8_t b[BUF];
    struct dhcp_reply r;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t len = with_opts(b, o, sizeof(o));
        net_put32(b + DHCP_OFF_YIADDR, bad[i]);
        CHECK_ST(parse(&g, b, len, &r), ERR_INVALID_ARGS);
    }
    /* a long run of pads before the options is fine */
    memset(b, 0, sizeof(b));
    size_t len = with_opts(b, o, 0) + 1000;
    memcpy(b + len, o, sizeof(o));
    CHECK_ST(parse(&g, b, len + sizeof(o), &r), OK);
    CHECK_EQ(r.yiaddr, TEST_ADDR);
    guard_close(&g);
    return true;
}

/* Option 52: options in the file and sname fields too. */
bool t_dhcp_overload(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint8_t b[BUF];
    struct dhcp_reply r;
    static const uint8_t main1[] = { TYPE(5), 52, 1, 1, 255 }, main2[] = { TYPE(5), 52, 1, 2, 255 },
                         main3[] = { TYPE(5), 52, 1, 3, 255 };
    static const uint8_t file[] = { SERVER, LEASE, MASK24, 255 }, srv[] = { SERVER, 255 },
                         lease[] = { LEASE, 255 };
    size_t len = with_opts(b, main1, sizeof(main1));
    memcpy(b + DHCP_OFF_FILE, file, sizeof(file));
    CHECK_ST(parse(&g, b, len, &r), OK);
    CHECK(r.type == DHCP_ACK && r.server == TEST_SERVER && r.lease_s == 86400 &&
          r.mask == 0xffffff00u);
    len = with_opts(b, main2, sizeof(main2));
    memcpy(b + DHCP_OFF_SNAME, file, sizeof(file));
    CHECK_ST(parse(&g, b, len, &r), OK);
    /* both: the server id from file, the lease from sname */
    len = with_opts(b, main3, sizeof(main3));
    memcpy(b + DHCP_OFF_FILE, srv, sizeof(srv));
    memcpy(b + DHCP_OFF_SNAME, lease, sizeof(lease));
    CHECK_ST(parse(&g, b, len, &r), OK);
    CHECK(r.server == TEST_SERVER && r.lease_s == 86400);
    /* the fields not named are not read: garbage in sname with 52 = 1 */
    len = with_opts(b, main1, sizeof(main1));
    memcpy(b + DHCP_OFF_FILE, file, sizeof(file));
    memset(b + DHCP_OFF_SNAME, 3, DHCP_SNAME_LEN);
    CHECK_ST(parse(&g, b, len, &r), OK);
    /* 52 inside the file field doesn't count (no reading sname by it) */
    static const uint8_t file52[] = { 52, 1, 2, SERVER, LEASE, 255 };
    memcpy(b + DHCP_OFF_FILE, file52, sizeof(file52));
    CHECK_ST(parse(&g, b, len, &r), OK);
    /* an option running past the file field, though bytes follow it */
    memset(b + DHCP_OFF_FILE, 0, DHCP_FILE_LEN);
    memcpy(b + DHCP_OFF_FILE, srv, sizeof(srv) - 1);
    b[DHCP_OFF_FILE + DHCP_FILE_LEN - 2] = 51;
    b[DHCP_OFF_FILE + DHCP_FILE_LEN - 1] = 4;
    CHECK_ST(parse(&g, b, len, &r), ERR_INVALID_ARGS);
    b[DHCP_OFF_FILE + DHCP_FILE_LEN - 2] = 0;
    b[DHCP_OFF_FILE + DHCP_FILE_LEN - 1] = 51;   /* a code with no length byte */
    CHECK_ST(parse(&g, b, len, &r), ERR_INVALID_ARGS);
    /* bad overload values */
    static const uint8_t v0[] = { TYPE(5), SERVER, LEASE, 52, 1, 0, 255 },
                         v4[] = { TYPE(5), SERVER, LEASE, 52, 1, 4, 255 },
                         l2[] = { TYPE(5), SERVER, LEASE, 52, 2, 1, 1, 255 };
    CHECK_ST(parse(&g, b, with_opts(b, v0, sizeof(v0)), &r), ERR_INVALID_ARGS);
    CHECK_ST(parse(&g, b, with_opts(b, v4, sizeof(v4)), &r), ERR_INVALID_ARGS);
    CHECK_ST(parse(&g, b, with_opts(b, l2, sizeof(l2)), &r), ERR_INVALID_ARGS);
    guard_close(&g);
    return true;
}

/* A parse that succeeded gave values a host can use. */
static bool sane(const struct dhcp_reply *r)
{
    CHECK(r->server && r->ndns <= DHCP_MAX_DNS);
    if (r->type == DHCP_NAK)
        return true;
    CHECK(dhcp_unicast(r->yiaddr) && (~r->mask & (~r->mask + 1)) == 0);
    CHECK(!r->router || (r->router & r->mask) == (r->yiaddr & r->mask));
    return true;
}

/* Mutated replies (the sample OFFER and ACK, and an overloaded ACK), and
 * random datagrams that pass the "is it ours" check: never a fault, a
 * status the contract names, and sane values when one is accepted. */
bool t_dhcp_fuzz(void)
{
    struct guard g;
    if (!guard_open(&g))
        return false;
    uint64_t seed = 0x5eed0dc9u;
    uint8_t b[BUF], over[BUF];
    static const uint8_t main3[] = { TYPE(5), 52, 1, 3, 255 }, file[] = { SERVER, LEASE, 255 },
                         sname[] = { MASK24, 3, 4, 10, 2, 21, 1, 255 };
    size_t over_len = with_opts(over, main3, sizeof(main3));
    memcpy(over + DHCP_OFF_FILE, file, sizeof(file));
    memcpy(over + DHCP_OFF_SNAME, sname, sizeof(sname));
    unsigned ok = 0;
    for (unsigned i = 0; i < 3 * FUZZ_ROUNDS; i++) {
        size_t len;
        if (i % 3 == 2) {
            memcpy(b, over, over_len);
            len = over_len;
        } else {
            len = sample_offer(b, i % 3 ? DHCP_ACK : DHCP_OFFER);
        }
        len = fuzz_mutate(&seed, b, len, sizeof(b));
        struct dhcp_reply r;
        status_t st = parse(&g, b, len, &r);
        CHECK(st == OK || st == ERR_NOT_FOUND || st == ERR_NOT_SUPPORTED ||
              st == ERR_INVALID_ARGS);
        if (st == OK && !sane(&r))
            return false;
        ok += st == OK;
    }
    for (unsigned i = 0; i < FUZZ_ROUNDS; i++) {
        size_t len = DHCP_OFF_OPTIONS + fuzz_rand(&seed) % 400;
        for (size_t k = 0; k < len; k++)
            b[k] = (uint8_t)fuzz_rand(&seed);
        memcpy(b, offer_head, sizeof(offer_head));
        net_put32(b + DHCP_OFF_COOKIE, DHCP_COOKIE);
        struct dhcp_reply r;
        status_t st = parse(&g, b, len, &r);
        CHECK(st == OK || st == ERR_NOT_SUPPORTED || st == ERR_INVALID_ARGS);
        if (st == OK && !sane(&r))
            return false;
    }
    CHECK(ok > 0);   /* some mutations leave a valid reply: the checks above ran */
    guard_close(&g);
    return true;
}
