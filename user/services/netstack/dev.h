/* netstack's network card: reaching its driver and moving frames through
 * the netdev rings (<jam/netdev.h>, abi/idl/netdev.idl).
 *
 * Two parts:
 * - connect.c, a thread of its own that serves nothing: asked by the loop,
 *   it calls devmgr (GET_SERVICE on a device channel init gave netstack)
 *   and the driver (netdev.info, netdev.open), each call with a deadline,
 *   and hands the session it opened back to the loop as one message with
 *   its handles. Those calls may wait seconds (a driver devmgr is
 *   restarting); the loop never waits for them (ARCHITECTURE.md "How a
 *   service waits").
 * - netif.c, in the loop: the session's rings mapped and attached
 *   (netdev_end), stack.h's edge on them (tx: netdev_room and netdev_put;
 *   the rx ring drained into stack_input), the to_stack event and the
 *   session channel watched on the loop's port, the link asked for with
 *   netdev.info without waiting (its reply read off the session channel).
 *   A session that ends (the driver died or ended) is dropped and the
 *   thread asked for a new one, after a backoff.
 *
 * The driver is trusted more than a program, but the rings are checked as
 * <jam/netdev.h> says: its counts clamped, each slot's length and flags
 * read once and checked, the frame copied before lwIP sees it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/netdev.h>
#include <os.h>

#define DEV_CARDS     4u                  /* devmgr device channels taken at the start */
#define DEV_CALL_WAIT (2 * NS_PER_S)      /* each of the thread's calls */
#define DEV_RETRY_MIN (250 * NS_PER_MS)   /* after a failed connect, doubling ... */
#define DEV_RETRY_MAX (5 * NS_PER_S)      /* ... up to this */
#define DEV_RX_BUDGET NETDEV_SLOTS        /* frames taken per turn of the loop */
#define DEV_STATS_AGAIN (2 * NS_PER_S)    /* a netdev.stats unanswered this long is sent again */

/* Port keys (main.c's are below 0x10). */
#define KEY_CONNECT  0x10u   /* the thread's channel: a session, or why none */
#define KEY_DEVMGR   0x11u   /* the first device channel closed: devmgr is gone */
#define KEY_SESSION  0x20u   /* | gen << 8: the session channel (replies, its end) */
#define KEY_EVENT    0x21u   /* | gen << 8: the to_stack event */

/* What the thread hands the loop (and its handles, when st is OK, in the
 * order of DEV_H_*). */
enum { DEV_H_SESSION, DEV_H_TX, DEV_H_RX, DEV_H_TO_DRIVER, DEV_H_TO_STACK, DEV_HANDLES };
struct dev_found {
    int32_t  st;          /* OK: a session; else why none (no driver yet, ...) */
    uint32_t card;        /* which device channel */
    uint8_t  mac[6];
    uint16_t vlan;
    uint32_t link;        /* NETDEV_LINK_* */
    uint32_t speed;       /* Mb/s */
    uint32_t changes;     /* the driver's link-change count */
    char     chip[16];    /* NUL-padded */
};

/* The card as netctl.device reports it. */
struct dev_report {
    bool     session;      /* a session is open */
    uint16_t vlan;
    uint32_t speed;        /* Mb/s, 0 while the link is down */
    uint32_t sessions;     /* sessions opened since netstack started */
    uint64_t ring_errors;  /* the driver's counts out of range (both rings) */
    uint64_t rx_bad;       /* rx slots refused: a bad length or flags */
    uint64_t tx_full;      /* frames dropped: the tx ring was full */
    char     chip[16];
};

struct dev {
    handle_t port;
    handle_t cards[DEV_CARDS];   /* devmgr's device channels for our cards */
    unsigned ncards;
    handle_t to_thread;          /* the loop's end of the thread's channel */
    bool     asked;              /* a connect is out (the thread is working on it) */
    uint64_t retry_at;           /* ns: when to ask again (0: not due) */
    uint64_t backoff;            /* ns: the last retry's delay */
    int32_t  last_st;            /* the last failure said in the log (said once) */
    /* the session, while open (session != 0) */
    handle_t session, tx_vmo, rx_vmo, to_driver, to_stack;
    void    *tx_map, *rx_map;
    struct netdev_end tx, rx;
    uint32_t gen;                /* the session's port keys' generation */
    bool     rx_pending;         /* the rx ring may hold frames */
    bool     tx_dirty;           /* frames put but not published */
    bool     info_out;           /* a netdev.info is out on the session channel */
    bool     stats_out;          /* a netdev.stats is out on it (dev_ask_stats) */
    uint32_t last_txid;          /* idl_txid_next's counter for the session channel */
    uint32_t stats_txid;
    uint64_t stats_at;           /* ns: when it went (one unanswered this long is asked again) */
    bool     link_up;            /* the link as last heard from the driver */
    uint32_t info_txid;
    uint32_t changes;            /* the link-change count last seen */
    struct dev_found found;      /* what the thread said about the card */
    struct dev_report rep;       /* counts for netctl.device */
};

/* ---- connect.c ------------------------------------------------------------- */

/* Start the thread; *to_thread: the loop's end of its channel (bound on
 * the port with KEY_CONNECT by the caller). */
status_t connect_start(struct dev *d);
/* Ask the thread for a session (one at a time). */
void     connect_ask(struct dev *d);

/* ---- netif.c --------------------------------------------------------------- */

/* Take the device channels from the startup message (SR_DEVMGR_DEVICE),
 * start the thread and ask for a session if there is a card. */
status_t dev_init(struct dev *d, handle_t port);
/* A packet with one of the keys above. */
void     dev_packet(struct dev *d, const struct port_packet *p);
/* Work left for the loop: frames in the rx ring, frames to publish, a
 * retry due. Returns the deadline for the next retry (DEADLINE_NEVER: none). */
uint64_t dev_work(struct dev *d);
/* Is there more to do at once (rx frames waiting)? */
bool     dev_pending(const struct dev *d);
/* netctl.device's answer. */
void     dev_get_report(const struct dev *d, struct dev_report *out);
/* Ask the driver for its counts (netdev.stats) without waiting; the answer
 * goes to dev_stats_done (once for every ask while one is out).
 * ERR_NOT_FOUND: no session; else the write's status. */
status_t dev_ask_stats(struct dev *d);
/* The driver's counts (NETDEV_STATS_SIZE bytes), or why there are none
 * (ERR_PEER_CLOSED: the session ended first; counts NULL). */
extern void (*dev_stats_done)(status_t st, const uint8_t *counts);
