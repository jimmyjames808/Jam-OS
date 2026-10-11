/* musl's raw DNS queries on Jam OS: overrides upstream's
 * src/network/res_msend.c (user/posix/musl/OVERRIDES; docs/M13-PLAN.md
 * section 8).
 *
 * Upstream sends the queries over UDP (and TCP) to resolv.conf's servers
 * and waits for their answers: res_query, res_search, res_send and
 * getnameinfo's reverse lookup (a PTR query) come here. Jam OS has one
 * resolver for the system, /svc/dns, and its protocol takes a name and
 * gives IPv4 addresses (A records), so a raw query of any type has nowhere
 * to go; and libc's resolv.conf names no server, so upstream's client asked
 * 127.0.0.1, where nothing listens, and waited out its whole timeout
 * (about 10 s) before failing. Here every query fails at once, as when no
 * server answers: res_query and res_send return -1, and getnameinfo gives
 * the numeric host (or EAI_NONAME with NI_NAMEREQD). getaddrinfo and
 * gethostbyname go to /svc/dns (the override of lookup_name.c), not here. */
#include <resolv.h>
#include "../../../../../third_party/musl/src/network/lookup.h"   /* upstream's, not installed */

int __res_msend_rc(int nqueries, const unsigned char *const *queries,
	const int *qlens, unsigned char *const *answers, int *alens, int asize,
	const struct resolvconf *conf)
{
	return -1;
}

int __res_msend(int nqueries, const unsigned char *const *queries,
	const int *qlens, unsigned char *const *answers, int *alens, int asize)
{
	return -1;
}
