/* The network for programs (user/lib/net.c): UDP sockets and ping on
 * netstack's /svc/net (abi/idl/net.idl; the design is docs/M9-PLAN.md
 * "Programs and sockets"). A program's list asks for it with `svc net`.
 *
 * Each datagram is one call on the socket's channel: netstack never waits
 * inside one (a sock_recv with nothing queued is answered when a datagram
 * comes or its timeout passes), and M9's traffic (DNS, DHCP, ping, the
 * log, an update's fetch) is a few thousand datagrams at most.
 *
 * Two ways to use a socket; don't mix them on one socket:
 *   - blocking, for programs and threads that serve nobody: net_sendto,
 *     net_recvfrom (with a deadline), net_ping;
 *   - without waiting, for a service's loop: net_sendto_async and
 *     net_recv_arm write requests and return; bind the socket's channel
 *     (s->ch) on the loop's port for SIG_READABLE | SIG_PEER_CLOSED and
 *     call net_sock_take when it fires, until it says ERR_SHOULD_WAIT
 *     (the binding fires on an edge): it reads the replies, counts the
 *     sends that failed, and hands over each datagram.
 * The opener's channel (net_svc) waits with the generated calls
 * (net_wait_change_send and its _result, <idl/net.h>) in a loop. Addresses
 * are host-order numbers, the first byte highest (<netbytes.h> NET_IPV4,
 * <ipv4.h> for text). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define NET_DGRAM_MAX          1472u   /* bytes of a datagram: one frame, never fragmented */
#define NET_OPENERS            32u     /* channels from /svc/net's connect at once */
#define NET_SOCKETS_PER_OPENER 16u     /* sockets one opener may hold */
#define NET_SOCKETS_MAX        32u     /* sockets of all openers together */
#define NET_RX_QUEUE           32u     /* datagrams queued a socket; more are dropped */
#define NET_LATER_PER_OPENER   8u      /* waits, echoes and chip_counts in flight an opener */
#define NET_WAIT_FOREVER       0xffffffffu   /* a timeout_ms that never passes */
#define NET_ECHO_TIMEOUT_MAX   60000u  /* ms: the longest echo timeout */
#define NET_PORT_LOW           1024u   /* udp: ports below are refused */
#define NET_PORT_EPHEMERAL     49152u  /* udp port 0: one from here up */
#define NET_PORT_DHCP_SERVER   67u
#define NET_PORT_DHCP_CLIENT   68u

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
    uint32_t openers;          /* /svc/net openers now */
    uint32_t sockets;          /* sockets open now (the DHCP socket included) */
    uint32_t queued;           /* datagrams queued on sockets now */
    uint32_t later;            /* waits, echoes and chip_counts in flight now */
    uint64_t dgrams_in;        /* datagrams queued for a socket */
    uint64_t dgrams_dropped;   /* datagrams dropped: a socket's queue was full */
    uint64_t dgrams_out;       /* datagrams sent */
    uint64_t echoes_sent;      /* echo requests sent for programs (ping) */
    uint64_t echoes_answered;  /* of those, answered */
    uint64_t reserved[10];     /* 0 */
};
#define NET_COUNTERS_SIZE 256u

/* A datagram received. */
struct net_dgram {
    uint32_t addr;                 /* who sent it */
    uint16_t port;                 /* from its port */
    uint16_t len;                  /* bytes in data */
    uint32_t dropped;              /* the socket's dropped datagrams so far (queue full) */
    uint8_t  data[NET_DGRAM_MAX];
};

/* A socket, the caller's. */
struct net_sock {
    handle_t ch;           /* the socket's channel (closing it closes the socket) */
    uint16_t port;         /* its local port */
    /* the forms that don't wait (net_sock_take) */
    uint32_t last_txid;    /* idl_txid_next's counter for this channel */
    uint32_t recv_txid;    /* the sock_recv in flight (0: none) */
    uint32_t sends;        /* net_sendto_async calls not answered yet */
    uint32_t send_errors;  /* of the answered, how many failed */
    status_t last_error;   /* the last failure's status */
};

/* /svc/net: libos's own channel to it (svc_get: opened again after a
 * netstack restart), or HANDLE_INVALID when the program's list didn't ask
 * for it (or netstack isn't running). Don't close it. */
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

/* A UDP socket on port (0: netstack picks one; 1..1023 refused). */
status_t net_udp_open(handle_t net, uint16_t port, struct net_sock *out);
/* A socket whose channel came from elsewhere (netctl's dhcp_open), on port. */
void     net_sock_adopt(struct net_sock *s, handle_t ch, uint16_t port);
/* Close it (nothing to do if s->ch is 0). */
void     net_close(struct net_sock *s);
/* Only datagrams from addr:port from now on, and net_send goes there (0, 0:
 * anyone again). */
status_t net_connect(struct net_sock *s, uint32_t addr, uint16_t port);

/* ---- blocking ---- */
/* len bytes (at most NET_DGRAM_MAX) to addr:port; OK when netstack sent it. */
status_t net_sendto(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                    size_t len);
/* To the peer net_connect set. */
status_t net_send(struct net_sock *s, const void *data, size_t len);
/* The next datagram, waiting until the deadline (DEADLINE_NEVER: no limit;
 * a deadline already past: ERR_SHOULD_WAIT if none is queued) for one:
 * ERR_TIMED_OUT then. */
status_t net_recvfrom(struct net_sock *s, struct net_dgram *d, uint64_t deadline);
/* One ping: an echo request of size data bytes, sequence seq, answered by
 * the deadline (at most NET_ECHO_TIMEOUT_MAX ms from now). *rtt_us and
 * *ttl (may be NULL): the round trip and the reply's TTL. ERR_TIMED_OUT:
 * no answer; ERR_NOT_FOUND: unreachable, said a router; ERR_BAD_STATE: no
 * address or no link. */
status_t net_ping(handle_t net, uint32_t addr, uint16_t seq, uint16_t size, uint64_t deadline,
                  uint32_t *rtt_us, uint8_t *ttl);

/* ---- without waiting (a service's loop) ---- */
/* Write a send request: its answer comes on s->ch (net_sock_take counts a
 * failure). ERR_SHOULD_WAIT: the channel's queue is full; try later. */
status_t net_sendto_async(struct net_sock *s, uint32_t addr, uint16_t port, const void *data,
                          size_t len);
/* Make sure a sock_recv (no timeout) is in flight. */
status_t net_recv_arm(struct net_sock *s);
/* Read s->ch: send answers are counted, and the first datagram is put in
 * *d (the next sock_recv is sent at once): OK. ERR_SHOULD_WAIT: no
 * datagram yet; ERR_PEER_CLOSED: netstack is gone (open the socket again);
 * another error: the socket's sock_recv failed (it is not sent again). */
status_t net_sock_take(struct net_sock *s, struct net_dgram *d);
