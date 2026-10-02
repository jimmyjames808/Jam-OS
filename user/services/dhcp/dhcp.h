/* dhcp: the DHCP client (RFC 2131, options RFC 2132), split so that
 * everything that parses the network's bytes or keeps the protocol's
 * state is a pure library utest can drive with no network at all:
 *
 *   msg.c     building the client's messages and parsing the server's
 *             (OFFER, ACK, NAK), every length checked against the bytes
 *             that arrived;
 *   client.c  the state machine (RFC 2131 figure 5): its timers are
 *             deadlines the caller's loop waits for, never sleeps;
 *   main.c    the process: the loop and the edge (struct dhcp_io).
 *
 * The edge is all the client does to the world: send a datagram from
 * port 68 to port 67, configure or clear the address, ask for an ARP
 * probe, draw random numbers. The library calls it and never blocks; the
 * caller's loop feeds it what arrives (dhcp_input), the time
 * (dhcp_tick at dhcp_deadline) and the ARP probe's answer
 * (dhcp_probe_done). Every call takes `now`, nanoseconds of uptime.
 *
 * Addresses are host-order uint32_t (<netbytes.h>). */
#pragma once

#include <netbytes.h>
#include <os.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_BROADCAST   0xffffffffu   /* 255.255.255.255 */
#define DHCP_INFINITE    0xffffffffu   /* a lease time that never ends (RFC 2132 9.2) */
#define DHCP_MAX_DNS     3u            /* DNS servers kept from option 6 */
#define DHCP_MSG_BUILT   300u          /* every message the client builds is this long */
#define DHCP_HOSTNAME    "jamos"       /* option 12 */
#define DHCP_MAX_MSG     1500u         /* option 57: an MTU-sized IP datagram */

/* The fixed part of a BOOTP/DHCP message (RFC 2131 figure 1), then the
 * magic cookie, then the options. */
#define DHCP_OFF_OP      0
#define DHCP_OFF_HTYPE   1
#define DHCP_OFF_HLEN    2
#define DHCP_OFF_XID     4
#define DHCP_OFF_SECS    8
#define DHCP_OFF_FLAGS   10
#define DHCP_OFF_CIADDR  12
#define DHCP_OFF_YIADDR  16
#define DHCP_OFF_CHADDR  28
#define DHCP_OFF_SNAME   44
#define DHCP_OFF_FILE    108
#define DHCP_OFF_COOKIE  236
#define DHCP_OFF_OPTIONS 240
#define DHCP_SNAME_LEN   64
#define DHCP_FILE_LEN    128
#define DHCP_COOKIE      0x63825363u   /* 99.130.83.99 */
#define DHCP_FLAG_BROADCAST 0x8000u    /* "answer by broadcast": we have no address yet */
#define BOOTREQUEST      1
#define BOOTREPLY        2
#define HTYPE_ETHER      1

/* Option 53's message types. */
enum dhcp_type {
    DHCP_DISCOVER = 1,
    DHCP_OFFER    = 2,
    DHCP_REQUEST  = 3,
    DHCP_DECLINE  = 4,
    DHCP_ACK      = 5,
    DHCP_NAK      = 6,
    DHCP_RELEASE  = 7,
};

/* A message the client sends. */
struct dhcp_out {
    uint8_t  type;        /* enum dhcp_type: DISCOVER, REQUEST, DECLINE or RELEASE */
    uint32_t xid;
    uint16_t secs;        /* seconds since the exchange began */
    bool     broadcast;   /* set the broadcast flag (no address yet) */
    uint32_t ciaddr;      /* our address, when we have one (renew, rebind, release) */
    uint32_t requested;   /* option 50, 0: none */
    uint32_t server;      /* option 54, 0: none */
};

/* What a server's OFFER, ACK or NAK said, checked (msg.c's header). */
struct dhcp_reply {
    uint8_t  type;                 /* DHCP_OFFER, DHCP_ACK or DHCP_NAK */
    uint32_t yiaddr;               /* the address (0 in a NAK) */
    uint32_t server;               /* option 54: always there */
    uint32_t mask;                 /* option 1, or the address class's own */
    uint32_t router;               /* option 3's first usable one, 0: none */
    uint32_t dns[DHCP_MAX_DNS];    /* option 6's usable ones */
    uint8_t  ndns;
    uint32_t lease_s, t1_s, t2_s;  /* options 51, 58, 59, seconds; 0: absent */
};

/* Build a message for this client (mac) into buf (at least
 * DHCP_MSG_BUILT bytes): returns its length, or 0 if cap is too small
 * or the type isn't one a client sends. */
size_t   dhcp_build(uint8_t *buf, size_t cap, const uint8_t mac[6], const struct dhcp_out *m);

/* Parse a datagram that arrived on port 68. OK: *out is an OFFER, ACK or
 * NAK for transaction xid and this mac, its addresses checked.
 * ERR_NOT_FOUND: not for us (not a reply, another xid or hardware
 * address); ERR_NOT_SUPPORTED: a well-formed message of another type (or
 * BOOTP, with no type); ERR_INVALID_ARGS: malformed (too short, no magic
 * cookie, an option running past its field, a wrong option length, a
 * missing required option, an address that can't be one). *out is
 * written only on OK. */
status_t dhcp_parse(const void *msg, size_t len, const uint8_t mac[6], uint32_t xid,
                    struct dhcp_reply *out);

/* The mask an address's class implies (RFC 1122 3.3.1), for an OFFER or
 * ACK without option 1: /8, /16 or /24. */
uint32_t dhcp_class_mask(uint32_t addr);
/* addr can be a host's own unicast address (not 0/8, 127/8, multicast,
 * reserved or broadcast). */
bool     dhcp_unicast(uint32_t addr);

/* ---- the client ---------------------------------------------------------- */

/* The lease the edge applies (netctl's set_ipv4 and set_dns). */
struct dhcp_lease {
    uint32_t addr, mask, router;   /* router 0: none */
    uint32_t server;               /* the DHCP server that gave it */
    uint32_t dns[DHCP_MAX_DNS];
    uint8_t  ndns;
    uint32_t lease_s;              /* DHCP_INFINITE: never ends */
    uint32_t t1_s, t2_s;           /* renew and rebind after this many seconds */
};

/* Why the address went away (dhcp_io.unbound). */
enum dhcp_why {
    DHCP_WHY_EXPIRED,    /* the lease ended with no server answering */
    DHCP_WHY_NAK,        /* the server said no (renewing, rebinding or rebooting) */
    DHCP_WHY_STOPPED,    /* dhcp_stop or dhcp_start */
};

/* The edge, all of it (main.c fills it; utest's is a script). */
struct dhcp_io {
    void *ctx;   /* passed to each call */
    /* Send msg from port 68 to port 67 of `to`: DHCP_BROADCAST, or a
     * server's address. A failure is counted and covered by the
     * retransmit timer, as a lost datagram would be. */
    status_t (*send)(void *ctx, uint32_t to, const void *msg, size_t len);
    /* The lease is ours: configure it. Called again when a renewal
     * changes anything but the times. */
    void (*bound)(void *ctx, const struct dhcp_lease *l);
    /* The address is gone: unconfigure it. Called only after bound. */
    void (*unbound)(void *ctx, enum dhcp_why why);
    /* Optional (NULL: no probe): start an RFC 5227 ARP probe of addr and
     * answer with dhcp_probe_done. Without an answer by DHCP_PROBE_WAIT
     * the address is taken as free. */
    void (*probe)(void *ctx, uint32_t addr);
    /* 32 random bits: transaction ids and the retransmit jitter. */
    uint32_t (*random)(void *ctx);
};

enum dhcp_state {
    DHCP_STOPPED,      /* nothing to do until dhcp_start */
    DHCP_INIT,         /* waiting to send a DISCOVER (after a NAK or a DECLINE) */
    DHCP_SELECTING,    /* DISCOVER sent, waiting for an OFFER */
    DHCP_REQUESTING,   /* REQUEST sent for an offer, waiting for its ACK */
    DHCP_REBOOTING,    /* REQUEST sent for the address we had (INIT-REBOOT) */
    DHCP_PROBING,      /* ACKed, the edge's ARP probe running */
    DHCP_BOUND,
    DHCP_RENEWING,     /* past T1: REQUEST to our server */
    DHCP_REBINDING,    /* past T2: REQUEST to any server */
};

/* Timing (RFC 2131 4.1, 4.4.5; RFC 5227 2.1). */
#define DHCP_RETX_FIRST_MS   4000u    /* the first retransmit, doubling ... */
#define DHCP_RETX_MAX_MS     64000u   /* ... up to this */
#define DHCP_JITTER_MS       1000u    /* each retransmit +- up to this */
#define DHCP_REQUEST_TRIES   4u       /* REQUESTs in REQUESTING/REBOOTING before starting over */
#define DHCP_RENEW_MIN_MS    60000u   /* retransmits while renewing/rebinding, at least */
#define DHCP_DECLINE_WAIT_MS 10000u   /* after a DECLINE, before the next DISCOVER */
#define DHCP_NAK_WAIT_MS     2000u    /* after a NAK while requesting, doubling up to RETX_MAX */
#define DHCP_PROBE_WAIT_MS   10000u   /* the edge's probe answers by then, or the address is free */
#define DHCP_MIN_LEASE_S     10u      /* a shorter lease is taken as this (no renewal storm) */

/* Counts, for `net stats`. */
struct dhcp_stats {
    uint32_t sent, send_failed;
    uint32_t received, malformed, foreign, ignored;   /* foreign: not ours; ignored: wrong now */
    uint32_t offers, acks, naks, declines;
    uint32_t leases;   /* bound was called: each new lease, each changed renewal */
};

struct dhcp_client {
    const struct dhcp_io *io;
    uint8_t  mac[6];
    enum dhcp_state state;
    uint32_t xid;                /* the exchange's transaction id */
    uint64_t deadline;           /* when dhcp_tick has work; DEADLINE_NEVER: none */
    uint64_t started;            /* the exchange's first send (the secs field) */
    uint64_t sent_at;            /* the exchange's first REQUEST: a lease's times count from it */
    uint32_t retx_ms;            /* the next retransmit's interval */
    uint32_t tries;              /* REQUESTs sent in REQUESTING/REBOOTING */
    uint32_t nak_wait_ms;        /* the next wait after a NAK; reset when bound */
    struct dhcp_reply offer;     /* REQUESTING/PROBING: what we asked for */
    bool     have_lease;         /* lease is applied (bound was called, unbound not since) */
    struct dhcp_lease lease;
    uint64_t t1_at, t2_at, end_at;   /* the lease's times, ns uptime (DEADLINE_NEVER: infinite) */
    struct dhcp_stats stats;
};

/* Set c up for this edge and hardware address; state STOPPED. */
void     dhcp_init(struct dhcp_client *c, const struct dhcp_io *io, const uint8_t mac[6]);
/* Start (or start over): any applied lease is cleared (unbound,
 * DHCP_WHY_STOPPED). With last_addr (the address of an earlier lease, 0:
 * none) the client asks for it again first (INIT-REBOOT), else it
 * DISCOVERs. Sends at once. */
void     dhcp_start(struct dhcp_client *c, uint64_t now, uint32_t last_addr);
/* Stop: with release and a lease, a RELEASE goes to its server; any
 * lease is cleared (unbound, DHCP_WHY_STOPPED). State STOPPED. */
void     dhcp_stop(struct dhcp_client *c, uint64_t now, bool release);
/* A datagram that arrived on port 68 (any length, any bytes). */
void     dhcp_input(struct dhcp_client *c, uint64_t now, const void *msg, size_t len);
/* The edge's ARP probe of addr ended: conflict, someone else answered.
 * An answer for another address, or when no probe is running, is
 * ignored. */
void     dhcp_probe_done(struct dhcp_client *c, uint64_t now, uint32_t addr, bool conflict);
/* Do what is due by now (retransmits, T1, T2, the lease's end). Safe to
 * call at any time. */
void     dhcp_tick(struct dhcp_client *c, uint64_t now);
/* When dhcp_tick next has work (DEADLINE_NEVER: none). */
uint64_t dhcp_deadline(const struct dhcp_client *c);
/* The state's name, for logs and tests. */
const char *dhcp_state_name(enum dhcp_state s);
