/* dns, the process around the resolver (dns.h): what its files share.
 *
 *   main.c    the loop: one port, the resolver's deadline and the
 *             askers' as its wait; the DNS servers from netstack
 *             (net.iface, again whenever net.wait_change answers)
 *   socks.c   the resolver's edge to the network (struct dns_io's send
 *             and release): a UDP socket per local port, opened at its
 *             first send without waiting, its datagrams to dns_input
 *   askers.c  /svc/dns: the shared channel (svc.connect only), a channel
 *             per opener, the resolves in flight and their answers
 *
 * One thread, and nothing in the loop waits for another process
 * (ARCHITECTURE.md "How a service waits"): every call to netstack is
 * written with its own txid and its answer read off the port, and a
 * resolve is answered when its answer comes (io->answer) or its timeout
 * passes. When netstack ends, every socket and the opener channel see
 * ERR_PEER_CLOSED: the resolver ends too, and init starts it again once
 * netstack is back (it keeps nothing but its cache, which goes). */
#pragma once

#include <dns.h>
#include <idl/dns.h>
#include <idl/net.h>
#include <net.h>
#include "dns.h"

#define SR_DNS     (SR_USER + 0)   /* the server end of /svc/dns's shared channel */
#define SR_DNS_SYS (SR_USER + 1)   /* /svc/dns-sys's: the network's own services' */

/* Port keys: the low byte says what fired, the bits above a slot's
 * generation (a stale packet for a reused slot is ignored). */
#define KEY_SHARED 1u
#define KEY_NET    2u            /* our opener channel to netstack */
#define KEY_SHARED_SYS 3u        /* /svc/dns-sys's shared channel */
#define KEY_ASKER  0x10u         /* + an asker's slot */
#define KEY_SOCK   0x40u         /* + a socket's slot */
#define BUDGET     16u           /* messages read off one channel a turn */

#define SOCK_SLOTS (DNS_MAX_QUERIES + 4u)   /* a released socket waits for its open's answer */
#define REQUESTS   (DNS_OPENERS * DNS_PER_OPENER)

/* A socket for one local port (socks.c). */
enum sock_state { SOCK_FREE, SOCK_OPENING, SOCK_OPEN, SOCK_FAILED };
struct sock {
    enum sock_state state;
    uint16_t port;           /* the local port the resolver chose */
    uint32_t gen;            /* the slot's generation (port keys) */
    uint32_t open_txid;      /* SOCK_OPENING: the net.udp in flight */
    bool     released;       /* SOCK_OPENING: the resolver let it go; closed when it opens */
    bool     pending;        /* SOCK_OPEN: datagrams may be queued */
    status_t failed;         /* SOCK_FAILED: why the open failed */
    struct net_sock s;       /* SOCK_OPEN */
    /* the first datagram, sent once the socket is open */
    bool     queued;
    uint32_t q_server;
    uint16_t q_len;
    uint8_t  q_msg[DNS_MSG_MAX];
};

/* An opener of /svc/dns (askers.c). */
struct asker {
    handle_t ch;        /* our end of its channel (0: free) */
    uint32_t gen;
    unsigned inflight;  /* its resolves waiting for an answer */
    bool     pending;   /* requests may be queued */
    bool     sys;       /* it came through /svc/dns-sys: a system asker (the reserve) */
};

/* A resolve waiting for its answer. */
struct request {
    bool           used;
    uint64_t       cookie;     /* the resolver's name for it (dns_ask) */
    unsigned       asker;      /* its opener's slot */
    struct idl_txn txn;
    uint64_t       deadline;   /* its timeout_ms from when it came */
};

struct dnsd {
    handle_t port;
    handle_t net;              /* our opener channel to /svc/net */
    uint32_t net_txid;         /* idl_txid_next's counter on it */
    uint32_t iface_txid;       /* the net.iface in flight (0: none) */
    uint32_t wait_txid;        /* the net.wait_change in flight (0: none) */
    uint32_t version;          /* iface's version the servers came with */
    uint64_t iface_retry;      /* a failed iface or wait is asked again then */
    bool     net_pending;
    handle_t shared;           /* /svc/dns's shared channel (0: closed) */
    bool     shared_pending;
    handle_t shared_sys;       /* /svc/dns-sys's (0: none, or closed) */
    bool     shared_sys_pending;
    uint32_t refused_shares;   /* connects refused for the ordinary openers' share */
    uint64_t next_cookie;
    struct dns_resolver r;
    struct dns_io io;
    struct sock     s[SOCK_SLOTS];
    struct asker    a[DNS_OPENERS];
    struct request  q[REQUESTS];
};

extern struct dnsd D;

/* socks.c */
void     socks_init(void);
/* A reply on the net opener channel that may be a socket's open (true:
 * it was, and is handled). */
bool     socks_open_reply(const void *rep, struct idl_msg *m);
/* A socket's packet (key's low byte KEY_SOCK + i). */
void     socks_packet(unsigned i, uint32_t gen);
/* Read every socket that has datagrams. ERR_PEER_CLOSED: netstack is gone. */
status_t socks_serve(void);
bool     socks_pending(void);

/* askers.c */
/* Serve /svc/dns's shared channel and /svc/dns-sys's (0: none; its
 * openers are system askers, which may use the reserve: DNS_PROG_OPENERS
 * and the resolver's DNS_PROG_QUERIES and DNS_PROG_WAITERS bound the
 * others). */
status_t askers_init(handle_t shared, handle_t shared_sys);
void     askers_packet(uint64_t key);
void     askers_serve(void);
bool     askers_pending(void);
/* Answer the requests whose timeout passed; the earliest deadline left. */
uint64_t askers_tick(uint64_t now);
/* io->answer. */
void     askers_answer(void *ctx, uint64_t cookie, status_t st, const uint32_t *addr, unsigned n,
                       uint32_t ttl_s);
