/* utest: what the netstack TCP tests share (nettcp.c: connections, data,
 * windows, closes and resets; tcpabuse.c: floods, malformed segments,
 * a fuzz and hostile rings). netstack's core (stack.c, lwIP inside) and
 * its TCP connections (tcp.c) run in-process over tcppeer.c's fake edge;
 * the test is the peer (tcppeer.h) and the program (the rings' other side,
 * in plain memory). No lwIP header is included. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sockring.h>
#include "tcp.h"
#include "tcppeer.h"

#define FIX_RINGS   6u                       /* connections with rings of their own at once */
#define FIX_BYTES   (SOCKRING_HDR + 96u * 1024)   /* each one's memory: header and both rings */
#define PEER_ISN    0x7a000000u              /* where the peer's sequence numbers start */

/* One connection as the test sees it: the peer's half, the program's side
 * of the rings, and what the peer knows of netstack's window. */
struct cx {
    struct tp         p;
    struct ntcp_conn *c;
    struct sockring   prog;       /* the program's side of c's rings */
    unsigned          slot;       /* its ring memory */
    uint32_t          acked;      /* the last ACK netstack sent: its rcv_nxt ... */
    uint32_t          window;     /* ... and its window then */
};

/* A fresh stack on the fake edge (link up, OUR_IP/24, the peer's MAC
 * known) with tcp.c hooked in; and taking it down: every connection gone
 * (the test freed its own), nothing left in lwIP's buffers or heap. */
bool fix_up(void);
bool fix_down(void);
/* Ring memory slot for tx/rx bytes (each a sockring size, together at
 * most FIX_BYTES - SOCKRING_HDR), made as netstack does; the program's
 * side attached into *prog. */
bool fix_rings(uint32_t tx, uint32_t rx, struct sockring *stack_side, struct sockring *prog,
               unsigned *slot);
void fix_rings_free(unsigned slot);
/* A connection netstack opens to the peer at port `port`, its handshake
 * answered: OPEN, with rings of tx/rx bytes. */
bool fix_connect(struct cx *x, uint16_t port, uint32_t tx, uint32_t rx);
/* Free x's connection and its rings. */
void fix_free(struct cx *x);
/* Run netstack's turn: ntcp_work. Then read what it sent to x's peer:
 * no RST unless rst_ok, ACKs noted (acked, window), segs into s (*n). */
bool fix_turn(struct cx *x, struct tp_seg *s, unsigned max, unsigned *n, bool rst_ok);
/* Let lwIP's timers run (the delayed ACK): sleep to its next deadline,
 * 300 ms at most, then a turn. */
void fix_tick(void);
/* The program publishes on a ring end and, if netstack sleeps on it,
 * signals it (ntcp_kick), as libos will. */
void fix_publish(struct cx *x, struct sockring_end *e);
/* The program's view of the status line. */
struct sockring_status fix_status(const struct cx *x);
/* lwIP's TCP counts now. */
struct stack_tcp_counts fix_counts(void);

/* The program sends `total` bytes of stream `id` to the peer (got_buf:
 * where the peer keeps them, total bytes); the peer sends `total` to the
 * program, which reads `chunk` at a time. Both check every byte, and the
 * second also that every byte sent is in the ring and none in lwIP. */
bool fix_send(struct cx *x, uint32_t id, size_t total, uint8_t *got_buf);
bool fix_receive(struct cx *x, uint32_t id, size_t total, uint32_t chunk);
/* A listener's ntcp_rings_fn and ntcp_drop_fn: rings of 8 KiB tx and the
 * listener's rx_size from the fixture's slots. */
status_t fix_lst_rings(struct ntcp_listener *l, struct ntcp_conn *c);
void     fix_lst_drop(struct ntcp_listener *l, struct ntcp_conn *c);
/* The peer (port `from`) opens a connection to listener port `port`: its
 * SYN-ACK checked (window `win`), the ACK not sent yet. */
bool fix_handshake(struct cx *x, uint16_t port, uint16_t from, uint32_t win);
/* The program accepts the oldest connection waiting on l into x. */
bool fix_accept(struct ntcp_listener *l, struct cx *x);
