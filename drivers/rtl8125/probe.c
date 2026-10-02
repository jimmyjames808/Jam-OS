/* rtl8125: the listen-only probe, `netprobe` (drv/rtl8125;
 * docs/M9-PLAN.md, stage 0).
 *
 * It SENDS NOTHING: it runs in RTL_MODE_PROBE, so tx.c's gate refuses
 * everything (no transmit ring, no transmitter enable, no doorbell), and
 * no file of the probe calls tx.c at all (tools/checknotx.sh). After the
 * shared bring-up (main.c) with every frame accepted and tags kept, it
 * waits for the link (10 s at most), listens for 60 s and counts frames
 * by tag, VLAN and EtherType (census.c), logging link changes; main.c
 * then compares the chip's count of frames sent (0) with the driver's (0)
 * and stops the chip. One RESULTS line with the verdict ("trunk carrying
 * our VLAN", ...). */
#include "rtl8125.h"

#define LINK_WAIT_NS  (10 * NS_PER_S)
#define LISTEN_NS     (60 * NS_PER_S)
#define LINK_POLL_NS  (50 * NS_PER_MS)

static bool linked(const struct rtl *t)
{
    return t->link;
}

void probe_run(struct rtl *t, struct outcome *o)
{
    o->cut = !loop_until(t, drv_clock_ns() + LINK_WAIT_NS, LINK_POLL_NS, linked);
    if (!o->cut && t->link) {
        drv_log("listening for 60 s: every frame accepted, tags kept, nothing sent");
        t->ev.polls = 0;   /* from here on, a poll is a second without an interrupt */
        o->cut = !loop_until(t, drv_clock_ns() + LISTEN_NS, NS_PER_S, NULL);
        if (o->cut)
            drv_log("%s: the listening ends early", t->tripped ? "the guard stopped the chip"
                    : "devmgr is stopping");
    }
    (void)census_harvest(t, false);
    census_log(&t->c);
    drv_log("interrupts: %u packet(s), %u poll(s) without one, isr bits seen %#x; link changes "
            "%u, interrupts with the link bit %u (the first link-up's included); ocp timeouts %u",
            t->ev.irqs, t->ev.polls, t->ev.isr_seen, t->ev.link_changes, t->ev.linkchg_irqs,
            t->ocp_timeouts);
}

void probe_report(const struct rtl *t, const struct outcome *o)
{
    char link[48];
    chip_link_summary(t, link, sizeof(link));
    const struct census *c = &t->c;
    uint32_t v = census_vlan(c, t->vlan), u = census_kind(c, NETFRAME_UNTAGGED);
    uint32_t other = c->frames - v - u;
    drv_report("8125B xid %03x, phy %08x patch %04x, %s, %s%u frames: vlan %u: %u, untagged %u, "
               "other %u, irqs %u, %s%s%s -> %s", t->xid, o->phy, o->rcode, link,
               t->link_at ? (o->cut ? "cut short: " : "60 s: ") : "", c->frames, t->vlan, v,
               u, other, t->ev.irqs, o->txcheck, t->refused ? ", WRITES REFUSED" : "",
               guard_note(t), t->link_at ? census_verdict(c, t->vlan) : "nothing heard (no link)");
}
