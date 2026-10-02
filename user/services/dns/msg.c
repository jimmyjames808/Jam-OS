/* dns: names, the query, and what a reply says (RFC 1035 sections 3.1,
 * 4.1 and 4.1.4; RFC 2181 for TTLs).
 *
 * A reply is hostile until checked, and nothing is read before its
 * length is checked against what arrived:
 *   - the header (12 bytes); the id, QR, the opcode and the question
 *     (exactly one, our name, A, IN) must be ours, else the datagram is
 *     someone else's and is dropped, so a forged reply must know the
 *     query's id and local port and name;
 *   - TC set: the reply is cut short (no TCP to fetch it whole), and
 *     nothing after the question is read;
 *   - every record of every section is walked: its name, then 10 bytes
 *     (type, class, TTL, data length), then the data, which must end
 *     inside the datagram; the counts in the header must all be there,
 *     and together at most DNS_RR_MAX;
 *   - a name is labels of 1 to 63 bytes or a compression pointer. A
 *     pointer must point before where the run of labels it ends began
 *     (so every jump goes strictly backwards and a chain of them must
 *     end), at most DNS_JUMPS_MAX of them; the name at most
 *     DNS_NAME_MAX characters as text; the label types 01 and 10
 *     (extended, unused) are refused;
 *   - an A record's data is exactly 4 bytes; a CNAME's is one name that
 *     ends exactly at the end of its data.
 *
 * Only answer records whose owner is the name being resolved count, so
 * a reply can't plant addresses for other names: the A records of the
 * name asked; failing those, its CNAME, and then the A records of the
 * CNAME's target, and so on for up to DNS_CNAME_MAX CNAMEs (a loop of
 * CNAMEs runs into that limit). A record in another class is ignored.
 * Duplicate addresses count once. The TTL kept is the smallest along
 * the chain; one with the top bit set counts as 0 (RFC 2181 8). */
#include <netbytes.h>
#include <os.h>
#include "dns.h"

#define HDR_LEN  12
#define RR_FIXED 10   /* type, class, TTL, data length */

static char lower(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
}

bool dns_name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b))
            return false;
    return *a == *b;
}

static bool host_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_';
}

bool dns_name_ok(const char *name, size_t *len)
{
    size_t n = strnlen(name, DNS_NAME_MAX + 2), label = 0;
    if (n && name[n - 1] == '.')
        n--;   /* the root's dot */
    if (n == 0 || n > DNS_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (name[i] == '.') {
            if (label == 0)
                return false;   /* an empty label */
            label = 0;
        } else if (!host_char(name[i]) || ++label > DNS_LABEL_MAX) {
            return false;
        }
    }
    if (label == 0)
        return false;
    *len = n;
    return true;
}

bool dns_ipv4_literal(const char *name, uint32_t *addr)
{
    uint32_t a = 0;
    const char *p = name;
    for (int part = 0; part < 4; part++) {
        if (part && *p++ != '.')
            return false;
        unsigned v = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) {
            v = v * 10 + (unsigned)(*p++ - '0');
            digits++;
        }
        if (digits == 0 || digits > 3 || v > 255 || (digits > 1 && p[-(int)digits] == '0'))
            return false;   /* no number, too long, too big, or a leading zero */
        a = a << 8 | v;
    }
    if (*p)
        return false;
    *addr = a;
    return true;
}

size_t dns_build_query(uint8_t *buf, size_t cap, uint16_t id, const char *name)
{
    size_t n;
    if (!dns_name_ok(name, &n) || cap < HDR_LEN + n + 2 + 4)
        return 0;
    memset(buf, 0, HDR_LEN);
    net_put16(buf, id);
    net_put16(buf + 2, DNS_RD);
    net_put16(buf + 4, 1);   /* one question */
    size_t at = HDR_LEN, start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && name[i] != '.')
            continue;
        buf[at++] = (uint8_t)(i - start);   /* dns_name_ok: 1..63 */
        memcpy(buf + at, name + start, i - start);
        at += i - start;
        start = i + 1;
    }
    buf[at++] = 0;
    net_put16(buf + at, DNS_TYPE_A);
    net_put16(buf + at + 2, DNS_CLASS_IN);
    return at + 4;
}

/* A label byte that can be shown in a dotted name. */
static bool label_char(uint8_t ch)
{
    return ch > ' ' && ch < 0x7f && ch != '.';
}

/* Append one label (n bytes at l) to out, which holds *at characters. */
static status_t add_label(char *out, size_t *at, const uint8_t *l, size_t n)
{
    size_t need = n + (*at ? 1 : 0);
    if (need > DNS_NAME_MAX - *at)
        return ERR_INVALID_ARGS;   /* the name is too long */
    if (*at)
        out[(*at)++] = '.';
    for (size_t i = 0; i < n; i++) {
        if (!label_char(l[i]))
            return ERR_INVALID_ARGS;
        out[(*at)++] = (char)l[i];
    }
    return OK;
}

status_t dns_read_name(const uint8_t *msg, size_t len, size_t *pos, char *out)
{
    size_t p = *pos, seg = *pos, after = 0, at = 0;
    unsigned jumps = 0;
    for (;;) {
        if (p >= len)
            return ERR_INVALID_ARGS;
        uint8_t b = msg[p];
        if (b == 0)
            break;
        if ((b & 0xc0) == 0xc0) {
            if (len - p < 2 || ++jumps > DNS_JUMPS_MAX)
                return ERR_INVALID_ARGS;
            size_t to = (size_t)(b & 0x3f) << 8 | msg[p + 1];
            if (to >= seg)
                return ERR_INVALID_ARGS;   /* forward, to itself, or into its own run */
            if (!after)
                after = p + 2;
            p = seg = to;
            continue;
        }
        if (b & 0xc0)
            return ERR_INVALID_ARGS;   /* label types 01 and 10 */
        if (b > len - p - 1)
            return ERR_INVALID_ARGS;   /* the label runs past the end */
        if (add_label(out, &at, msg + p + 1, b) != OK)
            return ERR_INVALID_ARGS;
        p += 1 + (size_t)b;
    }
    out[at] = 0;
    *pos = after ? after : p + 1;
    return OK;
}

/* ---- the reply ------------------------------------------------------------ */

/* One resource record, its name read and its data inside the reply. */
struct rr {
    char     owner[DNS_NAME_MAX + 1];
    uint16_t type, class;
    uint32_t ttl;
    size_t   data, dlen;   /* the data's offset and length */
};

static uint32_t ttl_of(uint32_t raw)
{
    if (raw & 0x80000000u)
        return 0;
    return raw > DNS_TTL_MAX ? DNS_TTL_MAX : raw;
}

/* Read the record at *pos, check it, move *pos past it. answer: it is in
 * the answer section, where A and CNAME data is checked too. */
static status_t rr_read(const uint8_t *m, size_t len, size_t *pos, bool answer, struct rr *rr)
{
    status_t st = dns_read_name(m, len, pos, rr->owner);
    if (st != OK || len - *pos < RR_FIXED)
        return ERR_INVALID_ARGS;
    const uint8_t *f = m + *pos;
    rr->type = net_get16(f);
    rr->class = net_get16(f + 2);
    rr->ttl = ttl_of(net_get32(f + 4));
    rr->dlen = net_get16(f + 8);
    rr->data = *pos + RR_FIXED;
    if (rr->dlen > len - rr->data)
        return ERR_INVALID_ARGS;   /* the data runs past the end */
    *pos = rr->data + rr->dlen;
    if (!answer || rr->class != DNS_CLASS_IN)
        return OK;
    if (rr->type == DNS_TYPE_A && rr->dlen != 4)
        return ERR_INVALID_ARGS;
    if (rr->type == DNS_TYPE_CNAME) {
        char target[DNS_NAME_MAX + 1];
        size_t t = rr->data;
        if (dns_read_name(m, rr->data + rr->dlen, &t, target) != OK || t != rr->data + rr->dlen)
            return ERR_INVALID_ARGS;   /* not exactly one name */
    }
    return OK;
}

/* Header, question: is this the reply to our query? *pos: after the question. */
static status_t question(const uint8_t *m, size_t len, uint16_t id, const char *name, size_t *pos)
{
    uint16_t flags = net_get16(m + 2);
    if (net_get16(m) != id || !(flags & DNS_QR) || (flags & DNS_OPCODE) || net_get16(m + 4) != 1)
        return ERR_NOT_FOUND;
    char q[DNS_NAME_MAX + 1];
    *pos = HDR_LEN;
    if (dns_read_name(m, len, pos, q) != OK || len - *pos < 4)
        return ERR_INVALID_ARGS;
    if (!dns_name_eq(q, name) || net_get16(m + *pos) != DNS_TYPE_A ||
        net_get16(m + *pos + 2) != DNS_CLASS_IN)
        return ERR_NOT_FOUND;
    *pos += 4;
    return OK;
}

/* Walk every record of every section once, to check them all. */
static status_t walk_all(const uint8_t *m, size_t len, size_t pos, unsigned an, unsigned total)
{
    struct rr rr;
    for (unsigned i = 0; i < total; i++)
        if (rr_read(m, len, &pos, i < an, &rr) != OK)
            return ERR_INVALID_ARGS;
    return OK;
}

/* One pass over the answers for cur: its A records into out (no
 * duplicates), or its CNAME's target into next. */
static void pass(const uint8_t *m, size_t len, size_t pos, unsigned an, const char *cur,
                 struct dns_result *out, char *next)
{
    struct rr rr;
    next[0] = 0;
    for (unsigned i = 0; i < an; i++) {
        (void)rr_read(m, len, &pos, true, &rr);   /* walk_all checked every one */
        if (rr.class != DNS_CLASS_IN || !dns_name_eq(rr.owner, cur))
            continue;
        if (rr.type == DNS_TYPE_CNAME && !next[0]) {
            size_t t = rr.data;
            (void)dns_read_name(m, len, &t, next);
            out->ttl = rr.ttl < out->ttl ? rr.ttl : out->ttl;
        }
        if (rr.type != DNS_TYPE_A || out->naddr == DNS_MAX_ADDRS)
            continue;
        uint32_t a = net_get32(m + rr.data);
        bool dup = false;
        for (unsigned k = 0; k < out->naddr; k++)
            dup |= out->addr[k] == a;
        if (!dup)
            out->addr[out->naddr++] = a;
        out->ttl = rr.ttl < out->ttl ? rr.ttl : out->ttl;
    }
}

/* Follow name through the answers: addresses, a CNAME to ask for next,
 * or nothing. */
static void chain(const uint8_t *m, size_t len, size_t pos, unsigned an, const char *name,
                  struct dns_result *out)
{
    char cur[DNS_NAME_MAX + 1], next[DNS_NAME_MAX + 1];
    memcpy(cur, name, strnlen(name, DNS_NAME_MAX) + 1);
    cur[DNS_NAME_MAX] = 0;
    out->ttl = DNS_TTL_MAX;
    for (;;) {
        pass(m, len, pos, an, cur, out, next);
        if (out->naddr) {
            out->outcome = DNS_ADDRS;
            return;
        }
        if (!next[0]) {
            out->outcome = out->cnames ? DNS_FOLLOW : DNS_NO_DATA;
            if (!out->cnames)
                out->ttl = 0;
            memcpy(out->next, cur, sizeof(cur));
            return;
        }
        if (++out->cnames > DNS_CNAME_MAX) {
            out->outcome = DNS_FOLLOW;   /* the resolver counts it as too many */
            memcpy(out->next, next, sizeof(next));
            return;
        }
        memcpy(cur, next, sizeof(cur));
    }
}

status_t dns_parse_reply(const uint8_t *msg, size_t len, uint16_t id, const char *name,
                         struct dns_result *out)
{
    if (len < HDR_LEN)
        return ERR_INVALID_ARGS;
    size_t pos;
    status_t st = question(msg, len, id, name, &pos);
    if (st != OK)
        return st;
    struct dns_result r = { .outcome = DNS_TRUNCATED };
    uint16_t flags = net_get16(msg + 2);
    if (flags & DNS_TC) {
        *out = r;   /* what follows the question may be cut anywhere */
        return OK;
    }
    unsigned an = net_get16(msg + 6);
    unsigned total = an + net_get16(msg + 8) + net_get16(msg + 10);
    if (total > DNS_RR_MAX || walk_all(msg, len, pos, an, total) != OK)
        return ERR_INVALID_ARGS;
    r.outcome = DNS_SERVER_FAIL;
    if ((flags & DNS_RCODE) == DNS_RCODE_NXDOMAIN)
        r.outcome = DNS_NO_NAME;
    else if ((flags & DNS_RCODE) == DNS_RCODE_OK)
        chain(msg, len, pos, an, name, &r);
    *out = r;
    return OK;
}
