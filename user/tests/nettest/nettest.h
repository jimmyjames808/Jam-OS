/* nettest: a network driver tested from user space as a hostile netstack
 * would use it (drivers/e1000e in QEMU; tools/net-test.sh runs it).
 *
 * It holds the NIC's netdev service through devmgr's control channel
 * (GET_SERVICE: test programs under user/tests/ may ask for it; netstack
 * gets it from init through the device channel), opens a session and
 * plays netstack, badly on purpose:
 *   nettest vlan  the session rules (one at a time, reopen after the
 *                 opener goes, open refused on the session channel, the
 *                 handles' rights), then the tx ring at its worst: every
 *                 bad length, flags, frames already tagged 0x8100, 0x88a8
 *                 and 0x9100, a produced count that jumps ahead or goes
 *                 back, and a thread rewriting the EtherType of the slots
 *                 while the driver copies them. Every refusal is counted
 *                 exactly; the host checks that every frame that left was
 *                 tagged VLAN 21 once (tools/net-test.sh: its peer and the
 *                 pcap);
 *   nettest rx    a receive census: the host's peer answers a "go" frame
 *                 with untagged, priority-tagged, other-VLAN, QinQ, nested,
 *                 too long and VLAN 21 frames; only the VLAN 21 ones arrive,
 *                 untagged and whole, and the driver's drop counts match
 *                 the peer's list exactly. Then a flood with the rx ring
 *                 left unread: the driver drops and counts (rx_ring_full)
 *                 and keeps answering;
 *   nettest off   a `vlan=off` boot: the driver finished at once, without
 *                 a service.
 * The summary goes to the RESULTS box; exit 0 if nothing failed. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/netdev.h>
#include <os.h>

#define E1K_VENDOR    0x8086u
#define E1K_DEVICE    0x10d3u
#define NT_ETHERTYPE  0x88b5u     /* IEEE 802 local experimental: our test frames */
#define NT_SOON       (5 * NS_PER_S)

/* The rx census the host's peer sends on "nettest-go 1" (tools/net-test.sh
 * builds the same list): what the driver must drop, by its counters, and
 * the VLAN frames it must pass. */
#define RX_UNTAGGED   5u
#define RX_PRIORITY   3u
#define RX_OTHER      12u         /* 6 other VLANs, 2 QinQ 0x88a8, 2 0x9100, 2 a tag inside 21 */
#define RX_LONG       1u          /* tagged 21, 1522 bytes: the chip drops it (RCTL.LPE is off),
                                   * so it never reaches the driver nor netstack */
#define RX_GOOD       10u
#define RX_FLOOD      300u        /* "nettest-go 2": VLAN 21 frames, the ring left unread */

/* One open session, mapped. */
struct sess {
    handle_t ch, tx, rx, to_driver, to_stack;
    void    *txm, *rxm;
    struct netdev_end txe, rxe;   /* we produce tx, consume rx */
};

extern handle_t dm;               /* devmgr's control channel */
extern handle_t nic;              /* the driver's service channel */
extern const char *cur;           /* the test running */
extern uint8_t mac[6];            /* the card's address */

static inline uint64_t soon(void) { return now() + NT_SOON; }

/* main.c */
status_t sess_open(struct sess *s);
void     sess_close(struct sess *s);
bool     get_stats(handle_t ch, struct netdev_stats *out);
bool     wait_link(void);
/* A test frame into buf: broadcast, from our address, NT_ETHERTYPE, then
 * the text `tag` and seq; len bytes (at least 14). */
void     frame_make(uint8_t *buf, uint32_t len, const char *tag, uint32_t seq);
/* Publish the tx ring and wake the driver (always: a hostile netstack may). */
void     tx_kick(struct sess *s);

/* tx.c and rx.c: the modes' tests */
bool t_session(void);
bool t_hostile(void);
bool t_rx_census(void);
bool t_rx_flood(void);
bool t_off(void);
