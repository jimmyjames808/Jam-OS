/* <jam/netdev.h>: between a network card's driver and netstack, the
 * `netdev` protocol's data path (docs/M9-PLAN.md "netdev: rings, not
 * calls"; the control calls are abi/idl/netdev.idl). A driver and
 * netstack implement it from this header and the IDL alone, and both use
 * the ring code below, so the index logic is written (and tested: utest's
 * netdev_*) once.
 *
 * ---- The model --------------------------------------------------------------
 * netdev.open gives the opener (netstack) a session channel, two ring
 * VMOs and two events. Frames travel only through the rings: no call and
 * no system call per frame while traffic flows.
 *
 *   tx ring   netstack produces, the driver consumes: plain Ethernet
 *             frames (no tag, no FCS), 14..1514 bytes. The driver copies
 *             each into its own DMA buffer, inserts the VLAN tag there and
 *             checks the copy (<jam/netframe.h>), then gives it to the chip.
 *   rx ring   the driver produces, netstack consumes: the frames the chip
 *             received tagged with the driver's VLAN, the tag removed
 *             (14..1514 bytes, no FCS). Untagged, priority-tagged and
 *             other-VLAN frames never get here (dropped and counted).
 *
 * Each ring is one VMO of NETDEV_RING_BYTES: a header page (struct
 * netdev_ring) then NETDEV_SLOTS slots of NETDEV_SLOT_SIZE bytes (struct
 * netdev_slot: a length, then the frame). The producer owns `produced`
 * and `producer_waits`, the consumer `consumed` and `consumer_waits`;
 * each writes only its own and reads the other's. The counts are frames
 * since the open and never wrap (2^64 frames is centuries at 2.5 Gb/s), so
 * frame f is in slot f % NETDEV_SLOTS, the ring is empty when produced ==
 * consumed and full when produced - consumed == NETDEV_SLOTS. A producer
 * writes a slot whole, then publishes `produced` with a release store; a
 * consumer reads `produced` with an acquire load, copies the frames out,
 * then publishes `consumed` (release), after which the producer may reuse
 * those slots.
 *
 * ---- Waking (the events) ------------------------------------------------------
 * Two events, one per waiter: `to_driver` (the driver waits on it,
 * netstack signals it) and `to_stack` (the other way). A side signals the
 * other only when that side said it is waiting, as the mixer's rings do:
 *
 *   NETDEV_SIG_TX       on to_driver: the tx ring has frames (netstack,
 *                       after producing, if tx's consumer_waits is set)
 *   NETDEV_SIG_RX       on to_stack: the rx ring has frames (the driver,
 *                       after producing, if rx's consumer_waits is set)
 *   NETDEV_SIG_TX_ROOM  on to_stack: the tx ring has room again (the
 *                       driver, after consuming, if tx's producer_waits
 *                       is set: netstack found it full)
 *   NETDEV_SIG_LINK     on to_stack: the link went up or down, or changed
 *                       speed (always signalled; netdev.info has the state
 *                       and a change count, so a change is never missed)
 *
 * No wake is lost: a waiter sets its flag, fences, and looks at the ring
 * again before it sleeps (netdev_sleep); a producer or consumer publishes
 * its count, fences, and then reads the flag (netdev_publish): one of the
 * two always sees the other's write. The waiter clears its event bits
 * (event_signal) BEFORE it looks at the ring, so a signal that comes
 * while it works is kept for the next wait. The driver never waits for
 * room in the rx ring: a full rx ring drops the frame and counts it
 * (rx_ring_full), so a stalled netstack can't stall the driver.
 *
 * A driver serving from one port (the service-loop rule) handles a
 * NETDEV_SIG_TX packet by clearing the bit and its flag (netdev_awake),
 * taking at most what netdev_ready says (at most NETDEV_SLOTS: one pass is
 * bounded however netstack writes), and then netdev_sleep; if that finds
 * frames again it signals NETDEV_SIG_TX to itself and goes back to its
 * port, so other work isn't starved by a busy ring.
 *
 * ---- Trust --------------------------------------------------------------------
 * netstack can write anything into both rings at any moment (it maps
 * them writable), so the driver treats every byte of them as hostile:
 *   - it keeps its own count of what it consumed or produced
 *     (netdev_end.count) and never reads its own field back from the ring;
 *   - netstack's count is read once per pass and clamped to the ring:
 *     a tx `produced` that went backwards is no frames, one more than
 *     NETDEV_SLOTS ahead is NETDEV_SLOTS frames; an rx `consumed` ahead of
 *     what was produced, or too far behind, is no room (each counted in
 *     netdev_end.errors and the driver's ring_errors);
 *   - a slot's length and flags are read once, checked (NETDEV_FRAME_MIN
 *     to NETDEV_FRAME_MAX, flags 0), and only then are that many bytes
 *     copied into the driver's own buffer, which netstack can't see; the
 *     frame's checks run on the copy, so changing the slot afterwards
 *     changes nothing. A bad slot is skipped and counted, never sent.
 * netstack treats the rx ring the same way: the driver is trusted more,
 * but a bad count or length must not crash netstack.
 *
 * ---- Who holds what (the rights) ------------------------------------------------
 * The driver makes both ring VMOs (ordinary memory, never DMA memory:
 * the chip never sees them) and maps them; its DMA rings and buffers
 * stay its own. netstack gets: each ring with NETDEV_RING_RIGHTS (read,
 * write, map: no RIGHT_RESIZE, so the rings can't shrink under the
 * driver's mapping, and no RIGHT_DUPLICATE); `to_driver` with
 * NETDEV_TO_DRIVER_RIGHTS (signal only); `to_stack` with
 * NETDEV_TO_STACK_RIGHTS (wait, and signal to clear its bits); the
 * session channel. Every handle also arrives with RIGHT_TRANSFER: an IDL
 * reply moves handles with channel_write, which keeps their rights. So
 * netstack could hand a ring on; whoever got it would have netstack's
 * power over that ring and no more (the driver checks everything). netstack
 * holds no dma_cap and no hardware handle; it reaches the driver only
 * through the device channel init gives it (ARCHITECTURE.md "Networking").
 *
 * ---- Ending ------------------------------------------------------------------
 * Closing the session channel ends the session: the driver stops reading
 * the tx ring, unmaps and closes both rings and events, and drops what it
 * receives (rx_no_session) until the next open. A driver that ends (or
 * dies) closes its end: netstack sees ERR_PEER_CLOSED / SIG_PEER_CLOSED on
 * the session, unmaps, closes everything and asks its devmgr device
 * channel for the service again (the reconnect rule), then opens anew.
 * Rings are never reused across sessions: each open makes new ones with
 * both counts 0.
 *
 * ---- The VLAN ------------------------------------------------------------------
 * A network driver is started by devmgr with the word `vlan=<id>`
 * (netdev_vlan_word) when the boot has a VLAN; without a valid one it
 * turns on neither receiver nor transmitter, logs "no VLAN: the network
 * stays off" and ends cleanly. netdev.info reports the VLAN: netstack
 * learns it from the driver and from nobody else. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/status.h>

/* ---- sizes ------------------------------------------------------------------ */

#define NETDEV_SLOTS       256u      /* a power of two */
#define NETDEV_SLOT_SIZE   2048u
#define NETDEV_SLOT_DATA   16u       /* the frame's offset in its slot */
#define NETDEV_RING_HDR    4096u     /* the header page; the slots follow */
#define NETDEV_RING_BYTES  (NETDEV_RING_HDR + NETDEV_SLOTS * NETDEV_SLOT_SIZE)   /* 516 KiB */
#define NETDEV_MTU         1500u     /* the largest payload */
#define NETDEV_FRAME_MIN   14u       /* the addresses and the EtherType */
#define NETDEV_FRAME_MAX   1514u     /* NETDEV_MTU + 14: untagged, no FCS */
#define NETDEV_RING_MAGIC  0x4752444eu   /* "NDRG" */
#define NETDEV_RING_TX     1u        /* netstack -> the driver */
#define NETDEV_RING_RX     2u        /* the driver -> netstack */

/* The events' bits (SIG_USER_ALL, set and cleared with event_signal). */
#define NETDEV_SIG_TX      (1u << 24)   /* on to_driver */
#define NETDEV_SIG_RX      (1u << 24)   /* on to_stack */
#define NETDEV_SIG_TX_ROOM (1u << 25)   /* on to_stack */
#define NETDEV_SIG_LINK    (1u << 26)   /* on to_stack */

/* The rights each handle of netdev.open's reply has (RIGHT_TRANSFER: an
 * IDL reply moves handles; see "Who holds what"). */
#define NETDEV_RING_RIGHTS      (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER)
#define NETDEV_TO_DRIVER_RIGHTS (RIGHT_SIGNAL | RIGHT_TRANSFER)
#define NETDEV_TO_STACK_RIGHTS  (RIGHT_WAIT | RIGHT_SIGNAL | RIGHT_TRANSFER)

/* netdev.info's `link` bits; `speed` is in Mb/s (0 while down). */
#define NETDEV_LINK_UP     (1u << 0)
#define NETDEV_LINK_FULL   (1u << 1)    /* full duplex */

/* ---- the shared layout ---------------------------------------------------------- */

/* A ring's header page. The driver writes the first line once, before it
 * hands the ring out; the other two lines are the two sides' own. */
struct netdev_ring {
    uint32_t magic;            /* NETDEV_RING_MAGIC */
    uint32_t kind;             /* NETDEV_RING_TX or NETDEV_RING_RX */
    uint32_t slots;            /* NETDEV_SLOTS */
    uint32_t slot_size;        /* NETDEV_SLOT_SIZE */
    uint32_t reserved0[12];    /* 0 */
    /* the producer's line */
    uint64_t produced;         /* frames written since the open */
    uint32_t producer_waits;   /* 1: it found the ring full and waits for room */
    uint32_t reserved1[13];    /* 0 */
    /* the consumer's line */
    uint64_t consumed;         /* frames taken since the open */
    uint32_t consumer_waits;   /* 1: it found the ring empty and waits for frames */
    uint32_t reserved2[13];    /* 0 */
};
#define NETDEV_RING_PRODUCER 64u    /* offset of the producer's line */
#define NETDEV_RING_CONSUMER 128u   /* offset of the consumer's line */
_Static_assert(sizeof(struct netdev_ring) == 192, "netdev_ring is three cache lines");
_Static_assert(offsetof(struct netdev_ring, produced) == NETDEV_RING_PRODUCER, "producer line");
_Static_assert(offsetof(struct netdev_ring, consumed) == NETDEV_RING_CONSUMER, "consumer line");

/* One slot: what a producer writes. */
struct netdev_slot {
    uint32_t len;              /* the frame's bytes: NETDEV_FRAME_MIN..NETDEV_FRAME_MAX */
    uint32_t flags;            /* 0; a consumer refuses a slot with anything else */
    uint64_t reserved;         /* 0 (not checked) */
    uint8_t  frame[NETDEV_SLOT_SIZE - NETDEV_SLOT_DATA];
};
_Static_assert(sizeof(struct netdev_slot) == NETDEV_SLOT_SIZE, "netdev_slot fills its slot");
_Static_assert(offsetof(struct netdev_slot, frame) == NETDEV_SLOT_DATA, "frame offset");
_Static_assert((NETDEV_SLOTS & (NETDEV_SLOTS - 1)) == 0, "NETDEV_SLOTS is a power of two");

/* netdev.stats' `counts` (a u8[256] in the IDL): the driver's counts since
 * it started (not per session) and the chip's own. */
struct netdev_stats {
    /* receive */
    uint64_t rx_frames;        /* put into the rx ring */
    uint64_t rx_bytes;         /* their bytes, the tag removed */
    uint64_t rx_untagged;      /* dropped: no tag */
    uint64_t rx_priority;      /* dropped: tagged with VLAN 0 (a priority tag) */
    uint64_t rx_other_vlan;    /* dropped: another VLAN, or an outer (QinQ) tag */
    uint64_t rx_bad;           /* dropped: a runt, too long, or an error the chip flagged */
    uint64_t rx_ring_full;     /* dropped: the rx ring was full (netstack is behind) */
    uint64_t rx_no_session;    /* dropped: no session open */
    /* transmit */
    uint64_t tx_frames;        /* tagged and given to the chip */
    uint64_t tx_bytes;         /* their bytes, the tag included */
    uint64_t tx_bad_len;       /* refused: length outside NETDEV_FRAME_MIN..MAX */
    uint64_t tx_bad_tag;       /* refused: its EtherType is a tag (netframe_is_tpid) */
    uint64_t tx_bad_flags;     /* refused: a slot's flags were not 0 */
    uint64_t tx_done;          /* the chip reported sent */
    /* the rings and the link */
    uint64_t ring_errors;      /* netstack's counts out of range (clamped) */
    uint64_t link_changes;     /* as netdev.info's `changes` */
    uint64_t sessions;         /* netdev.open calls answered OK */
    /* the chip's own counters (0 unless its bit is in chip_counted) */
    uint64_t chip_counted;     /* NETDEV_CHIP_* bits: which of these the chip has */
    uint64_t chip_tx_ok;       /* frames sent */
    uint64_t chip_rx_ok;       /* frames received (every VLAN, before the driver's filter) */
    uint64_t chip_tx_err;      /* send errors (collisions, aborts) */
    uint64_t chip_rx_err;      /* receive errors (CRC, alignment) */
    uint64_t chip_rx_missed;   /* frames lost for want of a descriptor */
    uint64_t reserved[9];      /* 0 */
};
#define NETDEV_CHIP_TX_OK     (1u << 0)
#define NETDEV_CHIP_RX_OK     (1u << 1)
#define NETDEV_CHIP_TX_ERR    (1u << 2)
#define NETDEV_CHIP_RX_ERR    (1u << 3)
#define NETDEV_CHIP_RX_MISSED (1u << 4)
#define NETDEV_STATS_SIZE     256u
_Static_assert(sizeof(struct netdev_stats) == NETDEV_STATS_SIZE, "netdev.stats' u8[256]");

/* ---- the VLAN word ---------------------------------------------------------------- */

/* The VLAN id in a driver's start word "vlan=<id>" (1..4094, 1 to 4
 * decimal digits), or 0 if `word` is anything else (NULL included). */
static inline uint16_t netdev_vlan_word(const char *word)
{
    if (!word || word[0] != 'v' || word[1] != 'l' || word[2] != 'a' || word[3] != 'n' ||
        word[4] != '=')
        return 0;
    uint32_t id = 0;
    unsigned n = 0;
    for (const char *p = word + 5; *p; p++, n++) {
        if (n == 4 || *p < '0' || *p > '9')
            return 0;
        id = id * 10 + (uint32_t)(*p - '0');
    }
    return n && id >= 1 && id <= 4094 ? (uint16_t)id : 0;
}

/* The VLAN of a driver's n start words: the one vlan= word's id; 0 (no
 * VLAN: stay down) if there is none, one isn't valid, or two disagree. */
static inline uint16_t netdev_vlan_args(const char *const *args, uint32_t n)
{
    uint16_t vlan = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *w = args[i];
        if (!w || w[0] != 'v' || w[1] != 'l' || w[2] != 'a' || w[3] != 'n')
            continue;
        uint16_t id = netdev_vlan_word(w);
        if (!id || (vlan && id != vlan))
            return 0;
        vlan = id;
    }
    return vlan;
}

/* ---- the ring logic, both sides --------------------------------------------------- */

/* Pure: frames a consumer may take, from the producer's count as read
 * (untrusted) and its own (trusted). Clamped to the ring; *bad set when
 * `produced` was out of range (went backwards, or more than a ring ahead). */
static inline uint32_t netdev_ring_ready(uint64_t produced, uint64_t consumed, bool *bad)
{
    if (produced < consumed) {
        *bad = true;
        return 0;
    }
    if (produced - consumed > NETDEV_SLOTS) {
        *bad = true;
        return NETDEV_SLOTS;
    }
    return (uint32_t)(produced - consumed);
}

/* Pure: slots a producer may fill, from its own count (trusted) and the
 * consumer's as read (untrusted). No room, and *bad set, when `consumed`
 * is ahead of `produced` or more than a ring behind it. */
static inline uint32_t netdev_ring_room(uint64_t produced, uint64_t consumed, bool *bad)
{
    if (consumed > produced || produced - consumed > NETDEV_SLOTS) {
        *bad = true;
        return 0;
    }
    return NETDEV_SLOTS - (uint32_t)(produced - consumed);
}

/* One side's private view of one ring: never in shared memory. */
struct netdev_end {
    struct netdev_ring *hdr;   /* the mapped header page (the peer may write any of it) */
    uint8_t  *base;            /* the mapping's start: slot i is at base + NETDEV_RING_HDR + ... */
    bool      producer;        /* this side produces (else it consumes) */
    uint64_t  count;           /* frames this side produced or consumed: the only copy it trusts */
    uint64_t  errors;          /* times the peer's count was out of range */
};

/* The slot holding frame f. */
static inline struct netdev_slot *netdev_slot_of(const struct netdev_end *e, uint64_t f)
{
    return (struct netdev_slot *)(e->base + NETDEV_RING_HDR +
                                  (size_t)(f & (NETDEV_SLOTS - 1)) * NETDEV_SLOT_SIZE);
}

/* The driver: set up a ring it just made and mapped (NETDEV_RING_BYTES at
 * map, zeroed as a new VMO is): the header's first line, both counts 0. */
static inline void netdev_end_make(struct netdev_end *e, void *map, uint32_t kind, bool producer)
{
    struct netdev_ring *h = (struct netdev_ring *)map;
    h->magic = NETDEV_RING_MAGIC;
    h->kind = kind;
    h->slots = NETDEV_SLOTS;
    h->slot_size = NETDEV_SLOT_SIZE;
    *e = (struct netdev_end){ .hdr = h, .base = (uint8_t *)map, .producer = producer };
}

/* netstack: take on a ring the driver made, mapped at map (at least
 * NETDEV_RING_BYTES). false (leave it) if its header is not this
 * layout or not the expected kind. */
static inline bool netdev_end_attach(struct netdev_end *e, void *map, uint32_t kind, bool producer)
{
    const volatile struct netdev_ring *h = (const volatile struct netdev_ring *)map;
    if (h->magic != NETDEV_RING_MAGIC || h->kind != kind || h->slots != NETDEV_SLOTS ||
        h->slot_size != NETDEV_SLOT_SIZE)
        return false;
    *e = (struct netdev_end){ .hdr = (struct netdev_ring *)map, .base = (uint8_t *)map,
                              .producer = producer };
    return true;
}

/* Consumer: frames waiting now (the producer's count read once, clamped). */
static inline uint32_t netdev_ready(struct netdev_end *e)
{
    bool bad = false;
    uint64_t p = __atomic_load_n(&e->hdr->produced, __ATOMIC_ACQUIRE);
    uint32_t n = netdev_ring_ready(p, e->count, &bad);
    e->errors += bad;
    return n;
}

/* Consumer: copy the next frame into dst (cap bytes) and move past it,
 * good or bad (call only when netdev_ready said there is one). Its length
 * and flags are read once. OK: *len bytes copied. ERR_OUT_OF_RANGE: the
 * length was outside NETDEV_FRAME_MIN..NETDEV_FRAME_MAX or over cap;
 * ERR_INVALID_ARGS: the flags were not 0; nothing copied, *len is the
 * length read. The copy may see the producer change the slot meanwhile:
 * check the frame in dst, never in the slot. */
static inline status_t netdev_take(struct netdev_end *e, void *dst, uint32_t cap, uint32_t *len)
{
    struct netdev_slot *s = netdev_slot_of(e, e->count);
    uint32_t n = __atomic_load_n(&s->len, __ATOMIC_RELAXED);
    uint32_t flags = __atomic_load_n(&s->flags, __ATOMIC_RELAXED);
    e->count++;
    *len = n;
    if (flags)
        return ERR_INVALID_ARGS;
    if (n < NETDEV_FRAME_MIN || n > NETDEV_FRAME_MAX || n > cap)
        return ERR_OUT_OF_RANGE;
    __builtin_memcpy(dst, s->frame, n);
    return OK;
}

/* Producer: room now (the consumer's count read once; none if out of
 * range). */
static inline uint32_t netdev_room(struct netdev_end *e)
{
    bool bad = false;
    uint64_t c = __atomic_load_n(&e->hdr->consumed, __ATOMIC_ACQUIRE);
    uint32_t n = netdev_ring_room(e->count, c, &bad);
    e->errors += bad;
    return n;
}

/* Producer: the next slot's frame area, to write a frame into in place
 * (up to NETDEV_FRAME_MAX bytes); netdev_commit then makes it a frame.
 * Call only when netdev_room said there is room. */
static inline uint8_t *netdev_slot_frame(struct netdev_end *e)
{
    return netdev_slot_of(e, e->count)->frame;
}

/* Producer: the frame written at netdev_slot_frame is len bytes
 * (NETDEV_FRAME_MIN..NETDEV_FRAME_MAX; the caller checked). Not yet
 * visible: netdev_publish shows every committed frame at once. */
static inline void netdev_commit(struct netdev_end *e, uint32_t len)
{
    struct netdev_slot *s = netdev_slot_of(e, e->count);
    s->len = len;
    s->flags = 0;
    s->reserved = 0;
    e->count++;
}

/* Producer: copy a frame of len bytes into the next slot and commit it. */
static inline void netdev_put(struct netdev_end *e, const void *frame, uint32_t len)
{
    __builtin_memcpy(netdev_slot_frame(e), frame, len);
    netdev_commit(e, len);
}

/* Either side: publish this side's count (release: the slots it filled or
 * emptied are done), then read the peer's waiting flag after a full fence.
 * true: signal the peer (the consumer's bit after producing, the
 * producer's after consuming). */
static inline bool netdev_publish(struct netdev_end *e)
{
    uint64_t *mine = e->producer ? &e->hdr->produced : &e->hdr->consumed;
    uint32_t *peer = e->producer ? &e->hdr->consumer_waits : &e->hdr->producer_waits;
    __atomic_store_n(mine, e->count, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(peer, __ATOMIC_RELAXED) != 0;
}

/* Either side, about to wait (a consumer for frames, a producer for
 * room): raise this side's flag, fence, and look again. true: nothing
 * yet, wait for the peer's signal (the flag stays up); false: there is
 * work after all (the flag is down again). */
static inline bool netdev_sleep(struct netdev_end *e)
{
    uint32_t *flag = e->producer ? &e->hdr->producer_waits : &e->hdr->consumer_waits;
    __atomic_store_n(flag, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if ((e->producer ? netdev_room(e) : netdev_ready(e)) == 0)
        return true;
    __atomic_store_n(flag, 0, __ATOMIC_RELAXED);
    return false;
}

/* Either side, woken (or working anyway): lower this side's flag, so the
 * peer stops signalling while this side is busy. */
static inline void netdev_awake(struct netdev_end *e)
{
    __atomic_store_n(e->producer ? &e->hdr->producer_waits : &e->hdr->consumer_waits, 0,
                     __ATOMIC_RELAXED);
}
