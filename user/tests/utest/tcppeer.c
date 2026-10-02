/* utest: the hand-driven TCP peer (tcppeer.h): segments built and read
 * byte by byte, on netpkt.c's Ethernet and IPv4 helpers. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "netpkt.h"
#include "stack.h"
#include "tcppeer.h"
#include "utest.h"

#define TCP_PROTO 6u
#define TCP_HDR   20u

static struct {
    uint8_t f[PKT_FRAME_MAX];
    size_t  n;
} cap[TP_CAP];
static unsigned ncap;
static unsigned room = TP_CAP;   /* frames the edge takes before it is full (tp_room) */

void tp_room(unsigned frames)
{
    room = frames;
}

status_t tp_edge_tx(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (ncap >= TP_CAP || ncap >= room || len > PKT_FRAME_MAX)
        return ERR_NO_RESOURCES;
    memcpy(cap[ncap].f, frame, len);
    cap[ncap++].n = len;
    return OK;
}

unsigned tp_caught(void)
{
    return ncap;
}

void tp_forget(void)
{
    ncap = 0;
}

void tp_arp(const uint8_t *mac, uint32_t ip)
{
    uint8_t f[64];
    stack_input(f, pkt_arp(f, 1, mac, ip, NULL, OUR_IP));
}

/* ---- building ------------------------------------------------------------------ */

/* The TCP checksum over the pseudo-header (src, dst, protocol, length)
 * and the segment's len bytes at t. */
static uint32_t tcp_sum(uint32_t src, uint32_t dst, const uint8_t *t, size_t len)
{
    uint8_t ph[12];
    pkt_put32(ph, src);
    pkt_put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = TCP_PROTO;
    pkt_put16(ph + 10, (uint32_t)len);
    uint32_t s = pkt_sum16(ph, 12) + pkt_sum16(t, len);
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return s;
}

void tp_refix(uint8_t *f, size_t n)
{
    uint8_t *ip = f + 14, *t = ip + 20;
    size_t len = pkt_get16(ip + 2) - 20;
    if (34 + len > n)
        len = n - 34;
    pkt_put16(t + 16, 0);
    pkt_put16(t + 16, ~tcp_sum(pkt_get32(ip + 12), pkt_get32(ip + 16), t, len) & 0xffff);
}

size_t tp_frame(uint8_t *f, const struct tp *p, uint8_t flags, const void *data, size_t len)
{
    size_t opt = (flags & TP_SYN) ? 4 : 0;
    pkt_eth(f, pkt_our_mac, p->mac, ETH_IPV4);
    uint8_t *t = f + 34;
    pkt_put16(t, p->port);
    pkt_put16(t + 2, p->our);
    pkt_put32(t + 4, p->snd);
    pkt_put32(t + 8, (flags & TP_ACK) ? p->rcv : 0);
    t[12] = (uint8_t)(((TCP_HDR + opt) / 4) << 4);
    t[13] = flags;
    pkt_put16(t + 14, p->win);
    pkt_put16(t + 16, 0);
    pkt_put16(t + 18, 0);
    if (opt) {   /* MSS 1460 */
        t[20] = 2;
        t[21] = 4;
        pkt_put16(t + 22, 1460);
    }
    if (len)
        memcpy(t + TCP_HDR + opt, data, len);
    size_t tl = TCP_HDR + opt + len;
    pkt_ipv4(f, TCP_PROTO, p->ip, OUR_IP, tl);
    tp_refix(f, 34 + tl);
    return 34 + tl;
}

void tp_input(const uint8_t *f, size_t n)
{
    stack_input(f, n);
}

void tp_send(struct tp *p, uint8_t flags, const void *data, size_t len)
{
    static uint8_t f[PKT_FRAME_MAX];
    tp_input(f, tp_frame(f, p, flags, data, len));
    p->snd += (uint32_t)len + ((flags & (TP_SYN | TP_FIN)) ? 1 : 0);
}

/* ---- reading -------------------------------------------------------------------- */

/* Frame i as a TCP segment from us to the peer's address, checked;
 * false (no failure) if it is not TCP to p->ip at all. */
static bool parse(unsigned i, const struct tp *p, struct tp_seg *s, bool *bad)
{
    const uint8_t *f = cap[i].f, *t;
    size_t n = cap[i].n, len;
    *bad = false;
    if (n < 34 || pkt_get16(f + 12) != ETH_IPV4 || f[23] != TCP_PROTO ||
        pkt_get32(f + 30) != p->ip)
        return false;
    *bad = true;
    CHECK(pkt_is_ipv4(f, n, p->ip, TCP_PROTO, &t, &len));
    CHECK(!memcmp(f, p->mac, 6));
    CHECK(len >= TCP_HDR);
    CHECK_EQ(tcp_sum(OUR_IP, p->ip, t, len), 0xffff);
    size_t hl = (size_t)(t[12] >> 4) * 4;
    CHECK(hl >= TCP_HDR && hl <= len);
    *s = (struct tp_seg){ .sport = (uint16_t)pkt_get16(t), .dport = (uint16_t)pkt_get16(t + 2),
                          .seq = pkt_get32(t + 4), .ack = pkt_get32(t + 8), .flags = t[13] & 0x3f,
                          .win = (uint16_t)pkt_get16(t + 14), .data = t + hl, .len = len - hl };
    for (size_t k = TCP_HDR; k + 1 < hl;) {   /* the options: MSS, padding */
        if (t[k] == 0)
            break;
        if (t[k] == 1) {
            k++;
            continue;
        }
        CHECK(t[k + 1] >= 2 && k + t[k + 1] <= hl);
        if (t[k] == 2 && t[k + 1] == 4)
            s->mss = (uint16_t)pkt_get16(t + k + 2);
        k += t[k + 1];
    }
    *bad = false;
    return true;
}

/* Frames that are not p's stay caught (moved to the front, in order); p's
 * are read and gone, their bytes copied out first (segdata), since a kept
 * frame may move over them. */
bool tp_read(const struct tp *p, struct tp_seg *out, unsigned max, unsigned *n,
             unsigned *others)
{
    static uint8_t segdata[TP_CAP][PKT_FRAME_MAX];
    *n = *others = 0;
    unsigned caught = ncap, kept = 0;
    for (unsigned i = 0; i < caught; i++) {
        struct tp_seg s;
        bool bad;
        bool mine = parse(i, p, &s, &bad) && s.dport == p->port && (!p->our || s.sport == p->our);
        if (bad)
            FAIL("frame %u of %u to the peer is malformed", i, caught);
        if (!mine) {
            if (kept != i)
                cap[kept] = cap[i];
            kept++;
            (*others)++;
            continue;
        }
        if (*n < max) {
            memcpy(segdata[*n], s.data, s.len);
            s.data = segdata[*n];
            out[*n] = s;
        }
        (*n)++;
    }
    ncap = kept;
    CHECK(*n <= max);
    return true;
}

unsigned tp_absorb(struct tp *p, const struct tp_seg *s, unsigned n, uint8_t *buf, size_t cap_,
                   size_t *got)
{
    unsigned took = 0;
    for (unsigned i = 0; i < n; i++) {
        if (s[i].seq != p->rcv)
            continue;
        if (s[i].len) {
            size_t k = s[i].len;
            if (buf && *got + k <= cap_)
                memcpy(buf + *got, s[i].data, k);
            *got += k;
            p->rcv += (uint32_t)k;
            took++;
        }
        if (s[i].flags & (TP_FIN | TP_SYN))
            p->rcv++;
    }
    return took;
}

/* ---- the test stream ------------------------------------------------------------ */

uint8_t tp_byte(uint32_t id, uint64_t i)
{
    uint64_t x = (i + 1) * 0x9e3779b97f4a7c15ull ^ (uint64_t)id << 40;
    return (uint8_t)(x >> 29 ^ x >> 13);
}

void tp_fill(uint32_t id, uint64_t from, uint8_t *buf, size_t n)
{
    for (size_t k = 0; k < n; k++)
        buf[k] = tp_byte(id, from + k);
}

bool tp_same(uint32_t id, uint64_t from, const uint8_t *buf, size_t n)
{
    for (size_t k = 0; k < n; k++)
        if (buf[k] != tp_byte(id, from + k))
            return false;
    return true;
}
