/* bin/sntp's core (ntp.c): the SNTP request it sends and the checks a
 * reply must pass before its time is believed (RFC 4330, and RFC 5905's
 * client mode). No I/O and no clock of its own: the caller gives the
 * uptimes the request left and the reply came, so the utest runs every
 * case (user/tests/utest/sntp.c).
 *
 * The request is 48 bytes: version 4, mode 3 (client), everything else 0
 * but the transmit timestamp, which is 64 random bits (the nonce): a
 * server copies it into its reply's origin timestamp, and a reply whose
 * origin isn't exactly that is not an answer to us (an old one, or one
 * forged by a device that didn't see our request). Our own idea of the
 * time is never sent (RFC 5905 9.1 allows it; it would tell anyone on the
 * VLAN what our clock says).
 *
 * The time a good reply gives is the server's transmit time plus half the
 * round trip less the server's own time between its receive and transmit
 * (t4 - t1 - (T3 - T2)), at the uptime the reply came: the kernel keeps
 * UTC as an offset from the uptime, so that pair is all wallclock_set
 * needs, with no time of ours in the sum. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTP_PORT        123u
#define NTP_PACKET      48u            /* bytes of a request; a reply has at least these */
#define NTP_UNIX_EPOCH  2208988800ull  /* seconds from 1900-01-01 to 1970-01-01 */
#define NTP_EARLIEST    1767225600ll   /* 2026-01-01 UTC: an earlier time is refused */
#define NTP_LATEST      7258118399ll   /* the last second before 2200: the kernel's limit */
#define NTP_DISTANCE_MAX 1000000000ull /* ns: root delay / 2 + root dispersion, at most */
#define NTP_ROUND_TRIP_MAX (5 * 1000000000ull) /* ns: a reply later than this is too vague */

/* What a reply is, by the first check it fails. */
enum ntp_verdict {
    NTP_OK,            /* believed: the time is in the result */
    NTP_SHORT,         /* fewer than NTP_PACKET bytes */
    NTP_NOT_SERVER,    /* its mode isn't 4 (server) */
    NTP_VERSION,       /* its version isn't 3 or 4 */
    NTP_ORIGIN,        /* its origin timestamp isn't the nonce we sent */
    NTP_KISS,          /* stratum 0: a kiss-o'-death (its code in the result) */
    NTP_UNSYNCED,      /* stratum 16 or more, or leap indicator 3: the server has no time */
    NTP_NO_TIME,       /* its receive or transmit timestamp is 0 */
    NTP_DISTANCE,      /* its root delay and dispersion say it is too far from a clock */
    NTP_BAD_ORDER,     /* it says it sent before it received, or the round trip is too long */
    NTP_RANGE,         /* the time is before NTP_EARLIEST or after NTP_LATEST */
};

struct ntp_result {
    int64_t  utc_ns;       /* the time, ns since 1970 UTC, ... */
    uint64_t uptime_ns;    /* ... at this uptime (when the reply came) */
    uint64_t delay_ns;     /* the round trip, less the server's time */
    uint8_t  stratum;      /* the server's (1: it has a reference clock) */
    char     kiss[5];      /* NTP_KISS: the code ("RATE", "DENY", ...), NUL-terminated */
};

/* The 48-byte request carrying nonce (nonzero: ntp_nonce makes one). */
void ntp_request(uint64_t nonce, uint8_t out[NTP_PACKET]);
/* A nonce from 64 random bits: never 0 (a server answers 0 as "none"). */
uint64_t ntp_nonce(uint64_t random);
/* Check reply r (len bytes, untrusted) to the request with nonce that left
 * at uptime sent_ns and was answered at uptime recv_ns. NTP_OK: *out has
 * the time; NTP_KISS: out->kiss has the code; otherwise *out is zero. */
enum ntp_verdict ntp_check(const uint8_t *r, size_t len, uint64_t nonce, uint64_t sent_ns,
                           uint64_t recv_ns, struct ntp_result *out);
/* A few words for the log: "the origin doesn't match what we sent". */
const char *ntp_verdict_str(enum ntp_verdict v);
/* An NTP timestamp (seconds since 1900 in the high 32 bits, a fraction in
 * the low) as ns since 1970. Era 0 ends in 2036: a timestamp whose top
 * bit is 0 is taken as era 1 (RFC 4330 3), so this works until 2104. */
int64_t ntp_to_unix_ns(uint64_t ts);
/* The other way, for tests and for nothing else (era 0 or 1 as above). */
uint64_t ntp_from_unix_ns(int64_t ns);
