/* netlog: the kernel log over UDP to the Mac (docs/M9-PLAN.md, "netlog:
 * the log over UDP to the Mac"). init starts it in shell mode (its net.c)
 * once /data is mounted, when /data/etc/settings has `net.host` and not
 * `netlog = off`, with
 *   argv          bin/netlog <net.host> <the boot id, hex>: the Mac's
 *                 address, and the kernel's start in UTC ns (init works it
 *                 out once a boot, so a restarted netlog sends into the
 *                 same file on the Mac; 0: the clock wasn't known)
 *   SR_RESOURCE   the root with RIGHT_ROOT_KLOG only: a kernel log reader
 *   SR_NS         a namespace holding /svc/net and nothing else
 *   SR_CRASHLOG   on the boot after a panic: the panicked boot's log
 *                 (init's read-only duplicate)
 * It can read the log and send UDP datagrams, nothing more; what it parses
 * of the network is the Mac's 24-byte acks, from net.host:5021 only (its
 * socket is connected there, and each one is checked again here).
 *
 * The protocol, the go-back-N window and the bounds are netlog's core
 * (<netlog.h>, user/lib/netlog.c); this file is its edge. It serves
 * nobody, so it may block: it waits for netstack and an address
 * (net_wait_up) before it sends anything. The loop: what came on the
 * socket's channel (acks to netlog_ack; the answers to its sends are
 * counted by libos), netlog_poll, then one port wait for the socket, the
 * log growing (only while the live stream's window has room:
 * netlog_wants_text; a full window leaves the klog reader readable) or the
 * poll's deadline.
 *
 * It reads the log from its first byte, so the whole boot goes, and after
 * a panic the panicked boot's log too, as a stream of its own. The Mac
 * away: the core backs off to a try every 30 s and keeps its place.
 * netstack ending closes the socket: one line, then netlog waits for
 * /svc/net and an address again, opens a new socket and goes on where
 * the Mac's acks left it. Its own lines are state changes only (started,
 * netstack gone, and the core's three), never one per datagram or retry,
 * so the lines it sends can't make it send more.
 *
 * Memory: the core's one datagram, one received datagram and the socket;
 * the log itself is the kernel's ring, never copied. */
#include <ipv4.h>
#include <net.h>
#include <netlog.h>
#include <os.h>

#define KEY_SOCK 1u
#define KEY_LOG  2u
#define RETRY    NS_PER_S   /* no /svc/net, or no socket: the next try */
#define ACKS_A_TURN 64u     /* acks taken a turn; more wait for the next */

static struct netlog         nl;
static struct net_sock       sock;        /* .ch 0: none open */
static struct net_dgram      dg;          /* the datagram being taken */
static struct netlog_vmo_ctx crash_ctx;   /* the crash stream's source */
static handle_t              port, klog;
static uint32_t              host;        /* net.host */

static status_t io_send(void *ctx, const uint8_t *d, size_t len)
{
    (void)ctx;
    /* ERR_SHOULD_WAIT (the channel's queue is full) is a lost datagram too */
    return sock.ch ? net_sendto_async(&sock, 0, 0, d, len) : ERR_BAD_STATE;
}

static void io_say(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

/* 1 to 16 hex digits: true and *out. */
static bool parse_hex(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    unsigned n = 0;
    for (; *s; s++, n++) {
        char c = (char)(*s | 0x20);
        unsigned d = *s >= '0' && *s <= '9' ? (unsigned)(*s - '0')
                     : c >= 'a' && c <= 'f'  ? (unsigned)(c - 'a' + 10)
                                             : 16;
        if (d > 15 || n == 16)
            return false;
        v = v << 4 | d;
    }
    if (!n)
        return false;
    *out = v;
    return true;
}

/* The crash stream, after a panic. Its name for the start line, or NULL. */
static const char *add_crash(char name[32])
{
    handle_t v = startup_handle(SR_CRASHLOG);
    struct netlog_source src;
    if (!v)
        return NULL;
    if (netlog_crash_source(v, &crash_ctx, &src, name) != OK) {
        printf("netlog: the last boot's log can't be read: only this boot's is sent\n");
        return NULL;
    }
    netlog_add_crash(&nl, &src);
    name[31] = '\0';
    return name;
}

/* A socket connected to net.host's port, once netstack runs and has an
 * address. Blocks until then: nothing can be sent before. */
static void open_socket(void)
{
    bool said = false;
    for (;;) {
        handle_t net = net_svc();
        status_t st = net ? net_wait_up(net, DEADLINE_NEVER, NULL) : ERR_NOT_FOUND;
        if (st == OK)
            st = net_udp_open(net, 0, &sock);
        if (st == OK)
            st = net_connect(&sock, host, NETLOG_PORT);   /* only the Mac's datagrams come */
        if (st == OK)
            return;
        net_close(&sock);
        if (!said)
            printf("netlog: no network (%s): trying again every second\n", status_str(st));
        said = true;
        (void)jam_nanosleep(now() + RETRY);
    }
}

/* What came on the socket's channel: acks to the core. false: the socket
 * is gone (netstack ended). */
static bool take_acks(bool *more)
{
    *more = false;
    /* each turn takes one datagram off a ring netstack bounds */
    for (unsigned guard = 0; guard < ACKS_A_TURN; guard++) {
        status_t st = net_sock_take(&sock, &dg);
        if (st == ERR_SHOULD_WAIT)
            return true;
        if (st != OK)
            return false;   /* ERR_PEER_CLOSED: netstack is gone */
        if (dg.addr == host && dg.port == NETLOG_PORT)
            netlog_ack(&nl, dg.data, dg.len, now());
    }
    *more = true;   /* more may wait: the next turn looks without waiting */
    return true;
}

/* Send and take acks until the socket goes. */
static void run(void)
{
    bool more = false;
    while (take_acks(&more)) {
        uint64_t t = now();
        uint64_t next = netlog_poll(&nl, t);
        if (more)
            continue;   /* acks still waiting: take them before sleeping */
        /* The log growing matters only with room in the window, and not
         * while the core paces a burst (it asks back within NETLOG_PACE). */
        bool log_armed = netlog_wants_text(&nl, NETLOG_LIVE) &&
                         (next == UINT64_MAX || next > t + NETLOG_PACE) &&
                         jam_port_bind(port, klog, KEY_LOG, SIG_READABLE, PORT_BIND_ONCE) == OK;
        bool sock_armed = net_sock_bind(&sock, port, KEY_SOCK, PORT_BIND_ONCE) == OK;
        uint64_t deadline = next == UINT64_MAX ? DEADLINE_NEVER : next;
        if (!sock_armed && deadline > t + RETRY)
            deadline = t + RETRY;   /* can't hear the socket: look at it now and then */
        struct port_packet pkt;
        status_t st = jam_port_wait(port, deadline, &pkt);
        if (st == OK && pkt.key == KEY_LOG)
            log_armed = false;
        if (st == OK && pkt.key == KEY_SOCK)
            sock_armed = false;
        /* not fired: nothing to undo but the binding itself */
        if (log_armed)
            (void)jam_port_unbind(port, klog, KEY_LOG);
        if (sock_armed)
            net_sock_unbind(&sock);
        if (st != OK && st != ERR_TIMED_OUT) {
            printf("netlog: its port failed (%s): ending\n", status_str(st));
            jam_process_exit(1);   /* init starts it again */
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t boot = 0;
    const char *end = NULL;
    if (argc != 3 || !ipv4_parse(argv[1], &host, &end) || *end || !parse_hex(argv[2], &boot)) {
        printf("netlog: started without <address> <boot id in hex>: init starts it\n");
        return 2;
    }
    status_t st = jam_klog_open(startup_handle(SR_RESOURCE), &klog);
    if (st == OK)
        st = jam_port_create(&port);
    if (st != OK) {
        printf("netlog: can't start (%s): SR_RESOURCE must be the root with RIGHT_ROOT_KLOG\n",
               status_str(st));
        return 1;
    }
    struct netlog_source live;
    netlog_klog_source(klog, &live);
    static const struct netlog_io io = { .send = io_send, .say = io_say };
    netlog_start(&nl, &io, boot, &live);
    char name[32], a[IPV4_TEXT_MAX];
    const char *crashed = add_crash(name);
    printf("netlog: sending this boot's log%s%s%s to %s port %u\n",
           crashed ? " and the last boot's (" : "", crashed ? crashed : "", crashed ? ")" : "",
           ipv4_format(host, a), NETLOG_PORT);
    for (;;) {
        open_socket();
        run();
        net_close(&sock);
        printf("netlog: netstack has gone: going on when it is back\n");
    }
}
