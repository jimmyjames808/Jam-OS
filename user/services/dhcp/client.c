/* dhcp: the client's state machine (RFC 2131 figure 5 and section 4.4).
 *
 *   INIT -> SELECTING: DISCOVER, broadcast, retransmitted at 4, 8, 16,
 *           32, 64, 64 ... s (+- 1 s) until an OFFER comes.
 *   SELECTING -> REQUESTING: the first acceptable OFFER is asked for
 *           (REQUEST with options 50 and 54, the DISCOVER's xid).
 *   REQUESTING -> PROBING (or BOUND without a probe hook): its ACK.
 *           A NAK, or no answer after DHCP_REQUEST_TRIES, starts over:
 *           after a NAK with a wait that doubles from 2 s, so a server
 *           that always says no costs at most one DISCOVER a minute.
 *   PROBING -> BOUND: the edge's ARP probe found no one (or didn't
 *           answer in DHCP_PROBE_WAIT); someone answered: DECLINE, and
 *           INIT for 10 s (RFC 2131 3.1.5).
 *   BOUND -> RENEWING at T1: REQUEST to our server, unicast.
 *   RENEWING -> REBINDING at T2: REQUEST broadcast, any server.
 *   RENEWING, REBINDING -> BOUND: an ACK (new times; the edge hears
 *           only if something else changed). A NAK, or the lease's
 *           end, clears the address and starts over at once.
 *   REBOOTING: dhcp_start with an earlier address asks for it again
 *           (INIT-REBOOT): an ACK keeps it, a NAK or silence starts over.
 *
 * A reply counts only if it is for the exchange's xid and our MAC
 * (msg.c), in a state that waits for it, and from the server we asked:
 * an ACK or NAK in REQUESTING or RENEWING must name the server our
 * REQUEST named (option 54), and an ACK must give the address we asked
 * for. Retransmits keep the exchange's xid, so a slow server's late
 * reply to an earlier send still counts. A lease's times count from
 * when the exchange's first REQUEST was sent, not from when the ACK came
 * (RFC 2131 4.4.1): the earliest the server can have counted from.
 *
 * Nothing here blocks or sleeps: each step sends at most one datagram
 * and sets c->deadline, which the caller's loop waits for. */
#include <os.h>
#include "dhcp.h"

static uint64_t add_ns(uint64_t at, uint64_t ns)
{
    return at > DEADLINE_NEVER - ns ? DEADLINE_NEVER : at + ns;
}

/* ms, plus or minus up to DHCP_JITTER_MS, in ns (ms > DHCP_JITTER_MS). */
static uint64_t jittered(struct dhcp_client *c, uint32_t ms)
{
    uint32_t r = c->io->random(c->io->ctx) % (2 * DHCP_JITTER_MS + 1);
    return ((uint64_t)ms + r - DHCP_JITTER_MS) * NS_PER_MS;
}

/* Send one message of this exchange. */
static void send_msg(struct dhcp_client *c, uint64_t now, uint32_t to, struct dhcp_out *m)
{
    uint8_t buf[DHCP_MSG_BUILT];
    uint64_t secs = now > c->started ? (now - c->started) / NS_PER_S : 0;
    m->xid = c->xid;
    m->secs = secs > 0xffff ? 0xffff : (uint16_t)secs;
    size_t len = dhcp_build(buf, sizeof(buf), c->mac, m);
    if (len && c->io->send(c->io->ctx, to, buf, len) == OK)
        c->stats.sent++;
    else
        c->stats.send_failed++;
}

/* A new exchange: a fresh xid, its clock started. */
static void exchange(struct dhcp_client *c, uint64_t now, enum dhcp_state s)
{
    c->state = s;
    c->xid = c->io->random(c->io->ctx);
    c->started = now;
    c->retx_ms = DHCP_RETX_FIRST_MS;
    c->tries = 0;
}

/* The next retransmit, backing off. */
static void backoff(struct dhcp_client *c, uint64_t now)
{
    c->deadline = now + jittered(c, c->retx_ms);
    c->retx_ms = c->retx_ms >= DHCP_RETX_MAX_MS / 2 ? DHCP_RETX_MAX_MS : c->retx_ms * 2;
}

static void send_discover(struct dhcp_client *c, uint64_t now)
{
    struct dhcp_out m = { .type = DHCP_DISCOVER, .broadcast = true };
    send_msg(c, now, DHCP_BROADCAST, &m);
    backoff(c, now);
}

static void discover(struct dhcp_client *c, uint64_t now)
{
    exchange(c, now, DHCP_SELECTING);
    send_discover(c, now);
}

/* A REQUEST in REQUESTING (for c->offer) or REBOOTING (for
 * c->offer.yiaddr, naming no server). */
static void send_request(struct dhcp_client *c, uint64_t now)
{
    struct dhcp_out m = { .type = DHCP_REQUEST, .broadcast = true,
                          .requested = c->offer.yiaddr };
    if (c->state == DHCP_REQUESTING)
        m.server = c->offer.server;
    send_msg(c, now, DHCP_BROADCAST, &m);
    if (!c->tries++)
        c->sent_at = now;   /* the ACK may answer this first send */
    backoff(c, now);
}

/* Give the address up and start over at once. */
static void lose(struct dhcp_client *c, uint64_t now, enum dhcp_why why)
{
    if (c->have_lease) {
        c->have_lease = false;
        c->io->unbound(c->io->ctx, why);
    }
    discover(c, now);
}

/* ---- the lease ----------------------------------------------------------- */

static bool same_config(const struct dhcp_lease *a, const struct dhcp_lease *b)
{
    if (a->addr != b->addr || a->mask != b->mask || a->router != b->router ||
        a->server != b->server || a->ndns != b->ndns)
        return false;
    for (unsigned i = 0; i < a->ndns; i++)
        if (a->dns[i] != b->dns[i])
            return false;
    return true;
}

/* The lease an ACK gives, its times checked: a lease under
 * DHCP_MIN_LEASE_S is taken as that; T1 and T2 must satisfy
 * 0 < T1 < T2 < lease, else they are 1/2 and 7/8 of it (RFC 2131 4.4.5). */
static struct dhcp_lease lease_of(const struct dhcp_reply *r)
{
    struct dhcp_lease l = { .addr = r->yiaddr, .mask = r->mask, .router = r->router,
                            .server = r->server, .ndns = r->ndns, .lease_s = r->lease_s };
    memcpy(l.dns, r->dns, sizeof(l.dns));
    if (l.lease_s == DHCP_INFINITE)
        return l;
    if (l.lease_s < DHCP_MIN_LEASE_S)
        l.lease_s = DHCP_MIN_LEASE_S;
    l.t1_s = r->t1_s;
    l.t2_s = r->t2_s;
    if (!l.t1_s || !l.t2_s || l.t1_s >= l.t2_s || l.t2_s >= l.lease_s) {
        l.t1_s = l.lease_s / 2;
        l.t2_s = (uint32_t)((uint64_t)l.lease_s * 7 / 8);
    }
    return l;
}

static void bind(struct dhcp_client *c, const struct dhcp_reply *r)
{
    struct dhcp_lease l = lease_of(r);
    bool tell = !c->have_lease || !same_config(&c->lease, &l);
    c->lease = l;
    c->have_lease = true;
    if (l.lease_s == DHCP_INFINITE) {
        c->t1_at = c->t2_at = c->end_at = DEADLINE_NEVER;
    } else {
        c->t1_at = add_ns(c->sent_at, (uint64_t)l.t1_s * NS_PER_S);
        c->t2_at = add_ns(c->sent_at, (uint64_t)l.t2_s * NS_PER_S);
        c->end_at = add_ns(c->sent_at, (uint64_t)l.lease_s * NS_PER_S);
    }
    c->state = DHCP_BOUND;
    c->deadline = c->t1_at;
    c->nak_wait_ms = DHCP_NAK_WAIT_MS;
    if (tell) {
        c->stats.leases++;
        c->io->bound(c->io->ctx, &c->lease);
    }
}

/* The ACK for a new address: probe it first if the edge can. */
static void acked(struct dhcp_client *c, uint64_t now, const struct dhcp_reply *r)
{
    c->stats.acks++;
    c->offer = *r;
    if (!c->io->probe) {
        bind(c, r);
        return;
    }
    c->state = DHCP_PROBING;
    c->deadline = now + (uint64_t)DHCP_PROBE_WAIT_MS * NS_PER_MS;
    c->io->probe(c->io->ctx, r->yiaddr);
}

/* ---- renewing and rebinding ----------------------------------------------- */

/* A REQUEST for our lease: unicast to its server while renewing,
 * broadcast while rebinding. The next try comes after half the time
 * left until `until` (T2 or the lease's end), at least
 * DHCP_RENEW_MIN_MS, and never after `until`. */
static void send_renewal(struct dhcp_client *c, uint64_t now)
{
    bool renewing = c->state == DHCP_RENEWING;
    struct dhcp_out m = { .type = DHCP_REQUEST, .ciaddr = c->lease.addr };
    send_msg(c, now, renewing ? c->lease.server : DHCP_BROADCAST, &m);
    c->sent_at = c->started;   /* the ACK may answer the exchange's first send */
    uint64_t until = renewing ? c->t2_at : c->end_at;
    uint64_t wait = until > now ? (until - now) / 2 : 0;
    if (wait < (uint64_t)DHCP_RENEW_MIN_MS * NS_PER_MS)
        wait = (uint64_t)DHCP_RENEW_MIN_MS * NS_PER_MS;
    uint64_t next = add_ns(now, wait);
    c->deadline = next < until ? next : until;
}

/* The lease's clock: the step due by now for a bound client. */
static void lease_due(struct dhcp_client *c, uint64_t now)
{
    if (now >= c->end_at) {
        lose(c, now, DHCP_WHY_EXPIRED);
    } else if (now >= c->t2_at) {
        if (c->state != DHCP_REBINDING)
            exchange(c, now, DHCP_REBINDING);
        send_renewal(c, now);
    } else if (now >= c->t1_at) {
        if (c->state != DHCP_RENEWING)
            exchange(c, now, DHCP_RENEWING);
        send_renewal(c, now);
    } else {
        c->deadline = c->t1_at;   /* early: nothing due yet */
    }
}

/* ---- what arrives ---------------------------------------------------------- */

static void on_nak(struct dhcp_client *c, uint64_t now)
{
    c->stats.naks++;
    if (c->state == DHCP_REQUESTING) {
        /* The offer was taken back: try again after a wait, not at once. */
        c->state = DHCP_INIT;
        c->deadline = now + (uint64_t)c->nak_wait_ms * NS_PER_MS;
        c->nak_wait_ms = c->nak_wait_ms >= DHCP_RETX_MAX_MS / 2 ? DHCP_RETX_MAX_MS
                                                                : c->nak_wait_ms * 2;
        return;
    }
    lose(c, now, DHCP_WHY_NAK);   /* rebooting, renewing or rebinding */
}

/* A reply to this exchange, checked by msg.c: does it count now? */
static void reply(struct dhcp_client *c, uint64_t now, const struct dhcp_reply *r)
{
    bool bound = c->state == DHCP_RENEWING || c->state == DHCP_REBINDING;
    uint32_t want = bound ? c->lease.addr : c->offer.yiaddr;
    /* the server we asked, where we named one */
    uint32_t server = c->state == DHCP_REQUESTING ? c->offer.server
                      : c->state == DHCP_RENEWING ? c->lease.server
                                                  : 0;
    if (c->state == DHCP_SELECTING && r->type == DHCP_OFFER) {
        c->stats.offers++;
        c->offer = *r;
        c->state = DHCP_REQUESTING;
        c->retx_ms = DHCP_RETX_FIRST_MS;
        send_request(c, now);
    } else if (c->state == DHCP_SELECTING || r->type == DHCP_OFFER ||
               (server && r->server != server)) {
        c->stats.ignored++;
    } else if (r->type == DHCP_NAK) {
        on_nak(c, now);
    } else if (r->yiaddr != want) {
        c->stats.ignored++;   /* an ACK for another address than we asked for */
    } else if (bound) {
        c->stats.acks++;
        bind(c, r);
    } else {
        acked(c, now, r);
    }
}

void dhcp_input(struct dhcp_client *c, uint64_t now, const void *msg, size_t len)
{
    c->stats.received++;
    if (c->state == DHCP_STOPPED || c->state == DHCP_INIT || c->state == DHCP_PROBING ||
        c->state == DHCP_BOUND) {
        c->stats.ignored++;   /* no exchange is waiting for a reply */
        return;
    }
    struct dhcp_reply r;
    status_t st = dhcp_parse(msg, len, c->mac, c->xid, &r);
    if (st == ERR_NOT_FOUND)
        c->stats.foreign++;
    else if (st == ERR_NOT_SUPPORTED)
        c->stats.ignored++;
    else if (st != OK)
        c->stats.malformed++;
    else
        reply(c, now, &r);
}

/* ---- the edge's calls -------------------------------------------------------- */

void dhcp_init(struct dhcp_client *c, const struct dhcp_io *io, const uint8_t mac[6])
{
    *c = (struct dhcp_client){ .io = io, .state = DHCP_STOPPED, .deadline = DEADLINE_NEVER,
                               .nak_wait_ms = DHCP_NAK_WAIT_MS };
    memcpy(c->mac, mac, 6);
}

void dhcp_start(struct dhcp_client *c, uint64_t now, uint32_t last_addr)
{
    if (c->have_lease) {
        c->have_lease = false;
        c->io->unbound(c->io->ctx, DHCP_WHY_STOPPED);
    }
    c->nak_wait_ms = DHCP_NAK_WAIT_MS;
    if (!dhcp_unicast(last_addr)) {
        discover(c, now);
        return;
    }
    exchange(c, now, DHCP_REBOOTING);
    c->offer = (struct dhcp_reply){ .yiaddr = last_addr };
    send_request(c, now);
}

void dhcp_stop(struct dhcp_client *c, uint64_t now, bool release)
{
    if (c->have_lease) {
        if (release) {
            exchange(c, now, c->state);
            struct dhcp_out m = { .type = DHCP_RELEASE, .ciaddr = c->lease.addr,
                                  .server = c->lease.server };
            send_msg(c, now, c->lease.server, &m);
        }
        c->have_lease = false;
        c->io->unbound(c->io->ctx, DHCP_WHY_STOPPED);
    }
    c->state = DHCP_STOPPED;
    c->deadline = DEADLINE_NEVER;
}

void dhcp_probe_done(struct dhcp_client *c, uint64_t now, uint32_t addr, bool conflict)
{
    if (c->state != DHCP_PROBING || addr != c->offer.yiaddr)
        return;
    if (!conflict) {
        bind(c, &c->offer);
        return;
    }
    /* Someone has it: tell the server (DECLINE, broadcast, RFC 2131
     * 4.4.1) and wait before asking again. */
    struct dhcp_out m = { .type = DHCP_DECLINE, .requested = addr, .server = c->offer.server };
    send_msg(c, now, DHCP_BROADCAST, &m);
    c->stats.declines++;
    c->state = DHCP_INIT;
    c->deadline = now + (uint64_t)DHCP_DECLINE_WAIT_MS * NS_PER_MS;
}

void dhcp_tick(struct dhcp_client *c, uint64_t now)
{
    if (now < c->deadline)
        return;
    switch (c->state) {
    case DHCP_STOPPED:
        c->deadline = DEADLINE_NEVER;
        break;
    case DHCP_INIT:
        discover(c, now);
        break;
    case DHCP_SELECTING:
        send_discover(c, now);
        break;
    case DHCP_REQUESTING:
    case DHCP_REBOOTING:
        if (c->tries >= DHCP_REQUEST_TRIES)
            discover(c, now);
        else
            send_request(c, now);
        break;
    case DHCP_PROBING:
        bind(c, &c->offer);   /* the edge never answered: taken as free */
        break;
    case DHCP_BOUND:
    case DHCP_RENEWING:
    case DHCP_REBINDING:
        lease_due(c, now);
        break;
    }
}

uint64_t dhcp_deadline(const struct dhcp_client *c)
{
    return c->deadline;
}

const char *dhcp_state_name(enum dhcp_state s)
{
    static const char *const names[] = {
        [DHCP_STOPPED] = "stopped",       [DHCP_INIT] = "init",
        [DHCP_SELECTING] = "selecting",   [DHCP_REQUESTING] = "requesting",
        [DHCP_REBOOTING] = "rebooting",   [DHCP_PROBING] = "probing",
        [DHCP_BOUND] = "bound",           [DHCP_RENEWING] = "renewing",
        [DHCP_REBINDING] = "rebinding",
    };
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}
