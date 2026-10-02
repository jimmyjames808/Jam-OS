/* netstack's control channel (abi/idl/netctl.idl), ctl.c: the checks on
 * what a client asks for and the handlers. Shared with main.c (the loop)
 * and user/tests/utest/netstack.c (which serves a channel of its own with
 * it, in-process). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include "dev.h"
#include "stack.h"

/* Requests taken off the control channel per turn of the loop, so a
 * client that keeps it full can't starve the network side. */
#define CTL_BUDGET 16u

/* Serve up to CTL_BUDGET requests from ch. OK: the budget was spent (more
 * may be queued); else the read's status: ERR_SHOULD_WAIT (none left),
 * ERR_PEER_CLOSED (no client end is left). */
status_t ctl_serve(handle_t ch);

/* Where netctl.device's answer comes from: main.c points it at the card
 * (netif.c's dev_get_report); NULL (a test's): no card, all 0. */
extern void (*ctl_device_report)(struct dev_report *out);

/* Called after the address, the gateway or the DNS servers changed
 * (clients.c bumps iface's version); NULL: nobody to tell. */
extern void (*ctl_changed)(void);
/* netctl.dhcp_open's socket (sock.c's); NULL (a test's): ERR_NOT_SUPPORTED. */
extern status_t (*ctl_dhcp_open)(handle_t *out);
/* The DNS servers set (0: none). */
void ctl_dns(uint32_t out[2]);

/* What netctl.set_ipv4 accepts (its comment in netctl.idl). */
bool ctl_ipv4_valid(const struct stack_ipv4 *ip);
/* A unicast address a host may use or talk to: not in 0.0.0.0/8,
 * 127.0.0.0/8, multicast (224.0.0.0/4) or 240.0.0.0/4 (with the
 * broadcast address). */
bool ctl_unicast(uint32_t a);
/* "a.b.c.d" into buf (at least 16 bytes); returns buf. */
const char *ctl_fmt_ip(char *buf, uint32_t a);
