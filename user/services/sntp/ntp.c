/* bin/sntp's core: the request and a reply's checks (ntp.h has the
 * model). Every field of a reply is read from the bytes as they came,
 * big-endian, and every sum is done on 64 bits with its range checked
 * first: a reply is whatever any device on the VLAN chose to send.
 *
 * The packet (RFC 5905 7.3): byte 0 the leap indicator (2 bits), the
 * version (3) and the mode (3); 1 the stratum; 2 the poll; 3 the
 * precision; 4-7 the root delay and 8-11 the root dispersion (seconds,
 * 16.16); 12-15 the reference id (for stratum 0, a kiss code in ASCII);
 * then four timestamps of 8 bytes: reference (16), origin (24), receive
 * (32) and transmit (40). */
#include <netbytes.h>
#include <os.h>
#include "ntp.h"

#define B0_LEAP(b)     ((b) >> 6)
#define B0_VERSION(b)  (((b) >> 3) & 7)
#define B0_MODE(b)     ((b) & 7)
#define MODE_CLIENT    3u
#define MODE_SERVER    4u
#define LEAP_ALARM     3u            /* the server's clock isn't synchronized */
#define STRATUM_MAX    15u           /* 16 means unsynchronized */
#define OFF_ROOT_DELAY 4
#define OFF_ROOT_DISP  8
#define OFF_REF_ID     12
#define OFF_ORIGIN     24
#define OFF_RECEIVE    32
#define OFF_TRANSMIT   40

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)net_get32(p) << 32 | net_get32(p + 4);
}

static void put64(uint8_t *p, uint64_t v)
{
    net_put32(p, (uint32_t)(v >> 32));
    net_put32(p + 4, (uint32_t)v);
}

uint64_t ntp_nonce(uint64_t random)
{
    return random ? random : 1;
}

void ntp_request(uint64_t nonce, uint8_t out[NTP_PACKET])
{
    memset(out, 0, NTP_PACKET);
    out[0] = 4u << 3 | MODE_CLIENT;   /* leap 0, version 4, client */
    put64(out + OFF_TRANSMIT, nonce);
}

int64_t ntp_to_unix_ns(uint64_t ts)
{
    uint64_t secs = ts >> 32;
    if (!(secs & 0x80000000u))
        secs += 1ull << 32;   /* era 1: 2036-02-07 and later */
    uint64_t frac_ns = ((ts & 0xffffffffu) * NS_PER_S) >> 32;
    return (int64_t)(secs - NTP_UNIX_EPOCH) * (int64_t)NS_PER_S + (int64_t)frac_ns;
}

uint64_t ntp_from_unix_ns(int64_t ns)
{
    uint64_t secs = (uint64_t)(ns / (int64_t)NS_PER_S) + NTP_UNIX_EPOCH;
    uint64_t frac = ((uint64_t)(ns % (int64_t)NS_PER_S) << 32) / NS_PER_S;
    return (secs & 0xffffffffu) << 32 | frac;
}

/* A 16.16 count of seconds as ns. */
static uint64_t short_ns(uint32_t v)
{
    return ((uint64_t)v * NS_PER_S) >> 16;
}

/* The checks on the header alone, in the order RFC 4330 5 gives them, the
 * origin first: nothing else in a reply that isn't ours is believed, a
 * kiss-o'-death included (else anyone could make us stop asking). */
static enum ntp_verdict check_header(const uint8_t *r, size_t len, uint64_t nonce,
                                     struct ntp_result *out)
{
    if (len < NTP_PACKET)
        return NTP_SHORT;
    if (B0_MODE(r[0]) != MODE_SERVER)
        return NTP_NOT_SERVER;
    if (B0_VERSION(r[0]) != 3 && B0_VERSION(r[0]) != 4)
        return NTP_VERSION;
    if (get64(r + OFF_ORIGIN) != nonce)
        return NTP_ORIGIN;
    if (r[1] == 0) {
        for (int i = 0; i < 4; i++) {
            char c = (char)r[OFF_REF_ID + i];
            out->kiss[i] = c >= 0x20 && c <= 0x7e ? c : '?';
        }
        return NTP_KISS;
    }
    if (r[1] > STRATUM_MAX || B0_LEAP(r[0]) == LEAP_ALARM)
        return NTP_UNSYNCED;
    if (!get64(r + OFF_RECEIVE) || !get64(r + OFF_TRANSMIT))
        return NTP_NO_TIME;
    uint64_t distance = short_ns(net_get32(r + OFF_ROOT_DELAY)) / 2 +
                        short_ns(net_get32(r + OFF_ROOT_DISP));
    if (distance > NTP_DISTANCE_MAX)
        return NTP_DISTANCE;
    return NTP_OK;
}

enum ntp_verdict ntp_check(const uint8_t *r, size_t len, uint64_t nonce, uint64_t sent_ns,
                           uint64_t recv_ns, struct ntp_result *out)
{
    memset(out, 0, sizeof(*out));
    enum ntp_verdict v = check_header(r, len, nonce, out);
    if (v != NTP_OK)
        return v;
    int64_t t2 = ntp_to_unix_ns(get64(r + OFF_RECEIVE));
    int64_t t3 = ntp_to_unix_ns(get64(r + OFF_TRANSMIT));
    if (t3 < t2 || recv_ns < sent_ns || recv_ns - sent_ns > NTP_ROUND_TRIP_MAX)
        return NTP_BAD_ORDER;
    /* The server's own time can't be longer than the whole round trip;
     * if its clock runs fast enough that it says so, the delay is 0. */
    uint64_t trip = recv_ns - sent_ns, held = (uint64_t)(t3 - t2);
    uint64_t delay = trip > held ? trip - held : 0;
    int64_t utc = t3 + (int64_t)(delay / 2);
    if (utc / (int64_t)NS_PER_S < NTP_EARLIEST || utc / (int64_t)NS_PER_S > NTP_LATEST)
        return NTP_RANGE;
    out->utc_ns = utc;
    out->uptime_ns = recv_ns;
    out->delay_ns = delay;
    out->stratum = r[1];
    return NTP_OK;
}

const char *ntp_verdict_str(enum ntp_verdict v)
{
    static const char *const words[] = {
        [NTP_OK] = "a good reply",
        [NTP_SHORT] = "too short",
        [NTP_NOT_SERVER] = "not a server's reply",
        [NTP_VERSION] = "a version that isn't 3 or 4",
        [NTP_ORIGIN] = "the origin doesn't match what we sent",
        [NTP_KISS] = "a kiss-o'-death",
        [NTP_UNSYNCED] = "the server isn't synchronized",
        [NTP_NO_TIME] = "no timestamps",
        [NTP_DISTANCE] = "the server is too far from a clock",
        [NTP_BAD_ORDER] = "its times are out of order",
        [NTP_RANGE] = "a time before 2026 or after 2199",
    };
    return (unsigned)v < sizeof(words) / sizeof(words[0]) ? words[v] : "?";
}
