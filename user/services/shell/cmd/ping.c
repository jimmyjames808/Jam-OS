/* ping: ICMP echo through netstack (net.idl's echo): one request a second,
 * each reply (or its absence) a line, then a summary. Ctrl+C stops it at
 * once. The echoes go out on a /svc/net channel of ping's own, written
 * without waiting, so the wait for a reply can watch the keyboard too. A
 * name is resolved first (sh_lookup: the resolver's first address). */
#include <dns.h>
#include <idl/net.h>
#include <ipv4.h>
#include <net.h>
#include "sh.h"

#define INTERVAL  NS_PER_S          /* between requests */
#define TIMEOUT   1000u             /* ms: a reply later than this is lost */
#define SLICE     (50 * NS_PER_MS)  /* how often the wait looks at the keyboard */
#define SIZE      56u               /* data bytes, as other systems' ping */

struct tally {
    unsigned sent, got;
    uint32_t min_us, max_us;
    uint64_t sum_us;
};

/* The reply to the echo with txid, or a failure; ERR_CANCELED on Ctrl+C. */
static status_t wait_reply(handle_t ch, uint32_t txid, uint32_t *rtt, uint8_t *ttl, uint16_t *size)
{
    uint64_t end = now() + (uint64_t)TIMEOUT * NS_PER_MS + NS_PER_S;   /* netstack answers first */
    for (;;) {
        _Alignas(8) uint8_t rep[NET_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if (st == OK && m.txid == txid)
            return net_echo_result(rep, &m, rtt, ttl, size);
        if (st == OK || st == ERR_INTERNAL) {
            idl_msg_drop(&m);   /* an earlier echo's: it was counted lost */
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return st;
        if (sh_interrupted())
            return ERR_CANCELED;
        uint64_t t = now();
        if (t >= end)
            return ERR_TIMED_OUT;
        signals_t seen;
        (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED,
                                  t + SLICE < end ? t + SLICE : end, &seen);
    }
}

/* One echo, its line said; false: stop (Ctrl+C, or netstack can't send). */
static bool one(handle_t ch, uint32_t addr, uint16_t seq, uint16_t size, uint32_t *last_txid,
                struct tally *t)
{
    char a[IPV4_TEXT_MAX];
    uint32_t txid = idl_txid_next(last_txid), rtt = 0;
    uint8_t ttl = 0;
    uint16_t got = 0;
    status_t st = net_echo_send(ch, txid, addr, seq, size, TIMEOUT);
    if (st == OK)
        st = wait_reply(ch, txid, &rtt, &ttl, &got);
    if (st == ERR_CANCELED)
        return false;
    t->sent++;
    if (st == OK) {
        t->got++;
        t->sum_us += rtt;
        t->min_us = t->got == 1 || rtt < t->min_us ? rtt : t->min_us;
        t->max_us = rtt > t->max_us ? rtt : t->max_us;
        sh_say("%u bytes from %s: seq=%u ttl=%u time=%u.%03u ms\n", got + 8u,
               ipv4_format(addr, a), seq, ttl, rtt / 1000, rtt % 1000);
        return true;
    }
    if (st == ERR_TIMED_OUT) {
        sh_say("seq=%u: no reply\n", seq);
        return true;
    }
    if (st == ERR_NOT_FOUND) {
        sh_say("seq=%u: unreachable (a router said so)\n", seq);
        return true;
    }
    t->sent--;
    sh_say("ping: %s\n", st == ERR_BAD_STATE ? "no address, no link or no route (see `net`)"
                                              : status_str(st));
    return false;
}

static void summary(uint32_t addr, const struct tally *t)
{
    char a[IPV4_TEXT_MAX];
    if (!t->sent)
        return;
    sh_say("--- %s: %u sent, %u received, %u%% lost", ipv4_format(addr, a), t->sent, t->got,
           (t->sent - t->got) * 100 / t->sent);
    if (t->got) {
        uint32_t avg = (uint32_t)(t->sum_us / t->got);
        sh_say("; round trip min/avg/max %u.%03u/%u.%03u/%u.%03u ms", t->min_us / 1000,
               t->min_us % 1000, avg / 1000, avg % 1000, t->max_us / 1000, t->max_us % 1000);
    }
    sh_say("\n");
}

/* The target (an address or a name), the count and the size. */
static bool args(int argc, char **argv, const char **target, uint64_t *count, uint64_t *size)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            if (!sh_parse_u64(argv[++i], count) || !*count || *count > 1000000)
                return false;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            if (!sh_parse_u64(argv[++i], size) || *size > NET_DGRAM_MAX)
                return false;
        } else if (!*target && argv[i][0] && argv[i][0] != '-') {
            *target = argv[i];
        } else {
            return false;
        }
    }
    return *target != NULL;
}

SH_CMD(ping)
{
    const char *target = NULL;
    uint32_t last_txid = 0;
    uint64_t count = 4, size = SIZE;
    if (!args(argc, argv, &target, &count, &size)) {
        sh_tty("usage: ping <address|name> [-c count] [-s size]   (e.g. ping 1.1.1.1 -c 10)\n");
        return 2;
    }
    struct dns_answer ans;
    status_t st = sh_lookup(target, &ans);   /* an address is answered as it is */
    if (st != OK) {
        sh_say("ping: %s: %s\n", target, sh_lookup_why(st));
        return 1;
    }
    uint32_t addr = ans.addr[0];
    handle_t ch;
    st = net_svc_open(&ch);
    if (st != OK) {
        sh_say("ping: no netstack (%s)\n", status_str(st));
        return 1;
    }
    char a[IPV4_TEXT_MAX];
    if (strcmp(target, ipv4_format(addr, a)))
        sh_say("PING %s (%s): %u data bytes\n", target, a, (unsigned)size);
    else
        sh_say("PING %s: %u data bytes\n", a, (unsigned)size);
    struct tally t = { 0 };
    for (uint64_t seq = 1; seq <= count; seq++) {
        uint64_t next = now() + INTERVAL;
        if (!one(ch, addr, (uint16_t)seq, (uint16_t)size, &last_txid, &t))
            break;
        if (seq < count && !sh_sleep(next > now() ? next - now() : 0))
            break;
    }
    jam_handle_close(ch);   /* an echo still in flight goes with it */
    summary(addr, &t);
    return t.got ? 0 : 1;
}
