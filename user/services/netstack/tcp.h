/* netstack's TCP connections (tcp.c): each one's socket rings
 * (<sockring.h>, the byte-stream framing) joined to its lwIP side
 * (stack.h's TCP edge). tcp.c never sees lwIP and never makes a handle:
 * whoever opens a connection (the program-facing side, or a test) gives
 * it rings already made with sockring_make(SOCKRING_STREAM), and frees
 * them after ntcp_conn_free.
 *
 * The data path, both ways without netstack ever waiting on a program:
 * - in: each segment's bytes go into the rx ring as they arrive
 *   (stack.h's hooks.rx). The receive window is the ring's free room:
 *   bytes are given back to it (stack_tcp_recved) only as the program
 *   takes them from the ring. While less than half the window (and a
 *   segment) is left, netstack keeps the rx producer's `waits` up, so the
 *   program's reads signal SOCKRING_SIG_RX_ROOM (ntcp_kick) and the window
 *   opens again at once; lwIP announces it when it grew by a segment.
 *   The peer's FIN is SOCKRING_END on the rx ring.
 * - out: on a kick (SOCKRING_SIG_TX), on acks (hooks.sent) and when the
 *   connection comes up, netstack takes from the tx ring what was ready
 *   when it looked, at most what the peer's window allows (and a segment
 *   more), copies it into lwIP and frees the ring room
 *   (SOCKRING_SIG_TX_ROOM if the program waits). The rest stays in the
 *   ring. SOCKRING_END on the tx ring, once every byte before it is in
 *   lwIP, sends a FIN. lwIP keeps at most the tx ring's size unacked (the
 *   connection's send buffer, at least STACK_TCP_SND_MIN), so a program
 *   that asks for a big tx ring keeps as much in flight.
 * - the status line: CONNECTING, OPEN, then CLOSED, with `error` OK when
 *   both FINs went and ours was acked, else why it failed (net.idl's TCP
 *   methods name them: ERR_NOT_FOUND refused, ERR_PEER_CLOSED reset,
 *   ERR_TIMED_OUT the peer stopped answering, ERR_BAD_STATE our address
 *   went away, ERR_OUT_OF_RANGE the program's ring counts went wrong, so
 *   the stream was reset). Each change signals SOCKRING_SIG_STATE.
 * Once CLOSED, lwIP's side is let go of (it finishes closing on its own)
 * and the connection is the program's to free.
 *
 * Hooks run inside lwIP; they only move bytes and note work, which
 * ntcp_work does from the loop (every turn, after the frames). One thread:
 * nothing locks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <sockring.h>
#include "stack.h"

#define TCP_CONNS     STACK_TCP_CONNS       /* connections, accepted ones waiting included */
#define TCP_LISTENERS STACK_TCP_LISTENERS
#define TCP_CHUNK     (16u * 1024)          /* bytes copied into lwIP at a time */

struct ntcp_listener;

struct ntcp_conn {
    bool                   used;        /* the slot is taken */
    struct stack_tcp      *t;           /* lwIP's side; NULL once gone or let go of */
    struct ntcp_listener  *queued_on;   /* accepted by lwIP, not yet by the program */
    struct sockring        r;           /* netstack's side of its rings (r.page NULL: none) */
    struct sockring_status st;          /* the status line, netstack's copy */
    uint32_t               window;      /* its receive window: its rx ring, at most
                                         * stack_tcp_window (settled once connected) */
    uint64_t               given_back;  /* rx bytes given back to the window */
    bool                   connected;   /* the handshake finished */
    bool                   fin_sent;    /* the tx ring's END reached lwIP */
    bool                   rx_ended;    /* the peer's FIN: SOCKRING_END on the rx ring */
    bool                   retry;       /* lwIP had no memory for its bytes or FIN: try again */
    bool                   closing;     /* both FINs went, ours not acked: looked at each turn */
    bool                   listed;      /* on the work list */
    struct ntcp_conn      *next;        /* the work list */
    handle_t               to_prog;     /* the program's event (0: none, as in a test) */
    uint32_t               signalled;   /* every bit ever signalled to the program */
    void                  *owner;       /* whoever opened it (the socket) */
};

/* A listener's connection, accepted by lwIP: give it rings (sockring_make,
 * SOCKRING_STREAM, rx at least the listener's rx_size) with ntcp_conn_rings.
 * An error resets the connection. */
typedef status_t (*ntcp_rings_fn)(struct ntcp_listener *l, struct ntcp_conn *c);
/* A connection the listener held is dropped before the program took it
 * (reset, or the listener closed): free what ntcp_rings_fn made. */
typedef void (*ntcp_drop_fn)(struct ntcp_listener *l, struct ntcp_conn *c);

struct ntcp_listen_req {
    uint16_t      port;      /* 0: lwIP picks one */
    uint32_t      backlog;   /* 1..STACK_TCP_BACKLOG */
    uint32_t      rx_size;   /* its connections' rx rings: windows of min(this, STACK_TCP_WND) */
    ntcp_rings_fn rings;
    ntcp_drop_fn  drop;
    void         *owner;
};

struct ntcp_listener {
    bool                     used;      /* the slot is taken */
    struct stack_tcp_listen *l;         /* lwIP's listener */
    uint16_t                 port;
    struct ntcp_listen_req   req;
    struct ntcp_conn        *q[STACK_TCP_BACKLOG];   /* accepted by lwIP, oldest at head */
    unsigned                 head, n;
    uint32_t                 refused;   /* connections reset: no slot or no rings for them */
    uint32_t                 arrived;   /* connections ever queued (a test's count) */
};

/* Hook into stack.h (once, at start). */
void     ntcp_init(void);
/* Run the work noted since the last turn: bytes to move, windows to open,
 * FINs, closes. Called by the loop every turn. */
void     ntcp_work(void);
/* Is work noted (the loop shouldn't sleep)? */
bool     ntcp_pending(void);

/* A free connection slot, no rings yet. ERR_NO_RESOURCES: all taken. */
status_t ntcp_conn_new(void *owner, struct ntcp_conn **out);
/* Its rings (netstack's side, made with SOCKRING_STREAM): copied in. */
void     ntcp_conn_rings(struct ntcp_conn *c, const struct sockring *r);
/* Connect to to:port (the caller checked them). CONNECTING now, OPEN or
 * CLOSED later. stack_tcp_connect's errors. */
status_t ntcp_connect(struct ntcp_conn *c, uint32_t to, uint16_t port);
/* The program signalled to_stack (bytes or the END to send, room read). */
void     ntcp_kick(struct ntcp_conn *c);
/* Reset it now: CLOSED with `why`. */
void     ntcp_conn_abort(struct ntcp_conn *c, status_t why);
/* The program is done with it: let go of lwIP's side (a reset if bytes it
 * never read are left, else a FIN after what lwIP holds) and free the
 * slot. Its rings are the caller's again. */
void     ntcp_conn_free(struct ntcp_conn *c);

/* Listen. stack_tcp_listen's errors; ERR_NO_RESOURCES: no listener free. */
status_t ntcp_listen(const struct ntcp_listen_req *req, struct ntcp_listener **out);
/* The oldest connection waiting, now the caller's (it stops counting
 * against the backlog). ERR_SHOULD_WAIT: none. */
status_t ntcp_accept(struct ntcp_listener *l, struct ntcp_conn **out);
/* Stop listening: the connections waiting are reset and dropped. */
void     ntcp_unlisten(struct ntcp_listener *l);

/* Connections and listeners in use now. */
void     ntcp_census(uint32_t *conns, uint32_t *listeners);
/* Bytes moved since netstack started. */
struct ntcp_counts {
    uint64_t bytes_in;    /* put in connections' rx rings */
    uint64_t bytes_out;   /* taken from their tx rings into lwIP */
};
void     ntcp_get_counts(struct ntcp_counts *out);
