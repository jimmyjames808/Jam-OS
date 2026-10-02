/* libos's network sockets inside (<net.h>): what net.c, netsock.c (a
 * socket's datagrams) and nettcp.c (a stream's bytes) share. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <net.h>

/* Take on a socket from netstack: its channel and its rings' handles
 * (VMO, to_stack, to_prog), mapped and attached with `framing` and these
 * sizes. On a failure everything is closed and *s is empty. */
status_t net_sock_attach(struct net_sock *s, handle_t ch, uint16_t port, const handle_t hs[3],
                         uint32_t framing, uint32_t tx, uint32_t rx);
/* netstack ended: its end of the socket's channel is closed. */
bool     net_sock_gone(const struct net_sock *s);
/* Clear to_prog's bits, so a signal from now on is a new edge. */
void     net_sock_clear(const struct net_sock *s);
/* Sleep on the socket's own port (made at its first use: to_prog and the
 * channel's SIG_PEER_CLOSED) until a signal or the deadline. */
status_t net_sock_sleep(struct net_sock *s, uint64_t deadline);
