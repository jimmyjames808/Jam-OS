/* Names to addresses for programs (user/lib/dns.c): the resolver's
 * /svc/dns (abi/idl/dns.idl; the design is docs/M9-PLAN.md "DHCP and
 * DNS: processes of their own"). A program's list asks for it with
 * `svc dns`.
 *
 * dns_lookup blocks, for programs and threads that serve nobody. A loop
 * that serves others sends the request itself without waiting
 * (dns_resolve_send on a channel of its own from svc_open, the answer
 * read with idl_reply_read and dns_resolve_result, <idl/dns.h>), as the
 * shell's `host` does so Ctrl+C works while it waits.
 *
 * Each name in flight is the resolver's own query, so a lookup of a name
 * whose server never answers delays only the lookups of that name. */
#pragma once

#include <stdint.h>
#include <os.h>

#define DNS_ADDRS_MAX    4u       /* addresses in one answer */
#define DNS_TEXT_MAX     256u     /* bytes of a name, with its NUL (dns.idl's field) */
#define DNS_OPENERS      16u      /* channels from /svc/dns's connect at once */
#define DNS_PER_OPENER   8u       /* resolves in flight on one opener's channel */
#define DNS_TIMEOUT_MAX  60000u   /* ms: the longest timeout a resolve may ask for */

/* An answer: n addresses (host order, <netbytes.h>), kept for ttl s. */
struct dns_answer {
    uint32_t addr[DNS_ADDRS_MAX];
    unsigned n;
    uint32_t ttl;
};

/* /svc/dns: libos's own channel to it (svc_get: opened again after the
 * resolver restarts), or HANDLE_INVALID when the program's list didn't
 * ask for it. Don't close it. */
handle_t dns_svc(void);
/* The IPv4 addresses of name (a host name, or a dotted address answered
 * as it is), waiting until the deadline at most (at most DNS_TIMEOUT_MAX
 * from now). Asked again once if the resolver restarted meanwhile.
 * Errors: ERR_NOT_FOUND (no such name, or no IPv4 address),
 * ERR_TIMED_OUT, ERR_INVALID_ARGS (not a name), ERR_BAD_STATE (no DNS
 * servers yet), ERR_NO_RESOURCES (too many in flight), ERR_IO (the
 * servers failed), ERR_NOT_SUPPORTED (the answer needs TCP),
 * ERR_PEER_CLOSED (no resolver). *out only on OK. */
status_t dns_lookup(const char *name, uint64_t deadline, struct dns_answer *out);
/* The same on a channel of the caller's (svc_open(SVC_DNS, ...)), asked
 * once. */
status_t dns_lookup_on(handle_t dns, const char *name, uint64_t deadline,
                       struct dns_answer *out);
