/* <jam/netserver.h>: the netdev server every network driver links
 * (drivers/lib/netserver.c): abi/idl/netdev.idl on the driver's DR_SERVE
 * channel, and the session's rings and events of <jam/netdev.h>.
 *
 * It knows nothing of the chip. The driver hands it a struct srv_dev: a
 * send function (the driver's transmit path: the copy, the tag and the
 * last check are the driver's, never this file's), the free transmit
 * descriptors, the card's info and the counts only the driver has.
 * Received frames come in by srv_rx, already kept for the VLAN and
 * untagged by the driver. So utest (user/tests/utest/netsrv.c) runs this
 * file as it is against a fake device and a fake netstack.
 *
 * Its code is linked into each network driver's own object (the Makefile's
 * DRV_LIB_<driver>), so tools/checkdriver.py checks it as that driver's
 * code: it uses nothing but <jam/driver.h> and the inline headers.
 *
 * The loop's side (the service-loop rule): every wait is the driver's one
 * port. srv_packet takes the server's packets (DR_SERVE readable, the
 * session channel, netstack's NETDEV_SIG_TX) and only notes the work;
 * srv_work does it, each piece within a budget, and says whether more is
 * waiting (the loop then waits with no delay). Nothing in it blocks.
 *
 * One session at a time. `open` while a session's channel has a client
 * is ERR_BAD_STATE; a session whose opener has gone is ended first (so a
 * restarted netstack can open at once), and so is one whose channel
 * closes. Port keys carry a generation, so a packet of a session already
 * ended is ignored. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <idl/netdev.h>
#include <jam/driver.h>
#include <jam/netdev.h>

/* The server's port keys: kind | generation << 8 (the driver's own keys
 * stay below SRV_KEY_FIRST). */
#define SRV_KEY_FIRST   0x10u
#define SRV_KEY_SERVE   0x10u     /* DR_SERVE readable */
#define SRV_KEY_SESSION 0x11u     /* the session channel: readable or closed */
#define SRV_KEY_TX      0x12u     /* to_driver: NETDEV_SIG_TX */

/* netdev.info's answer, from the driver. */
struct srv_info {
    uint8_t  mac[6];
    uint16_t vlan;
    uint32_t link;                /* NETDEV_LINK_* */
    uint32_t speed;               /* Mb/s, 0 while down */
    uint32_t changes;             /* link changes since the driver started */
    char     chip[16];            /* NUL-padded */
};

/* What the server needs from the card's driver. */
struct srv_dev {
    /* Send one untagged frame of len bytes (14..1514; the driver copies,
     * tags and checks it): OK, ERR_INVALID_ARGS (refused: it carried a
     * tag), anything else (not sent: counted as refused too). */
    status_t (*send)(void *ctx, const uint8_t *frame, size_t len);
    /* Frames send would take now (free transmit descriptors). */
    uint32_t (*room)(void *ctx);
    void     (*info)(void *ctx, struct srv_info *out);
    /* The driver's own counts, added into s: the receive drops
     * (rx_untagged, rx_priority, rx_other_vlan, rx_bad), tx_done, and the
     * chip's (chip_counted and chip_*). */
    void     (*stats)(void *ctx, struct netdev_stats *s);
};

/* An open session: the driver's ends of everything open handed out. */
struct srv_session {
    handle_t ch;                  /* the session channel, our end */
    handle_t txv, rxv;            /* the ring VMOs */
    uint8_t *txmap, *rxmap;       /* mapped, NETDEV_RING_BYTES each */
    handle_t to_driver, to_stack; /* the events (full rights) */
    struct netdev_end tx, rx;     /* our view of the rings: we consume tx, produce rx */
};

struct srv {
    handle_t port;                /* the driver's port */
    handle_t serve;               /* DR_SERVE */
    const struct srv_dev *dev;
    void    *ctx;                 /* dev's */
    bool     open;                /* a session is open (s is valid) */
    uint32_t gen;                 /* the session's generation, in its port keys */
    struct srv_session s;
    /* work noted by srv_packet, done by srv_work */
    bool     serve_ready;         /* DR_SERVE may have requests */
    bool     session_ready;       /* the session channel may have requests (or closed) */
    bool     tx_ready;            /* the tx ring may have frames */
    bool     tx_blocked;          /* stopped for want of a descriptor: srv_tx_room resumes */
    bool     rx_dirty;            /* frames put into the rx ring, not yet published */
    bool     stopping;            /* DR_SERVE's clients are all gone */
    uint64_t ring_errors_past;    /* ring errors of sessions already ended */
    uint64_t tx_failed;           /* frames send refused for anything but their tag */
    struct netdev_stats st;       /* the server's own counts (stats fills the driver's) */
    uint8_t  frame[NETDEV_FRAME_MAX];   /* a tx ring frame, copied out before any check */
};

/* Bind DR_SERVE (`serve`) to `port` and start serving it. */
status_t srv_init(struct srv *v, handle_t port, handle_t serve, const struct srv_dev *dev,
                  void *ctx);
/* A port packet: true if it was the server's (stale ones included). */
bool     srv_packet(struct srv *v, const struct port_packet *p);
/* Do what the packets asked for, each within a budget. True: more is
 * waiting (wait no longer than now). v->stopping says DR_SERVE is gone. */
bool     srv_work(struct srv *v);
/* A received frame for netstack (untagged, 14..1514 bytes): into the rx
 * ring, or dropped and counted (no session; the ring full). */
void     srv_rx(struct srv *v, const uint8_t *frame, size_t len);
/* After a batch of srv_rx: publish the rx ring, wake netstack if it waits. */
void     srv_rx_done(struct srv *v);
/* Transmit descriptors came back: carry on with the tx ring. */
void     srv_tx_room(struct srv *v);
/* The link changed: NETDEV_SIG_LINK to the session. */
void     srv_link(struct srv *v);
/* The session ended (the driver is stopping): everything closed. */
void     srv_end(struct srv *v);
void     srv_log(const struct srv *v);
