/* dns: the answer cache. DNS_CACHE_SIZE entries, each a name and up to
 * DNS_MAX_ADDRS addresses, kept for the answer's TTL (at most
 * DNS_TTL_MAX; a TTL of 0 is not kept). Only whole answers to names
 * askers asked for go in (resolver.c), keyed by that name, never a
 * record a reply carried for some other name. Full: an expired entry
 * is reused first, else the one that would expire first. Lookups are a
 * linear scan: 32 entries. Failures are not cached. */
#include <os.h>
#include "dns.h"

static bool live(const struct dns_cache_entry *e, uint64_t now)
{
    return e->name[0] && now < e->expires;
}

status_t dns_cache_get(struct dns_cache *c, uint64_t now, const char *name, uint32_t *addr,
                       uint8_t *n, uint32_t *ttl_s)
{
    for (unsigned i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry *e = &c->e[i];
        if (!live(e, now) || !dns_name_eq(e->name, name))
            continue;
        memcpy(addr, e->addr, e->naddr * sizeof(uint32_t));
        *n = e->naddr;
        *ttl_s = (uint32_t)((e->expires - now + NS_PER_S - 1) / NS_PER_S);
        return OK;
    }
    return ERR_NOT_FOUND;
}

/* Where name goes: its own entry, a free or expired one, or the one
 * that expires first. */
static struct dns_cache_entry *victim(struct dns_cache *c, uint64_t now, const char *name)
{
    struct dns_cache_entry *best = &c->e[0];
    for (unsigned i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry *e = &c->e[i];
        if (e->name[0] && dns_name_eq(e->name, name))
            return e;
    }
    for (unsigned i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry *e = &c->e[i];
        if (!live(e, now))
            return e;
        if (e->expires < best->expires)
            best = e;
    }
    return best;
}

void dns_cache_put(struct dns_cache *c, uint64_t now, const char *name, const uint32_t *addr,
                   uint8_t n, uint32_t ttl_s)
{
    size_t len = strnlen(name, DNS_NAME_MAX + 1);
    if (ttl_s == 0 || n == 0 || n > DNS_MAX_ADDRS || len == 0 || len > DNS_NAME_MAX)
        return;
    if (ttl_s > DNS_TTL_MAX)
        ttl_s = DNS_TTL_MAX;
    struct dns_cache_entry *e = victim(c, now, name);
    memcpy(e->name, name, len + 1);
    memcpy(e->addr, addr, n * sizeof(uint32_t));
    e->naddr = n;
    e->expires = now + (uint64_t)ttl_s * NS_PER_S;
}

void dns_cache_flush(struct dns_cache *c)
{
    memset(c, 0, sizeof(*c));
}
