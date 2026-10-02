/* utest: bin/sntp's core (user/services/sntp/ntp.c): the request's bytes,
 * the time a good reply gives (the round trip halved, the server's own
 * time taken out), every check a reply must pass (each one failed on its
 * own, the rest of the reply good), timestamps across NTP's era 1 (2036
 * on), and hostile replies: random bytes with the right origin never give
 * a time outside the kernel's range or crash. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netbytes.h>
#include <os.h>
#include "ntp.h"
#include "utest.h"

#define NONCE  0x0123456789abcdefull
#define T_2031 1930000000ll                     /* 2031-02-27 23:06:40 UTC, Unix seconds */
#define SENT   (5 * NS_PER_S)                   /* the request left at uptime 5 s ... */
#define RECV   (SENT + 20 * NS_PER_MS)          /* ... and the reply came 20 ms later */

static void put64(uint8_t *p, uint64_t v)
{
    net_put32(p, (uint32_t)(v >> 32));
    net_put32(p + 4, (uint32_t)v);
}

/* A good reply to NONCE: version 4, stratum 2, received at T_2031 and
 * sent 1 ms later, a root delay of 10 ms and dispersion of 5 ms. */
static void good(uint8_t r[NTP_PACKET])
{
    memset(r, 0, NTP_PACKET);
    r[0] = 4u << 3 | 4u;
    r[1] = 2;
    net_put32(r + 4, 655);    /* 10 ms in 16.16 */
    net_put32(r + 8, 328);    /* 5 ms */
    memcpy(r + 12, "GPS\0", 4);
    put64(r + 24, NONCE);
    put64(r + 32, ntp_from_unix_ns(T_2031 * (int64_t)NS_PER_S));
    put64(r + 40, ntp_from_unix_ns(T_2031 * (int64_t)NS_PER_S + (int64_t)NS_PER_MS));
}

static enum ntp_verdict check_of(const uint8_t *r, size_t len, struct ntp_result *out)
{
    return ntp_check(r, len, NONCE, SENT, RECV, out);
}

bool t_sntp_request_and_reply(void)
{
    uint8_t q[NTP_PACKET], r[NTP_PACKET + 20];
    ntp_request(NONCE, q);
    CHECK_EQ(q[0], 0x23);   /* leap 0, version 4, mode 3 */
    for (unsigned i = 1; i < 40; i++)
        CHECK_EQ(q[i], 0);   /* nothing of ours: no time, no stratum */
    CHECK_EQ(net_get32(q + 40), 0x01234567u);
    CHECK_EQ(net_get32(q + 44), 0x89abcdefu);
    CHECK_EQ(ntp_nonce(0), 1);
    CHECK_EQ(ntp_nonce(7), 7);
    struct ntp_result res;
    good(r);
    CHECK_EQ(check_of(r, NTP_PACKET, &res), NTP_OK);
    /* the round trip less the server's 1 ms, halved, after its send */
    CHECK(res.delay_ns - 19 * NS_PER_MS <= 2);   /* the 1 ms is 0.999999999 in 32-bit fractions */
    int64_t want = T_2031 * (int64_t)NS_PER_S + (int64_t)NS_PER_MS + 9500 * (int64_t)NS_PER_US;
    CHECK(res.utc_ns - want < 2 && want - res.utc_ns < 2);   /* the fractions round */
    CHECK_EQ(res.uptime_ns, RECV);
    CHECK_EQ(res.stratum, 2);
    CHECK_EQ(check_of(r, sizeof(r), &res), NTP_OK);   /* extensions after it: ignored */
    r[0] = 3u << 3 | 4u;
    CHECK_EQ(check_of(r, NTP_PACKET, &res), NTP_OK);   /* version 3 */
    /* a server that says it held the request longer than the round trip */
    good(r);
    put64(r + 40, ntp_from_unix_ns(T_2031 * (int64_t)NS_PER_S + 50 * (int64_t)NS_PER_MS));
    CHECK_EQ(check_of(r, NTP_PACKET, &res), NTP_OK);
    CHECK_EQ(res.delay_ns, 0);
    return true;
}

/* good() with one change: verdict want, and nothing in *out but a kiss. */
static bool refused(void (*edit)(uint8_t *r), enum ntp_verdict want, size_t len)
{
    uint8_t r[NTP_PACKET];
    struct ntp_result res;
    good(r);
    edit(r);
    enum ntp_verdict v = check_of(r, len, &res);
    if (v != want)
        FAIL("verdict %s, want %s", ntp_verdict_str(v), ntp_verdict_str(want));
    CHECK(res.utc_ns == 0 && res.uptime_ns == 0);
    return true;
}

static void as_is(uint8_t *r) { (void)r; }
static void mode3(uint8_t *r) { r[0] = 4u << 3 | 3u; }
static void mode5(uint8_t *r) { r[0] = 4u << 3 | 5u; }
static void version2(uint8_t *r) { r[0] = 2u << 3 | 4u; }
static void version5(uint8_t *r) { r[0] = 5u << 3 | 4u; }
static void origin_bit(uint8_t *r) { r[31] ^= 1; }
static void origin_zero(uint8_t *r) { memset(r + 24, 0, 8); }
static void kiss(uint8_t *r) { r[1] = 0; memcpy(r + 12, "RATE", 4); }
static void stratum16(uint8_t *r) { r[1] = 16; }
static void leap3(uint8_t *r) { r[0] |= 0xc0; }
static void no_receive(uint8_t *r) { memset(r + 32, 0, 8); }
static void no_transmit(uint8_t *r) { memset(r + 40, 0, 8); }
static void far_dispersion(uint8_t *r) { net_put32(r + 8, 0x00011000); }   /* 1.06 s */
static void far_delay(uint8_t *r) { net_put32(r + 4, 0x00030000); }        /* 3 s: 1.5 s */
static void sent_before_received(uint8_t *r)
{
    put64(r + 40, ntp_from_unix_ns(T_2031 * (int64_t)NS_PER_S - 1000));
}
static void in_2025(uint8_t *r)
{
    put64(r + 32, ntp_from_unix_ns(1750000000ll * (int64_t)NS_PER_S));
    put64(r + 40, ntp_from_unix_ns(1750000000ll * (int64_t)NS_PER_S));
}
static void in_1900(uint8_t *r)   /* era 0's first second: 2036 by the era rule, still fine */
{
    put64(r + 32, 1ull << 32);
    put64(r + 40, 1ull << 32);
}

bool t_sntp_checks(void)
{
    CHECK(refused(as_is, NTP_SHORT, NTP_PACKET - 1));
    CHECK(refused(as_is, NTP_SHORT, 0));
    CHECK(refused(mode3, NTP_NOT_SERVER, NTP_PACKET));
    CHECK(refused(mode5, NTP_NOT_SERVER, NTP_PACKET));
    CHECK(refused(version2, NTP_VERSION, NTP_PACKET));
    CHECK(refused(version5, NTP_VERSION, NTP_PACKET));
    CHECK(refused(origin_bit, NTP_ORIGIN, NTP_PACKET));
    CHECK(refused(origin_zero, NTP_ORIGIN, NTP_PACKET));
    CHECK(refused(kiss, NTP_KISS, NTP_PACKET));
    CHECK(refused(stratum16, NTP_UNSYNCED, NTP_PACKET));
    CHECK(refused(leap3, NTP_UNSYNCED, NTP_PACKET));
    CHECK(refused(no_receive, NTP_NO_TIME, NTP_PACKET));
    CHECK(refused(no_transmit, NTP_NO_TIME, NTP_PACKET));
    CHECK(refused(far_dispersion, NTP_DISTANCE, NTP_PACKET));
    CHECK(refused(far_delay, NTP_DISTANCE, NTP_PACKET));
    CHECK(refused(sent_before_received, NTP_BAD_ORDER, NTP_PACKET));
    CHECK(refused(in_2025, NTP_RANGE, NTP_PACKET));
    /* a kiss with the wrong origin is no kiss: anyone could send one */
    uint8_t r[NTP_PACKET];
    struct ntp_result res;
    good(r);
    kiss(r);
    CHECK_EQ(check_of(r, NTP_PACKET, &res), NTP_KISS);
    CHECK(!strcmp(res.kiss, "RATE"));
    origin_bit(r);
    CHECK_EQ(check_of(r, NTP_PACKET, &res), NTP_ORIGIN);
    /* uptimes out of order, or a round trip too long to trust */
    good(r);
    CHECK_EQ(ntp_check(r, NTP_PACKET, NONCE, RECV, SENT, &res), NTP_BAD_ORDER);
    CHECK_EQ(ntp_check(r, NTP_PACKET, NONCE, SENT, SENT + NTP_ROUND_TRIP_MAX + 1, &res),
             NTP_BAD_ORDER);
    /* era 1: 1900's first seconds read as 2036 */
    good(r);
    in_1900(r);
    CHECK_EQ(check_of(r, NTP_PACKET, &res), NTP_OK);
    CHECK_EQ(res.utc_ns / (int64_t)NS_PER_S, 2085978497ll);   /* 2036-02-07 06:28:17 */
    return true;
}

bool t_sntp_times(void)
{
    static const int64_t secs[] = { 0, 1767225600ll, T_2031, 2085978495ll, 2085978496ll,
                                    4102444799ll };
    /* 1970 to 2099, both eras: back within the fraction's rounding */
    for (unsigned i = 0; i < sizeof(secs) / sizeof(secs[0]); i++)
        for (int64_t frac = 0; frac < (int64_t)NS_PER_S; frac += 123456789) {
            int64_t ns = secs[i] * (int64_t)NS_PER_S + frac;
            int64_t back = ntp_to_unix_ns(ntp_from_unix_ns(ns));
            CHECK(back - ns <= 1 && ns - back <= 1);
        }
    CHECK_EQ(ntp_to_unix_ns(0xe5f0d0c080000000ull), 1648775744ll * (int64_t)NS_PER_S +
                                                    500000000ll);   /* 2022, half a second */
    return true;
}

/* Random bytes, more of them plausible as i goes round: 0 anything, 1 ours
 * and a server's, 2 that with a stratum, no alarm and small root fields,
 * 3 that with both times 2026..2099. */
static size_t fuzz_reply(uint8_t *r, size_t cap, unsigned i, uint32_t *seed)
{
    for (size_t k = 0; k < cap; k++) {
        *seed = *seed * 1664525u + 1013904223u;
        r[k] = (uint8_t)(*seed >> 24);
    }
    size_t len = (*seed >> 8) % (cap + 1);
    if (i % 4 == 0)
        return len;
    put64(r + 24, NONCE);
    r[0] = (uint8_t)((r[0] & 0x40) | 4u << 3 | 4u);   /* leap 0 or 1 */
    if (i % 4 >= 2) {
        r[1] = (uint8_t)(1 + r[1] % 15);
        net_put32(r + 4, net_get32(r + 4) & 0xffff);   /* under a second */
        net_put32(r + 8, net_get32(r + 8) & 0x3fff);
    }
    if (i % 4 == 3) {
        int64_t t = 1767225600ll + (int64_t)(net_get32(r + 32) % 2300000000u);
        put64(r + 32, ntp_from_unix_ns(t * (int64_t)NS_PER_S));
        put64(r + 40, ntp_from_unix_ns(t * (int64_t)NS_PER_S + (r[44] & 7) * 1000000ll));
        len = NTP_PACKET + (len & 7);
    }
    return len;
}

bool t_sntp_fuzz(void)
{
    uint8_t r[NTP_PACKET + 8];
    uint32_t seed = 0x5eed1234u;
    unsigned ok = 0;
    for (unsigned i = 0; i < 4000; i++) {
        size_t len = fuzz_reply(r, sizeof(r), i, &seed);
        struct ntp_result res;
        enum ntp_verdict v = check_of(r, len, &res);
        CHECK(v <= NTP_RANGE);
        if (v != NTP_OK)
            continue;
        ok++;
        CHECK(len >= NTP_PACKET && i % 4);   /* only one with our origin */
        CHECK(res.utc_ns / (int64_t)NS_PER_S >= NTP_EARLIEST &&
              res.utc_ns / (int64_t)NS_PER_S <= NTP_LATEST);
        CHECK(res.uptime_ns == RECV && res.delay_ns <= RECV - SENT);
    }
    CHECK(ok >= 500);   /* the plausible ones mostly pass: the checks above ran */
    printf("utest: sntp_fuzz: %u of 4000 random replies believed (all with our origin)\n", ok);
    return true;
}
