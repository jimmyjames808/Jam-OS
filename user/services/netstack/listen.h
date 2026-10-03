/* netstack: the listen permissions (listen.c; the design is
 * docs/M9.5-PLAN.md "Track B, built" and "Ports below 1024").
 *
 * Accepting what nobody asked for (a datagram to a port a program chose,
 * a TCP connection) is a permission of its own, and doing it on a port
 * below 1024, where the well-known services live, a narrower one. netstack
 * learns them from the channel an opener came through, never from anything
 * the opener says: init makes the shared channels and keeps their server
 * ends, /svc/net (SR_USER + 1), /svc/net-listen (SR_USER + 2) and
 * /svc/net-low (SR_USER + 4); an opener from the second is a
 * listening one (struct opener's `listen`, set once at connect), one from
 * the third a listening one that may also take a low port (`low` too). The
 * shell gives /svc/net-listen only to a program whose list says `svc net
 * listen`, and /svc/net-low only to one whose list says `svc net
 * listen low` (<wants.h>); `allow` shows the first to the owner for a
 * program on /data and refuses the second for any program on /data;
 * checkwants.py approves them for the boot image, the second only for a
 * service (bin/serve).
 *
 * The rule for UDP: a port below NET_PORT_LOW only for a low opener (and
 * never the DHCP ports, 67 and 68: netctl's DHCP socket's); port 0
 * (netstack picks one, NET_PORT_EPHEMERAL and up) or a port a program picks
 * in NET_PORT_EPHEMERAL..65535 (the resolver's random source ports) for
 * anyone; a fixed port in NET_PORT_LOW..NET_PORT_EPHEMERAL - 1, where
 * servers live and where a peer would send unasked, only for a listening
 * opener. A socket without the permission still gets the replies to what
 * it sent, and anything else sent to its port (UDP can't tell the two
 * apart): what the permission guards is a port someone else could know in
 * advance. TCP's listen asks listen_may_accept: a listening opener, and a
 * low one for a port below NET_PORT_LOW. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include "progs.h"

/* Take /svc/net-listen's and /svc/net-low's shared channels (0: none:
 * nobody may listen there, said in the log) and watch them on port. */
status_t listen_init(handle_t port, handle_t shared, handle_t shared_low);
/* A packet with one of their keys: true if it was one. */
bool     listen_packet(const struct port_packet *p);
/* Serve the shared channels that may have requests, a budget at a time. */
void     listen_serve(void);
/* Requests may be left on one (its budget ran out). */
bool     listen_pending(void);
/* May opener o take UDP port `port` (0: netstack's pick)? */
bool     listen_may_bind(const struct opener *o, uint16_t port);
/* May opener o accept connections on TCP port `port` (0: netstack's pick)? */
bool     listen_may_accept(const struct opener *o, uint16_t port);
