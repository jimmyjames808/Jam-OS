/* netstack's side for programs: /svc/net and /svc/net-sys
 * (abi/idl/net.idl, <net.h>).
 *
 * Four kinds of channel, all on the loop's port, each bound PERSISTENT
 * and served PROGS_BUDGET requests a turn with a flag saying more may be
 * queued (a binding fires on edges only):
 * - the two shared channels (startup roles SR_USER + 1, /svc/net, and
 *   SR_USER + 2, /svc/net-sys; init keeps their server ends across
 *   restarts and publishes the client ends): each answers svc.connect
 *   with an opener's channel, and iface and counts. An opener made on
 *   /svc/net-sys is a system opener (init gives that name only to the
 *   network's own services), one made on /svc/net an ordinary one;
 * - an opener's channel (clients.c): iface, counts, wait_change,
 *   chip_counts, udp, udp_rings, echo. What it starts is its own: its
 *   sockets (closed with it), its requests in flight (dropped with it) and
 *   its ICMP echo id;
 * - a socket's channel (sock.c): the sock_* methods; closing it closes
 *   the socket. netctl's dhcp_open makes the one socket that belongs to
 *   no opener.
 * And each socket's rings (<sockring.h>), its to_stack event bound
 * PERSISTENT too: its datagrams go through them, never through calls.
 *
 * Fair shares (<net.h> NET_PROG_*): ordinary openers together may hold
 * only part of the openers, sockets, requests in flight and ring bytes;
 * the rest is kept for system openers, so no program can stop dns, netlog
 * or update. A refusal for the share is ERR_NO_RESOURCES, as for any
 * limit, and counted (refused_shares). Within a class netstack can't tell
 * programs apart (it knows channels, not senders): the per-opener limits
 * bound one opener; a program that opens many can take its class's share.
 *
 * Nothing here waits (ARCHITECTURE.md "How a service waits"): a request
 * that can't be answered at once (an echo, a wait_change, chip_counts) is
 * kept in the table of requests in flight and answered when its answer
 * comes or its deadline passes (progs_tick gives the loop the next one). A
 * datagram for a socket goes into its rx ring, or is dropped and counted
 * when the ring is full: lwIP's receive buffers are never held by a slow
 * reader. A socket's tx ring is read only while the card's tx ring has
 * room, at most SOCK_TX_BUDGET records a turn, the sockets in turn.
 *
 * Every slot carries a generation, in its port keys (bits 8 and up) and in
 * what refers to it, so a packet or a datagram left from a closed slot is
 * never taken for the slot's next user. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <idl/net.h>
#include <net.h>
#include <sockring.h>
#include "dev.h"
#include "stack.h"

/* Port keys (main.c's are below 0x10, dev.h's 0x10 to 0x21). The low byte
 * names the slot, the rest its generation. */
#define KEY_OPENER     0x40u   /* + the opener's slot */
#define KEY_SOCK       0x80u   /* + the socket's slot: its channel */
#define KEY_RING       0xc0u   /* + the socket's slot: its to_stack event */
#define SOCK_SLOTS     (NET_SOCKETS_MAX + 1)   /* the DHCP socket has one of its own */
#define LATER_MAX      NET_LATER_MAX
#define PROGS_BUDGET   8u      /* requests taken off one channel a turn */
#define SOCK_TX_BUDGET 32u     /* records taken off one socket's tx ring a turn */
#define SOCK_CARD_ROOM 2u      /* card slots a datagram needs (one more: an ARP request) */
#define CHIP_WAIT      (3 * NS_PER_S)   /* chip_counts: the driver's answer, at most */

_Static_assert(KEY_OPENER + NET_OPENERS <= KEY_SOCK, "opener keys");
_Static_assert(KEY_SOCK + SOCK_SLOTS <= KEY_RING, "socket keys");
_Static_assert(KEY_RING + SOCK_SLOTS <= 0x100, "ring keys fit the low byte");

/* An opener's class: which shared channel it came from. */
enum { CLASS_PROG, CLASS_SYS, CLASSES };

struct opener {
    handle_t ch;          /* our end of its channel; 0: the slot is free */
    uint32_t gen;         /* the slot's generation */
    bool     pending;     /* requests may be queued */
    uint8_t  cls;         /* CLASS_PROG or CLASS_SYS */
    uint16_t echo_id;     /* its ICMP echo id: the slot in the low 5 bits, random above */
    unsigned socks;       /* sockets it holds */
    unsigned later;       /* its requests in flight */
    uint64_t ring_bytes;  /* its sockets' ring bytes (sockring_bytes) */
};

struct sock {
    handle_t           ch;          /* our end of its channel; 0: the slot is free */
    uint32_t           gen;         /* the slot's generation */
    bool               pending;     /* requests may be queued */
    bool               dhcp;        /* netctl's DHCP socket (belongs to no opener) */
    uint8_t            cls;         /* its opener's class (CLASS_SYS for the DHCP socket) */
    unsigned           opener;      /* the opener that made it, and that opener's ... */
    uint32_t           opener_gen;  /* ... generation then */
    struct stack_udp  *u;           /* lwIP's socket */
    uint16_t           port;        /* its local port */
    uint32_t           peer;        /* sock_connect's peer (0: anyone) */
    uint16_t           peer_port;
    /* its rings (vmo 0: none yet) */
    handle_t           vmo, to_stack, to_prog;   /* ours, every right */
    uint8_t           *map;         /* the VMO mapped here (bytes long) */
    uint64_t           bytes;       /* sockring_bytes: what it counts against the shares */
    struct sockring    r;           /* our ends: tx consumer, rx producer */
    struct sockring_status st;      /* our copy of its status line (never read back) */
    bool               tx_ready;    /* its tx ring may hold records */
    bool               rx_dirty;    /* records put in the rx ring, not published */
    bool               st_dirty;    /* st changed, not written */
    uint32_t           changes_put; /* st.changes when last written (SIG_STATE on a change) */
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

/* What a class holds now: the fair shares are counted on these. */
struct share {
    unsigned openers;           /* openers */
    unsigned socks;             /* sockets */
    unsigned later;             /* requests in flight */
    uint64_t ring_bytes;        /* its sockets' sockring_bytes */
};

struct progs {
    handle_t       port;
    struct dev    *dev;              /* the card: iface, chip_counts, its tx ring's room */
    handle_t       shared[CLASSES];  /* the shared channels (0: none, or their clients gone) */
    bool           shared_pending[CLASSES];
    struct opener  o[NET_OPENERS];
    struct sock    s[SOCK_SLOTS];
    struct later   l[LATER_MAX];
    struct share   held[CLASSES];    /* what each class holds now */
    unsigned       next_tx;          /* the socket the next turn's tx starts with */
    bool           card_full;        /* the card's tx ring had no room: tx waits for it */
    uint32_t       version;          /* iface's version */
    struct net_counters c;           /* the programs' counts (the rest is filled when asked) */
};

extern struct progs pg;

/* ---- clients.c ------------------------------------------------------------- */

/* Take the shared channels (0: none: no opener of that class) and watch
 * them; hook into stack.h, ctl.h and dev.h. */
status_t progs_init(handle_t port, handle_t shared, handle_t shared_sys, struct dev *d);
/* A packet with one of the keys above (or clients.c's KEY_NET*): true if
 * it was one. */
bool     progs_packet(const struct port_packet *p);
/* Serve every channel that may have requests, a budget each, then the tx
 * rings that may hold records. */
void     progs_serve(void);
/* Answer what timed out; the next deadline (DEADLINE_NEVER: none). */
uint64_t progs_tick(void);
/* Is there more to do at once: a channel with requests left (its budget
 * ran out), a tx ring with records the card has room for? */
bool     progs_pending(void);
/* Publish what this turn put in the rx rings (after the card's frames
 * went to lwIP), waking the programs that sleep. */
void     progs_flush(void);
/* An opener's slot, if it still holds that generation. */
struct opener *progs_opener(unsigned slot, uint32_t gen);
/* May class cls take `socks` more sockets, `later` more requests in flight
 * and `ring_bytes` more ring bytes? Always for CLASS_SYS (only the totals
 * bound it); an ordinary opener only within NET_PROG_*. A refusal is
 * counted. */
bool     progs_share_ok(uint8_t cls, unsigned socks, unsigned later, uint64_t ring_bytes);

/* ---- sock.c ---------------------------------------------------------------- */

/* A socket for opener `slot` (dhcp: netctl's, on port 68) on `port`: its
 * channel's client end into *out, its port, its slot here. The limits and
 * errors: net.idl's udp. */
status_t sock_open(unsigned slot, uint16_t port, bool dhcp, handle_t *out, uint16_t *out_port,
                   unsigned *out_i);
/* Make socket i's rings, *tx and *rx bytes (0: the defaults; the sizes
 * made come back): the program's VMO and events into hs (ring, to_stack,
 * to_prog). The errors: net.idl's sock_rings. */
status_t sock_rings_make(unsigned i, uint32_t *tx, uint32_t *rx, handle_t hs[3]);
/* Close socket i: its rings unmapped and shrunk to nothing, its channel. */
void     sock_close(unsigned i);
/* Close every socket of opener `slot` (it is gone). */
void     sock_close_opener(unsigned slot);
/* Serve socket i's channel (its pending flag set). */
void     sock_serve(unsigned i);
/* A socket's to_stack event fired (KEY_RING): its rings need a look. */
void     sock_ring_event(unsigned i, uint32_t gen);
/* Read the tx rings that may hold records, the sockets in turn, while the
 * card has room. */
void     sock_tx_all(void);
/* Any tx ring to read now (and room on the card)? */
bool     sock_tx_pending(void);
/* Publish the rx rings this turn filled, and the status lines it changed. */
void     sock_flush(void);
/* Bytes waiting in all sockets' rx rings now; sockets open now. */
void     sock_census(uint32_t *queued, uint32_t *open);
/* Point stack.h's stack_udp_input and ctl.h's ctl_dhcp_open here. */
void     sock_hooks(void);
