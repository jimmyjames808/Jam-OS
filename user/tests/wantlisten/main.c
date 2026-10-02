/* wantlisten: a program whose list asks for `svc net listen` (the network,
 * and the permission to listen: netstack's listen.h) and nothing else. It
 * tries a UDP socket on port 5000, a port only a listening program may
 * take, once on each channel it was given: /svc/net (refused) and
 * /svc/net-listen (taken), and says what each answered, then a port of
 * netstack's picking on /svc/net (taken). The shell's allow test
 * (tools/shell-tests/allow.txt) runs it from /boot, then a copy from
 * /data, which `allow` shows the owner and runs once approved.
 *
 * Startup handles: none but its namespace (/svc/net and /svc/net-listen)
 * and its terminal. Exits 0 when /svc/net-listen took the port and
 * /svc/net didn't, 1 otherwise. */
#include <net.h>
#include <os.h>
#include <wants.h>

#define PORT 5000

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc net listen\n");

/* Port `port` on the opener h: the answer, said; the socket closed again. */
static status_t try_port(const char *name, handle_t h, uint16_t port)
{
    struct net_sock s;
    status_t st = h ? net_udp_open(h, port, &s) : ERR_NOT_FOUND;
    printf("wantlisten: port %u on /svc/%s: %s\n", port, name, status_str(st));
    if (st == OK)
        net_close(&s);
    return st;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    status_t plain = try_port(SVC_NET, net_svc(), PORT);
    status_t listen = try_port(SVC_NET_LISTEN, svc_get(SVC_NET_LISTEN), PORT);
    status_t picked = try_port(SVC_NET, net_svc(), 0);
    return plain == ERR_ACCESS_DENIED && listen == OK && picked == OK ? 0 : 1;
}
