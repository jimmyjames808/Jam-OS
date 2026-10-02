/* netstack: programs' TCP (tcpsock.c): net.idl's tcp, tcp_listener and
 * accept on top of tcp.c's connections.
 *
 * A TCP socket is a slot here, a channel of its own (sock_state on it;
 * closing it closes the connection), its rings (sockmem.h, byte-stream
 * framing) and tcp.c's connection. A listener is a slot, a channel
 * (accept on it; closing it stops listening) and tcp.c's listener. A
 * listener's connection gets its socket, channel and rings when lwIP
 * finishes its handshake, charged to the listener's opener from then on
 * (so its bytes wait in its rings until the program accepts it); accept
 * hands the program the channel's other end and the rings' handles.
 *
 * Every channel and to_stack event is on the loop's port with a key of
 * its own: KEY_TCP set, what it is (TK_*) at bit 48, the slot's
 * generation at bits 16-47 and the slot below (progs.h's keys never set
 * KEY_TCP). Channels are served PROGS_BUDGET requests a turn; an accept
 * that can't be answered at once waits in its listener (one a listener)
 * until a connection comes or its deadline.
 *
 * Limits and fair shares (<net.h> NET_TCP_*, NET_LISTENERS_*,
 * NET_BACKLOG_*): connections, listeners and backlog per opener, in all,
 * and for ordinary openers together; ring bytes as UDP's (sockmem.h). A
 * listener's connection that doesn't fit is reset (counted as refused).
 *
 * An opener that goes takes its listeners with it at once; its TCP
 * sockets are reset (CLOSED, ERR_PEER_CLOSED) but kept, rings and all,
 * until the program closes them, as UDP's are (sock.c "Closing"). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include "progs.h"

#define KEY_TCP (1ull << 63)   /* a port key of tcpsock.c's */

/* Hook tcp.c into stack.h. */
void     tcpsock_init(void);
/* A packet with a KEY_TCP key: true if it was one. */
bool     tcpsock_packet(const struct port_packet *p);
/* Serve the channels that may have requests, a budget each, and the
 * accepts waiting whose connection came. */
void     tcpsock_serve(void);
/* Answer the accepts that timed out; the next deadline. */
uint64_t tcpsock_tick(uint64_t t);
/* A channel known to have requests left, or an accept to answer. */
bool     tcpsock_pending(void);
/* Opener `slot` is going: its listeners closed, its sockets reset. */
void     tcpsock_close_opener(unsigned slot);
/* The TCP counts for net.counts. */
void     tcpsock_counts(struct net_counters *c);
/* net.idl's tcp and tcp_listener on opener ctx's channel (clients.c's
 * opener_ops). */
status_t tcpsock_op_tcp(void *ctx, uint32_t address, uint16_t port, uint32_t tx_bytes,
                        uint32_t rx_bytes, handle_t *out_socket, handle_t *out_ring,
                        handle_t *out_to_stack, handle_t *out_to_prog, uint16_t *out_port,
                        uint32_t *out_tx_bytes, uint32_t *out_rx_bytes);
status_t tcpsock_op_listener(void *ctx, uint16_t port, uint32_t backlog, uint32_t tx_bytes,
                             uint32_t rx_bytes, handle_t *out_listener, uint16_t *out_port);
