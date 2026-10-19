/* Names to addresses (<dns.h>): blocking calls over abi/idl/dns.idl.
 *
 * The wait is the resolver's: the request carries its timeout, the
 * resolver answers by then, and the call itself waits a little longer
 * (MARGIN), so an answer never comes after its caller gave up and lies
 * on the channel for a later call to trip over. */
#include <dns.h>
#include <idl/dns.h>

#define MARGIN NS_PER_S   /* the call waits this much past the resolver's timeout */
#define ROOM_FIRST (10 * NS_PER_MS)    /* the first wait for a lookup in flight to end */
#define ROOM_MAX   (160 * NS_PER_MS)   /* ... doubling up to this */

handle_t dns_svc(void)
{
    handle_t h = svc_get(SVC_DNS_SYS);
    return h ? h : svc_get(SVC_DNS);
}

status_t dns_lookup_on(handle_t dns, const char *name, uint64_t deadline,
                       struct dns_answer *out)
{
    size_t len = strnlen(name, DNS_NAME_MAX + 1);
    if (!len || len > DNS_NAME_MAX)
        return ERR_INVALID_ARGS;
    uint64_t t = now();
    if (deadline <= t)
        return ERR_TIMED_OUT;
    uint64_t ms = (deadline - t + NS_PER_MS - 1) / NS_PER_MS;
    if (ms > DNS_TIMEOUT_MAX)
        ms = DNS_TIMEOUT_MAX;
    uint32_t n = 0;
    uint32_t a[DNS_ADDRS_MAX] = { 0 }, ttl = 0;
    status_t st = dns_resolve_until(dns, t + ms * NS_PER_MS + MARGIN, name, (uint32_t)len,
                                    (uint32_t)ms, a, &n, &ttl);
    if (st != OK)
        return st;
    if (!n || n > DNS_ADDRS_MAX)
        return ERR_INTERNAL;   /* a resolver that says OK has an address */
    *out = (struct dns_answer){ .n = n, .ttl = ttl };
    memcpy(out->addr, a, sizeof(a));
    return OK;
}

/* Asked again once if the resolver restarted meanwhile. */
static status_t lookup_once(const char *name, uint64_t deadline, struct dns_answer *out)
{
    handle_t dns = dns_svc();
    if (!dns)
        return ERR_PEER_CLOSED;
    status_t st = dns_lookup_on(dns, name, deadline, out);
    if (st != ERR_PEER_CLOSED)
        return st;
    dns = dns_svc();   /* the resolver restarted: its new channel */
    return dns ? dns_lookup_on(dns, name, deadline, out) : st;
}

status_t dns_lookup(const char *name, uint64_t deadline, struct dns_answer *out)
{
    /* Too many in flight: the program's (one opener, DNS_PER_OPENER) or
     * every program's together. Wait for one to end, a little longer each
     * time, as long as the deadline allows. */
    uint64_t pause = ROOM_FIRST;
    for (;;) {
        status_t st = lookup_once(name, deadline, out);
        if (st != ERR_NO_RESOURCES || now() + pause >= deadline)
            return st;
        (void)jam_sleep_until(now() + pause);
        pause = pause * 2 < ROOM_MAX ? pause * 2 : ROOM_MAX;
    }
}
