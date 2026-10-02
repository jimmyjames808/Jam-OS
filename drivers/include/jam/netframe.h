/* <jam/netframe.h>: what a network driver may know about a frame, as pure
 * functions over its raw bytes (ARCHITECTURE.md "Networking";
 * docs/M9-PLAN.md "Where the VLAN tag is enforced").
 *
 * A NIC driver holds a dma_cap, so it parses as little as it can: a
 * frame's length and the bytes after the two addresses, never the
 * addresses and never the payload. Everything else is netstack's, which
 * holds no hardware. This header is that whole surface, shared by the
 * drivers and tested in utest (user/tests/utest/netframe.c) over
 * hand-made frames.
 *
 * Built so far: netframe_classify, which the listen-only probe
 * (drivers/rtl8125) uses to count frames by tag. Not built yet: the
 * transmit side (inserting the VLAN tag into the driver's own copy, and
 * the check of what netstack hands over); it comes with the first driver
 * that transmits.
 *
 * Layout (IEEE 802.3 and 802.1Q): bytes 0-5 destination, 6-11 source,
 * 12-13 the EtherType or a tag's TPID. A tag is 4 bytes: the TPID, then
 * the TCI (priority 3 bits, DEI 1 bit, VLAN id 12 bits); the frame's
 * EtherType follows it at bytes 16-17. All big-endian. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETFRAME_TPID_8021Q  0x8100u   /* a customer VLAN tag (802.1Q) */
#define NETFRAME_TPID_8021AD 0x88a8u   /* a service tag (802.1ad, "QinQ") */
#define NETFRAME_TPID_9100   0x9100u   /* the older, non-standard QinQ outer tag */

#define NETFRAME_HDR      14u      /* addresses and EtherType: the shortest frame read */
#define NETFRAME_TAGGED   18u      /* the same with one tag: the EtherType after it at 16 */
#define NETFRAME_VID_MASK 0x0fffu

/* What a frame is, by its tag. */
enum netframe_kind {
    NETFRAME_RUNT,        /* too short to read the bytes its kind needs */
    NETFRAME_UNTAGGED,    /* bytes 12-13 are the EtherType */
    NETFRAME_PRIORITY,    /* 802.1Q with VLAN id 0: a priority tag, no VLAN */
    NETFRAME_VLAN,        /* 802.1Q with VLAN id 1..4095 */
    NETFRAME_OUTER,       /* an outer (service) tag: 0x88a8 or 0x9100 */
};

struct netframe_class {
    enum netframe_kind kind;
    uint16_t tpid;        /* the tag's TPID (0: untagged or runt) */
    uint16_t vid;         /* the tag's VLAN id (0: none, or a priority tag) */
    uint8_t  pcp;         /* the tag's priority (0..7) */
    uint16_t ethertype;   /* the EtherType: bytes 12-13 untagged, 16-17 after one tag (for
                           * an outer tag, the inner tag's TPID as a rule); 0 for a runt */
};

static inline uint16_t netframe_be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Is v the TPID of a tag (one this header knows)? */
static inline bool netframe_is_tpid(uint16_t v)
{
    return v == NETFRAME_TPID_8021Q || v == NETFRAME_TPID_8021AD || v == NETFRAME_TPID_9100;
}

/* The frame's kind, read from its length and bytes 12-17 only (len is the
 * bytes there are at frame, a trailing CRC included or not). A runt is any
 * frame under 14 bytes, or a tagged one under 18. */
static inline struct netframe_class netframe_classify(const uint8_t *frame, size_t len)
{
    struct netframe_class c = { .kind = NETFRAME_RUNT };
    if (len < NETFRAME_HDR)
        return c;
    uint16_t type = netframe_be16(frame + 12);
    if (!netframe_is_tpid(type)) {
        c.kind = NETFRAME_UNTAGGED;
        c.ethertype = type;
        return c;
    }
    if (len < NETFRAME_TAGGED)
        return c;
    uint16_t tci = netframe_be16(frame + 14);
    c.tpid = type;
    c.vid = tci & NETFRAME_VID_MASK;
    c.pcp = (uint8_t)(tci >> 13);
    c.ethertype = netframe_be16(frame + 16);
    if (type != NETFRAME_TPID_8021Q)
        c.kind = NETFRAME_OUTER;
    else
        c.kind = c.vid ? NETFRAME_VLAN : NETFRAME_PRIORITY;
    return c;
}
