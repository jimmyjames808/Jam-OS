/* bin/sntp: the clock from the network (RFC 4330, SNTP; the checks are
 * ntp.c's). init starts it in shell mode (its net.c) once /data is
 * mounted, unless the settings say `ntp = off`, with
 *   argv          bin/sntp [<server>]: the setting `ntp.server`, an IPv4
 *                 address or a name; without one, the network's gateway
 *                 (the DHCP lease's router, or net.address's), then
 *                 pool.ntp.org if the gateway gives no time
 *   SR_RESOURCE   the root with RIGHT_ROOT_CLOCK only: it may set the
 *                 kernel's clock (wallclock_set) and read the RTC, nothing
 *                 more. No other program holds that power but init and
 *                 the shell
 *   SR_NS         /svc/net and /svc/dns, nothing else
 *
 * It serves nobody, so it may block, always with a deadline. It waits for
 * an address (net_wait_up), then asks: a socket of its own on a port
 * netstack picks, connected to the server's port 123 (netstack keeps
 * every other sender's datagrams away), up to TRIES requests 2 s apart,
 * each with a fresh random nonce; only a reply that passes every check of
 * ntp_check, its origin the nonce of the request it answers, is believed.
 * Anything else is counted and said in one line per request, never used.
 *
 * Setting the clock: the first time, by whatever the network says (the
 * RTC may be hours out: local time or UTC, <settings.h> `rtc`), but a
 * step of more than CONFIRM_OVER needs a second reply, to a second nonce,
 * that agrees within a second. After that it asks again every hour
 * (RESYNC) and moves the clock at most STEP_MAX each time; a step cut
 * short asks again in RESYNC_SOON, so a real error is caught up in
 * steps, and a lying server can only drag the clock slowly. A round with
 * no time backs off from 16 s to 1024 s. Each set is a log line with the
 * offset found; failures are a line a round. The kernel marks the clock
 * WALLCLOCK_NET, so init keeps it when /data comes back (settings.c), and
 * `date -r` says where it came from. */
#include <dns.h>
#include <ipv4.h>
#include <net.h>
#include <os.h>
#include "ntp.h"

#define TRIES         4                  /* requests to one server a round */
#define TRY_WAIT      (2 * NS_PER_S)     /* for each one's reply */
#define LOOKUP_WAIT   (5 * NS_PER_S)     /* for a server's name */
#define NET_RETRY     NS_PER_S           /* no /svc/net yet, or netstack gone */
#define RETRY_FIRST   (16 * NS_PER_S)    /* after a round with no time; doubled each time ... */
#define RETRY_MAX     (1024 * NS_PER_S)  /* ... up to this */
#define RESYNC        (3600 * NS_PER_S)  /* after a round that set the clock */
#define RESYNC_SOON   (64 * NS_PER_S)    /* after a re-sync's step was cut short */
#define STEP_MAX      (5 * (int64_t)NS_PER_S)    /* a re-sync moves the clock this much at most */
#define CONFIRM_OVER  (60 * (int64_t)NS_PER_S)   /* the first set: a bigger step needs ... */
#define CONFIRM_AGREE ((int64_t)NS_PER_S)        /* ... two replies that agree this closely */
#define READ_GUARD    (2 * NET_RX_QUEUE)  /* datagrams read for one request at most */
#define POOL          "pool.ntp.org"

static handle_t         root;        /* SR_RESOURCE: RIGHT_ROOT_CLOCK */
static const char      *server;      /* ntp.server; NULL: the gateway, then POOL */
static bool             synced;      /* the clock has been set from the network */
static struct net_dgram dg;

/* What one round found: a time, and from whom. */
struct found {
    struct ntp_result r;
    uint32_t          addr;
    const char       *name;   /* as asked: an address, a name, or "the gateway" */
};

/* One request on s (connected to the server); a believed reply into *out.
 * ERR_TIMED_OUT: none came; ERR_ACCESS_DENIED: the server sent a
 * kiss-o'-death (*kiss its code): don't ask it again this round. */
static status_t ask(struct net_sock *s, struct ntp_result *out, char kiss[5])
{
    uint64_t rnd = 0;
    os_random(&rnd, sizeof(rnd));
    uint64_t nonce = ntp_nonce(rnd), sent = now();
    uint8_t req[NTP_PACKET];
    ntp_request(nonce, req);
    status_t st = net_send(s, req, sizeof(req));
    unsigned ignored = 0;
    enum ntp_verdict why = NTP_OK;
    for (unsigned k = 0; st == OK && k < READ_GUARD; k++) {
        st = net_recvfrom(s, &dg, sent + TRY_WAIT);
        if (st != OK)
            break;
        enum ntp_verdict v = ntp_check(dg.data, dg.len, nonce, sent, now(), out);
        if (v == NTP_OK || v == NTP_KISS) {
            st = v == NTP_OK ? OK : ERR_ACCESS_DENIED;
            memcpy(kiss, out->kiss, 5);
            break;
        }
        ignored++;
        why = v;
    }
    if (ignored)
        printf("sntp: %u repl%s ignored (the last: %s)\n", ignored, ignored == 1 ? "y" : "ies",
               ntp_verdict_str(why));
    if (st == OK || st == ERR_ACCESS_DENIED || st == ERR_PEER_CLOSED)
        return st;
    return ERR_TIMED_OUT;   /* the deadline, or READ_GUARD replies that weren't ours */
}

/* The clock's time at uptime up, or false if there is no clock. */
static bool clock_at(uint64_t up, int64_t *out)
{
    struct wall_clock w;
    if (jam_wallclock_get(&w) != OK)
        return false;
    *out = w.utc_ns + (int64_t)(up - w.uptime_ns);
    return true;
}

/* r's step from our clock (0 without one: anything is better). */
static int64_t step_of(const struct ntp_result *r)
{
    int64_t ours;
    return clock_at(r->uptime_ns, &ours) ? r->utc_ns - ours : 0;
}

/* The first set, by more than CONFIRM_OVER: a second reply must agree. */
static status_t confirm(struct net_sock *s, struct ntp_result *r)
{
    struct ntp_result again;
    char kiss[5];
    status_t st = ask(s, &again, kiss);
    if (st != OK)
        return st;
    int64_t a = r->utc_ns - (int64_t)r->uptime_ns, b = again.utc_ns - (int64_t)again.uptime_ns;
    if (a - b > CONFIRM_AGREE || b - a > CONFIRM_AGREE) {
        printf("sntp: two replies disagree by %ld ms: the clock is not set\n",
               (long)((a - b) / (int64_t)NS_PER_MS));
        return ERR_BAD_STATE;
    }
    *r = again;
    return OK;
}

/* Ask the server at addr, up to TRIES times: OK and *r, or why not. */
static status_t ask_server(handle_t net, uint32_t addr, struct ntp_result *r)
{
    struct net_sock s;
    status_t st = net_udp_open(net, 0, &s);
    if (st == OK)
        st = net_connect(&s, addr, NTP_PORT);   /* only the server's datagrams come */
    char kiss[5];
    unsigned t = 0;
    if (st == OK)
        do
            st = ask(&s, r, kiss);
        while (st == ERR_TIMED_OUT && ++t < TRIES);
    if (st == ERR_ACCESS_DENIED)
        printf("sntp: the server says %.4s (a kiss-o'-death): not asked again this round\n", kiss);
    int64_t step = st == OK ? step_of(r) : 0;
    if (st == OK && !synced && (step > CONFIRM_OVER || step < -CONFIRM_OVER))
        st = confirm(&s, r);
    net_close(&s);
    return st;
}

/* The address of what is asked: an IPv4 address as it is, a name through
 * /svc/dns. */
static status_t address_of(const char *name, uint32_t *out)
{
    const char *end = NULL;
    if (ipv4_parse(name, out, &end) && !*end)
        return OK;
    struct dns_answer a;
    status_t st = dns_lookup(name, now() + LOOKUP_WAIT, &a);
    if (st == OK)
        *out = a.addr[0];
    return st;
}

/* One round: the servers in turn until one gives a time. */
static status_t sync_round(handle_t net, struct found *f)
{
    struct net_info in = { 0 };
    status_t st = net_info(net, &in);
    const char *names[2] = { server, NULL };
    if (!server) {
        names[0] = "the gateway";
        names[1] = POOL;
    }
    for (unsigned i = 0; st != ERR_PEER_CLOSED && i < 2 && names[i]; i++) {
        uint32_t addr = in.gateway;
        bool gw = !server && i == 0;
        st = gw ? (addr ? OK : ERR_NOT_FOUND) : address_of(names[i], &addr);
        if (st == OK)
            st = ask_server(net, addr, &f->r);
        if (st == OK) {
            f->addr = addr;
            f->name = names[i];
            return OK;
        }
        printf("sntp: no time from %s (%s)\n", names[i], status_str(st));
    }
    return st;
}

/* Set the clock from what the round found: the whole step the first
 * time, at most STEP_MAX after. true if the step was cut short. */
static bool apply(const struct found *f)
{
    int64_t ours = 0, step = 0;
    bool have = clock_at(f->r.uptime_ns, &ours);
    if (have)
        step = f->r.utc_ns - ours;
    int64_t take = step;
    if (synced && take > STEP_MAX)
        take = STEP_MAX;
    else if (synced && take < -STEP_MAX)
        take = -STEP_MAX;
    struct wall_clock w = { 0 };
    w.utc_ns = have ? ours + take : f->r.utc_ns;
    w.uptime_ns = f->r.uptime_ns;
    w.flags = WALLCLOCK_NET;   /* the zone ("") stays as init set it */
    status_t st = jam_wallclock_set(root, &w);
    char a[IPV4_TEXT_MAX], did[40];
    ipv4_format(f->addr, a);
    if (st != OK)
        snprintf(did, sizeof(did), "NOT set (%s)", status_str(st));
    else if (take != step)
        snprintf(did, sizeof(did), "moved %ld s of it", (long)(take / (int64_t)NS_PER_S));
    else
        snprintf(did, sizeof(did), "set");
    uint64_t ms = (uint64_t)(step < 0 ? -step : step) / NS_PER_MS;
    printf("sntp: the clock was %c%lu.%03lu s off: %s from %s (%s, stratum %u, round trip "
           "%lu us)\n", step < 0 ? '-' : '+', (unsigned long)(ms / 1000),
           (unsigned long)(ms % 1000), did, f->name, a, f->r.stratum,
           (unsigned long)(f->r.delay_ns / 1000));
    if (st == OK)
        synced = true;
    return st == OK && take != step;
}

/* /svc/net with an address, waiting as long as it takes. */
static handle_t wait_up(void)
{
    bool said = false;
    for (;;) {
        handle_t net = net_svc();
        status_t st = net ? net_wait_up(net, DEADLINE_NEVER, NULL) : ERR_NOT_FOUND;
        if (st == OK)
            return net;
        if (!said)
            printf("sntp: no network (%s): trying again every second\n", status_str(st));
        said = true;
        (void)jam_nanosleep(now() + NET_RETRY);
    }
}

int main(int argc, char **argv)
{
    root = startup_handle(SR_RESOURCE);
    server = argc >= 2 && argv[1][0] ? argv[1] : NULL;
    if (!root || argc > 2) {
        printf("sntp: started without the root (RIGHT_ROOT_CLOCK) or with more than a server: "
               "init starts it\n");
        return 2;
    }
    printf("sntp: asking %s for the time\n", server ? server : "the gateway, then " POOL);
    uint64_t backoff = RETRY_FIRST;
    for (;;) {
        struct found f;
        status_t st = sync_round(wait_up(), &f);
        uint64_t wait = st == ERR_PEER_CLOSED ? NET_RETRY : backoff;
        if (st == OK) {
            wait = apply(&f) ? RESYNC_SOON : RESYNC;
            backoff = RETRY_FIRST;
        } else if (st != ERR_PEER_CLOSED) {
            printf("sntp: no time this round: asking again in %lu s\n",
                   (unsigned long)(backoff / NS_PER_S));
            backoff = backoff * 2 > RETRY_MAX ? RETRY_MAX : backoff * 2;
        }
        (void)jam_nanosleep(now() + wait);
    }
}
