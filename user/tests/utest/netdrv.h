/* utest: netdrv.c's harness, bin/netstack over a fake driver, for the
 * socket tests (netsock.c). The test is the driver (frames in and out of
 * the rings, netpkt.h's builders and checks) and init (netctl, and
 * /svc/net's shared channel). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define NETDRV_WAIT  (5 * NS_PER_S)     /* for netstack to do what it must */
#define NETDRV_QUIET (300 * NS_PER_MS)  /* for netstack to do nothing */

/* netstack started with a session, OUR_IP/24 and its announcement taken;
 * stopped (killed: its job must end empty). */
bool     netdrv_start(void);
bool     netdrv_stop(void);
/* A frame into the rx ring; the next frame netstack sent within `wait`
 * (ERR_TIMED_OUT: none). */
bool     netdrv_send(const uint8_t *frame, size_t len);
status_t netdrv_recv(uint8_t *f, uint32_t *n, uint64_t wait);
/* The peer pings us (after an ARP request for us, if arp: netstack then
 * knows the peer's MAC) and the reply is checked. */
bool     netdrv_ping(uint32_t seq, bool arp);
/* Answer one request netstack sent on the session channel (netdev.info,
 * netdev.stats: rx_frames 42, the rest 0). */
bool     netdrv_serve_session(void);
/* netctl's client end; /svc/net's shared channel's client end;
 * /svc/net-listen's (its openers may listen: take a fixed port below
 * NET_PORT_EPHEMERAL). */
handle_t netdrv_ctl(void);
handle_t netdrv_net(void);
handle_t netdrv_net_listen(void);
