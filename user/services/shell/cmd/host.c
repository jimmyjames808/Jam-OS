/* host: a name's IPv4 addresses from the resolver (/svc/dns, sh_lookup):
 * one line per address, with how long the answer may be kept. Ctrl+C
 * stops the wait. */
#include <dns.h>
#include <ipv4.h>
#include "sh.h"

SH_CMD(host)
{
    if (argc != 2) {
        sh_tty("usage: host <name>   (e.g. host one.one.one.one)\n");
        return 2;
    }
    struct dns_answer ans;
    uint64_t t0 = now();
    status_t st = sh_lookup(argv[1], &ans);
    if (st != OK) {
        sh_say("host: %s: %s\n", argv[1], sh_lookup_why(st));
        return 1;
    }
    uint64_t ms = (now() - t0) / NS_PER_MS;
    for (unsigned i = 0; i < ans.n; i++) {
        char a[IPV4_TEXT_MAX];
        sh_say("%s has address %s\n", argv[1], ipv4_format(ans.addr[i], a));
    }
    sh_say("(ttl %u s, %lu ms)\n", (unsigned)ans.ttl, (unsigned long)ms);
    return 0;
}
