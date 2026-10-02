/* netstack's side for programs: /svc/net (abi/idl/net.idl, <net.h>).
 *
 * Three kinds of channel, all on the loop's port, each bound PERSISTENT
 * and served PROGS_BUDGET requests a turn with a flag saying more may be
 * queued (a binding fires on edges only):
 * - the shared channel (startup role SR_USER + 1; init keeps its server
 *   end across restarts and publishes the client end as /svc/net): it
 *   answers svc.connect with an opener's channel, and iface and counts;
 * - an opener's channel (clients.c): iface, counts, wait_change,
 *   chip_counts, udp, echo. What it starts is its own: its sockets
 *   (closed with it), its requests in flight (dropped with it) and its
 *   ICMP echo id;
 * - a socket's channel (sock.c): the sock_* methods; closing it closes
 *   the socket. netctl's dhcp_open makes the one socket that belongs to
 *   no opener.
 *
 * Nothing here waits (ARCHITECTURE.md "How a service waits"): a request
 * that can't be answered at once (a sock_recv with nothing queued, an
 * echo, a wait_change, chip_counts) is kept, in the socket or in the
 * table of requests in flight, and answered when its answer comes or its
 * deadline passes (progs_tick gives the loop the next one). A datagram
 * for a socket is answered to its waiting sock_recv or queued (at most
 * NET_RX_QUEUE, copied into netstack's own memory, so lwIP's receive
 * buffers are never held by a slow reader); a full queue drops it and
 * counts it.
 *
 * Every slot carries a generation, in its port key (bits 8 and up) and in
 * what refers to it, so a packet or a datagram left from a closed slot is
 * never taken for the slot's next user. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <idl/net.h>
#include <net.h>
#include "dev.h"
#include "stack.h"

/* Port keys (main.c's are below 0x10, dev.h's 0x10 to 0x21). The low byte
 * names the slot, the rest its generation. */
#define KEY_OPENER    0x40u   /* + the opener's slot */
#define KEY_SOCK      0x80u   /* + the socket's slot */
#define SOCK_SLOTS    (NET_SOCKETS_MAX + 1)   /* the DHCP socket has one of its own */
#define LATER_MAX     64u     /* requests in flight, every opener's together */
#define PROGS_BUDGET  8u      /* requests taken off one channel a turn */
#define CHIP_WAIT     (3 * NS_PER_S)   /* chip_counts: the driver's answer, at most */

_Static_assert(KEY_OPENER + NET_OPENERS <= KEY_SOCK, "opener keys");
_Static_assert(KEY_SOCK + SOCK_SLOTS <= 0x100, "socket keys fit the low byte");

struct opener {
    handle_t ch;        /* our end of its channel; 0: the slot is free */
    uint32_t gen;       /* the slot's generation */
    bool     pending;   /* requests may be queued */
    uint16_t echo_id;   /* its ICMP echo id: the slot in the low 5 bits, random above */
    unsigned socks;     /* sockets it holds */
    unsigned later;     /* its requests in flight */
};

/* A datagram queued for a socket (malloc'd, its bytes after it). */
struct dgram {
    uint32_t addr;
    uint16_t port;
    uint16_t len;
    uint8_t  data[];
};

struct sock {
    handle_t           ch;          /* our end of its channel; 0: the slot is free */
    uint32_t           gen;         /* the slot's generation */
    bool               pending;     /* requests may be queued */
    bool               dhcp;        /* netctl's DHCP socket (belongs to no opener) */
    unsigned           opener;      /* the opener that made it, and that opener's ... */
    uint32_t           opener_gen;  /* ... generation then */
    struct stack_udp  *u;           /* lwIP's socket */
    uint16_t           port;        /* its local port */
    uint32_t           peer;        /* sock_connect's peer (0: anyone) */
    uint16_t           peer_port;
    struct dgram      *q[NET_RX_QUEUE];   /* datagrams queued, oldest at head */
    unsigned           head, n;
    uint32_t           dropped;     /* datagrams dropped: the queue was full */
    bool               waiting;     /* a sock_recv waits (txn, deadline) */
    struct idl_txn     txn;
    uint64_t           deadline;    /* ns; DEADLINE_NEVER: no timeout */
};

enum later_kind { LATER_FREE, LATER_WAIT, LATER_ECHO, LATER_CHIP };

/* A request in flight on an opener's channel. */
struct later {
    uint8_t        kind;        /* enum later_kind */
    unsigned       opener;      /* its opener's slot */
    struct idl_txn txn;         /* its channel is the opener's */
    uint64_t       deadline;    /* ns: answered ERR_TIMED_OUT then (DEADLINE_NEVER: never) */
    uint32_t       version;     /* LATER_WAIT: the version the caller has */
    uint32_t       addr;        /* LATER_ECHO: the address pinged, ... */
    uint16_t       seq;         /* ... the request's seq ... */
    uint64_t       sent;        /* ... and when it went (ns) */
};

struct progs {
    handle_t       port;
    struct dev    *dev;              /* the card, for iface and chip_counts */
    handle_t       shared;           /* /svc/net's shared channel (0: none, or its clients gone) */
    bool           shared_pending;
    struct opener  o[NET_OPENERS];
    struct sock    s[SOCK_SLOTS];
    struct later   l[LATER_MAX];
    uint32_t       version;          /* iface's version */
    struct net_counters c;           /* the programs' counts (the rest is filled when asked) */
};

extern struct progs pg;

/* ---- clients.c ------------------------------------------------------------- */

/* Take /svc/net's shared channel (0: none: no program reaches netstack)
 * and watch it; hook into stack.h and ctl.h. */
status_t progs_init(handle_t port, handle_t shared, struct dev *d);
/* A packet with one of the keys above (or KEY_NET, main.c's): true if it
 * was one. */
bool     progs_packet(const struct port_packet *p);
/* Serve every channel that may have requests, a budget each. */
void     progs_serve(void);
/* Answer what timed out; the next deadline (DEADLINE_NEVER: none). */
uint64_t progs_tick(void);
/* Is a channel known to have requests left (its budget ran out)? */
bool     progs_pending(void);
/* An opener's slot, if it still holds that generation. */
struct opener *progs_opener(unsigned slot, uint32_t gen);

/* ---- sock.c ---------------------------------------------------------------- */

/* A socket for opener `slot` (dhcp: netctl's, on port 68) on `port`; its
 * channel's client end into *out. The limits and errors: net.idl's udp. */
status_t sock_open(unsigned slot, uint16_t port, bool dhcp, handle_t *out, uint16_t *out_port);
/* Close every socket of opener `slot` (it is gone). */
void     sock_close_opener(unsigned slot);
/* Serve socket i's channel (its pending flag set). */
void     sock_serve(unsigned i);
/* Answer the sock_recvs that timed out; the next deadline. */
uint64_t sock_tick(uint64_t t);
/* Datagrams queued on all sockets now; sockets open now. */
void     sock_census(uint32_t *queued, uint32_t *open);
/* Point stack.h's stack_udp_input and ctl.h's ctl_dhcp_open here. */
void     sock_hooks(void);
