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
 * A driver runs in one network mode, fixed when it starts (the boot word
 * `vlan=`, <jam/netdev.h> netdev_vlan_args): tagged with a VLAN
 * (1..4094), untagged (NETFRAME_MODE_UNTAGGED), or off (anything else:
 * the driver leaves its card alone). Four parts:
 *   - netframe_classify: a frame's kind by its tag (the listen-only probe
 *     counts frames with it);
 *   - transmit, tagged: netframe_tag copies an untagged frame into the
 *     driver's own buffer with the VLAN tag inserted, and
 *     netframe_tx_check is the last look at that copy before its
 *     descriptor goes to the NIC; untagged: netframe_plain copies the
 *     frame as it is, refusing one that carries a tag, and
 *     netframe_tx_check_plain is the last look; netframe_tx_copy and
 *     netframe_tx_final pick by the mode;
 *   - receive: netframe_rx_check keeps only frames tagged with the
 *     configured VLAN, and netframe_untag copies one out without its tag;
 *     netframe_rx_check_plain keeps only untagged frames; netframe_rx_mode
 *     and netframe_rx_take pick by the mode.
 *
 * The transmit rule (ARCHITECTURE.md "Networking"): in a VLAN mode every
 * frame sent is tagged 802.1Q with TPID 0x8100, priority 0 and the
 * configured VLAN, and nothing else leaves; in the untagged mode every
 * frame sent is untagged (bytes 12-13 are never a tag's TPID), so
 * netstack can never choose a VLAN. The caller's frame is read once, byte
 * by byte, into the driver's copy; every check is made on that copy,
 * which only the driver can write, so the caller changing its frame
 * afterwards (netstack writing its ring again) changes nothing that was
 * checked.
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

/* ---- the VLAN ------------------------------------------------------------------ */

#define NETFRAME_VID_MIN  1u       /* VLAN 0 means "no VLAN" (a priority tag) */
#define NETFRAME_VID_MAX  4094u    /* 4095 is reserved (802.1Q) */

/* Is v a VLAN a frame may be sent on (1..4094)? */
static inline bool netframe_vlan_ok(uint32_t v)
{
    return v >= NETFRAME_VID_MIN && v <= NETFRAME_VID_MAX;
}

/* ---- the network mode ---------------------------------------------------------- */

/* A driver's mode is one number: 1..4094, tagged with that VLAN;
 * NETFRAME_MODE_UNTAGGED, plain untagged Ethernet; anything else (0
 * included) is off, and every function below refuses it. 0x1000 needs 13
 * bits, so no VLAN id (12 bits) and no tag's TCI field is ever it. */
#define NETFRAME_MODE_OFF      0u
#define NETFRAME_MODE_UNTAGGED 0x1000u

/* Is m a mode a driver may run the network in (a VLAN, or untagged)? */
static inline bool netframe_mode_ok(uint32_t m)
{
    return netframe_vlan_ok(m) || m == NETFRAME_MODE_UNTAGGED;
}

/* ---- transmit ------------------------------------------------------------------ */

#define NETFRAME_TAG_LEN  4u       /* TPID and TCI */
#define NETFRAME_MIN_IN   14u      /* an untagged frame handed over: addresses and EtherType */
#define NETFRAME_MAX_IN   1514u    /* ... and at most 1500 bytes of payload (no CRC) */
#define NETFRAME_MIN_OUT  18u      /* a tagged frame as it is checked and sent */
#define NETFRAME_MAX_OUT  1518u
/* Short frames are padded with zeros to this length after tagging: the
 * shortest Ethernet frame (60 bytes before the CRC) plus the tag, so a
 * switch that strips the tag still forwards a full-length frame. */
#define NETFRAME_PAD_OUT  64u

static inline void netframe_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* The last check before a frame goes to the NIC, made on the driver's own
 * copy: the len bytes at buf are a frame of 18..1518 bytes whose bytes
 * 12-13 are the 802.1Q TPID, 14-15 the tag of VLAN `vid` with priority 0
 * and DEI 0 exactly, and 16-17 not another tag. */
static inline bool netframe_tx_check(const uint8_t *buf, size_t len, uint16_t vid)
{
    if (!netframe_vlan_ok(vid) || len < NETFRAME_MIN_OUT || len > NETFRAME_MAX_OUT)
        return false;
    return netframe_be16(buf + 12) == NETFRAME_TPID_8021Q && netframe_be16(buf + 14) == vid &&
           !netframe_is_tpid(netframe_be16(buf + 16));
}

/* Copy the untagged frame `in` (len bytes: 14..1514, no CRC) into `out`
 * (cap bytes) with the tag of VLAN `vid` (priority 0) after the two
 * addresses, padded with zeros to NETFRAME_PAD_OUT. Each byte of `in` is
 * read once, and the result is checked on `out` (netframe_tx_check).
 * Returns the tagged frame's length, or 0: refused (a bad VLAN or length,
 * too small an `out`, or a frame that already carries a tag: EtherType
 * 0x8100, 0x88a8 or 0x9100). After a refusal `out` holds nothing that
 * passes netframe_tx_check. */
static inline size_t netframe_tag(uint8_t *out, size_t cap, const uint8_t *in, size_t len,
                                  uint16_t vid)
{
    if (!netframe_vlan_ok(vid) || len < NETFRAME_MIN_IN || len > NETFRAME_MAX_IN)
        return 0;
    size_t n = len + NETFRAME_TAG_LEN;
    if (n < NETFRAME_PAD_OUT)
        n = NETFRAME_PAD_OUT;
    if (!out || cap < n)
        return 0;
    for (size_t i = 0; i < 12; i++)
        out[i] = in[i];
    netframe_put_be16(out + 12, NETFRAME_TPID_8021Q);
    netframe_put_be16(out + 14, vid);
    for (size_t i = 12; i < len; i++)
        out[i + NETFRAME_TAG_LEN] = in[i];
    for (size_t i = len + NETFRAME_TAG_LEN; i < n; i++)
        out[i] = 0;   /* never what was in the buffer before */
    if (!netframe_tx_check(out, n, vid)) {
        netframe_put_be16(out + 12, 0);
        return 0;
    }
    return n;
}

/* ---- transmit, untagged ------------------------------------------------------- */

/* Short frames in the untagged mode are padded with zeros to this
 * length: the shortest Ethernet frame, before its CRC. */
#define NETFRAME_PAD_PLAIN 60u

/* The last check in the untagged mode, made on the driver's own copy: the
 * len bytes at buf are a frame of 60..1514 bytes whose bytes 12-13 are
 * not a tag's TPID (0x8100, 0x88a8, 0x9100): no frame leaves tagged. */
static inline bool netframe_tx_check_plain(const uint8_t *buf, size_t len)
{
    if (!buf || len < NETFRAME_PAD_PLAIN || len > NETFRAME_MAX_IN)
        return false;
    return !netframe_is_tpid(netframe_be16(buf + 12));
}

/* Copy the untagged frame `in` (len bytes: 14..1514, no CRC) into `out`
 * (cap bytes) as it is, padded with zeros to NETFRAME_PAD_PLAIN. Each
 * byte of `in` is read once, and the result is checked on `out`
 * (netframe_tx_check_plain). Returns the frame's length, or 0: refused (a
 * bad length, too small an `out`, or a frame that carries a tag: EtherType
 * 0x8100, 0x88a8 or 0x9100). After a refusal `out` holds nothing that
 * passes netframe_tx_check_plain or netframe_tx_check. */
static inline size_t netframe_plain(uint8_t *out, size_t cap, const uint8_t *in, size_t len)
{
    if (len < NETFRAME_MIN_IN || len > NETFRAME_MAX_IN)
        return 0;
    size_t n = len < NETFRAME_PAD_PLAIN ? NETFRAME_PAD_PLAIN : len;
    if (!out || cap < n)
        return 0;
    for (size_t i = 0; i < len; i++)
        out[i] = in[i];
    for (size_t i = len; i < n; i++)
        out[i] = 0;   /* never what was in the buffer before */
    if (!netframe_tx_check_plain(out, n)) {
        /* a tag of VLAN 0: refused by both last checks */
        netframe_put_be16(out + 12, NETFRAME_TPID_8021Q);
        netframe_put_be16(out + 14, 0);
        return 0;
    }
    return n;
}

/* netstack's frame into the driver's buffer for the driver's mode:
 * tagged with the VLAN (netframe_tag) or as it is (netframe_plain).
 * Returns the length to send, or 0: refused, and always 0 when the mode
 * is off. */
static inline size_t netframe_tx_copy(uint8_t *out, size_t cap, const uint8_t *in, size_t len,
                                      uint16_t mode)
{
    if (mode == NETFRAME_MODE_UNTAGGED)
        return netframe_plain(out, cap, in, len);
    return netframe_tag(out, cap, in, len, mode);   /* refuses anything but a VLAN */
}

/* The last check before a descriptor goes to the NIC, for the driver's
 * mode (netframe_tx_check or netframe_tx_check_plain); false when the
 * mode is off. */
static inline bool netframe_tx_final(const uint8_t *buf, size_t len, uint16_t mode)
{
    if (mode == NETFRAME_MODE_UNTAGGED)
        return netframe_tx_check_plain(buf, len);
    return netframe_tx_check(buf, len, mode);   /* false for anything but a VLAN */
}

/* ---- receive ------------------------------------------------------------------- */

/* What becomes of a received frame. */
enum netframe_rx {
    NETFRAME_RX_KEEP,         /* ours: tagged with the configured VLAN (passed on untagged), or
                               * in the untagged mode untagged */
    NETFRAME_RX_RUNT,         /* too short to read its tag (under 14, or tagged under 18) */
    NETFRAME_RX_LONG,         /* over 1518 bytes */
    NETFRAME_RX_UNTAGGED,     /* no tag (the switch's native VLAN), in a VLAN mode */
    NETFRAME_RX_PRIORITY,     /* tagged with VLAN 0 */
    NETFRAME_RX_OTHER_VLAN,   /* tagged with another VLAN (any VLAN, in the untagged mode) */
    NETFRAME_RX_OUTER,        /* an outer tag (0x88a8, 0x9100) */
    NETFRAME_RX_NESTED,       /* our VLAN, but another tag inside it */
    NETFRAME_RX_KINDS
};

/* Keep or drop a received frame of len bytes (its CRC not counted) for
 * VLAN `vid` (any priority, any DEI). Reads bytes 12-17 only. */
static inline enum netframe_rx netframe_rx_check(const uint8_t *frame, size_t len, uint16_t vid)
{
    if (len > NETFRAME_MAX_OUT)
        return NETFRAME_RX_LONG;
    struct netframe_class c = netframe_classify(frame, len);
    switch (c.kind) {
    case NETFRAME_UNTAGGED:
        return NETFRAME_RX_UNTAGGED;
    case NETFRAME_PRIORITY:
        return NETFRAME_RX_PRIORITY;
    case NETFRAME_OUTER:
        return NETFRAME_RX_OUTER;
    case NETFRAME_VLAN:
        if (c.vid != vid || !netframe_vlan_ok(vid))
            return NETFRAME_RX_OTHER_VLAN;
        return netframe_is_tpid(c.ethertype) ? NETFRAME_RX_NESTED : NETFRAME_RX_KEEP;
    case NETFRAME_RUNT:
    default:
        return NETFRAME_RX_RUNT;
    }
}

/* Copy the tagged frame `in` (len bytes, 18..1518) into `out` (cap bytes)
 * without its 4-byte tag. Returns the untagged length (len - 4), or 0 if
 * len is out of range or `out` too small. */
static inline size_t netframe_untag(uint8_t *out, size_t cap, const uint8_t *in, size_t len)
{
    if (len < NETFRAME_MIN_OUT || len > NETFRAME_MAX_OUT || !out ||
        cap < len - NETFRAME_TAG_LEN)
        return 0;
    for (size_t i = 0; i < 12; i++)
        out[i] = in[i];
    for (size_t i = 16; i < len; i++)
        out[i - NETFRAME_TAG_LEN] = in[i];
    return len - NETFRAME_TAG_LEN;
}

/* Keep or drop a received frame of len bytes (its CRC not counted) in the
 * untagged mode: only an untagged frame of 14..1514 bytes is kept. Every
 * tagged frame is dropped, a priority tag (VLAN 0) too: on an untagged
 * network nothing should tag a frame to us, and taking one would mean
 * taking a tag. Reads bytes 12-17 only. */
static inline enum netframe_rx netframe_rx_check_plain(const uint8_t *frame, size_t len)
{
    if (len > NETFRAME_MAX_OUT)
        return NETFRAME_RX_LONG;
    struct netframe_class c = netframe_classify(frame, len);
    switch (c.kind) {
    case NETFRAME_UNTAGGED:
        return len > NETFRAME_MAX_IN ? NETFRAME_RX_LONG : NETFRAME_RX_KEEP;
    case NETFRAME_PRIORITY:
        return NETFRAME_RX_PRIORITY;
    case NETFRAME_VLAN:
        return NETFRAME_RX_OTHER_VLAN;
    case NETFRAME_OUTER:
        return NETFRAME_RX_OUTER;
    case NETFRAME_RUNT:
    default:
        return NETFRAME_RX_RUNT;
    }
}

/* Keep or drop a received frame for the driver's mode
 * (netframe_rx_check_plain, or netframe_rx_check for its VLAN); with the
 * mode off nothing is kept. */
static inline enum netframe_rx netframe_rx_mode(const uint8_t *frame, size_t len, uint16_t mode)
{
    if (mode == NETFRAME_MODE_UNTAGGED)
        return netframe_rx_check_plain(frame, len);
    return netframe_rx_check(frame, len, mode);   /* keeps nothing unless mode is a VLAN */
}

/* Copy a frame netframe_rx_mode kept (len bytes at `in`) into `out` (cap
 * bytes) as netstack gets it: without its tag in a VLAN mode
 * (netframe_untag), as it is in the untagged mode (14..1514 bytes).
 * Returns the length copied, or 0 if len is out of range, `out` too
 * small, or the mode off. */
static inline size_t netframe_rx_take(uint8_t *out, size_t cap, const uint8_t *in, size_t len,
                                      uint16_t mode)
{
    if (mode != NETFRAME_MODE_UNTAGGED)
        return netframe_vlan_ok(mode) ? netframe_untag(out, cap, in, len) : 0;
    if (len < NETFRAME_MIN_IN || len > NETFRAME_MAX_IN || !out || cap < len)
        return 0;
    for (size_t i = 0; i < len; i++)
        out[i] = in[i];
    return len;
}
