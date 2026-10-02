/* rtl8125: the driver's arguments, as pure functions (drv/rtl8125).
 *
 * devmgr starts the driver with words after its name (driver_start.args):
 *   netprobe       the listen-only probe: nothing is ever sent;
 *   netsend        the send test: full mode, a few ARP probes, then stop
 *                  (sendtest.c);
 *   vlan=<n>       the VLAN every frame is tagged with, 1..4094
 *                  (ARCHITECTURE.md "Networking": the kernel decides it
 *                  and devmgr passes it on; the driver never takes it from
 *                  anyone else), read by <jam/netdev.h>'s
 *                  netdev_vlan_args: one valid value, else none;
 *   arpto=<a.b.c.d> the send test's target (default: VLAN 21's router).
 * Without a valid vlan= the network stays off: full mode doesn't start.
 * `netprobe` wins over `netsend`: given both, nothing is sent.
 *
 * user/tests/utest/netframe.c tests these over hostile words. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/netdev.h>
#include "notx.h"

#define RTL_ARP_TARGET_DEFAULT 0x0a021501u   /* 10.2.21.1: VLAN 21's router (docs/M9-PLAN.md) */

struct rtl_args {
    enum rtl_mode mode;   /* RTL_MODE_PROBE or RTL_MODE_FULL */
    bool     sendtest;    /* `netsend`: full mode runs the ARP send test */
    uint16_t vlan;        /* 1..4094; 0: none (or an invalid or conflicting vlan= word) */
    uint32_t arp_target;  /* the send test's target IPv4 address, host order */
    bool     bad_target;  /* an arpto= word that isn't an address (the default is used) */
};

/* Does word start with prefix? (Drivers have no string library.) */
static inline bool rtl_word_is(const char *word, const char *prefix)
{
    if (!word)
        return false;
    for (; *prefix; prefix++, word++)
        if (*word != *prefix)
            return false;
    return true;
}

/* A decimal number of 1..maxdigits digits, nothing after it, at most max.
 * False for anything else (a sign, a space, an empty string, too long). */
static inline bool rtl_parse_dec(const char *s, unsigned maxdigits, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    unsigned n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n == maxdigits)
            return false;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    if (n == 0 || v > max)
        return false;
    *out = v;
    return true;
}

/* "a.b.c.d", each 0..255 in at most 3 digits, into a host-order address. */
static inline bool rtl_parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t a = 0;
    for (unsigned part = 0; part < 4; part++) {
        char num[4];
        unsigned k = 0;
        while (*s >= '0' && *s <= '9' && k < 3)
            num[k++] = *s++;
        num[k] = 0;
        uint32_t v;
        if (!rtl_parse_dec(num, 3, 255, &v))
            return false;
        a = a << 8 | v;
        if (part < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *out = a;
    return true;
}

static inline struct rtl_args rtl_args_parse(const char *const *args, unsigned n)
{
    struct rtl_args a = { .mode = RTL_MODE_FULL, .arp_target = RTL_ARP_TARGET_DEFAULT };
    for (unsigned i = 0; i < n; i++) {
        if (rtl_word_is(args[i], "netsend") && !args[i][7])
            a.sendtest = true;
        if (rtl_word_is(args[i], "arpto=") && !rtl_parse_ipv4(args[i] + 6, &a.arp_target)) {
            a.arp_target = RTL_ARP_TARGET_DEFAULT;
            a.bad_target = true;
        }
    }
    for (unsigned i = 0; i < n; i++)
        if (rtl_word_is(args[i], "netprobe") && !args[i][8])
            a.mode = RTL_MODE_PROBE;
    if (a.mode == RTL_MODE_PROBE)
        a.sendtest = false;
    a.vlan = netdev_vlan_args(args, n);   /* THE VLAN: the one place the driver learns it */
    return a;
}
