/* dns: the resolver process (docs/M9-PLAN.md, "DHCP and DNS: processes
 * of their own"). It will hold `/svc/net` and its own `/svc/dns` server
 * end: its loop waits on one port for its askers' channels (resolve is a
 * `later` method, answered from io->answer), for datagrams on its UDP
 * sockets, and for the resolver's deadline (dns_deadline); it fills
 * struct dns_io with a socket per local port and takes the DNS servers
 * from netstack (dns_set_servers).
 *
 * Not built yet: that loop and edge, which need `/svc/net`'s sockets and
 * dns.idl. The resolver itself (msg.c, cache.c, resolver.c) is complete
 * and tested by utest (user/tests/utest/dns.c). Until then the program
 * says so and ends. */
#include <os.h>
#include "dns.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("dns: not built yet: needs /svc/net's sockets\n");
    return 1;
}
