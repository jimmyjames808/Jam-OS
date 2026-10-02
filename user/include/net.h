/* The network for programs (user/lib/net.c): UDP sockets and ping on
 * netstack's /svc/net (abi/idl/net.idl; the design is docs/M9-PLAN.md
 * "Programs and sockets" and docs/M9.5-PLAN.md). A program's list asks
 * for it with `svc net`.
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
 *     the sends netstack refused counted on the way.
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
#define NET_PROG_RING_BYTES    (8u << 20)   /* ring bytes of ordinary openers' sockets */
#define NET_WAIT_FOREVER       0xffffffffu   /* a timeout_ms that never passes */
#define NET_ECHO_TIMEOUT_MAX   60000u  /* ms: the longest echo timeout */
#define NET_PORT_LOW           1024u   /* udp: ports below are refused */
#define NET_PORT_EPHEMERAL     49152u  /* udp port 0: one from here up */
#define NET_PORT_DHCP_SERVER   67u
#define NET_PORT_DHCP_CLIENT   68u
_Static_assert(NET_DGRAM_MAX == SOCKRING_DGRAM_MAX, "one datagram size");

/* The interface (net.iface). */
struct net_info {
    uint32_t address, mask, gateway;   /* 0: none */
    uint32_t dns[2];                   /* the DNS servers, 0: none */
    uint8_t  mac[6];
    bool     device;                   /* a card's driver has a session with netstack */
    bool     link;                     /* its link is up */
    uint16_t vlan;                     /* the VLAN every frame is tagged with (0: no card) */
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
    uint64_t reserved[8];      /* 0 */
};
#define NET_COUNTERS_SIZE 256u

struct idl_msg;   /* <idl/common.h>: a reply read off a channel */

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
/* The interface now. Errors: the call's (ERR_PEER_CLOSED: no netstack). */
status_t net_info(handle_t net, struct net_info *out);
/* Wait until the interface has an address (at once if it has), or the
 * deadline: ERR_TIMED_OUT. *out (may be NULL): the interface then. */
status_t net_wait_up(handle_t net, uint64_t deadline, struct net_info *out);
/* netstack's counts; the card's own (struct netdev_stats, <jam/netdev.h>:
 * NETDEV_STATS_SIZE bytes into out). */
status_t net_get_counters(handle_t net, struct net_counters *out);
status_t net_get_chip_counters(handle_t net, void *out);

/* A UDP socket on port (0: netstack picks one; 1..1023 refused) with its
 * rings (net.udp_rings, the default sizes), mapped. Errors: the call's,
 * the map's. */
status_t net_udp_open(handle_t net, uint16_t port, struct net_sock *out);
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
/* Wait until s may have something: a datagram, room in the tx ring, a
 * change of its status, netstack gone; or the deadline (ERR_TIMED_OUT). It
 * may also return early: look, and wait again. */
status_t net_sock_wait(struct net_sock *s, uint64_t deadline);

/* ---- without waiting (a service's loop) ---- */
/* Put a datagram in the tx ring (netstack woken if it sleeps): OK. A
 * refusal of netstack's comes later (net_sock_take counts it).
 * ERR_SHOULD_WAIT: the ring has no room now (the loop's key fires when it
 * has); ERR_INVALID_ARGS: len over NET_DGRAM_MAX; ERR_BAD_STATE: no rings. */
status_t net_sendto_async(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                          size_t len);
/* Watch s on the loop's port with `key`: its to_prog event (PERSISTENT)
 * and its channel's end (SIG_PEER_CLOSED). flags: PORT_BIND_PERSISTENT, or
 * PORT_BIND_ONCE to bind again each turn (net_sock_unbind first). One port
 * at a time; net_close unbinds. */
status_t net_sock_bind(struct net_sock *s, handle_t port, uint64_t key, uint32_t flags);
void     net_sock_unbind(struct net_sock *s);
/* The next datagram into *d: OK. ERR_SHOULD_WAIT: none now (the socket is
 * set to wake the loop when one comes); ERR_PEER_CLOSED: netstack is gone
 * (open the socket again). Records netstack refused since the last call
 * are added to s->send_errors (s->last_error their reason); a broken record
 * from netstack is skipped. */
status_t net_sock_take(struct net_sock *s, struct net_dgram *d);
