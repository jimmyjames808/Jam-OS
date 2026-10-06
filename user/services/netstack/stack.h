/* netstack's core: lwIP over one network interface, behind a narrow edge
 * (docs/M9-PLAN.md "netstack: lwIP, single-threaded"). stack.c is the only
 * file that sees lwIP; everything here is plain bytes and numbers, so the
 * loop (main.c), the control channel (ctl.c) and the tests
 * (user/tests/utest/netstack.c, which links these objects and drives
 * them in-process) never include an lwIP header.
 *
 * The edge, frames in and frames out, is where a device plugs in:
 * - frames out: lwIP calls the edge's `tx` with each finished Ethernet
 *   frame, untagged (the NIC driver adds the VLAN tag; netstack never
 *   sees one), 60 to 1514 bytes, padded with zeros to the minimum, no
 *   FCS. The frame is netstack's buffer: tx copies it before it returns.
 * - frames in: the device's reader calls stack_input once per received
 *   frame (untagged, no FCS), which lwIP handles to the end before it
 *   returns (an ARP or echo reply goes out through tx meanwhile).
 * - the link: stack_set_link when the device says it changed.
 * netif.c plugs the network card's netdev rings (<jam/netdev.h>) in here:
 * the rx ring's reader takes each frame (netdev_take) into a buffer and
 * calls stack_input; tx is netdev_room and netdev_put on the tx ring (a
 * full ring is an error, counted), then netdev_publish; NETDEV_SIG_LINK
 * becomes stack_set_link. With no session (no card, or its driver
 * restarting) the edge is stack_no_device: link down, every frame out
 * dropped and counted. A frame from the rx ring may be as short as 14
 * bytes (56 for a minimum-size frame whose tag the driver removed):
 * stack_input pads it.
 *
 * One thread: nothing here locks. Addresses are IPv4 addresses as
 * numbers, the first byte highest (10.2.21.5 is 0x0a021505). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define STACK_MTU        1500u   /* IP bytes in one frame */
#define STACK_FRAME_MIN  60u     /* an Ethernet frame without its FCS, at least */
#define STACK_FRAME_MAX  1514u   /* and at most: 14 bytes of header + the MTU */
#define STACK_MAC_LEN    6u

/* Where frames go out (above). OK if the frame was taken; any error counts
 * it as dropped (lwIP is told, and does nothing more about it). */
struct stack_edge {
    status_t (*tx)(void *ctx, const uint8_t *frame, size_t len);
    void    *ctx;
    uint8_t  mac[STACK_MAC_LEN];   /* the device's address: lwIP's source and ARP's */
};

/* The interface's address, all 0 when none is set. */
struct stack_ipv4 {
    uint32_t address;
    uint32_t mask;
    uint32_t gateway;   /* 0: none (only the subnet is reachable) */
};

/* What netctl.info answers about the interface. */
struct stack_state {
    struct stack_ipv4 ip;
    uint8_t mac[STACK_MAC_LEN];
    bool    device;     /* an edge that can send is attached (not stack_no_device) */
    bool    link;       /* the device says the link is up */
};

/* Counts since netstack started (never reset; netctl.stats). */
struct stack_counts {
    uint64_t rx_frames;       /* frames given to stack_input */
    uint64_t rx_refused;      /* of those, refused before lwIP: under 14 or over 1514 bytes, or
                               * no receive buffer free */
    uint64_t tx_frames;       /* frames the edge took */
    uint64_t tx_dropped;      /* frames the edge refused (no device, a full ring) or too long */
    uint64_t rx_bytes;        /* bytes of the frames given to stack_input */
    uint64_t tx_bytes;        /* ... and of those the edge took */
    uint64_t echo_replies;    /* ICMP echo replies sent (pings answered) */
    uint64_t icmp_errors;     /* ICMP errors sent (port or protocol unreachable) */
    uint64_t icmp_limited;    /* ICMP errors not sent: over STACK_ICMP_ERR_PER_S */
    /* lwIP's own counts (lwip_stats), by layer: */
    uint32_t link_dropped;    /* Ethernet: an unknown EtherType, a frame too short for its header */
    uint32_t arp_dropped;     /* ARP: malformed, or not for us */
    uint32_t ip_dropped;      /* IPv4: bad header, bad checksum, a fragment, options, not ours */
    uint32_t icmp_dropped;    /* ICMP: bad checksum or length, a type we don't answer */
    uint32_t udp_dropped;     /* UDP: bad checksum or length; no socket on the port */
    uint32_t bad_checksums;   /* IPv4 headers, ICMP, UDP and TCP together */
    /* lwIP's memory in use now: back to where it was once every frame is
     * dealt with (a leak shows here). */
    uint32_t rx_buffers_used; /* receive buffers, of lwipopts.h's PBUF_POOL_SIZE */
    uint32_t rx_buffers_most; /* ... the most ever in use at once */
    uint32_t rx_buffers_none; /* times a frame found none free (refused) */
    uint32_t heap_used;       /* bytes of its heap, of MEM_SIZE */
};

/* ICMP errors (destination unreachable) are rate-limited, as hosts
 * usually do (RFC 1812 4.3.2.8), so a flood of datagrams to closed ports
 * gets at most this many answers a second (after a burst of as many). */
#define STACK_ICMP_ERR_PER_S 10u

/* Start lwIP (once per process) and add the interface on the edge `e`:
 * administratively up, link down, no address. ERR_BAD_STATE: already
 * started. */
status_t stack_start(const struct stack_edge *e);
/* Remove the interface: address, ARP entries and sockets' route gone.
 * lwIP itself stays initialised (stack_start may follow). */
void     stack_stop(void);
/* The edge of a netstack with no device: refuses every frame. */
extern const struct stack_edge stack_no_device;
/* Swap the edge (a device came, went, or restarted with another MAC):
 * the ARP table is flushed if the MAC changed; the address stays. */
void     stack_set_edge(const struct stack_edge *e);
/* The device says the link went up or down. */
void     stack_set_link(bool up);

/* A received frame (above). Never fails: a frame lwIP doesn't want is
 * dropped and counted. */
void     stack_input(const uint8_t *frame, size_t len);
/* Run lwIP's timers that are due (ARP's ageing); the time (ns, absolute)
 * the next one is due, DEADLINE_NEVER if none is: the loop's deadline. */
uint64_t stack_poll(void);

/* Set the address. The caller checked it (ctl.c's ipv4_valid). */
void     stack_set_ipv4(const struct stack_ipv4 *ip);
/* Remove the address and forget every ARP entry. */
void     stack_clear(void);
void     stack_get(struct stack_state *out);
void     stack_get_counts(struct stack_counts *out);

/* ---- programs' UDP sockets and pings (sock.c, clients.c) ---------------------- */

#define STACK_UDP_MAX 1472u   /* bytes of a datagram: one frame, never fragmented */

/* An lwIP UDP socket, opaque outside stack.c. */
struct stack_udp;

/* Where every socket's datagrams go: ctx is the socket's (stack_udp_open),
 * the bytes are stack.c's (copy them before returning). Called from inside
 * stack_input. */
extern void (*stack_udp_input)(void *ctx, uint32_t from, uint16_t port, const uint8_t *data,
                               size_t len);
/* A UDP socket on `port` (0: lwIP picks one from 49152 up), any local
 * address. bcast: it may send and receive broadcasts (the DHCP socket
 * only; any other gets lwIP's refusal of both). ERR_ALREADY_BOUND: the
 * port is taken; ERR_NO_RESOURCES: lwIP's sockets are all in use;
 * ERR_BAD_STATE: no stack_start. */
status_t stack_udp_open(uint16_t port, bool bcast, void *ctx, struct stack_udp **out,
                        uint16_t *out_port);
void     stack_udp_close(struct stack_udp *u);
/* Send len bytes (at most STACK_UDP_MAX) to to:port. on_link: out of the
 * interface without a route, from its address or 0.0.0.0 while it has
 * none (the DHCP socket's broadcasts). The caller checked `to`.
 * ERR_BAD_STATE: no address, no route or the link down; ERR_NO_MEMORY:
 * lwIP's heap is full; ERR_NO_RESOURCES: the device refused the frame;
 * ERR_INVALID_ARGS: too long, or a broadcast from a socket without bcast. */
status_t stack_udp_send(struct stack_udp *u, uint32_t to, uint16_t port, const void *data,
                        size_t len, bool on_link);

/* Where echo replies go (NULL: lwIP's own handling): from the peer that
 * was pinged, the request's id and seq, the reply's TTL and data bytes; or
 * unreachable (ttl and len 0): an ICMP destination unreachable carrying
 * our echo request to `peer`. Called from inside stack_input; a reply that
 * is not an intact echo reply (or unreachable about an echo) goes on to
 * lwIP, which drops it. */
extern void (*stack_echo_input)(uint32_t peer, uint16_t id, uint16_t seq, uint8_t ttl,
                                size_t len, bool unreachable);
/* An echo request (8 bytes of header, `size` of data) to `to`.
 * ERR_BAD_STATE: no address, no route or the link down; ERR_NO_MEMORY:
 * lwIP's heap is full; ERR_NO_RESOURCES: the device refused the frame. */
status_t stack_echo_send(uint32_t to, uint16_t id, uint16_t seq, size_t size);

/* ---- TCP connections (tcp.c) ---------------------------------------------------
 * stack.c turns lwIP's TCP into plain calls and hooks, so that tcp.c, which
 * moves the bytes between a connection and its socket's rings, never sees
 * a pbuf or a pcb. The model:
 * - received bytes come to hooks.rx in order, straight from the frame,
 *   and the hook copies what it takes (all of it, when the program reads:
 *   see the window). What it doesn't take stays in lwIP (lwIP's "refused
 *   data", offered again on lwIP's next look, at most every 250 ms) and
 *   lwIP takes no more bytes on that connection meanwhile. Segments past
 *   a hole wait in lwIP (its out-of-order queue, within the window and
 *   the shares below; the ACKs say which ranges with SACK blocks when the
 *   peer offered SACK) and come to hooks.rx in order once it is filled;
 * - the window: each connection announces at most the window it was given
 *   (its rx ring), and at most STACK_TCP_WND when the peer scales windows
 *   (RFC 7323: both SYNs carry the option), STACK_TCP_WND_PLAIN when it
 *   doesn't: stack_tcp_window says which, once the handshake has. Less
 *   what was received and not yet given back with stack_tcp_recved. tcp.c
 *   gives bytes back as the program takes them from the rx ring, so the
 *   window is the ring's free room and a slow reader stops its sender at
 *   once, without any byte held in lwIP. A scaled window is counted in
 *   2^6-byte units, rounded down: never more than the room. A connection
 *   netstack opens sends its SYN with lwIP's unscaled 65535 (lwIP sets it
 *   for the scaling option's sake) and announces its own window from the
 *   handshake's ACK on, before any byte can come; a listener's
 *   connections announce theirs from the SYN-ACK on;
 * - bytes to send are copied into lwIP (stack_tcp_send, all or nothing),
 *   at most stack_tcp_room at a time: what the peer's window takes now,
 *   and one segment more (a zero window is probed only while bytes wait),
 *   within the connection's send buffer (stack_tcp_send_buffer) and lwIP's
 *   memory as its shares allow (below);
 * - hooks.gone says a connection is over for lwIP's reasons (reset, timed
 *   out, the address went away, or both sides closed and the last ACK
 *   came): its stack_tcp is freed by then and must not be used again.
 *   A connection netstack lets go of (stack_tcp_release, _abort) gets no
 *   more hooks.
 * Hooks run inside stack_input and stack_poll, and inside stack_set_ipv4,
 * stack_clear and stack_stop (a changed or removed address ends every
 * connection on it: gone, ERR_BAD_STATE); never inside a stack_tcp_* call. */

#define STACK_TCP_MSS       1460u   /* a segment's bytes: one frame */
#define STACK_TCP_WND       (2u << 20)   /* the largest window, when the peer scales too */
#define STACK_TCP_WND_PLAIN (44u * STACK_TCP_MSS)   /* ... when it doesn't (64240 bytes) */
#define STACK_TCP_CONNS     256u    /* connections netstack holds at most (lwIP keeps more pcbs) */
#define STACK_TCP_LISTENERS 16u     /* listening sockets at most */
#define STACK_TCP_BACKLOG   16u     /* a listener's half-open and not-yet-accepted, at most */
/* A connection's send buffer (bytes lwIP keeps until acked): what tcp.c
 * gives it (its tx ring), at least STACK_TCP_SND_MIN and at most
 * STACK_TCP_SND_MAX; its queue at most four times that in segments. */
#define STACK_TCP_SND_MIN   (44u * STACK_TCP_MSS)
#define STACK_TCP_SND_MAX   (2u << 20)
/* lwIP's heap and segments, shared out by stack_tcp_room so that no
 * connection, nor all of them, can take what the rest needs: TCP's bytes
 * never use the last STACK_HEAP_KEEP heap bytes (UDP datagrams, ARP's
 * waiting packets, ICMP, TCP's own ACKs and FINs), and a connection's bytes
 * past its first STACK_TCP_SND_MIN (a bulk sender's) never the last
 * STACK_HEAP_KEEP_BULK bytes beyond those, nor the last STACK_SEGS_KEEP_BULK
 * segments: they are every connection's first 64240 bytes (15 of them at
 * full speed at once, as before window scaling). The bulk senders share
 * the rest. A segment costs about 6% more heap than its bytes (its pbuf
 * and headers): counted. */
#define STACK_HEAP           (6u << 20)   /* lwIP's heap (lwipopts.h MEM_SIZE) */
#define STACK_HEAP_KEEP      (64u * 1024)
#define STACK_HEAP_KEEP_BULK (1u << 20)
#define STACK_SEGS_KEEP_BULK 1024u
/* Segments that come past a hole wait on their connection's out-of-order
 * queue (lwipopts.h TCP_QUEUE_OOSEQ), each in its frame's receive buffer,
 * shared out the same way: the queues never hold the last STACK_RX_KEEP
 * of lwIP's STACK_RX_BUFS buffers (every frame needs one while lwIP looks
 * at it), so together at most STACK_OOSEQ_MAX; and a connection's buffers
 * past its first STACK_OOSEQ_FIRST (a 64 KiB window's segments, a default
 * rx ring's) never the last STACK_OOSEQ_KEEP_BULK beyond those. On top, a
 * connection's queue holds at most its window in bytes and in full
 * segments. lwIP's segment pool has STACK_OOSEQ_MAX more for them, which
 * stack_tcp_room keeps out of the senders' shares. */
#define STACK_RX_BUFS         (128u + 1024u)   /* lwipopts.h PBUF_POOL_SIZE */
#define STACK_RX_KEEP         128u
#define STACK_OOSEQ_MAX       (STACK_RX_BUFS - STACK_RX_KEEP)
#define STACK_OOSEQ_FIRST     45u
#define STACK_OOSEQ_KEEP_BULK 256u
#define STACK_SEGS_SEND       4608u            /* the segment pool less STACK_OOSEQ_MAX */

/* A connection and a listener, opaque outside stack.c. */
struct stack_tcp;
struct stack_tcp_listen;

/* What lwIP tells tcp.c; ctx is the connection's (stack_tcp_connect, or
 * the one `accepted` returned), lctx a listener's. A hook never calls
 * stack_tcp_release or stack_tcp_abort (lwIP is in the middle of that
 * connection): it notes the work for the loop. `connected` and `accepted`
 * set the connection up (stack_tcp_window, stack_tcp_send_buffer). */
struct stack_tcp_hooks {
    /* n (> 0) bytes from the peer, in order: how many it took (0..n). */
    size_t (*rx)(void *ctx, const uint8_t *data, size_t n);
    /* The peer's FIN: no byte after those already taken. */
    void   (*rx_end)(void *ctx);
    /* connect's handshake finished. */
    void   (*connected)(void *ctx);
    /* The peer acked bytes: room to send, and the heap freed. */
    void   (*sent)(void *ctx);
    /* The connection is over and freed (above). why: OK (both sides closed
     * and acked), ERR_PEER_CLOSED (reset by the peer: before connected,
     * the peer refused it), ERR_TIMED_OUT (the peer stopped answering),
     * ERR_BAD_STATE (our address went away). */
    void   (*gone)(void *ctx, status_t why);
    /* A listener's new connection, its handshake done: the connection's
     * ctx, or NULL to refuse it (it is reset). Its window is the
     * listener's. */
    void  *(*accepted)(void *lctx, struct stack_tcp *t);
};

/* Where lwIP's TCP events go (NULL: none; nothing can be opened then). */
void     stack_tcp_set_hooks(const struct stack_tcp_hooks *h);
/* Open a connection to to:port from a port lwIP picks, announcing a window
 * of at most `window` (the largest the peer's answer allows, above): its
 * SYN goes now, hooks.connected or hooks.gone come later. The caller
 * checked `to`. ERR_BAD_STATE: no address, no route or the link down;
 * ERR_NO_RESOURCES: no pcb free or no local port; ERR_NO_MEMORY: lwIP's
 * heap is full. */
status_t stack_tcp_connect(uint32_t to, uint16_t port, uint32_t window, void *ctx,
                           struct stack_tcp **out);
/* Listen on `port` (0: lwIP picks one) on any local address, backlog
 * half-open and not-yet-accepted connections at most (1..STACK_TCP_BACKLOG),
 * each announcing at most `window` (the largest its SYN allows, above).
 * ERR_ALREADY_BOUND: the port is taken; ERR_NO_RESOURCES: no listener free;
 * ERR_INVALID_ARGS: a bad backlog. */
status_t stack_tcp_listen(uint16_t port, uint32_t backlog, uint32_t window, void *lctx,
                          struct stack_tcp_listen **out, uint16_t *out_port);
/* Stop listening. Half-open connections are dropped; those already given
 * to hooks.accepted are the caller's. */
void     stack_tcp_unlisten(struct stack_tcp_listen *l);
/* The program took an accepted connection: it no longer counts against its
 * listener's backlog. */
void     stack_tcp_taken(struct stack_tcp *t);
/* The window a connected one announces at most: the one it was given, and
 * at most STACK_TCP_WND if the peer scales, else STACK_TCP_WND_PLAIN. Known
 * from hooks.connected and hooks.accepted on. */
uint32_t stack_tcp_window(struct stack_tcp *t);
/* Its send buffer: bytes, clamped to STACK_TCP_SND_MIN..STACK_TCP_SND_MAX.
 * From hooks.connected or hooks.accepted, before any byte is given. */
void     stack_tcp_send_buffer(struct stack_tcp *t, uint32_t bytes);
/* Bytes stack_tcp_send would take now (above), 0 when none or after
 * stack_tcp_shutdown. */
size_t   stack_tcp_room(struct stack_tcp *t);
/* Is it 0 for lwIP's memory (its shares, or its queue of segments) while
 * the peer's window would take bytes? Then only an ack anywhere (hooks.sent,
 * hooks.gone) frees some: try again after one. */
bool     stack_tcp_mem_short(struct stack_tcp *t);
/* Has lwIP's heap or segment pool less in use than at the last call? (A
 * connection netstack let go of frees memory with no hook.) */
bool     stack_tcp_mem_freed(void);
/* Copy n bytes (at most stack_tcp_room, at most 65535) into lwIP to send,
 * all or none.
 * ERR_NO_MEMORY: no heap or segment free now (try after hooks.sent);
 * ERR_BAD_STATE: shut down, or not connected yet. */
status_t stack_tcp_send(struct stack_tcp *t, const void *data, size_t n);
/* Send what was given now (segments as the windows allow). */
void     stack_tcp_push(struct stack_tcp *t);
/* The program took n received bytes: give them back to the window. */
void     stack_tcp_recved(struct stack_tcp *t, size_t n);
/* No more bytes to send: a FIN after the last one. ERR_NO_MEMORY: try
 * again later. */
status_t stack_tcp_shutdown(struct stack_tcp *t);
/* Has the peer acked our FIN? */
bool     stack_tcp_fin_acked(struct stack_tcp *t);
/* Let go of it: it closes on its own (a FIN after the bytes given, if not
 * sent yet; the rest of the closing, TIME_WAIT included, is lwIP's). Bytes
 * the program never read: call stack_tcp_abort instead. */
void     stack_tcp_release(struct stack_tcp *t);
/* Reset it now (a RST) and free it. No hook is called. */
void     stack_tcp_abort(struct stack_tcp *t);
/* Its peer's address and port, and its own port. */
void     stack_tcp_ends(struct stack_tcp *t, uint32_t *peer, uint16_t *peer_port,
                        uint16_t *port);

/* lwIP's TCP now: pcbs in use by state, segments queued, and drops (bad
 * checksum or header, no room, out of order or out of window). */
struct stack_tcp_counts {
    uint32_t live;           /* pcbs of connections lwIP is running (any state but these:) */
    uint32_t half_open;      /* ... in SYN_RCVD (a listener's, not accepted yet) */
    uint32_t time_wait;      /* ... in TIME_WAIT */
    uint32_t listeners;      /* listening pcbs */
    uint32_t segs_used;      /* segments queued to send, every connection's */
    uint32_t dropped;        /* segments lwIP dropped */
    uint32_t bad_checksums;  /* ... of them, with a bad checksum */
    uint32_t pcbs_none;      /* times a pcb was wanted and none was free */
    uint32_t bad_acks;       /* segments with bytes and an ACK for bytes never sent or long
                              * acked, dropped before lwIP (RFC 5961 section 5) */
    uint32_t no_acks;        /* segments with no ACK, RST or SYN flag to a connection past
                              * SYN_SENT, dropped before lwIP (RFC 9293 3.10.7.4) */
    uint32_t ooseq_held;     /* receive buffers on out-of-order queues now, every connection's */
    uint32_t ooseq_cut;      /* times a queue went past its limits and lwIP dropped its top */
};
void     stack_tcp_get_counts(struct stack_tcp_counts *out);
/* A frame found the edge full (its tx said ERR_NO_RESOURCES) since the
 * last stack_tx_resume. lwIP keeps a TCP segment it couldn't send and, on
 * its own, sends it again only on its next timer (TCP_TMR_INTERVAL), so the
 * loop waits for room on the card and then calls stack_tx_resume. */
bool     stack_tx_blocked(void);
/* The edge has room again: what lwIP holds for each TCP connection
 * (segments not sent, an ACK owed) goes now, the connections in turn,
 * until the edge is full again (stack_tx_blocked). */
void     stack_tx_resume(void);

/* ---- port/sys_arch.c --------------------------------------------------------- */

/* A line to the log, "netstack: " in front, rate-limited with lwIP's
 * diagnostics (a burst of 20, then 5 a second; what is dropped is counted
 * on the next line that gets through). */
void nstack_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
