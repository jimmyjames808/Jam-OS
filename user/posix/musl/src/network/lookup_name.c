/* musl's __lookup_name on Jam OS: overrides upstream's
 * src/network/lookup_name.c (user/posix/musl/OVERRIDES; docs/M13-PLAN.md
 * section 8). Upstream sends DNS queries of its own to resolv.conf's
 * servers; here a name that isn't numeric and isn't in /etc/hosts goes to
 * the system's one resolver, /svc/dns, through the call layer
 * (user/posix/sys/resolve.c's __jam_resolve). The null name, numeric
 * addresses and /etc/hosts are upstream's code as it was.
 *
 * Also changed: no search domains (the resolver takes whole names, and
 * libc's resolv.conf names none), and no RFC 6724 sorting of mixed
 * results (upstream connects a UDP socket to each address to choose;
 * Jam OS has no IPv6, so IPv4 addresses simply come first). The resolver
 * knows A records only: an AF_INET6 lookup it would answer is
 * EAI_NODATA. */
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../../../../third_party/musl/src/network/lookup.h"   /* upstream's, not installed */
#include "glue.h"
#include "stdio_impl.h"

static int is_valid_hostname(const char *host)
{
	const unsigned char *s;
	if (strnlen(host, 255)-1 >= 254 || mbstowcs(0, host, 0) == -1) return 0;
	for (s=(void *)host; *s>=0x80 || *s=='.' || *s=='-' || isalnum(*s); s++);
	return !*s;
}

static int name_from_null(struct address buf[static 2], const char *name, int family, int flags)
{
	int cnt = 0;
	if (name) return 0;
	if (flags & AI_PASSIVE) {
		if (family != AF_INET6)
			buf[cnt++] = (struct address){ .family = AF_INET };
		if (family != AF_INET)
			buf[cnt++] = (struct address){ .family = AF_INET6 };
	} else {
		if (family != AF_INET6)
			buf[cnt++] = (struct address){ .family = AF_INET, .addr = { 127,0,0,1 } };
		if (family != AF_INET)
			buf[cnt++] = (struct address){ .family = AF_INET6, .addr = { [15] = 1 } };
	}
	return cnt;
}

static int name_from_numeric(struct address buf[static 1], const char *name, int family)
{
	return __lookup_ipliteral(buf, name, family);
}

static int name_from_hosts(struct address buf[static MAXADDRS], char canon[static 256], const char *name, int family)
{
	char line[512];
	size_t l = strlen(name);
	int cnt = 0, badfam = 0, have_canon = 0;
	unsigned char _buf[1032];
	FILE _f, *f = __fopen_rb_ca("/etc/hosts", &_f, _buf, sizeof _buf);
	if (!f) switch (errno) {
	case ENOENT:
	case ENOTDIR:
	case EACCES:
		return 0;
	default:
		return EAI_SYSTEM;
	}
	while (fgets(line, sizeof line, f) && cnt < MAXADDRS) {
		char *p, *z;

		if ((p=strchr(line, '#'))) *p++='\n', *p=0;
		for(p=line+1; (p=strstr(p, name)) &&
			(!isspace(p[-1]) || !isspace(p[l])); p++);
		if (!p) continue;

		/* Isolate IP address to parse */
		for (p=line; *p && !isspace(*p); p++);
		*p++ = 0;
		switch (name_from_numeric(buf+cnt, line, family)) {
		case 1:
			cnt++;
			break;
		case 0:
			continue;
		default:
			badfam = EAI_NODATA;
			break;
		}

		if (have_canon) continue;

		/* Extract first name as canonical name */
		for (; *p && isspace(*p); p++);
		for (z=p; *z && !isspace(*z); z++);
		*z = 0;
		if (is_valid_hostname(p)) {
			have_canon = 1;
			memcpy(canon, p, z-p+1);
		}
	}
	__fclose_ca(f);
	return cnt ? cnt : badfam;
}

/* The resolver's answer: its IPv4 addresses, the name itself as the
 * canonical one (no trailing dot). */
static int name_from_resolver(struct address buf[static MAXADDRS], char canon[static 256], const char *name, int family)
{
	unsigned char a[MAXADDRS][4];
	size_t l = strlen(name);

	/* One trailing dot asks for the global name, as upstream's; more is no name. */
	if (l && name[l-1] == '.') l--;
	if (!l || name[l-1] == '.') return EAI_NONAME;
	memcpy(canon, name, l);
	canon[l] = 0;
	if (family == AF_INET6) return EAI_NODATA;

	int cnt = __jam_resolve(canon, a, MAXADDRS);
	for (int i = 0; i < cnt; i++) {
		buf[i] = (struct address){ .family = AF_INET };
		memcpy(buf[i].addr, a[i], 4);
	}
	return cnt;
}

int __lookup_name(struct address buf[static MAXADDRS], char canon[static 256], const char *name, int family, int flags)
{
	int cnt = 0, i, j;

	*canon = 0;
	if (name) {
		/* reject empty name and check len so it fits into temp bufs */
		size_t l = strnlen(name, 255);
		if (l-1 >= 254)
			return EAI_NONAME;
		memcpy(canon, name, l+1);
	}

	/* Procedurally, a request for v6 addresses with the v4-mapped
	 * flag set is like a request for unspecified family, followed
	 * by filtering of the results. */
	if (flags & AI_V4MAPPED) {
		if (family == AF_INET6) family = AF_UNSPEC;
		else flags -= AI_V4MAPPED;
	}

	/* Try each backend until there's at least one result. */
	cnt = name_from_null(buf, name, family, flags);
	if (!cnt) cnt = name_from_numeric(buf, name, family);
	if (!cnt && !(flags & AI_NUMERICHOST)) {
		cnt = name_from_hosts(buf, canon, name, family);
		if (!cnt) cnt = name_from_resolver(buf, canon, name, family);
	}
	if (cnt<=0) return cnt ? cnt : EAI_NONAME;

	/* Filter/transform results for v4-mapped lookup, if requested. */
	if (flags & AI_V4MAPPED) {
		if (!(flags & AI_ALL)) {
			/* If any v6 results exist, remove v4 results. */
			for (i=0; i<cnt && buf[i].family != AF_INET6; i++);
			if (i<cnt) {
				for (j=0; i<cnt; i++) {
					if (buf[i].family == AF_INET6)
						buf[j++] = buf[i];
				}
				cnt = i = j;
			}
		}
		/* Translate any remaining v4 results to v6 */
		for (i=0; i<cnt; i++) {
			if (buf[i].family != AF_INET) continue;
			memcpy(buf[i].addr+12, buf[i].addr, 4);
			memcpy(buf[i].addr, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
			buf[i].family = AF_INET6;
		}
	}

	/* IPv4 first, each family in the order found: the only addresses
	 * Jam OS can reach come before the ones it can't. */
	struct address v6[MAXADDRS];
	int n6 = 0;
	for (i=j=0; i<cnt; i++) {
		if (buf[i].family == AF_INET) buf[j++] = buf[i];
		else v6[n6++] = buf[i];
	}
	for (i=0; i<n6; i++) buf[j++] = v6[i];
	return cnt;
}
