/* netstack: the listen permission (listen.c; the design is
 * docs/M9.5-PLAN.md "Track B, built").
 *
 * Accepting what nobody asked for (a datagram to a port a program chose,
 * a TCP connection) is a permission of its own. netstack learns it from
 * the channel an opener came through, never from anything the opener
 * says: init makes two shared channels and keeps their server ends,
 * /svc/net (SR_USER + 1) and /svc/net-listen (SR_USER + 2), and an opener
 * from the second is a listening one (struct opener's `listen`, set once
 * at connect). The shell gives /svc/net-listen only to a program whose
 * list says `svc net listen` (<wants.h>), which `allow` shows the owner
 * for a program on /data; checkwants.py approves it for the boot image.
 *
 * The rule for UDP: a port below NET_PORT_LOW never (but netctl's DHCP
 * socket); port 0 (netstack picks one, NET_PORT_EPHEMERAL and up) or a
 * port a program picks in NET_PORT_EPHEMERAL..65535 (the resolver's random
 * source ports) for anyone; a fixed port in NET_PORT_LOW..NET_PORT_EPHEMERAL
 * - 1, where servers live and where a peer would send unasked, only for a
 * listening opener. A socket without the permission still gets the
 * replies to what it sent, and anything else sent to its port (UDP can't
 * tell the two apart): what the permission guards is a port someone else
 * could know in advance. TCP's listen and accept will ask
 * listen_may_accept. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include "progs.h"

/* Take /svc/net-listen's shared channel (0: none: nobody may listen, said
 * in the log) and watch it on port. */
status_t listen_init(handle_t port, handle_t shared);
/* A packet with its key: true if it was one. */
bool     listen_packet(const struct port_packet *p);
/* Serve the shared channel if it may have requests, a budget at a time. */
void     listen_serve(void);
/* Requests may be left on it (its budget ran out). */
bool     listen_pending(void);
/* May opener o take UDP port `port` (0: netstack's pick)? */
bool     listen_may_bind(const struct opener *o, uint16_t port);
/* May opener o accept connections (TCP's listen)? */
bool     listen_may_accept(const struct opener *o);
