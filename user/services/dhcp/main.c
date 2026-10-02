/* dhcp: the DHCP client process (docs/M9-PLAN.md, "DHCP and DNS:
 * processes of their own"). It will hold netctl and nothing else:
 * its loop waits on one port for netctl's DHCP socket (port 68) and for
 * the client's deadline (dhcp_deadline), and fills struct dhcp_io:
 * send through the socket, bound and unbound as netctl's set_ipv4,
 * set_dns and clear, the lease logged once.
 *
 * Not built yet: that loop and edge, which need netctl's dhcp_open.
 * The client itself (msg.c, client.c) is complete and tested by utest
 * (user/tests/utest/dhcp.c). Until then the program says so and ends. */
#include <os.h>
#include "dhcp.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("dhcp: not built yet: needs netctl's DHCP socket\n");
    return 1;
}
