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
 * Programs' UDP sockets, pings and TCP connections are lwIP's raw API
 * turned into stack.h's calls and hooks at the bottom of the file; TCP's
 * model (the window is the socket's rx ring) is stack.h's.
 *
 * State is file-static: one interface per process, one thread. */
#include "lwip/etharp.h"
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/raw.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include "stack.h"

#define ETHERTYPE_IPV4   0x0800u
#define ICMP_ECHO_REPLY  0u
#define ICMP_UNREACHABLE 3u       /* (ICMP_ECHO, 8, is lwIP's: lwip/prot/icmp.h) */
#define ICMP_HDR         8u       /* type, code, checksum, id, seq */

_Static_assert(PBUF_POOL_BUFSIZE >= STACK_FRAME_MAX, "a received frame must fit one pbuf");
_Static_assert(STACK_TCP_MSS == TCP_MSS && STACK_TCP_WND == TCP_WND, "stack.h's TCP sizes");
_Static_assert(STACK_TCP_WND <= 0xffff, "a window without scaling");
_Static_assert(STACK_TCP_LISTENERS == MEMP_NUM_TCP_PCB_LISTEN, "a listener's pcb each");
_Static_assert(STACK_TCP_BACKLOG == TCP_DEFAULT_LISTEN_BACKLOG && STACK_TCP_BACKLOG <= 0xff,
               "lwIP's backlog is a byte");
_Static_assert(MEMP_NUM_TCP_PCB == STACK_TCP_CONNS + 128, "netstack's connections and 128 more");

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
static const struct stack_tcp_hooks *tcp_hooks;    /* where TCP's events go (NULL: none) */
static uint8_t            tcp_ext = LWIP_TCP_PCB_NUM_EXT_ARG_ID_INVALID;   /* our ext arg slot */
static bool               addr_going;   /* the address is changing: lwIP aborts its connections */
static uint32_t           tcp_bad_acks; /* segments dropped by tcp_drop: a bad ACK */
static uint32_t           tcp_no_acks;  /* ... no ACK flag */
static bool               tx_blocked;   /* a frame found the edge full (stack_tx_blocked) */
static unsigned           resume_from;  /* the active pcb stack_tx_resume starts with */

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

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return be16(p) << 16 | be16(p + 2);
}

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
    status_t st = edge.tx(edge.ctx, txbuf, len);
    if (st != OK) {
        counts.tx_dropped++;
        tx_blocked |= st == ERR_NO_RESOURCES;   /* full: TCP keeps the segment (unsent) */
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
        tcp_ext = tcp_ext_arg_alloc_id();   /* LWIP_TCP_PCB_NUM_EXT_ARGS's first: can't fail */
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
    addr_going = true;
    netif_remove(&nif);   /* takes it down first: its ARP entries and connections go with it */
    addr_going = false;
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

/* Two checks lwIP lacks, on segments to one of our connections, before
 * lwIP sees them (anything that is not plainly TCP to one of them is left
 * to lwIP's own checks):
 * - RFC 9293 3.10.7.4, the fifth check: on a connection past SYN_SENT a
 *   segment with neither ACK, RST nor SYN is dropped (lwIP would still take
 *   its bytes or FIN). A RST without ACK stays lwIP's (its own check comes
 *   first), as does a SYN (answered with a challenge ACK);
 * - RFC 5961 section 5: a segment with bytes or a FIN on a synchronized
 *   connection whose ACK is for bytes never sent (past snd_nxt), or older
 *   than any window the peer was given (before lastack - snd_wnd_max), is
 *   not from the peer the connection talks to: lwIP sees the bad ACK but
 *   still takes the bytes, so a blind injection would need only a sequence
 *   number in the window. A bare ACK lwIP treats right.
 * The counter of the check that dropped it, or NULL to pass it on. */
static uint32_t *tcp_drop(const uint8_t *f, size_t len)
{
    if (len < 14 + 20 || be16(f + 12) != ETHERTYPE_IPV4 || f[14 + 9] != IP_PROTO_TCP)
        return NULL;
    size_t ihl = (size_t)(f[14] & 0x0f) * 4, total = be16(f + 16);
    if (ihl < 20 || total > len - 14 || total < ihl + 20)
        return NULL;
    const uint8_t *t = f + 14 + ihl;
    size_t hl = (size_t)(t[12] >> 4) * 4;
    if (hl < 20 || hl > total - ihl)
        return NULL;
    uint8_t flags = t[13];
    bool payload = total - ihl > hl || (flags & TCP_FIN);
    uint32_t src = be32(f + 26), ack = be32(t + 8);
    for (const struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
        if (pcb->local_port != be16(t + 2) || pcb->remote_port != be16(t) ||
            from_lwip(ip_2_ip4(&pcb->remote_ip)) != src)
            continue;
        if (pcb->state >= SYN_RCVD && !(flags & (TCP_ACK | TCP_RST | TCP_SYN)))
            return &tcp_no_acks;
        bool bad = pcb->state >= ESTABLISHED && payload && (flags & TCP_ACK) &&
                   (TCP_SEQ_GT(ack, pcb->snd_nxt) ||
                    TCP_SEQ_LT(ack, pcb->lastack - pcb->snd_wnd_max));
        return bad ? &tcp_bad_acks : NULL;
    }
    return NULL;
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
    uint32_t *dropped = tcp_drop(frame, len);
    if (dropped) {
        (*dropped)++;
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

bool stack_tx_blocked(void)
{
    return tx_blocked;
}

/* tcp_output for each active pcb at list index [from, to) that holds
 * something to send (segments, an ACK owed), until the edge is full again:
 * the index it stopped at, or `to`. */
static unsigned output_range(unsigned from, unsigned to)
{
    unsigned i = 0;
    for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb && i < to; pcb = pcb->next, i++) {
        if (i < from || !(pcb->unsent || (pcb->flags & TF_ACK_NOW)))
            continue;
        (void)tcp_output(pcb);   /* a failure leaves it unsent again, and tx_blocked set */
        if (tx_blocked)
            return i;
    }
    return to;
}

void stack_tx_resume(void)
{
    tx_blocked = false;
    /* From where the last resume found the edge full, so the first
     * connections on the list can't take every slot each time. */
    unsigned at = output_range(resume_from, UINT32_MAX);
    if (!tx_blocked)
        at = output_range(0, resume_from);
    resume_from = tx_blocked ? at : 0;
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
    addr_going = true;   /* a new address ends the connections on the old one */
    netif_set_addr(&nif, &a, &m, &g);
    addr_going = false;
}

void stack_clear(void)
{
    if (!started)
        return;
    addr_going = true;
    netif_set_addr(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    addr_going = false;
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
    out->bad_checksums = lwip_stats.ip.chkerr + lwip_stats.icmp.chkerr + lwip_stats.udp.chkerr +
                         lwip_stats.tcp.chkerr;
    out->rx_buffers_used = lwip_stats.memp[MEMP_PBUF_POOL]->used;
    out->rx_buffers_most = lwip_stats.memp[MEMP_PBUF_POOL]->max;
    out->rx_buffers_none = lwip_stats.memp[MEMP_PBUF_POOL]->err;
    out->heap_used = (uint32_t)lwip_stats.mem.used;
}

/* ---- programs' UDP sockets ----------------------------------------------------- */

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

/* ---- programs' TCP connections (stack.h has the model) -------------------------- */

static struct tcp_pcb *pcb_of(struct stack_tcp *t)
{
    return (struct tcp_pcb *)t;
}

void stack_tcp_set_hooks(const struct stack_tcp_hooks *h)
{
    tcp_hooks = h;
}

/* The window a connection announces from now on: min(TCP_WND, w). lwIP
 * starts every connection at TCP_WND and gives bytes back up to it; set
 * before any byte can come, the smaller window is where the counting
 * starts, and stack_tcp_recved never gives back more than came. */
static void window_set(struct tcp_pcb *pcb, uint32_t w)
{
    if (w > TCP_WND)
        w = TCP_WND;
    pcb->rcv_wnd = pcb->rcv_ann_wnd = (tcpwnd_size_t)w;
}

/* Drop the first n bytes of a received chain in place, so that what lwIP
 * keeps as refused data is what the hook didn't take. In place, because
 * lwIP keeps the pointer it gave: a pbuf emptied this way stays in the
 * chain with no bytes. Every pbuf's tot_len counts the bytes from it on. */
static void take_front(struct pbuf *p, size_t n)
{
    for (struct pbuf *q = p; q && n; q = q->next) {
        u16_t k = (u16_t)(n < q->len ? n : q->len);
        for (struct pbuf *r = p; r != q; r = r->next)
            r->tot_len = (u16_t)(r->tot_len - k);
        pbuf_remove_header(q, k);   /* k <= q->len: can't fail */
        n -= k;
    }
}

static err_t tcp_in_cb(void *ctx, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb;
    (void)err;   /* always ERR_OK in lwIP 2.2 */
    if (!p) {
        tcp_hooks->rx_end(ctx);
        return ERR_OK;
    }
    size_t took = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (!q->len)
            continue;
        size_t k = tcp_hooks->rx(ctx, q->payload, q->len);
        took += k;
        if (k < q->len)
            break;
    }
    if (took == p->tot_len) {
        pbuf_free(p);
        return ERR_OK;
    }
    take_front(p, took);
    return ERR_MEM;   /* lwIP keeps the rest and offers it again */
}

static err_t tcp_sent_cb(void *ctx, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    tcp_hooks->sent(ctx);
    return ERR_OK;
}

static err_t tcp_connected_cb(void *ctx, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    (void)err;   /* always ERR_OK: a failure comes to tcp_err_cb */
    tcp_hooks->connected(ctx);
    return ERR_OK;
}

/* lwIP's word for why a connection ended, as stack.h's hooks.gone says it. */
static status_t gone_why(err_t e)
{
    switch (e) {
    case ERR_CLSD: return OK;                /* LAST_ACK's ACK: both sides closed */
    case ERR_ABRT: return addr_going ? ERR_BAD_STATE : ERR_TIMED_OUT;   /* rtx gave up */
    default:       return ERR_PEER_CLOSED;   /* ERR_RST */
    }
}

/* The pcb is already freed when lwIP calls this. */
static void tcp_err_cb(void *ctx, err_t e)
{
    if (ctx && tcp_hooks)
        tcp_hooks->gone(ctx, gone_why(e));
}

static void callbacks_set(struct tcp_pcb *pcb, void *ctx)
{
    tcp_arg(pcb, ctx);
    tcp_recv(pcb, ctx ? tcp_in_cb : NULL);   /* NULL: lwIP's own, which drops and closes */
    tcp_sent(pcb, ctx ? tcp_sent_cb : NULL);
    tcp_err(pcb, ctx ? tcp_err_cb : NULL);
}

status_t stack_tcp_connect(uint32_t to, uint16_t port, uint32_t window, void *ctx,
                           struct stack_tcp **out)
{
    if (!started || !tcp_hooks)
        return ERR_BAD_STATE;
    if (!netif_is_link_up(&nif) || ip4_addr_isany_val(*netif_ip4_addr(&nif)))
        return ERR_BAD_STATE;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb)
        return ERR_NO_RESOURCES;
    tcp_nagle_disable(pcb);   /* publishing is the push: send what the ring has */
    callbacks_set(pcb, ctx);
    ip4_addr_t dst = to_lwip(to);
    err_t e = tcp_connect(pcb, &dst, port, tcp_connected_cb);
    if (e != ERR_OK) {
        callbacks_set(pcb, NULL);
        tcp_abort(pcb);   /* never connected: nothing is sent */
        return e == ERR_RTE ? ERR_BAD_STATE : e == ERR_MEM ? ERR_NO_MEMORY : ERR_NO_RESOURCES;
    }
    /* The SYN went with TCP_WND; the handshake's ACK, before any byte can
     * come, carries this one. */
    window_set(pcb, window);
    *out = (struct stack_tcp *)pcb;
    return OK;
}

/* Before a listener's SYN-ACK goes: the new connection's window is the
 * listener's (its ext arg). */
static err_t passive_open(u8_t id, struct tcp_pcb_listen *lpcb, struct tcp_pcb *cpcb)
{
    window_set(cpcb, (uint32_t)(uintptr_t)lpcb->ext_args[id].data);
    tcp_nagle_disable(cpcb);
    return ERR_OK;
}

static const struct tcp_ext_arg_callbacks listen_ext = { .passive_open = passive_open };

/* A listener's connection finished its handshake. Its prio goes from the
 * listener's lowest (a half-open pcb lwIP may recycle) to a program's. */
static err_t tcp_accept_cb(void *lctx, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK || !pcb)
        return ERR_VAL;   /* lwIP had no pcb: the SYN is dropped, nothing to undo */
    void *ctx = tcp_hooks ? tcp_hooks->accepted(lctx, (struct stack_tcp *)pcb) : NULL;
    if (!ctx) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    tcp_setprio(pcb, TCP_PRIO_NORMAL);
    callbacks_set(pcb, ctx);
    tcp_backlog_delayed(pcb);   /* it counts against the backlog until the program takes it */
    return ERR_OK;
}

status_t stack_tcp_listen(uint16_t port, uint32_t backlog, uint32_t window, void *lctx,
                          struct stack_tcp_listen **out, uint16_t *out_port)
{
    if (!started || !tcp_hooks)
        return ERR_BAD_STATE;
    if (backlog < 1 || backlog > STACK_TCP_BACKLOG)
        return ERR_INVALID_ARGS;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb)
        return ERR_NO_RESOURCES;
    tcp_setprio(pcb, TCP_PRIO_MIN);   /* its half-open connections inherit it */
    err_t e = tcp_bind(pcb, IP4_ADDR_ANY, port);
    if (e != ERR_OK) {
        tcp_abort(pcb);
        return e == ERR_USE ? ERR_ALREADY_BOUND : ERR_NO_RESOURCES;
    }
    tcp_ext_arg_set_callbacks(pcb, tcp_ext, &listen_ext);
    tcp_ext_arg_set(pcb, tcp_ext, (void *)(uintptr_t)window);
    struct tcp_pcb *l = tcp_listen_with_backlog_and_err(pcb, (u8_t)backlog, &e);
    if (!l) {
        tcp_abort(pcb);   /* lwIP keeps the bound pcb when it has no listener for it */
        return ERR_NO_RESOURCES;
    }
    tcp_arg(l, lctx);
    tcp_accept(l, tcp_accept_cb);
    *out = (struct stack_tcp_listen *)l;
    *out_port = l->local_port;
    return OK;
}

void stack_tcp_unlisten(struct stack_tcp_listen *l)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *)l;
    tcp_arg(pcb, NULL);
    (void)tcp_close(pcb);   /* a listener always closes: its half-open ones are dropped */
}

void stack_tcp_taken(struct stack_tcp *t)
{
    tcp_backlog_accepted(pcb_of(t));
}

size_t stack_tcp_room(struct stack_tcp *t)
{
    struct tcp_pcb *pcb = pcb_of(t);
    if ((pcb->state != ESTABLISHED && pcb->state != CLOSE_WAIT) || (pcb->flags & TF_FIN))
        return 0;
    uint32_t queued = pcb->snd_lbb - pcb->lastack;   /* given and not yet acked */
    uint32_t allowed = (uint32_t)pcb->snd_wnd + pcb->mss;
    uint32_t room = allowed > queued ? allowed - queued : 0;
    if (room > tcp_sndbuf(pcb))
        room = tcp_sndbuf(pcb);
    return room;
}

status_t stack_tcp_send(struct stack_tcp *t, const void *data, size_t n)
{
    if (!n)
        return OK;
    if (n > 0xffff)
        return ERR_INVALID_ARGS;
    err_t e = tcp_write(pcb_of(t), data, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e == ERR_MEM)
        return ERR_NO_MEMORY;
    return e == ERR_OK ? OK : ERR_BAD_STATE;
}

void stack_tcp_push(struct stack_tcp *t)
{
    (void)tcp_output(pcb_of(t));   /* a frame the edge refused is resent by TCP's timers */
}

void stack_tcp_recved(struct stack_tcp *t, size_t n)
{
    while (n) {
        u16_t k = (u16_t)(n < 0xffff ? n : 0xffff);
        tcp_recved(pcb_of(t), k);   /* it sends the window update when one is worth it */
        n -= k;
    }
}

status_t stack_tcp_shutdown(struct stack_tcp *t)
{
    err_t e = tcp_shutdown(pcb_of(t), 0, 1);
    return e == ERR_OK ? OK : e == ERR_MEM ? ERR_NO_MEMORY : ERR_BAD_STATE;
}

bool stack_tcp_fin_acked(struct stack_tcp *t)
{
    enum tcp_state s = pcb_of(t)->state;
    return s == FIN_WAIT_2 || s == TIME_WAIT;
}

void stack_tcp_release(struct stack_tcp *t)
{
    struct tcp_pcb *pcb = pcb_of(t);
    callbacks_set(pcb, NULL);
    tcp_setprio(pcb, TCP_PRIO_MIN);   /* lwIP may recycle it now */
    /* tcp_close resets a connection whose received bytes were not all
     * given back to a window of TCP_WND; this one's window is its ring's,
     * and every byte was read (stack.h). */
    if (pcb->state == ESTABLISHED || pcb->state == CLOSE_WAIT)
        pcb->rcv_wnd = TCP_WND_MAX(pcb);
    if (tcp_close(pcb) != ERR_OK)
        tcp_abort(pcb);   /* no memory for the FIN: a reset still ends it */
}

void stack_tcp_abort(struct stack_tcp *t)
{
    struct tcp_pcb *pcb = pcb_of(t);
    callbacks_set(pcb, NULL);
    tcp_abort(pcb);
}

void stack_tcp_ends(struct stack_tcp *t, uint32_t *peer, uint16_t *peer_port, uint16_t *port)
{
    struct tcp_pcb *pcb = pcb_of(t);
    *peer = from_lwip(ip_2_ip4(&pcb->remote_ip));
    *peer_port = pcb->remote_port;
    *port = pcb->local_port;
}

/* pcbs on a list (all, or those in state `only`): bounded by lwIP's pool. */
static uint32_t list_len(const struct tcp_pcb *pcb, bool all, enum tcp_state only)
{
    uint32_t n = 0;
    for (; pcb; pcb = pcb->next)
        n += all || pcb->state == only;
    return n;
}

void stack_tcp_get_counts(struct stack_tcp_counts *out)
{
    memset(out, 0, sizeof(*out));
    out->half_open = list_len(tcp_active_pcbs, false, SYN_RCVD);
    out->live = list_len(tcp_active_pcbs, true, CLOSED) - out->half_open;
    out->time_wait = list_len(tcp_tw_pcbs, true, CLOSED);
    for (const struct tcp_pcb_listen *l = tcp_listen_pcbs.listen_pcbs; l; l = l->next)
        out->listeners++;
    out->segs_used = lwip_stats.memp[MEMP_TCP_SEG]->used;
    out->dropped = lwip_stats.tcp.drop;
    out->bad_checksums = lwip_stats.tcp.chkerr;
    out->pcbs_none = lwip_stats.memp[MEMP_TCP_PCB]->err;
    out->bad_acks = tcp_bad_acks;
    out->no_acks = tcp_no_acks;
}
