/* The kernel command line from the loader: space-separated boot words
 * ("selftest", "nopcid") and key=N values ("stress=60"). Set once at boot;
 * read-only afterwards. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cmdline_set(const char *cmdline);
const char *cmdline_get(void);
/* True if `word` appears as a whole space-separated word. */
bool cmdline_has(const char *word);
/* Value of key=N as a number; `dflt` if the key is absent, `bare` if it
 * appears without "=N". */
uint64_t cmdline_get_u64(const char *key, uint64_t dflt, uint64_t bare);
/* cmdline_vlan's untagged mode: plain Ethernet, no VLAN tag on any frame.
 * Not a VLAN id (those have 12 bits); the drivers' NETFRAME_MODE_UNTAGGED
 * (<jam/netframe.h>, which the kernel can't include). */
#define CMDLINE_VLAN_UNTAGGED 0x1000u

/* The network mode the boot words of `line` choose (ARCHITECTURE.md
 * "Networking"): `dflt` with no `vlan` word; the id of `vlan=<n>` when n
 * is 1..4094 in decimal (at most 4 digits); CMDLINE_VLAN_UNTAGGED for
 * `vlan=none` or `vlan=untagged`; 0, the network off, for anything else:
 * `vlan=off`, another value, a bare `vlan` or `vlan=`, or two `vlan=`
 * words that disagree. Pure: reads only `line`. */
uint32_t cmdline_vlan(const char *line, uint32_t dflt);
