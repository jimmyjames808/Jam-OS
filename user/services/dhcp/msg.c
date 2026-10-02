/* dhcp: the client's messages, built and parsed (RFC 2131 section 2 and
 * 4.3.1's tables, options RFC 2132, long options RFC 3396).
 *
 * Building: every message is DHCP_MSG_BUILT (300) bytes, the BOOTP
 * minimum some relays insist on, zero-padded after the END option. It
 * carries the client id (option 61: hardware type 1 and the MAC), the
 * host name, the parameter list (1, 3, 6, 51, 54, 58, 59) and the
 * largest message we take (option 57).
 *
 * Parsing treats the datagram as hostile. Nothing is read before its
 * length is checked against what arrived: the fixed part and the magic
 * cookie first, then each option's code, its length byte and its value.
 * An option that runs past the end of its field makes the message
 * malformed, as does a length byte with nothing after it. Options are
 * read from the options field, then, if option 52 (overload) says so,
 * from the file field and then the sname field (RFC 2131 4.1), each
 * walk bounded by its own field. Pad (0) is skipped, END (255) ends a
 * field; a field may also end exactly at its last byte with no END.
 *
 * An option that appears more than once is one long option (RFC 3396):
 * the pieces are joined, so a repeated fixed-length option (two
 * option 51s) has the wrong length and makes the message malformed, and
 * a router or DNS list may be split anywhere. Only the options read
 * here are kept; others are skipped by their length. The values are
 * then checked: the address offered must be a unicast address that is
 * neither its subnet's network nor broadcast address; the mask must be
 * contiguous, /1 to /30; a router outside the subnet (or the offered
 * address itself, or the subnet's network or broadcast address) is
 * dropped, as are DNS servers that aren't unicast;
 * an ACK must carry a lease time and every reply a server id. */
#include <netbytes.h>
#include <os.h>
#include "dhcp.h"

/* Option codes (RFC 2132). */
#define OPT_PAD       0
#define OPT_MASK      1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_HOSTNAME  12
#define OPT_REQUESTED 50
#define OPT_LEASE     51
#define OPT_OVERLOAD  52
#define OPT_TYPE      53
#define OPT_SERVER    54
#define OPT_PARAMS    55
#define OPT_MAX_SIZE  57
#define OPT_T1        58
#define OPT_T2        59
#define OPT_CLIENT_ID 61
#define OPT_END       255

#define OVERLOAD_FILE  1u   /* option 52's value: the file field holds options */
#define OVERLOAD_SNAME 2u   /* ... the sname field does */
#define LIST_KEEP      16u  /* bytes kept of a list option: 4 addresses */

/* ---- building ------------------------------------------------------------ */

struct builder {
    uint8_t *p;
    size_t   at, cap;
};

static void opt_bytes(struct builder *b, uint8_t code, const void *v, uint8_t len)
{
    /* dhcp_build checked cap against the largest message there is */
    b->p[b->at++] = code;
    b->p[b->at++] = len;
    memcpy(b->p + b->at, v, len);
    b->at += len;
}

static void opt_addr(struct builder *b, uint8_t code, uint32_t addr)
{
    uint8_t v[4];
    net_put32(v, addr);
    opt_bytes(b, code, v, 4);
}

static void build_fixed(uint8_t *p, const uint8_t mac[6], const struct dhcp_out *m)
{
    p[DHCP_OFF_OP] = BOOTREQUEST;
    p[DHCP_OFF_HTYPE] = HTYPE_ETHER;
    p[DHCP_OFF_HLEN] = 6;
    net_put32(p + DHCP_OFF_XID, m->xid);
    net_put16(p + DHCP_OFF_SECS, m->secs);
    net_put16(p + DHCP_OFF_FLAGS, m->broadcast ? DHCP_FLAG_BROADCAST : 0);
    net_put32(p + DHCP_OFF_CIADDR, m->ciaddr);
    memcpy(p + DHCP_OFF_CHADDR, mac, 6);
    net_put32(p + DHCP_OFF_COOKIE, DHCP_COOKIE);
}

size_t dhcp_build(uint8_t *buf, size_t cap, const uint8_t mac[6], const struct dhcp_out *m)
{
    static const uint8_t params[] = { OPT_MASK,  OPT_ROUTER, OPT_DNS, OPT_LEASE,
                                      OPT_SERVER, OPT_T1,    OPT_T2 };
    if (cap < DHCP_MSG_BUILT)
        return 0;
    if (m->type != DHCP_DISCOVER && m->type != DHCP_REQUEST && m->type != DHCP_DECLINE &&
        m->type != DHCP_RELEASE)
        return 0;
    memset(buf, 0, DHCP_MSG_BUILT);
    build_fixed(buf, mac, m);
    struct builder b = { buf, DHCP_OFF_OPTIONS, DHCP_MSG_BUILT };
    opt_bytes(&b, OPT_TYPE, &m->type, 1);
    uint8_t id[7] = { HTYPE_ETHER };
    memcpy(id + 1, mac, 6);
    opt_bytes(&b, OPT_CLIENT_ID, id, sizeof(id));
    if (m->requested)
        opt_addr(&b, OPT_REQUESTED, m->requested);
    if (m->server)
        opt_addr(&b, OPT_SERVER, m->server);
    if (m->type == DHCP_DISCOVER || m->type == DHCP_REQUEST) {
        uint8_t size[2];
        net_put16(size, DHCP_MAX_MSG);
        opt_bytes(&b, OPT_MAX_SIZE, size, 2);
        opt_bytes(&b, OPT_HOSTNAME, DHCP_HOSTNAME, sizeof(DHCP_HOSTNAME) - 1);
        opt_bytes(&b, OPT_PARAMS, params, sizeof(params));
    }
    /* 240 + 3 + 9 + 6 + 6 + 4 + 7 + 9 = 284 at most, END included: room
     * left in the 300 */
    b.p[b.at++] = OPT_END;
    return DHCP_MSG_BUILT;
}

/* ---- parsing ------------------------------------------------------------- */

/* One option's value, its pieces joined (RFC 3396): the first `keep`
 * bytes kept, the whole length counted. */
struct optval {
    uint8_t  v[LIST_KEEP];
    uint32_t len;    /* total length of every piece */
    bool     seen;
};

/* The options this parser reads. */
struct opts {
    struct optval mask, router, dns, lease, overload, type, server, t1, t2;
};

static struct optval *slot(struct opts *o, uint8_t code)
{
    switch (code) {
    case OPT_MASK:     return &o->mask;
    case OPT_ROUTER:   return &o->router;
    case OPT_DNS:      return &o->dns;
    case OPT_LEASE:    return &o->lease;
    case OPT_OVERLOAD: return &o->overload;
    case OPT_TYPE:     return &o->type;
    case OPT_SERVER:   return &o->server;
    case OPT_T1:       return &o->t1;
    case OPT_T2:       return &o->t2;
    default:           return NULL;
    }
}

static void take(struct opts *o, uint8_t code, const uint8_t *v, uint8_t len)
{
    struct optval *s = slot(o, code);
    if (!s)
        return;
    if (s->len < LIST_KEEP) {
        uint32_t room = LIST_KEEP - s->len, n = len < room ? len : room;
        memcpy(s->v + s->len, v, n);
    }
    s->len += len;   /* at most 3 fields x 255 options x 255 bytes: no wrap */
    s->seen = true;
}

/* Walk one field's options (n bytes at p). main: the options field,
 * where option 52 counts; elsewhere it is skipped. */
static status_t walk(const uint8_t *p, size_t n, bool main, struct opts *o)
{
    size_t i = 0;
    while (i < n) {
        uint8_t code = p[i];
        if (code == OPT_PAD) {
            i++;
            continue;
        }
        if (code == OPT_END)
            return OK;
        if (n - i < 2)
            return ERR_INVALID_ARGS;   /* a code with no length byte */
        uint8_t len = p[i + 1];
        if (len > n - i - 2)
            return ERR_INVALID_ARGS;   /* the value runs past the field */
        if (main || code != OPT_OVERLOAD)
            take(o, code, p + i + 2, len);
        i += 2 + (size_t)len;
    }
    return OK;   /* the field ended exactly at its last byte */
}

static status_t read_options(const uint8_t *m, size_t len, struct opts *o)
{
    *o = (struct opts){ 0 };
    status_t st = walk(m + DHCP_OFF_OPTIONS, len - DHCP_OFF_OPTIONS, true, o);
    if (st != OK || !o->overload.seen)
        return st;
    if (o->overload.len != 1 || o->overload.v[0] < 1 || o->overload.v[0] > 3)
        return ERR_INVALID_ARGS;
    uint8_t which = o->overload.v[0];
    if (which & OVERLOAD_FILE)
        st = walk(m + DHCP_OFF_FILE, DHCP_FILE_LEN, false, o);
    if (st == OK && (which & OVERLOAD_SNAME))
        st = walk(m + DHCP_OFF_SNAME, DHCP_SNAME_LEN, false, o);
    return st;
}

/* A fixed 4-byte option: absent (0), or exactly 4 bytes. */
static status_t addr_opt(const struct optval *v, uint32_t *out)
{
    *out = 0;
    if (!v->seen)
        return OK;
    if (v->len != 4)
        return ERR_INVALID_ARGS;
    *out = net_get32(v->v);
    return OK;
}

/* A list of addresses: absent, or a non-zero multiple of 4 bytes. */
static status_t list_ok(const struct optval *v)
{
    if (v->seen && (v->len == 0 || v->len % 4))
        return ERR_INVALID_ARGS;
    return OK;
}

uint32_t dhcp_class_mask(uint32_t addr)
{
    if (!(addr & 0x80000000u))
        return 0xff000000u;   /* class A */
    if (!(addr & 0x40000000u))
        return 0xffff0000u;   /* class B */
    return 0xffffff00u;       /* class C */
}

bool dhcp_unicast(uint32_t addr)
{
    uint8_t top = (uint8_t)(addr >> 24);
    return top != 0 && top != 127 && top < 224;   /* not 0/8, loopback, multicast, reserved */
}

/* A contiguous mask from /1 to /30. */
static bool mask_ok(uint32_t mask)
{
    uint32_t host = ~mask;
    return mask && (host & (host + 1)) == 0 && host >= 3;
}

/* The fixed-length options and the lists' lengths, into r. */
static status_t lengths(const struct opts *o, struct dhcp_reply *r)
{
    status_t st = addr_opt(&o->server, &r->server);
    if (st == OK)
        st = addr_opt(&o->mask, &r->mask);
    if (st == OK)
        st = addr_opt(&o->lease, &r->lease_s);
    if (st == OK)
        st = addr_opt(&o->t1, &r->t1_s);
    if (st == OK)
        st = addr_opt(&o->t2, &r->t2_s);
    if (st == OK)
        st = list_ok(&o->router);
    if (st == OK)
        st = list_ok(&o->dns);
    return st;
}

/* The bytes kept of a list option. */
static uint32_t kept(const struct optval *v)
{
    return v->len < LIST_KEEP ? v->len : LIST_KEEP;
}

/* The router (the first usable one) and the DNS servers, into r, whose
 * address and mask are checked. */
static void lists(const struct opts *o, struct dhcp_reply *r)
{
    for (uint32_t i = 0; i + 4 <= kept(&o->router) && !r->router; i += 4) {
        uint32_t a = net_get32(o->router.v + i), host = a & ~r->mask;
        if (a != r->yiaddr && dhcp_unicast(a) && (a & r->mask) == (r->yiaddr & r->mask) &&
            host != 0 && host != ~r->mask)
            r->router = a;
    }
    for (uint32_t i = 0; i + 4 <= kept(&o->dns) && r->ndns < DHCP_MAX_DNS; i += 4) {
        uint32_t a = net_get32(o->dns.v + i);
        if (dhcp_unicast(a))
            r->dns[r->ndns++] = a;
    }
}

/* The values of a reply, checked; o was read without error. */
static status_t values(const struct opts *o, uint32_t yiaddr, struct dhcp_reply *r)
{
    status_t st = lengths(o, r);
    if (st != OK)
        return st;
    if (!r->server)
        return ERR_INVALID_ARGS;   /* RFC 2131 table 3: every reply names its server */
    if (r->type == DHCP_NAK) {
        *r = (struct dhcp_reply){ .type = DHCP_NAK, .server = r->server };
        return OK;
    }
    if (!dhcp_unicast(yiaddr) || (r->type == DHCP_ACK && !r->lease_s))
        return ERR_INVALID_ARGS;
    if (!o->mask.seen)
        r->mask = dhcp_class_mask(yiaddr);
    uint32_t host = yiaddr & ~r->mask;
    if (!mask_ok(r->mask) || host == 0 || host == ~r->mask)
        return ERR_INVALID_ARGS;   /* a bad mask, or the subnet's own address */
    r->yiaddr = yiaddr;
    lists(o, r);
    return OK;
}

/* Is this datagram a reply to our transaction? */
static status_t ours(const uint8_t *m, const uint8_t mac[6], uint32_t xid)
{
    if (m[DHCP_OFF_OP] != BOOTREPLY || m[DHCP_OFF_HTYPE] != HTYPE_ETHER ||
        m[DHCP_OFF_HLEN] != 6 || net_get32(m + DHCP_OFF_XID) != xid ||
        memcmp(m + DHCP_OFF_CHADDR, mac, 6) != 0)
        return ERR_NOT_FOUND;
    return OK;
}

status_t dhcp_parse(const void *msg, size_t len, const uint8_t mac[6], uint32_t xid,
                    struct dhcp_reply *out)
{
    const uint8_t *m = msg;
    if (len < DHCP_OFF_OPTIONS)
        return ERR_INVALID_ARGS;   /* not even the fixed part and the cookie */
    status_t st = ours(m, mac, xid);
    if (st != OK)
        return st;
    if (net_get32(m + DHCP_OFF_COOKIE) != DHCP_COOKIE)
        return ERR_INVALID_ARGS;
    struct opts o;
    if ((st = read_options(m, len, &o)) != OK)
        return st;
    if (!o.type.seen)
        return ERR_NOT_SUPPORTED;   /* BOOTP */
    if (o.type.len != 1)
        return ERR_INVALID_ARGS;
    struct dhcp_reply r = { .type = o.type.v[0] };
    if (r.type != DHCP_OFFER && r.type != DHCP_ACK && r.type != DHCP_NAK)
        return ERR_NOT_SUPPORTED;
    if ((st = values(&o, net_get32(m + DHCP_OFF_YIADDR), &r)) != OK)
        return st;
    *out = r;
    return OK;
}
