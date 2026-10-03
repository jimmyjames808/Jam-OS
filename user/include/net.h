/* The network for programs (user/lib/net.c, netsock.c, nettcp.c): UDP
 * and TCP sockets and ping on netstack's /svc/net (abi/idl/net.idl; the
 * design is docs/M9-PLAN.md "Programs and sockets" and docs/M9.5-PLAN.md;
 * TCP's calls are under "TCP" below). A program's list asks
 * for it with `svc net`, or with `svc net listen` for /svc/net-listen too:
 * the same protocol, but its openers may take a fixed port below
 * NET_PORT_EPHEMERAL (open it with svc_get(SVC_NET_LISTEN) and pass it
 * where these take `net`).
 *
 * A socket's datagrams go through its rings (<sockring.h>: one VMO
 * mapped here, a tx and an rx ring, two events), not through calls: while
 * datagrams flow, a send or a receive is a copy into or out of the ring,
 * and a system call only to wake netstack (or this program) when the
 * other side said it sleeps. The socket's channel carries the few control
 * calls (connect, state) and tells, by closing, that netstack ended.
 *
 * Two ways to use a socket; don't mix them on one socket:
 *   - blocking, for programs and threads that serve nobody: net_sendto
 *     (OK once netstack sent the datagram, or its reason not to),
 *     net_recvfrom (with a deadline), net_ping;
 *   - without waiting, for a service's loop: net_sendto_async puts the
 *     datagram in the ring and returns; net_sock_bind puts the socket on
 *     the loop's port (its to_prog event and its channel's end), and when
 *     the key fires the loop calls net_sock_take until it says
 *     ERR_SHOULD_WAIT (the binding fires on an edge): each datagram, and
 *     the sends netstack refused counted on the way;
 *   - or in a wait set with other sockets and handles (<netwait.h>,
 *     net_sock_waitable), with the calls that don't wait.
 * The rings' `waits` flags (<sockring.h> "Waking") are set only by what
 * sleeps: the blocking calls raise the one they need before they sleep and
 * lower it when they wake; net_sock_bind raises rx's and leaves it up; a
 * wait set raises and lowers its own. net_sendto_async and net_sock_take
 * never touch them. So a blocking call on a socket that is also in a wait
 * set lowers the set's flag: call netwait_touch after it.
 * The opener's channel (net_svc) waits with the generated calls
 * (net_wait_change_send and its _result, <idl/net.h>) in a loop. Addresses
 * are host-order numbers, the first byte highest (<netbytes.h> NET_IPV4,
 * <ipv4.h> for text).
 *
 * Fair shares: netstack serves two shared channels. /svc/net's openers
 * are ordinary programs' and together may hold only NET_PROG_* of its
 * openers, sockets, waits and ring bytes; /svc/net-sys's openers (init
 * gives it to dns, netlog and update alone, and no program from /data may
 * ask for it) may use the rest, so a program that takes its whole share
 * can't stop the network's own services. net_svc opens /svc/net-sys when
 * the namespace has it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>
#include <sockring.h>

#define NET_DGRAM_MAX          1472u   /* bytes of a datagram: one frame, never fragmented */
#define NET_OPENERS            32u     /* channels from the shared channels' connect at once */
#define NET_PROG_OPENERS       24u     /* ... of them /svc/net's (ordinary programs') */
#define NET_SOCKETS_PER_OPENER 16u     /* sockets one opener may hold */
#define NET_SOCKETS_MAX        48u     /* sockets of all openers together */
#define NET_PROG_SOCKETS       24u     /* ... of them ordinary openers' */
#define NET_LATER_PER_OPENER   8u      /* waits, echoes and chip_counts in flight an opener */
#define NET_LATER_MAX          64u     /* ... of all openers together */
#define NET_PROG_LATER         48u     /* ... of them ordinary openers' */
#define NET_PROG_RING_BYTES    (16u << 20)  /* ring bytes of ordinary openers' sockets */
#define NET_WAIT_FOREVER       0xffffffffu   /* a timeout_ms that never passes */
#define NET_ECHO_TIMEOUT_MAX   60000u  /* ms: the longest echo timeout */
#define NET_PORT_LOW           1024u   /* udp: ports below are refused; up to ... */
#define NET_PORT_EPHEMERAL     49152u  /* ... here only on /svc/net-listen; port 0: from here */
#define NET_PORT_DHCP_SERVER   67u
#define NET_PORT_DHCP_CLIENT   68u
/* TCP (net.idl's tcp, tcp_listener, accept): connections (a listener's
 * waiting ones count against its opener), listeners and the listeners'
 * backlogs, each limited per opener, in all, and for ordinary openers
 * together (the rest is the network services' reserve). */
#define NET_TCP_MAX              256u   /* connections of all openers */
#define NET_PROG_TCP             192u   /* ... of them ordinary openers' */
#define NET_TCP_PER_OPENER       64u    /* ... one opener's */
#define NET_LISTENERS_MAX        16u    /* listeners of all openers */
#define NET_PROG_LISTENERS       12u    /* ... of them ordinary openers' */
#define NET_LISTENERS_PER_OPENER 4u     /* ... one opener's */
#define NET_BACKLOG_MAX          16u    /* a listener's half-open and waiting connections */
#define NET_BACKLOG_TOTAL        128u   /* every listener's backlog together */
#define NET_PROG_BACKLOG         96u    /* ... ordinary openers' listeners' */
#define NET_TCP_TX               (16u * 1024)   /* a connection's rings unless it asks: tx ... */
#define NET_TCP_RX               (64u * 1024)   /* ... and rx (64 KiB: an unscaled window's) */
/* The rings a bulk transfer asks for (fetch, serve, speed): a whole scaled
 * window to receive (2 MiB: 1 Gb/s at 16 ms), and as much in flight when
 * sending (a connection keeps at most its tx ring unacked, at least 64240
 * bytes). A connection with both counts 4 MiB against its opener's
 * SOCKRING_OPENER_BYTES; a listener's connection that doesn't fit with its
 * listener's tx ring gets NET_TCP_TX instead (the rx ring is as asked). */
#define NET_TCP_BULK             (2u << 20)
_Static_assert(NET_DGRAM_MAX == SOCKRING_DGRAM_MAX, "one datagram size");

/* net_info.vlan on an untagged network: <jam/netframe.h>'s
 * NETFRAME_MODE_UNTAGGED, the driver's (netdev.idl's info), which netstack
 * passes on as it is. */
#define NET_VLAN_UNTAGGED 0x1000u

/* The interface (net.iface). */
struct net_info {
    uint32_t address, mask, gateway;   /* 0: none */
    uint32_t dns[2];                   /* the DNS servers, 0: none */
    uint8_t  mac[6];
    bool     device;                   /* a card's driver has a session with netstack */
    bool     link;                     /* its link is up */
    uint16_t vlan;                     /* the VLAN every frame is tagged with (0: no card;
                                        * NET_VLAN_UNTAGGED: none, untagged frames only) */
    uint32_t speed;                    /* Mb/s, 0 while down */
    uint32_t version;                  /* changes to the address and DNS servers (wait_change) */
};

/* netstack's counts (net.counts' u8[256], little-endian): since it started. */
struct net_counters {
    /* frames, and what lwIP did with them */
    uint64_t rx_frames;        /* frames from the card */
    uint64_t rx_refused;       /* of those, refused before lwIP (bad length, no buffer) */
    uint64_t tx_frames;        /* frames the card's ring took */
    uint64_t tx_dropped;       /* frames not sent (no card, the ring full, the link down) */
    uint64_t echo_replies;     /* pings answered */
    uint64_t icmp_errors;      /* ICMP unreachables sent */
    uint64_t icmp_limited;     /* ICMP unreachables held back by the rate limit */
    uint32_t link_dropped;     /* lwIP's drops, by layer */
    uint32_t arp_dropped;
    uint32_t ip_dropped;
    uint32_t icmp_dropped;
    uint32_t udp_dropped;      /* bad UDP, or no socket on the port */
    uint32_t bad_checksums;
    uint32_t rx_buffers_used;  /* lwIP's receive buffers in use now */
    uint32_t heap_used;        /* lwIP's heap bytes in use now */
    /* the card's session */
    uint64_t sessions;         /* sessions opened with its driver */
    uint64_t ring_errors;      /* the driver's ring counts out of range */
    uint64_t rx_bad;           /* rx slots refused (a bad length or flags) */
    uint64_t tx_full;          /* frames dropped: the tx ring was full */
    /* programs */
    uint32_t openers;          /* openers now (both shared channels') */
    uint32_t sockets;          /* sockets open now (the DHCP socket included) */
    uint32_t queued;           /* bytes waiting in sockets' rx rings now */
    uint32_t later;            /* waits, echoes and chip_counts in flight now */
    uint64_t dgrams_in;        /* datagrams put in a socket's rx ring */
    uint64_t dgrams_dropped;   /* datagrams dropped: a socket's rx ring was full (or it had none) */
    uint64_t dgrams_out;       /* datagrams sent */
    uint64_t echoes_sent;      /* echo requests sent for programs (ping) */
    uint64_t echoes_answered;  /* of those, answered */
    uint64_t dgrams_refused;   /* tx records refused (an address, port 0, no route, a bad record) */
    uint32_t ring_bytes;       /* sockets' ring bytes now (sockring_bytes), every opener's */
    uint32_t refused_shares;   /* opens and requests refused for an ordinary opener's share */
    /* TCP */
    uint32_t tcp_conns;        /* connections now (a listener's waiting ones too) */
    uint32_t tcp_listeners;    /* listeners now */
    uint64_t tcp_bytes_in;     /* bytes put in connections' rx rings */
    uint64_t tcp_bytes_out;    /* bytes taken from connections' tx rings into lwIP */
    uint32_t tcp_refused;      /* connections a listener reset: no room for them under the limits */
    uint32_t tcp_dropped;      /* segments lwIP dropped (malformed, out of the window, no room) */
    uint64_t reserved[4];      /* 0 */
};
#define NET_COUNTERS_SIZE 256u

struct idl_msg;        /* <idl/common.h>: a reply read off a channel */
struct netwait_sock;   /* <netwait.h>: a socket as a wait set takes it */
struct netwait_handle; /* <netwait.h>: a handle as a wait set takes it */

/* A datagram received. */
struct net_dgram {
    uint32_t addr;                 /* who sent it */
    uint16_t port;                 /* from its port */
    uint16_t len;                  /* bytes in data (the rest of data is 0) */
    uint32_t dropped;              /* the socket's dropped datagrams so far (its rx ring full) */
    uint8_t  data[NET_DGRAM_MAX];
};

/* A socket, the caller's: its channel, and its rings mapped here. */
struct net_sock {
    handle_t        ch;            /* the socket's channel (closing it closes the socket) */
    uint16_t        port;          /* its local port */
    handle_t        ring;          /* the rings' VMO (SOCKRING_VMO_RIGHTS); 0: no rings */
    handle_t        to_stack;      /* netstack's event: signal only */
    handle_t        to_prog;       /* ours: wait, and signal to clear */
    uint8_t        *map;           /* the VMO, mapped (map_len bytes) */
    uint64_t        map_len;
    struct sockring r;             /* our ends of its two rings */
    handle_t        waiter;        /* a port for the blocking forms (0: not made yet) */
    handle_t        bound_port;    /* net_sock_bind's port and key (0: none) */
    uint64_t        bound_key;
    uint64_t        refused_seen;  /* the status line's tx_refused already counted */
    uint32_t        send_errors;   /* sends netstack refused, as net_sock_take saw them */
    status_t        last_error;    /* ... the last one's reason */
};

/* The network's shared channel: /svc/net-sys when the namespace has it
 * (the network's own services), else /svc/net. libos keeps it (svc_get:
 * opened again after a netstack restart); HANDLE_INVALID when the
 * program's list didn't ask for it (or netstack isn't running). Don't
 * close it. */
handle_t net_svc(void);
/* A channel of the caller's own to the same (svc_open: the caller closes
 * it), for a loop that binds it on its port. ERR_NOT_FOUND: neither name
 * is in the namespace. */
status_t net_svc_open(handle_t *out);
/* The interface now. Errors: the call's (ERR_PEER_CLOSED: no netstack). */
status_t net_info(handle_t net, struct net_info *out);
/* Wait until the interface has an address (at once if it has), or the
 * deadline: ERR_TIMED_OUT. *out (may be NULL): the interface then. */
status_t net_wait_up(handle_t net, uint64_t deadline, struct net_info *out);
/* netstack's counts; the card's own (struct netdev_stats, <jam/netdev.h>:
 * NETDEV_STATS_SIZE bytes into out). */
status_t net_get_counters(handle_t net, struct net_counters *out);
status_t net_get_chip_counters(handle_t net, void *out);

/* A UDP socket on port (0: netstack picks one; 1..1023 refused;
 * 1024..49151 only on an opener of /svc/net-listen: ERR_ACCESS_DENIED)
 * with its rings (net.udp_rings, the default sizes), mapped. Errors: the
 * call's, the map's. */
status_t net_udp_open(handle_t net, uint16_t port, struct net_sock *out);
/* The same with rings of tx_bytes and rx_bytes (0: the default; else a
 * power of two in SOCKRING_MIN..SOCKRING_MAX): a socket that must hold more
 * datagrams than SOCKRING_UDP_RX does, or fewer. */
status_t net_udp_open_rings(handle_t net, uint16_t port, uint32_t tx_bytes, uint32_t rx_bytes,
                            struct net_sock *out);
/* The same without waiting, for a service's loop: send the request with
 * the caller's txid (not 0); when the reply comes on net (idl_reply_read),
 * net_udp_opened takes the socket from it. */
status_t net_udp_open_async(handle_t net, uint32_t txid, uint16_t port);
status_t net_udp_opened(const void *rep, struct idl_msg *m, struct net_sock *out);
/* A socket whose channel came from elsewhere (netctl's dhcp_open), on
 * port: asks for its rings (net.sock_rings, blocking) and maps them. On a
 * failure the channel is closed and *s is empty. */
status_t net_sock_adopt(struct net_sock *s, handle_t ch, uint16_t port);
/* Unbind it (net_sock_bind), unmap its rings and close everything
 * (nothing to do if s->ch is 0). */
void     net_close(struct net_sock *s);
/* Only datagrams from addr:port from now on, and net_send goes there (0, 0:
 * anyone again). */
status_t net_connect(struct net_sock *s, uint32_t addr, uint16_t port);

/* ---- blocking ---- */
/* len bytes (at most NET_DGRAM_MAX) to addr:port, into the tx ring (waiting
 * for room), then waiting until netstack took it: OK when it went to the
 * card (or waits for the peer's ARP answer), else the reason netstack
 * refused it (ERR_INVALID_ARGS: an address a program can't send to, or
 * port 0; ERR_BAD_STATE: no address, the link down, no route; ...).
 * ERR_TIMED_OUT: not taken within a few seconds (the card's ring full all
 * that time: the datagram stays in the ring); ERR_PEER_CLOSED: netstack is
 * gone. */
status_t net_sendto(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                    size_t len);
/* To the peer net_connect set. */
status_t net_send(struct net_sock *s, const void *data, size_t len);
/* The next datagram, waiting until the deadline (DEADLINE_NEVER: no limit;
 * a deadline already past: ERR_SHOULD_WAIT if none is waiting) for one:
 * ERR_TIMED_OUT then; ERR_PEER_CLOSED: netstack is gone. */
status_t net_recvfrom(struct net_sock *s, struct net_dgram *d, uint64_t deadline);
/* One ping: an echo request of size data bytes, sequence seq, answered by
 * the deadline (at most NET_ECHO_TIMEOUT_MAX ms from now). *rtt_us and
 * *ttl (may be NULL): the round trip and the reply's TTL. ERR_TIMED_OUT:
 * no answer; ERR_NOT_FOUND: unreachable, said a router; ERR_BAD_STATE: no
 * address or no link. */
status_t net_ping(handle_t net, uint32_t addr, uint16_t seq, uint16_t size, uint64_t deadline,
                  uint32_t *rtt_us, uint8_t *ttl);
/* Wait until s may have a datagram, or netstack is gone (a look says
 * which), or the deadline (ERR_TIMED_OUT). It may also return early (a
 * change of the socket's status, room in its tx ring): look, and wait
 * again. */
status_t net_sock_wait(struct net_sock *s, uint64_t deadline);

/* ---- TCP ---- */
/* A TCP connection is a net_sock whose rings carry bytes (<sockring.h>'s
 * byte-stream framing): net_read and net_write move them, net_shutdown
 * ends this side's direction (a FIN after the last byte), and net_close
 * closes it (a reset if bytes it never read are left). Its window is its
 * rx ring's free room: a program that reads slowly stops its sender, and
 * nobody else. net_sock_waitable puts it in a wait set (READ: bytes or the
 * peer's end; WRITE: room; HUP and ERROR: CLOSED, and why). Its status
 * line (net_tcp_status) says CONNECTING, OPEN or CLOSED and the error. */

/* Open a connection to addr:port (net.tcp), rings of tx_bytes and rx_bytes
 * (0: NET_TCP_TX, NET_TCP_RX), without waiting for the handshake: the
 * socket is CONNECTING. net.tcp's errors. */
status_t net_tcp_open(handle_t net, uint32_t addr, uint16_t port, uint32_t tx_bytes,
                      uint32_t rx_bytes, struct net_sock *out);
/* Wait until it is OPEN (OK), CLOSED (its error: ERR_NOT_FOUND refused,
 * ERR_TIMED_OUT no answer, ...) or the deadline (ERR_TIMED_OUT, still
 * CONNECTING). */
status_t net_tcp_wait_open(struct net_sock *s, uint64_t deadline);
/* Wait until it is CLOSED: OK when both directions ended and our FIN was
 * acked (every byte we sent arrived: what a program that must know waits
 * for after net_shutdown and reading to the end), else why it closed;
 * ERR_TIMED_OUT at the deadline. */
status_t net_tcp_wait_closed(struct net_sock *s, uint64_t deadline);
/* net_tcp_open with the default rings, then net_tcp_wait_open: on a
 * failure the socket is closed again and *out empty. */
status_t net_tcp_connect(handle_t net, uint32_t addr, uint16_t port, uint64_t deadline,
                         struct net_sock *out);
/* The connection's status line now: state (SOCKRING_STATE_*) and error. */
void     net_tcp_status(const struct net_sock *s, uint32_t *state, status_t *error);

/* Up to len bytes into the tx ring, as many as there is room for now
 * (netstack woken if it sleeps): how many. 0 with no room, after
 * net_shutdown, or once CLOSED. */
size_t   net_write_some(struct net_sock *s, const void *data, size_t len);
/* All len bytes, waiting for room: OK once they are all in the ring (sent
 * as the peer's window allows). *out_n (may be NULL): how many went in.
 * ERR_TIMED_OUT at the deadline; the connection's error once it is CLOSED
 * (ERR_PEER_CLOSED if it closed with none); ERR_BAD_STATE after
 * net_shutdown; ERR_PEER_CLOSED: netstack is gone. */
status_t net_write(struct net_sock *s, const void *data, size_t len, uint64_t deadline,
                   size_t *out_n);
/* Up to cap bytes out of the rx ring now: how many (0: none yet, or the
 * peer's end: net_read says which). */
size_t   net_read_some(struct net_sock *s, void *buf, size_t cap);
/* At least one byte (up to cap), waiting for it: OK with *out_n > 0; OK
 * with *out_n 0: the peer ended its direction (no byte will come). The
 * connection's error once it is CLOSED with nothing left to read;
 * ERR_TIMED_OUT at the deadline (a deadline already past: ERR_SHOULD_WAIT
 * when nothing is there); ERR_PEER_CLOSED: netstack is gone. */
status_t net_read(struct net_sock *s, void *buf, size_t cap, uint64_t deadline, size_t *out_n);
/* No more bytes from this side: SOCKRING_END on the tx ring, a FIN after
 * the last byte. Reading goes on. ERR_BAD_STATE: no rings. */
status_t net_shutdown(struct net_sock *s);

/* A listener (net.tcp_listener): its channel (closing it stops listening)
 * and its port. */
struct net_listener {
    handle_t ch;          /* the listener's channel */
    uint16_t port;        /* its port */
    bool     asked;       /* an accept was sent and its answer not taken yet */
    uint32_t txid;        /* ... its txid */
    uint32_t last_txid;   /* the counter for those (idl_txid_next) */
};

/* Listen on port (1024 and up, 0: netstack picks one) for at most backlog
 * (1..NET_BACKLOG_MAX) connections half-open or waiting, each with rings of
 * tx_bytes and rx_bytes (0: NET_TCP_TX, NET_TCP_RX). net is an opener of
 * /svc/net-listen (svc_get(SVC_NET_LISTEN): the program's list says `svc
 * net listen`); on any other ERR_ACCESS_DENIED. net.tcp_listener's errors. */
status_t net_tcp_listen(handle_t net, uint16_t port, uint32_t backlog, uint32_t tx_bytes,
                        uint32_t rx_bytes, struct net_listener *out);
/* The next connection, waiting for one until the deadline (ERR_TIMED_OUT;
 * a deadline already past: ERR_SHOULD_WAIT when none is waiting): the
 * socket into *out, its peer's address and port into *peer and *peer_port
 * (may be NULL). ERR_PEER_CLOSED: netstack is gone. */
status_t net_tcp_accept(struct net_listener *l, uint64_t deadline, struct net_sock *out,
                        uint32_t *peer, uint16_t *peer_port);
/* Without waiting (a loop or a wait set): ask for the next connection (once
 * until its answer is taken), and take the answer once l's channel is
 * readable (net_listener_waitable): ERR_SHOULD_WAIT: not there yet. A
 * failed answer is returned and the next take asks again. */
status_t net_tcp_accept_send(struct net_listener *l);
status_t net_tcp_accept_take(struct net_listener *l, struct net_sock *out, uint32_t *peer,
                             uint16_t *peer_port);
/* l as a wait set takes it (netwait_add_handle): READ when an answer to
 * net_tcp_accept_send is there, HUP when netstack is gone. */
void     net_listener_waitable(const struct net_listener *l, struct netwait_handle *out);
/* Stop listening: the connections waiting are reset (nothing to do if
 * l->ch is 0). */
void     net_listener_close(struct net_listener *l);

/* ---- without waiting (a service's loop) ---- */
/* Put a datagram in the tx ring (netstack woken if it sleeps): OK. A
 * refusal of netstack's comes later (net_sock_take counts it).
 * ERR_SHOULD_WAIT: the ring has no room now (nothing says when it has but a
 * wait set's NETWAIT_WRITE: a loop counts it lost, or tries again later);
 * ERR_INVALID_ARGS: len over NET_DGRAM_MAX; ERR_BAD_STATE: no rings. */
status_t net_sendto_async(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                          size_t len);
/* Watch s on the loop's port with `key`: its to_prog event and its
 * channel's end (SIG_PEER_CLOSED), and rx's consumer flag raised for good,
 * so a datagram netstack puts in the ring fires the key (one already there
 * fires it at once). flags: PORT_BIND_PERSISTENT, or PORT_BIND_ONCE to bind
 * again each turn. One port at a time; net_close unbinds. Not with a wait
 * set or the blocking calls on the same socket (they lower the flag). */
status_t net_sock_bind(struct net_sock *s, handle_t port, uint64_t key, uint32_t flags);
void     net_sock_unbind(struct net_sock *s);
/* s as a wait set takes it (netwait_add_sock): its rings, to_prog and
 * channel; tx_need 0. Valid while s is open. */
void     net_sock_waitable(struct net_sock *s, struct netwait_sock *out);
/* The next datagram into *d: OK. ERR_SHOULD_WAIT: none now (the socket is
 * set to wake the loop when one comes); ERR_PEER_CLOSED: netstack is gone
 * (open the socket again). Records netstack refused since the last call
 * are added to s->send_errors (s->last_error their reason); a broken record
 * from netstack is skipped. */
status_t net_sock_take(struct net_sock *s, struct net_dgram *d);
