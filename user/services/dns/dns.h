/* dns: the resolver (RFC 1035; A records only, over UDP), split so that
 * everything that parses the network's bytes or keeps the protocol's
 * state is a pure library utest can drive with no network at all:
 *
 *   msg.c       names, the query, and the reply's checks (compression
 *               pointers, label and record lengths, counts against the
 *               bytes that arrived, the CNAME chain);
 *   cache.c     a small cache bounded by count and by TTL;
 *   resolver.c  the queries in flight: one record each, its own id,
 *               port, retries and deadline, so a slow server or a slow
 *               name holds up only its own askers;
 *   main.c      the process: `/svc/dns`, the loop and the edge
 *               (struct dns_io).
 *
 * The edge is all the resolver does to the world: send a datagram from
 * a local port it chose to a server's port 53, release that port, answer
 * an asker, draw random numbers. The library never blocks; the caller
 * feeds it what arrives (dns_input), the time (dns_tick at
 * dns_deadline) and the askers (dns_resolve, dns_cancel). Every call
 * takes `now`, nanoseconds of uptime.
 *
 * Addresses are host-order uint32_t (<netbytes.h>). A name is text,
 * dot-separated, without the root's trailing dot. */
#pragma once

#include <netbytes.h>
#include <os.h>

#define DNS_PORT        53
#define DNS_NAME_MAX    253u   /* as text, no trailing dot (RFC 1035 2.3.4: 255 on the wire) */
#define DNS_LABEL_MAX   63u
#define DNS_MSG_MAX     512u   /* a query fits; replies may be longer, up to what arrived */
#define DNS_MAX_ADDRS   4u     /* A records kept from one answer */
#define DNS_MAX_SERVERS 3u
#define DNS_CNAME_MAX   8u     /* CNAMEs followed for one name, in one reply or over several */
#define DNS_TTL_MAX     86400u /* a longer TTL is cached for this long */
#define DNS_RR_MAX      64u    /* records looked at in one reply, all sections */
#define DNS_JUMPS_MAX   16u    /* compression pointers followed in one name */

/* Header flags (RFC 1035 4.1.1). */
#define DNS_QR     0x8000u
#define DNS_OPCODE 0x7800u
#define DNS_TC     0x0200u
#define DNS_RD     0x0100u
#define DNS_RCODE  0x000fu
#define DNS_TYPE_A     1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN   1

enum dns_rcode {
    DNS_RCODE_OK       = 0,
    DNS_RCODE_FORMERR  = 1,
    DNS_RCODE_SERVFAIL = 2,
    DNS_RCODE_NXDOMAIN = 3,
    DNS_RCODE_NOTIMP   = 4,
    DNS_RCODE_REFUSED  = 5,
};

/* What a reply says about the name asked. */
enum dns_outcome {
    DNS_ADDRS,       /* addresses (perhaps after CNAMEs in the reply) */
    DNS_NO_NAME,     /* NXDOMAIN */
    DNS_NO_DATA,     /* the name has no A record */
    DNS_FOLLOW,      /* a CNAME whose target this reply doesn't answer: ask for it */
    DNS_SERVER_FAIL, /* SERVFAIL, REFUSED, NOTIMP, FORMERR or another rcode: ask another server */
    DNS_TRUNCATED,   /* TC: too long for UDP, and there is no TCP */
};

struct dns_result {
    enum dns_outcome outcome;
    uint32_t addr[DNS_MAX_ADDRS];   /* DNS_ADDRS: no duplicates */
    uint8_t  naddr;
    uint8_t  cnames;                /* CNAMEs followed inside this reply */
    uint32_t ttl;                   /* DNS_ADDRS, DNS_FOLLOW: the chain's smallest TTL, s */
    char     next[DNS_NAME_MAX + 1];   /* DNS_FOLLOW: the name to ask next */
};

/* name is a host name we may ask for: 1 to DNS_NAME_MAX characters (one
 * trailing dot allowed, not counted), labels of 1 to 63 letters, digits,
 * '-' and '_'. *len: its length without the trailing dot. */
bool     dns_name_ok(const char *name, size_t *len);
/* a and b are the same name (ASCII letters in any case). */
bool     dns_name_eq(const char *a, const char *b);
/* name is a dotted IPv4 address ("1.1.1.1"): *addr. */
bool     dns_ipv4_literal(const char *name, uint32_t *addr);
/* Build the query for name's A record (recursion desired) into buf:
 * its length, or 0 if the name isn't dns_name_ok or cap is too small. */
size_t   dns_build_query(uint8_t *buf, size_t cap, uint16_t id, const char *name);
/* Read the name at *pos in msg (len bytes) into out (DNS_NAME_MAX + 1),
 * following compression pointers (only backwards, at most
 * DNS_JUMPS_MAX); *pos moves past the name where it is written. A label
 * of a byte that can't be in a name ('.', a control character, a space)
 * is refused. ERR_INVALID_ARGS for anything malformed. */
status_t dns_read_name(const uint8_t *msg, size_t len, size_t *pos, char *out);
/* Check a reply (len bytes) to query id for name, every record of every
 * section walked: OK and *out, or ERR_NOT_FOUND (not a reply to this
 * query: another id, not a response, another question) or
 * ERR_INVALID_ARGS (malformed: too short, a bad name, a record past the
 * end, more records than the bytes hold, an A record that isn't 4
 * bytes). The caller drops the datagram in both cases. */
status_t dns_parse_reply(const uint8_t *msg, size_t len, uint16_t id, const char *name,
                         struct dns_result *out);

/* ---- the cache ------------------------------------------------------------ */

#define DNS_CACHE_SIZE 32u

struct dns_cache_entry {
    char     name[DNS_NAME_MAX + 1];   /* "": free */
    uint32_t addr[DNS_MAX_ADDRS];
    uint8_t  naddr;
    uint64_t expires;                  /* ns uptime */
};

struct dns_cache {
    struct dns_cache_entry e[DNS_CACHE_SIZE];
};

/* An unexpired entry for name: OK, its addresses into addr[]
 * (DNS_MAX_ADDRS), *n, *ttl_s (seconds left, rounded up). ERR_NOT_FOUND. */
status_t dns_cache_get(struct dns_cache *c, uint64_t now, const char *name, uint32_t *addr,
                       uint8_t *n, uint32_t *ttl_s);
/* Keep n (1 to DNS_MAX_ADDRS) addresses for name for ttl_s seconds
 * (at most DNS_TTL_MAX; 0: not kept). Replaces name's entry; when full,
 * an expired entry goes, else the one that expires first. */
void     dns_cache_put(struct dns_cache *c, uint64_t now, const char *name, const uint32_t *addr,
                       uint8_t n, uint32_t ttl_s);
void     dns_cache_flush(struct dns_cache *c);

/* ---- the resolver ------------------------------------------------------------ */

#define DNS_MAX_QUERIES  16u     /* names in flight at once: a socket each (netstack's cap) */
#define DNS_MAX_WAITERS  8u      /* askers of one name in flight */
#define DNS_TRIES        4u      /* sends of one name (servers taken in turn) */
#define DNS_TRY_MS       1000u   /* the first try's wait; each next one 1 s longer: 10 s in all */
#define DNS_PORT_MIN     1024u   /* local ports are random in DNS_PORT_MIN..65535 */
#define DNS_PORT_TRIES   4u      /* ports tried when the edge says one is taken */

/* The edge, all of it (main.c fills it; utest's is a script). */
struct dns_io {
    void *ctx;   /* passed to each call */
    /* Send msg from local port `port` (the edge opens a socket on it at
     * its first use) to port 53 of server. ERR_ALREADY_BOUND: the port
     * is taken (the resolver picks another); any other error is a lost
     * datagram (the retry covers it). */
    status_t (*send)(void *ctx, uint16_t port, uint32_t server, const void *msg, size_t len);
    /* No query uses port any more: the edge may close its socket. */
    void (*release)(void *ctx, uint16_t port);
    /* The answer for the asker `cookie`: OK and n addresses (ttl_s
     * seconds left), or ERR_NOT_FOUND (no such name, or no address),
     * ERR_TIMED_OUT (no server answered), ERR_IO (the servers failed),
     * ERR_NOT_SUPPORTED (the answer needs TCP), ERR_OUT_OF_RANGE (more
     * than DNS_CNAME_MAX CNAMEs), ERR_BAD_STATE (no servers). Called once
     * per accepted dns_resolve, maybe from inside it. */
    void (*answer)(void *ctx, uint64_t cookie, status_t st, const uint32_t *addr, unsigned n,
                   uint32_t ttl_s);
    /* 32 random bits: query ids and local ports. */
    uint32_t (*random)(void *ctx);
};

/* One name in flight. */
struct dns_query {
    bool     used;
    char     name[DNS_NAME_MAX + 1];   /* asked for (the cache's key) */
    char     cur[DNS_NAME_MAX + 1];    /* asked now: name, or a CNAME's target */
    uint16_t id, port;                 /* of the query now out */
    uint8_t  hops;                     /* CNAMEs followed so far */
    uint8_t  tries;                    /* sends of cur */
    uint32_t ttl;                      /* the smallest TTL along the chain so far, s */
    uint64_t deadline;                 /* the next send, or giving up */
    uint64_t cookies[DNS_MAX_WAITERS];
    uint8_t  nwait;
};

struct dns_stats {
    uint32_t asked, cached, literal, sent, send_failed;
    uint32_t received, foreign, malformed;   /* foreign: no query's port, id, server or question */
    uint32_t answered, failed, timeouts;
};

struct dns_resolver {
    const struct dns_io *io;
    uint32_t servers[DNS_MAX_SERVERS];
    uint8_t  nservers;
    struct dns_query q[DNS_MAX_QUERIES];
    struct dns_cache cache;
    struct dns_stats stats;
};

void     dns_init(struct dns_resolver *r, const struct dns_io *io);
/* The servers to ask (netctl's DNS list; up to DNS_MAX_SERVERS, the
 * non-unicast ones skipped). Queries in flight go on with the new list;
 * a change of list empties the cache. */
void     dns_set_servers(struct dns_resolver *r, const uint32_t *servers, unsigned n);
/* Resolve name for the asker `cookie`. OK: io->answer will be called
 * exactly once for it (already, for a cached name or an IPv4 literal).
 * An asker of a name already in flight joins it. ERR_INVALID_ARGS (not
 * a name), ERR_BAD_STATE (no servers), ERR_NO_RESOURCES (DNS_MAX_QUERIES
 * names, or DNS_MAX_WAITERS askers of this one, in flight): no answer
 * comes. */
status_t dns_resolve(struct dns_resolver *r, uint64_t now, const char *name, uint64_t cookie);
/* The asker went away: no answer for it. A query left with no askers
 * ends. */
void     dns_cancel(struct dns_resolver *r, uint64_t cookie);
/* A datagram that arrived on one of the resolver's ports. */
struct dns_datagram {
    uint16_t    port;       /* the local port it came to */
    uint32_t    src;        /* from this address ... */
    uint16_t    src_port;   /* ... and port */
    const void *msg;
    size_t      len;
};
void     dns_input(struct dns_resolver *r, uint64_t now, const struct dns_datagram *d);
/* Do what is due by now: resends and give-ups. Safe at any time. */
void     dns_tick(struct dns_resolver *r, uint64_t now);
/* The earliest deadline of any query (DEADLINE_NEVER: none in flight). */
uint64_t dns_deadline(const struct dns_resolver *r);
