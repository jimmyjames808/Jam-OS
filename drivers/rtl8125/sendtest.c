/* rtl8125: the send test, `netsend` (drv/rtl8125).
 *
 * The proof on the PC that transmitting works, without netstack: with the
 * chip up in full mode on the configured VLAN, it sends PROBES ARP probes
 * (arp.h: "who has <target>?", sender 0.0.0.0) GAP_NS apart, each tagged
 * with the VLAN by tx.c like any frame, and watches for the target's ARP
 * reply, which comes back tagged too and is kept by rx.c. For every probe
 * it logs when it was queued, how long the chip took to hand its
 * descriptor back and how long the reply took, so one run shows a loss
 * pattern (every other frame, only the first, ...) at a glance; then the
 * waits' summary, and the caller compares the chip's count of frames sent
 * with the driver's (main.c). Nothing else is ever sent.
 *
 * The probes are all the same frame, so a reply is the latest probe's if
 * that one has none yet (they are GAP_NS apart and a reply on the LAN
 * takes milliseconds); any other reply is counted as extra.
 *
 * Before the first probe it waits for the link (10 s at most) and then
 * for the first frame kept on our VLAN (5 s at most; it sends anyway
 * after that): that frame shows the switch port forwards our VLAN, so a
 * probe isn't lost to a port that is still starting. Every wait is the
 * driver's loop (loop.c): interrupts and polls, nothing that blocks. */
#include "arp.h"
#include "rtl8125.h"

#define PROBES        20
#define GAP_NS        (200 * NS_PER_MS)     /* between probes */
#define LATE_NS       (2 * NS_PER_S)        /* after the last, for its reply */
#define LINK_NS       (10 * NS_PER_S)
#define HEARD_NS      (5 * NS_PER_S)
#define DRAIN_NS      (200 * NS_PER_MS)     /* for the last descriptors to come back */
#define POLL_NS       (50 * NS_PER_MS)
#define LINK_POLL_NS  (50 * NS_PER_MS)

struct probe {
    status_t err;                 /* tx_send's answer */
    uint32_t seq;                 /* its descriptor (tx.c's free-running index) */
    uint64_t sent_at;             /* when its doorbell was rung (uptime, ns) */
    bool     got;                 /* its reply came */
    uint64_t rtt_ns;              /* the doorbell to the reply's harvest */
};

struct sendtest {
    uint32_t target;              /* the address asked for (host order) */
    int      last;                /* the latest probe sent (index), -1: none yet */
    struct probe p[PROBES];
    unsigned tried;               /* probes tried (the rest: the run was cut, or no link) */
    unsigned sent;                /* probes tx_send took */
    unsigned replies;             /* replies from the target (late or repeated ones too) */
    uint64_t heard_at;            /* the first frame kept on our VLAN (uptime, ns) */
    unsigned answered;            /* probes with a reply */
    uint64_t rtt_min, rtt_max, rtt_sum;   /* over the answered probes */
    char     pattern[PROBES + 1]; /* '+' answered, '-' not, 'x' not sent */
};

static struct sendtest s;

static void on_frame(struct rtl *t, const uint8_t *f, size_t len, uint64_t at)
{
    if (!s.heard_at)
        s.heard_at = at;
    if (!arp_is_reply(f, len, t->mac, s.target))
        return;
    s.replies++;
    if (s.last < 0 || s.p[s.last].got)
        return;
    struct probe *p = &s.p[s.last];
    p->got = true;
    p->rtt_ns = at - p->sent_at;
    s.rtt_min = !s.answered || p->rtt_ns < s.rtt_min ? p->rtt_ns : s.rtt_min;
    s.rtt_max = p->rtt_ns > s.rtt_max ? p->rtt_ns : s.rtt_max;
    s.rtt_sum += p->rtt_ns;
    s.answered++;
}

static bool linked(const struct rtl *t)
{
    return t->link;
}

static bool heard(const struct rtl *t)
{
    (void)t;
    return s.heard_at != 0;
}

static bool all_answered(const struct rtl *t)
{
    (void)t;
    return s.last == PROBES - 1 && s.answered == s.sent;
}

static bool drained(const struct rtl *t)
{
    return tx_pending(t) == 0;
}

static void send_probe(struct rtl *t, unsigned k)
{
    uint8_t f[ARP_FRAME_LEN];
    arp_probe(f, t->mac, s.target);
    struct probe *p = &s.p[k];
    s.tried = k + 1;
    p->seq = t->tx_prod;          /* the descriptor tx_send fills */
    p->err = tx_send(t, f, sizeof(f));
    p->sent_at = drv_clock_ns();
    if (p->err != OK) {
        drv_log("probe %u: not sent (%s)", k + 1, status_str(p->err));
        return;
    }
    s.sent++;
    s.last = (int)k;
}

/* Wait for the link, then for the first frame on our VLAN. False if cut. */
static bool wait_ready(struct rtl *t)
{
    uint64_t t0 = drv_clock_ns();
    if (!loop_until(t, t0 + LINK_NS, LINK_POLL_NS, linked))
        return false;
    if (!t->link) {
        drv_log("no link in 10 s: nothing sent");
        return true;
    }
    t0 = drv_clock_ns();
    if (!loop_until(t, t0 + HEARD_NS, POLL_NS, heard))
        return false;
    if (s.heard_at)
        drv_log("first frame on vlan %u %lu ms after the link", t->vlan,
                (unsigned long)((s.heard_at - t->link_at) / NS_PER_MS));
    else
        drv_log("nothing heard on vlan %u in 5 s: sending anyway", t->vlan);
    return true;
}

/* "1.234" (ms, to the microsecond) of ns into buf. */
static const char *ms(uint64_t ns, char *buf, size_t size)
{
    drv_snprintf(buf, size, "%lu.%03lu", (unsigned long)(ns / NS_PER_MS),
                 (unsigned long)(ns % NS_PER_MS / NS_PER_US));
    return buf;
}

static void log_probe(const struct rtl *t, unsigned k, uint64_t t0)
{
    const struct probe *p = &s.p[k];
    char at[16], back[40], reply[32], a[16], b[16];
    uint64_t wait;
    s.pattern[k] = p->err != OK ? 'x' : p->got ? '+' : '-';
    if (p->err != OK) {
        drv_log("probe %2u: not sent (%s)", k + 1, status_str(p->err));
        return;
    }
    if (tx_wait_of(t, p->seq, &wait))
        drv_snprintf(back, sizeof(back), "back after %s ms", ms(wait, a, sizeof(a)));
    else
        drv_snprintf(back, sizeof(back), "NOT BACK (the chip still has it)");
    if (p->got)
        drv_snprintf(reply, sizeof(reply), "reply after %s ms", ms(p->rtt_ns, b, sizeof(b)));
    else
        drv_snprintf(reply, sizeof(reply), "NO REPLY");
    drv_log("probe %2u: descriptor %u queued at +%s ms, %s, %s", k + 1, p->seq,
            ms(p->sent_at - t0, at, sizeof(at)), back, reply);
}

static void log_probes(const struct rtl *t)
{
    uint64_t t0 = s.p[0].sent_at;
    for (unsigned k = 0; k < s.tried; k++)
        log_probe(t, k, t0);
    s.pattern[PROBES] = 0;
    char lo[16], avg[16], hi[16], wait[48];
    tx_wait_str(t, wait, sizeof(wait));
    drv_log("send test: %u of %u probes answered by %u.%u.%u.%u (%s), %u reply frame(s) in all; "
            "reply min/avg/max %s/%s/%s ms; doorbell to descriptor back min/avg/max %s",
            s.answered, s.sent, s.target >> 24, (s.target >> 16) & 0xff, (s.target >> 8) & 0xff,
            s.target & 0xff, s.pattern, s.replies, ms(s.rtt_min, lo, sizeof(lo)),
            ms(s.answered ? s.rtt_sum / s.answered : 0, avg, sizeof(avg)),
            ms(s.rtt_max, hi, sizeof(hi)), wait);
    rx_log(t);
    tx_log(t);
}

void sendtest_run(struct rtl *t, const struct rtl_args *a, struct outcome *o)
{
    s = (struct sendtest){ .target = a->arp_target, .last = -1 };
    for (unsigned k = 0; k < PROBES; k++)
        s.pattern[k] = 'x';
    t->on_frame = on_frame;
    bool cut = !wait_ready(t);
    for (unsigned k = 0; !cut && t->link && k < PROBES; k++) {
        send_probe(t, k);
        uint64_t wait = k == PROBES - 1 ? LATE_NS : GAP_NS;
        cut = !loop_until(t, drv_clock_ns() + wait, POLL_NS,
                          k == PROBES - 1 ? all_answered : NULL);
    }
    if (!cut)
        cut = !loop_until(t, drv_clock_ns() + DRAIN_NS, 10 * NS_PER_MS, drained);
    o->cut = cut;
    t->on_frame = NULL;
    if (s.tried)
        log_probes(t);
    else
        tx_log(t);
}

void sendtest_report(const struct rtl *t, const struct outcome *o)
{
    char link[48], lo[16], hi[16], wait[48];
    chip_link_summary(t, link, sizeof(link));
    tx_wait_str(t, wait, sizeof(wait));
    uint32_t dropped = 0;
    for (unsigned k = NETFRAME_RX_RUNT; k < NETFRAME_RX_KINDS; k++)
        dropped += t->rx.drop[k];
    drv_report("netsend vlan %u, %s%s, %u/%u probes to %u.%u.%u.%u answered %s, reply %s-%s ms, "
               "%s, wait %s, %u stalled, rx kept %u dropped %u%s", t->vlan, link,
               o->cut ? ", CUT SHORT" : "", s.answered, PROBES, s.target >> 24,
               (s.target >> 16) & 0xff, (s.target >> 8) & 0xff, s.target & 0xff, s.pattern,
               ms(s.rtt_min, lo, sizeof(lo)), ms(s.rtt_max, hi, sizeof(hi)), o->txcheck, wait,
               t->tx.stalls, t->rx.kept, dropped, t->refused || t->tx.gate ? ", WRITES REFUSED"
               : "");
}
