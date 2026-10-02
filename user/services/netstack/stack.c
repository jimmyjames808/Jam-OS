/* netstack's core: lwIP and its one network interface ("en0"), behind
 * the frame edge stack.h describes. The only file that includes lwIP.
 *
 * Frames in are copied into a pbuf from lwIP's pool (one buffer holds a
 * whole frame: lwipopts.h; a short one padded with zeros to 60 bytes:
 * stack_input says why) and handed to lwIP's Ethernet input, which
 * deals with them to the end: an ARP request for our address or an echo
 * request answered, a UDP datagram queued on its socket or answered with
 * a port unreachable, anything else dropped and counted by lwIP. Frames
 * out come from lwIP as a pbuf chain; they are flattened into one buffer,
 * padded to 60 bytes with zeros (so a short frame never carries old
 * bytes), and given to the edge. ICMP errors are rate-limited on the way
 * out (stack.h).
 *
 * State is file-static: one interface per process, one thread. */
#include "lwip/etharp.h"
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#include "lwip/stats.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include "stack.h"

#define ETHERTYPE_IPV4   0x0800u
#define ICMP_ECHO_REPLY  0u
#define ICMP_UNREACHABLE 3u
#define ICMP_ECHO        8u
#define ICMP_HDR         8u       /* type, code, checksum, id, seq */

_Static_assert(PBUF_POOL_BUFSIZE >= STACK_FRAME_MAX, "a received frame must fit one pbuf");

static struct netif       nif;          /* the interface: lwIP's netif_default */
static bool               lwip_ready;   /* lwip_init has run (it may only run once) */
static bool               started;      /* nif is added */
static struct stack_edge  edge;         /* where frames go out */
static struct stack_counts counts;      /* ours; lwIP's are added in stack_get_counts */
static uint8_t            txbuf[STACK_FRAME_MAX];   /* the frame being sent, flattened */
static uint64_t           icmp_tokens = STACK_ICMP_ERR_PER_S;   /* ICMP errors allowed now */
static uint64_t           icmp_topped;  /* ns: when icmp_tokens was last topped up */
static struct raw_pcb    *echo_pcb;     /* raw ICMP: programs' echo requests and their replies */
static uint8_t            udpbuf[STACK_UDP_MAX];   /* a datagram being handed to stack_udp_input */

void (*stack_udp_input)(void *ctx, uint32_t from, uint16_t port, const uint8_t *data,
                        size_t len);
void (*stack_echo_input)(uint32_t peer, uint16_t id, uint16_t seq, uint8_t ttl, size_t len,
                         bool unreachable);

static status_t refuse_tx(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    (void)frame;
    (void)len;
    return ERR_BAD_STATE;
}

const struct stack_edge stack_no_device = { .tx = refuse_tx, .mac = { 0x02 } };

static ip4_addr_t to_lwip(uint32_t a)
{
    ip4_addr_t r;
    ip4_addr_set_u32(&r, lwip_htonl(a));
    return r;
}

static uint32_t from_lwip(const ip4_addr_t *a)
{
    return lwip_htonl(ip4_addr_get_u32(a));   /* htonl is its own inverse */
}

/* The ICMP type of an IPv4 ICMP frame of len bytes, or -1 for any other
 * frame. Our own frame, built by lwIP: still, nothing is read past len. */
static int icmp_type(const uint8_t *f, size_t len)
{
    if (len < 14 + 20 || ((uint32_t)f[12] << 8 | f[13]) != ETHERTYPE_IPV4 ||
        f[14 + 9] != IP_PROTO_ICMP)
        return -1;
    size_t ihl = (size_t)(f[14] & 0x0f) * 4;
    return ihl >= 20 && 14 + ihl < len ? f[14 + ihl] : -1;
}

/* May an ICMP error go out now? Takes a token if so. */
static bool icmp_error_allowed(void)
{
    uint64_t t = now();
    uint64_t earned = (t - icmp_topped) * STACK_ICMP_ERR_PER_S / NS_PER_S;
    if (earned) {
        icmp_tokens += earned;
        if (icmp_tokens > STACK_ICMP_ERR_PER_S)
            icmp_tokens = STACK_ICMP_ERR_PER_S;
        icmp_topped = t;
    }
    if (!icmp_tokens)
        return false;
    icmp_tokens--;
    return true;
}

/* lwIP's linkoutput: one finished frame, possibly a pbuf chain. */
static err_t link_out(struct netif *n, struct pbuf *p)
{
    /* lwIP answers on the interface a frame came in on (an echo, an ARP
     * request) without looking at the link: with it down, nothing goes. */
    if (!netif_is_link_up(n) || p->tot_len > STACK_FRAME_MAX) {
        counts.tx_dropped++;
        return ERR_IF;
    }
    size_t len = pbuf_copy_partial(p, txbuf, p->tot_len, 0);
    if (len < STACK_FRAME_MIN) {
        memset(txbuf + len, 0, STACK_FRAME_MIN - len);
        len = STACK_FRAME_MIN;
    }
    int type = icmp_type(txbuf, len);
    if (type == ICMP_UNREACHABLE && !icmp_error_allowed()) {
        counts.icmp_limited++;
        return ERR_OK;   /* as if sent: the sender has nothing to do about it */
    }
    if (edge.tx(edge.ctx, txbuf, len) != OK) {
        counts.tx_dropped++;
        return ERR_IF;
    }
    counts.tx_frames++;
    if (type == ICMP_ECHO_REPLY)
        counts.echo_replies++;
    else if (type == ICMP_UNREACHABLE)
        counts.icmp_errors++;
    return ERR_OK;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'e';
    n->name[1] = 'n';
    n->mtu = STACK_MTU;
    n->hwaddr_len = STACK_MAC_LEN;
    memcpy(n->hwaddr, edge.mac, STACK_MAC_LEN);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->output = etharp_output;
    n->linkoutput = link_out;
    return ERR_OK;
}

static u8_t echo_in(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr);

status_t stack_start(const struct stack_edge *e)
{
    if (started)
        return ERR_BAD_STATE;
    if (!lwip_ready) {
        lwip_init();
        lwip_ready = true;
        echo_pcb = raw_new(IP_PROTO_ICMP);   /* the pool is empty only if this failed */
        if (echo_pcb)
            raw_recv(echo_pcb, echo_in, NULL);
    }
    edge = *e;
    if (!netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nif_init,
                   netif_input))
        return ERR_INTERNAL;
    netif_set_default(&nif);
    netif_set_up(&nif);
    started = true;
    return OK;
}

void stack_stop(void)
{
    if (!started)
        return;
    netif_remove(&nif);   /* takes it down first: its ARP entries go with it */
    started = false;
}

void stack_set_edge(const struct stack_edge *e)
{
    bool new_mac = memcmp(e->mac, edge.mac, STACK_MAC_LEN) != 0;
    edge = *e;
    if (!started || !new_mac)
        return;
    memcpy(nif.hwaddr, edge.mac, STACK_MAC_LEN);
    etharp_cleanup_netif(&nif);
}

void stack_set_link(bool up)
{
    if (!started)
        return;
    if (up)
        netif_set_link_up(&nif);
    else
        netif_set_link_down(&nif);
}

/* A frame shorter than Ethernet's minimum is padded with zeros to it, as
 * the wire pads it (one can arrive: a driver that takes the VLAN tag off
 * a 60-byte frame hands over 56). lwIP needs that: its ARP input reads
 * the 28-byte ARP header without checking the frame holds it, and would
 * otherwise read the rest from whatever the buffer held last (an earlier
 * frame: a cut-short request was answered as that one). The padding is
 * harmless to IP, which trims a datagram to its own total length. */
void stack_input(const uint8_t *frame, size_t len)
{
    counts.rx_frames++;
    if (!started || len < 14 || len > STACK_FRAME_MAX) {
        counts.rx_refused++;
        return;
    }
    size_t padded = len < STACK_FRAME_MIN ? STACK_FRAME_MIN : len;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)padded, PBUF_POOL);
    if (!p) {
        counts.rx_refused++;
        return;
    }
    if (p->len != padded) {   /* a chain: lwipopts.h's buffers can't make one */
        pbuf_free(p);
        counts.rx_refused++;
        return;
    }
    memcpy(p->payload, frame, len);
    memset((uint8_t *)p->payload + len, 0, padded - len);
    if (nif.input(p, &nif) != ERR_OK)
        pbuf_free(p);   /* not taken: still ours */
}

uint64_t stack_poll(void)
{
    sys_check_timeouts();
    u32_t ms = sys_timeouts_sleeptime();
    if (ms == SYS_TIMEOUTS_SLEEPTIME_INFINITE)
        return DEADLINE_NEVER;
    return now() + (uint64_t)ms * NS_PER_MS;
}

void stack_set_ipv4(const struct stack_ipv4 *ip)
{
    if (!started)
        return;
    ip4_addr_t a = to_lwip(ip->address), m = to_lwip(ip->mask), g = to_lwip(ip->gateway);
    netif_set_addr(&nif, &a, &m, &g);
}

void stack_clear(void)
{
    if (!started)
        return;
    netif_set_addr(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    etharp_cleanup_netif(&nif);
}

void stack_get(struct stack_state *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, edge.mac, STACK_MAC_LEN);
    out->device = edge.tx != refuse_tx;
    if (!started)
        return;
    out->link = netif_is_link_up(&nif);
    out->ip.address = from_lwip(netif_ip4_addr(&nif));
    out->ip.mask = from_lwip(netif_ip4_netmask(&nif));
    out->ip.gateway = from_lwip(netif_ip4_gw(&nif));
}

void stack_get_counts(struct stack_counts *out)
{
    *out = counts;
    out->link_dropped = lwip_stats.link.drop;
    out->arp_dropped = lwip_stats.etharp.drop;
    out->ip_dropped = lwip_stats.ip.drop;
    out->icmp_dropped = lwip_stats.icmp.drop;
    out->udp_dropped = lwip_stats.udp.drop;
    out->bad_checksums = lwip_stats.ip.chkerr + lwip_stats.icmp.chkerr + lwip_stats.udp.chkerr;
    out->rx_buffers_used = lwip_stats.memp[MEMP_PBUF_POOL]->used;
    out->rx_buffers_most = lwip_stats.memp[MEMP_PBUF_POOL]->max;
    out->rx_buffers_none = lwip_stats.memp[MEMP_PBUF_POOL]->err;
    out->heap_used = (uint32_t)lwip_stats.mem.used;
}

/* ---- programs' UDP sockets ----------------------------------------------------- */

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static status_t from_err(err_t e)
{
    switch (e) {
    case ERR_OK:  return OK;
    case ERR_RTE: return ERR_BAD_STATE;      /* no address, no route, or the link down */
    case ERR_MEM: return ERR_NO_MEMORY;
    case ERR_VAL: return ERR_INVALID_ARGS;   /* a broadcast without SOF_BROADCAST */
    case ERR_USE: return ERR_ALREADY_BOUND;
    default:      return ERR_NO_RESOURCES;   /* ERR_IF: the device refused the frame */
    }
}

/* lwIP's receive callback, every socket's: the datagram (copied out of
 * the pbuf, which could in principle be a chain) to stack_udp_input. */
static void udp_in(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                   u16_t port)
{
    (void)pcb;
    if (stack_udp_input && p->tot_len <= STACK_UDP_MAX) {
        u16_t n = pbuf_copy_partial(p, udpbuf, p->tot_len, 0);
        stack_udp_input(arg, from_lwip(ip_2_ip4(addr)), port, udpbuf, n);
    }
    pbuf_free(p);
}

status_t stack_udp_open(uint16_t port, bool bcast, void *ctx, struct stack_udp **out,
                        uint16_t *out_port)
{
    if (!started)
        return ERR_BAD_STATE;
    struct udp_pcb *pcb = udp_new();
    if (!pcb)
        return ERR_NO_RESOURCES;
    if (bcast)
        ip_set_option(pcb, SOF_BROADCAST);
    err_t e = udp_bind(pcb, IP4_ADDR_ANY, port);
    if (e != ERR_OK) {
        udp_remove(pcb);
        return e == ERR_USE ? ERR_ALREADY_BOUND : ERR_NO_RESOURCES;
    }
    udp_recv(pcb, udp_in, ctx);
    *out = (struct stack_udp *)pcb;
    *out_port = pcb->local_port;
    return OK;
}

void stack_udp_close(struct stack_udp *u)
{
    if (u)
        udp_remove((struct udp_pcb *)u);
}

status_t stack_udp_send(struct stack_udp *u, uint32_t to, uint16_t port, const void *data,
                        size_t len, bool on_link)
{
    struct udp_pcb *pcb = (struct udp_pcb *)u;
    if (len > STACK_UDP_MAX)
        return ERR_INVALID_ARGS;
    if (!started || !netif_is_link_up(&nif))
        return ERR_BAD_STATE;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return ERR_NO_MEMORY;
    memcpy(p->payload, data, len);
    ip4_addr_t dst = to_lwip(to);
    uint64_t dropped = counts.tx_dropped;
    err_t e = on_link ? udp_sendto_if(pcb, p, &dst, port, &nif) : udp_sendto(pcb, p, &dst, port);
    pbuf_free(p);
    if (e == ERR_OK && counts.tx_dropped != dropped)
        return ERR_NO_RESOURCES;   /* lwIP doesn't pass on every refusal (one queued for ARP) */
    return from_err(e);
}

/* ---- programs' pings ----------------------------------------------------------- */

/* The raw ICMP socket's callback, ahead of lwIP's own ICMP input: an
 * intact echo reply, or a destination unreachable about one of our echo
 * requests, goes to stack_echo_input and is eaten (1); anything else goes
 * on to lwIP (0: lwIP still owns p). p starts at the IP header, which
 * lwIP's IP input checked, and the pbuf is trimmed to the datagram. */
static u8_t echo_in(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)arg;
    (void)pcb;
    if (!stack_echo_input || p->len != p->tot_len || p->len < 20 + ICMP_HDR)
        return 0;
    const uint8_t *ip = p->payload;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    if (ihl < 20 || p->len < ihl + ICMP_HDR)
        return 0;
    const uint8_t *m = ip + ihl;
    size_t n = p->len - ihl;
    if (m[0] == ICMP_ECHO_REPLY && inet_chksum(m, (u16_t)n) == 0) {
        stack_echo_input(from_lwip(ip_2_ip4(addr)), (uint16_t)be16(m + 4), (uint16_t)be16(m + 6),
                         ip[8], n - ICMP_HDR, false);
        pbuf_free(p);
        return 1;
    }
    /* Unreachable: our request's IP header and its first 8 bytes inside. */
    const uint8_t *in = m + ICMP_HDR;
    size_t in_ihl = n >= ICMP_HDR + 20 ? (size_t)(in[0] & 0x0f) * 4 : 0;
    if (m[0] != ICMP_UNREACHABLE || in_ihl < 20 || n < ICMP_HDR + in_ihl + ICMP_HDR ||
        in[9] != IP_PROTO_ICMP || in[in_ihl] != ICMP_ECHO)
        return 0;
    uint32_t peer = (uint32_t)in[16] << 24 | (uint32_t)in[17] << 16 | (uint32_t)in[18] << 8 |
                    in[19];
    stack_echo_input(peer, (uint16_t)be16(in + in_ihl + 4), (uint16_t)be16(in + in_ihl + 6), 0,
                     0, true);
    pbuf_free(p);
    return 1;
}

status_t stack_echo_send(uint32_t to, uint16_t id, uint16_t seq, size_t size)
{
    if (size > STACK_UDP_MAX)
        return ERR_INVALID_ARGS;
    if (!started || !echo_pcb)
        return ERR_BAD_STATE;
    struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)(ICMP_HDR + size), PBUF_RAM);
    if (!p)
        return ERR_NO_MEMORY;
    uint8_t *m = p->payload;
    m[0] = ICMP_ECHO;
    m[1] = 0;
    m[2] = m[3] = 0;
    m[4] = (uint8_t)(id >> 8);
    m[5] = (uint8_t)id;
    m[6] = (uint8_t)(seq >> 8);
    m[7] = (uint8_t)seq;
    for (size_t i = 0; i < size; i++)
        m[ICMP_HDR + i] = (uint8_t)i;
    u16_t sum = inet_chksum(m, (u16_t)(ICMP_HDR + size));   /* in the wire's byte order */
    memcpy(m + 2, &sum, sizeof(sum));
    ip4_addr_t dst = to_lwip(to);
    uint64_t dropped = counts.tx_dropped;
    err_t e = raw_sendto(echo_pcb, p, &dst);
    pbuf_free(p);
    if (e == ERR_OK && counts.tx_dropped != dropped)
        return ERR_NO_RESOURCES;
    return from_err(e);
}
