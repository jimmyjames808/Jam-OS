/* rtl8125: the send test, `netsend` (drv/rtl8125).
 *
 * The first proof on the PC that transmitting works, before netstack
 * exists: with the chip up in full mode on the configured VLAN, it sends
 * PROBES ARP probes (arp.h: "who has <target>?", sender 0.0.0.0) one
 * second apart, each tagged with the VLAN by tx.c like any frame, and
 * waits for the target's ARP reply, which comes back tagged too and is
 * kept by rx.c. It logs each probe's round trip (the doorbell to the
 * reply's harvest), then stops; the caller compares the chip's count of
 * frames sent with the driver's (main.c). Nothing else is ever sent.
 *
 * Before the first probe it waits for the link (10 s at most) and then
 * for the first frame kept on our VLAN (5 s at most; it sends anyway
 * after that): that frame shows the switch port forwards our VLAN, so a
 * probe isn't lost to a port that is still starting. Every wait is the
 * driver's loop (loop.c): interrupts and polls, nothing that blocks. */
#include "arp.h"
#include "rtl8125.h"

#define PROBES        3
#define GAP_NS        NS_PER_S              /* between probes (RFC 5227's PROBE_MIN) */
#define LATE_NS       (2 * NS_PER_S)        /* after the last, for its reply */
#define LINK_NS       (10 * NS_PER_S)
#define HEARD_NS      (5 * NS_PER_S)
#define DRAIN_NS      (100 * NS_PER_MS)     /* for the last descriptors to come back */
#define POLL_NS       (100 * NS_PER_MS)
#define LINK_POLL_NS  (50 * NS_PER_MS)

struct sendtest {
    uint32_t target;              /* the address asked for (host order) */
    int      last;                /* the latest probe sent (index), -1: none yet */
    bool     sent[PROBES];        /* tx_send took it */
    status_t err[PROBES];         /* tx_send's answer */
    uint64_t sent_at[PROBES];     /* when its doorbell was rung (uptime, ns) */
    bool     got[PROBES];         /* its reply came */
    uint64_t rtt_ns[PROBES];      /* the round trip */
    unsigned replies;             /* replies from the target (late or repeated ones too) */
    uint64_t heard_at;            /* the first frame kept on our VLAN (uptime, ns) */
    unsigned answered;            /* probes with a reply */
    bool     cut;                 /* devmgr stopped the test */
};

static struct sendtest s;

static void on_frame(struct rtl *t, const uint8_t *f, size_t len, uint64_t at)
{
    if (!s.heard_at)
        s.heard_at = at;
    if (!arp_is_reply(f, len, t->mac, s.target))
        return;
    s.replies++;
    if (s.last < 0 || s.got[s.last])
        return;
    s.got[s.last] = true;
    s.rtt_ns[s.last] = at - s.sent_at[s.last];
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
    return s.last == PROBES - 1 && s.answered == PROBES;
}

static bool drained(const struct rtl *t)
{
    return tx_pending(t) == 0;
}

static void send_probe(struct rtl *t, unsigned k)
{
    uint8_t f[ARP_FRAME_LEN];
    arp_probe(f, t->mac, s.target);
    s.err[k] = tx_send(t, f, sizeof(f));
    s.sent_at[k] = drv_clock_ns();
    s.sent[k] = s.err[k] == OK;
    if (s.sent[k])
        s.last = (int)k;
    else
        drv_log("probe %u: not sent (%s)", k + 1, status_str(s.err[k]));
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

static void log_probes(const struct rtl *t)
{
    for (unsigned k = 0; k < PROBES; k++) {
        if (!s.sent[k])
            drv_log("probe %u: not sent (%s)", k + 1, status_str(s.err[k]));
        else if (s.got[k])
            drv_log("probe %u: reply in %lu.%03lu ms", k + 1,
                    (unsigned long)(s.rtt_ns[k] / NS_PER_MS),
                    (unsigned long)(s.rtt_ns[k] % NS_PER_MS / NS_PER_US));
        else
            drv_log("probe %u: no reply", k + 1);
    }
    drv_log("send test: %u of %u probes answered by %u.%u.%u.%u, %u reply frame(s) in all",
            s.answered, PROBES, s.target >> 24, (s.target >> 16) & 0xff, (s.target >> 8) & 0xff,
            s.target & 0xff, s.replies);
    rx_log(t);
    tx_log(t);
}

void sendtest_run(struct rtl *t, const struct rtl_args *a, struct outcome *o)
{
    s = (struct sendtest){ .target = a->arp_target, .last = -1 };
    t->on_frame = on_frame;
    s.cut = !wait_ready(t);
    for (unsigned k = 0; !s.cut && t->link && k < PROBES; k++) {
        send_probe(t, k);
        uint64_t wait = k == PROBES - 1 ? LATE_NS : GAP_NS;
        s.cut = !loop_until(t, drv_clock_ns() + wait, POLL_NS,
                            k == PROBES - 1 ? all_answered : NULL);
    }
    if (!s.cut)
        s.cut = !loop_until(t, drv_clock_ns() + DRAIN_NS, 10 * NS_PER_MS, drained);
    o->cut = s.cut;
    t->on_frame = NULL;
    log_probes(t);
}

void sendtest_report(const struct rtl *t, const struct outcome *o)
{
    char link[48], rtt[48] = "";
    chip_link_summary(t, link, sizeof(link));
    size_t len = 0;
    for (unsigned k = 0; k < PROBES && len < sizeof(rtt); k++) {
        if (!s.got[k])
            len += (size_t)drv_snprintf(rtt + len, sizeof(rtt) - len, "%s-", k ? "/" : "");
        else
            len += (size_t)drv_snprintf(rtt + len, sizeof(rtt) - len, "%s%lu.%02lu", k ? "/" : "",
                                        (unsigned long)(s.rtt_ns[k] / NS_PER_MS),
                                        (unsigned long)(s.rtt_ns[k] % NS_PER_MS / 10000));
    }
    uint32_t dropped = 0;
    for (unsigned k = NETFRAME_RX_RUNT; k < NETFRAME_RX_KINDS; k++)
        dropped += t->rx.drop[k];
    drv_report("netsend vlan %u, %s%s, %u of %u ARP probes to %u.%u.%u.%u answered (ms %s), %s, "
               "rx kept %u dropped %u%s", t->vlan, link, o->cut ? ", CUT SHORT"
               : "", s.answered, PROBES, s.target >> 24, (s.target >> 16) & 0xff,
               (s.target >> 8) & 0xff, s.target & 0xff, rtt, o->txcheck,
               t->rx.kept, dropped, t->refused || t->tx.gate ? ", WRITES REFUSED" : "");
}
