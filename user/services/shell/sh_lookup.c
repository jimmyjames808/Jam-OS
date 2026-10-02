/* sh_lookup: a name to addresses for the shell's commands (`host`,
 * `ping <name>`), through the resolver's /svc/dns (abi/idl/dns.idl). The
 * request is written without waiting on a channel of the command's own
 * and its answer waited for in slices, so Ctrl+C stops the wait at once
 * (closing the channel: the resolver forgets the request). */
#include <dns.h>
#include <idl/dns.h>
#include <ipv4.h>
#include "sh.h"

#define TIMEOUT 12000u              /* ms: past the resolver's own 10 s of tries */
#define SLICE   (50 * NS_PER_MS)    /* how often the wait looks at the keyboard */

/* The answer to txid on ch; ERR_CANCELED on Ctrl+C. */
static status_t wait_answer(handle_t ch, uint32_t txid, struct dns_answer *out)
{
    /* A second past the resolver's own timeout: it answers first. */
    uint64_t end = now() + (uint64_t)TIMEOUT * NS_PER_MS + NS_PER_S;
    for (;;) {
        _Alignas(8) uint8_t rep[DNS_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if (st == OK && m.txid == txid) {
            uint8_t n = 0;
            uint32_t a[DNS_ADDRS_MAX] = { 0 }, ttl = 0;
            st = dns_resolve_result(rep, &m, &n, &a[0], &a[1], &a[2], &a[3], &ttl);
            if (st == OK && (!n || n > DNS_ADDRS_MAX))
                st = ERR_INTERNAL;
            if (st == OK) {
                *out = (struct dns_answer){ .n = n, .ttl = ttl };
                memcpy(out->addr, a, sizeof(a));
            }
            return st;
        }
        if (st == ERR_INTERNAL && m.txid == txid)
            return st;   /* our answer, broken */
        if (st == OK || st == ERR_INTERNAL) {
            idl_msg_drop(&m);   /* not ours */
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

status_t sh_lookup(const char *name, struct dns_answer *out)
{
    uint32_t literal;
    const char *end = NULL;
    if (ipv4_parse(name, &literal, &end) && !*end) {
        *out = (struct dns_answer){ .addr = { literal }, .n = 1 };
        return OK;   /* an address needs no resolver */
    }
    uint8_t field[DNS_TEXT_MAX] = { 0 };
    size_t len = strnlen(name, DNS_TEXT_MAX);
    if (len == DNS_TEXT_MAX)
        return ERR_INVALID_ARGS;
    memcpy(field, name, len);
    handle_t ch;
    status_t st = svc_open(SVC_DNS, &ch);
    if (st != OK)
        return st == ERR_NOT_FOUND ? ERR_PEER_CLOSED : st;   /* no resolver */
    uint32_t last = 0, txid = idl_txid_next(&last);
    st = dns_resolve_send(ch, txid, field, TIMEOUT);
    if (st == OK)
        st = wait_answer(ch, txid, out);
    jam_handle_close(ch);
    return st;
}

const char *sh_lookup_why(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:     return "no such name (or it has no IPv4 address)";
    case ERR_TIMED_OUT:     return "no DNS server answered";
    case ERR_BAD_STATE:     return "no DNS server yet (see `net`)";
    case ERR_INVALID_ARGS:  return "not a host name";
    case ERR_IO:            return "the DNS servers failed";
    case ERR_NOT_SUPPORTED: return "the answer is too long for UDP";
    case ERR_OUT_OF_RANGE:  return "too many CNAMEs";
    case ERR_PEER_CLOSED:   return "no resolver (/svc/dns)";
    case ERR_CANCELED:      return "stopped";
    default:                return status_str(st);
    }
}
