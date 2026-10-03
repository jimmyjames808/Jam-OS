/* nolisten: a program whose list asks for the network but not the
 * permission to listen (`svc net`, no `listen`), as `fetch`'s does. It
 * tries a TCP listener on port 8081 and one on port 80 (where the serve
 * test serves) through what it was given, /svc/net (netstack must refuse:
 * ERR_ACCESS_DENIED), and looks for /svc/net-listen and
 * /svc/net-low in its namespace (they must not be there), and says
 * each. tools/serve-test.sh runs it. Exit 0 when all were refused, 1
 * otherwise. */
#include <net.h>
#include <os.h>
#include <wants.h>

JAM_WANTS("svc net\n");

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct net_listener l;
    status_t up = net_wait_up(net_svc(), now() + 10 * NS_PER_S, NULL), st[2];
    static const uint16_t ports[2] = { 8081, 80 };
    for (unsigned i = 0; i < 2; i++) {
        st[i] = up == OK ? net_tcp_listen(net_svc(), ports[i], 4, 0, 0, &l) : up;
        printf("nolisten: a listener on port %u through /svc/net: %s\n", ports[i],
               status_str(st[i]));
        if (st[i] == OK)
            net_listener_close(&l);
    }
    handle_t h = svc_get(SVC_NET_LISTEN), hl = svc_get(SVC_NET_LISTEN_LOW);
    printf("nolisten: /svc/net-listen in its namespace: %s\n", h ? "yes" : "no");
    printf("nolisten: /svc/net-low in its namespace: %s\n", hl ? "yes" : "no");
    bool refused = st[0] == ERR_ACCESS_DENIED && st[1] == ERR_ACCESS_DENIED && !h && !hl;
    printf("nolisten: %s\n", refused ? "PASS: refused without the listen permission"
                                     : "FAIL: listened without the permission");
    return refused ? 0 : 1;
}
