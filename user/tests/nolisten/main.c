/* nolisten: a program whose list asks for the network but not the
 * permission to listen (`svc net`, no `listen`), as `fetch`'s does. It
 * tries a TCP listener on port 8081 (where the serve test serves) through
 * what it was given, /svc/net (netstack must refuse: ERR_ACCESS_DENIED),
 * and looks for /svc/net-listen in its namespace (it must not be there),
 * and says each. tools/serve-test.sh runs it. Exit 0 when both were
 * refused, 1 otherwise. */
#include <net.h>
#include <os.h>
#include <wants.h>

JAM_WANTS("svc net\n");

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct net_listener l;
    status_t st = net_wait_up(net_svc(), now() + 10 * NS_PER_S, NULL);
    if (st == OK)
        st = net_tcp_listen(net_svc(), 8081, 4, 0, 0, &l);
    printf("nolisten: a listener on port 8081 through /svc/net: %s\n", status_str(st));
    if (st == OK)
        net_listener_close(&l);
    handle_t h = svc_get(SVC_NET_LISTEN);
    printf("nolisten: /svc/net-listen in its namespace: %s\n", h ? "yes" : "no");
    bool refused = st == ERR_ACCESS_DENIED && !h;
    printf("nolisten: %s\n", refused ? "PASS: refused without the listen permission"
                                     : "FAIL: listened without the permission");
    return refused ? 0 : 1;
}
