/* IPv4 addresses as text (user/lib/ipv4.c): "10.2.21.5" to and from the
 * number netstack's protocols use (the first byte highest: 0x0a021505),
 * and the settings file's static address (`net.address`, read by init's
 * net.c). Pure functions over strings: nothing here touches the network. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPV4_TEXT_MAX 16   /* "255.255.255.255" and its NUL */

/* A dotted quad at the start of s: four decimal numbers 0..255 of 1 to 3
 * digits, separated by single dots. true: *out is the address and *end
 * (may be NULL) the first character after it. */
bool ipv4_parse(const char *s, uint32_t *out, const char **end);
/* "a.b.c.d" into buf (IPV4_TEXT_MAX bytes); returns buf. */
const char *ipv4_format(uint32_t a, char buf[IPV4_TEXT_MAX]);

/* A static configuration: `net.address = <address>/<prefix> [<gateway>
 * [<dns> [<dns>]]]`, separated by spaces, e.g.
 * "10.2.21.50/24 10.2.21.1 10.2.21.1". The prefix is 1..32; a missing
 * gateway or DNS server is 0. Whether the address suits a host is
 * netstack's to say (netctl.set_ipv4); this checks only the form. */
struct ipv4_config {
    uint32_t address, mask, gateway;
    uint32_t dns[2];
};
bool ipv4_config_parse(const char *s, struct ipv4_config *out);
