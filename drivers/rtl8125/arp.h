/* rtl8125: the send test's one frame, an ARP probe, and the reply it
 * waits for, as pure functions (drv/rtl8125, sendtest.c).
 *
 * An ARP probe (RFC 5227, 2.1.1) asks "who has <target>?" from a sender
 * address of 0.0.0.0, so no neighbour learns a binding for us from it: the
 * most harmless frame there is that still gets an answer. It is built
 * untagged; tx.c tags it like any other frame.
 *
 * The NIC driver otherwise never reads past a frame's bytes 12-17
 * (netframe.h). The send test is the one exception, and only for frames
 * already kept for our VLAN: arp_is_reply reads the 28-byte ARP body of a
 * frame of at least 42 bytes, at fixed offsets, to recognise the target's
 * answer. Layout (RFC 826): Ethernet header (14), then hardware type 1,
 * protocol 0x0800, lengths 6 and 4, the operation (1 request, 2 reply),
 * sender MAC and IPv4 address, target MAC and IPv4 address. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/netframe.h>

#define ARP_FRAME_LEN  42u        /* Ethernet header + the ARP body, untagged, no padding */
#define ARP_ETHERTYPE  0x0806u
#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY   2u

static inline void arp_put_be32(uint8_t *p, uint32_t v)
{
    netframe_put_be16(p, (uint16_t)(v >> 16));
    netframe_put_be16(p + 2, (uint16_t)v);
}

static inline uint32_t arp_be32(const uint8_t *p)
{
    return (uint32_t)netframe_be16(p) << 16 | netframe_be16(p + 2);
}

/* The probe for `target` (host order) from `mac`, into out (42 bytes):
 * broadcast, sender IP 0.0.0.0, target MAC 0. */
static inline void arp_probe(uint8_t out[ARP_FRAME_LEN], const uint8_t mac[6], uint32_t target)
{
    for (unsigned i = 0; i < 6; i++) {
        out[i] = 0xff;            /* to everyone */
        out[6 + i] = mac[i];      /* from us */
        out[22 + i] = mac[i];     /* sender MAC */
        out[32 + i] = 0;          /* target MAC: unknown */
    }
    netframe_put_be16(out + 12, ARP_ETHERTYPE);
    netframe_put_be16(out + 14, 1);        /* Ethernet */
    netframe_put_be16(out + 16, 0x0800);   /* IPv4 */
    out[18] = 6;
    out[19] = 4;
    netframe_put_be16(out + 20, ARP_OP_REQUEST);
    arp_put_be32(out + 28, 0);             /* sender IP 0.0.0.0: a probe */
    arp_put_be32(out + 38, target);
}

/* Is the untagged frame (len bytes) an ARP reply from `target` to `mac`? */
static inline bool arp_is_reply(const uint8_t *f, size_t len, const uint8_t mac[6],
                                uint32_t target)
{
    if (len < ARP_FRAME_LEN || netframe_be16(f + 12) != ARP_ETHERTYPE)
        return false;
    if (netframe_be16(f + 14) != 1 || netframe_be16(f + 16) != 0x0800 || f[18] != 6 ||
        f[19] != 4 || netframe_be16(f + 20) != ARP_OP_REPLY || arp_be32(f + 28) != target)
        return false;
    for (unsigned i = 0; i < 6; i++)
        if (f[32 + i] != mac[i])
            return false;
    return true;
}
