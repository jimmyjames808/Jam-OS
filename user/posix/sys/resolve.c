/* Names for musl's lookups (the override of src/network/lookup_name.c,
 * docs/M13-PLAN.md section 8): getaddrinfo, gethostbyname and their
 * friends ask the system's one resolver, /svc/dns (libjam's dns_lookup),
 * after /etc/hosts (libc's, etc.c), instead of sending DNS queries of
 * their own. A program's list asks for it with `svc dns`.
 *
 * The resolver knows IPv4 addresses only (A records), keeps its own
 * cache and waits at most 10 s for its servers; the call here waits a
 * little longer than that, so its answer always comes back. The wait is
 * a channel call: a signal doesn't end it. */
#include <netdb.h>
#include <string.h>
#include "layer.h"
#include "sig.h"

#include <dns.h>

#define RESOLVE_WAIT (15 * NS_PER_S)   /* past the resolver's own 10 s */

static int resolve(const char *name, unsigned char (*addrs)[4], int max);

int __jam_resolve(const char *name, unsigned char (*addrs)[4], int max)
{
    struct sig_thread *q = sig_quiet_begin();   /* the layer's own: no handler runs inside */
    int r = resolve(name, addrs, max);
    sig_quiet_end(q);
    return r;
}

static int resolve(const char *name, unsigned char (*addrs)[4], int max)
{
    struct dns_answer a;
    status_t st = dns_lookup(name, now() + RESOLVE_WAIT, &a);
    switch (st) {
    case OK:
        break;
    case ERR_NOT_FOUND:
    case ERR_INVALID_ARGS:
        return EAI_NONAME;
    case ERR_TIMED_OUT:
    case ERR_BAD_STATE:      /* no DNS servers yet */
    case ERR_NO_RESOURCES:   /* too many lookups in flight */
        return EAI_AGAIN;
    default:                 /* the servers failed, TCP needed, no resolver */
        return EAI_FAIL;
    }
    int n = 0;
    for (unsigned i = 0; i < a.n && n < max; i++, n++) {
        addrs[n][0] = (unsigned char)(a.addr[i] >> 24);
        addrs[n][1] = (unsigned char)(a.addr[i] >> 16);
        addrs[n][2] = (unsigned char)(a.addr[i] >> 8);
        addrs[n][3] = (unsigned char)a.addr[i];
    }
    return n ? n : EAI_NONAME;
}
