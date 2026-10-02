/* utest: what a network driver may know of a frame (<jam/netframe.h>),
 * over hand-made frames, and the RTL8125 probe's listen-only rule
 * (drivers/rtl8125/notx.h's guard, and the driver left alone without
 * the boot word `netprobe` or without its hardware). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/driver.h>
#include <jam/netframe.h>
#include <os.h>
#include "notx.h"
#include "utest.h"

/* A frame of len bytes whose bytes from 12 on are `tail` (n bytes); the
 * addresses are 0xee, so a classifier reading them would show up. */
static struct netframe_class frame(const uint8_t *tail, size_t n, size_t len)
{
    uint8_t f[64];
    memset(f, 0xee, sizeof(f));
    memcpy(f + 12, tail, n);
    return netframe_classify(f, len);
}

bool t_netframe_classify(void)
{
    static const uint8_t ipv4[] = { 0x08, 0x00 }, arp[] = { 0x08, 0x06 };
    struct netframe_class c = frame(ipv4, 2, 60);
    CHECK(c.kind == NETFRAME_UNTAGGED && c.ethertype == 0x0800 && !c.tpid && !c.vid);
    c = frame(arp, 2, 14);   /* the shortest untagged frame read */
    CHECK(c.kind == NETFRAME_UNTAGGED && c.ethertype == 0x0806);
    /* tagged 21, priority 5, ARP inside */
    static const uint8_t v21[] = { 0x81, 0x00, 0xa0, 0x15, 0x08, 0x06 };
    c = frame(v21, 6, 64);
    CHECK(c.kind == NETFRAME_VLAN && c.vid == 21 && c.pcp == 5 && c.ethertype == 0x0806);
    CHECK_EQ(c.tpid, NETFRAME_TPID_8021Q);
    /* another VLAN, and the largest id; DEI set doesn't change the id */
    static const uint8_t v100[] = { 0x81, 0x00, 0x00, 0x64, 0x86, 0xdd };
    c = frame(v100, 6, 18);   /* the shortest tagged frame read */
    CHECK(c.kind == NETFRAME_VLAN && c.vid == 100 && c.pcp == 0 && c.ethertype == 0x86dd);
    static const uint8_t vmax[] = { 0x81, 0x00, 0x1f, 0xff, 0x08, 0x00 };
    c = frame(vmax, 6, 64);
    CHECK(c.kind == NETFRAME_VLAN && c.vid == 4095 && c.pcp == 0);
    /* priority-tagged: VLAN 0, a priority only */
    static const uint8_t prio[] = { 0x81, 0x00, 0xe0, 0x00, 0x08, 0x00 };
    c = frame(prio, 6, 64);
    CHECK(c.kind == NETFRAME_PRIORITY && c.vid == 0 && c.pcp == 7 && c.ethertype == 0x0800);
    /* QinQ: an 802.1ad outer tag on 21 over an 802.1Q one, and the old 0x9100 */
    static const uint8_t qinq[] = { 0x88, 0xa8, 0x00, 0x15, 0x81, 0x00 };
    c = frame(qinq, 6, 64);
    CHECK(c.kind == NETFRAME_OUTER && c.tpid == 0x88a8 && c.vid == 21 && c.ethertype == 0x8100);
    static const uint8_t q9100[] = { 0x91, 0x00, 0x00, 0x07, 0x08, 0x00 };
    c = frame(q9100, 6, 64);
    CHECK(c.kind == NETFRAME_OUTER && c.tpid == 0x9100 && c.vid == 7);
    return true;
}

/* Too short to read what its kind needs: a runt, with nothing read past
 * the end. */
bool t_netframe_short_frames(void)
{
    static const uint8_t ipv4[] = { 0x08, 0x00 }, v21[] = { 0x81, 0x00, 0x00, 0x15, 0x08, 0x00 };
    for (size_t len = 0; len < NETFRAME_HDR; len++)
        CHECK(frame(ipv4, 2, len).kind == NETFRAME_RUNT);
    for (size_t len = NETFRAME_HDR; len < NETFRAME_TAGGED; len++) {
        struct netframe_class c = frame(v21, 6, len);
        CHECK(c.kind == NETFRAME_RUNT && !c.ethertype && !c.vid);
    }
    CHECK(frame(v21, 6, NETFRAME_TAGGED).kind == NETFRAME_VLAN);
    /* A frame of exactly 14 bytes in a buffer that ends there. */
    uint8_t exact[14];
    memset(exact, 0, sizeof(exact));
    exact[12] = 0x08;
    exact[13] = 0x06;
    CHECK(netframe_classify(exact, sizeof(exact)).ethertype == 0x0806);
    CHECK(netframe_classify(NULL, 0).kind == NETFRAME_RUNT);
    return true;
}

bool t_rtl8125_write_guard(void)
{
    /* the transmit rings' addresses, in any width or overlap */
    CHECK(!rtl_write_allowed(0x20, 4, 0) && !rtl_write_allowed(0x24, 4, 0));
    CHECK(!rtl_write_allowed(0x28, 4, 0) && !rtl_write_allowed(0x2f, 1, 0));
    CHECK(!rtl_write_allowed(0x1e, 4, 0));   /* 0x1e-0x21 reaches 0x20 */
    CHECK(rtl_write_allowed(0x10, 4, 0) && rtl_write_allowed(0x14, 4, 0));   /* DTCCR */
    CHECK(rtl_write_allowed(0x1c, 4, 0) && rtl_write_allowed(0x30, 4, 0));
    /* the command register: the receiver yes, the transmitter never */
    CHECK(rtl_write_allowed(RTL_CMD, 1, 0x08) && rtl_write_allowed(RTL_CMD, 1, 0x10));
    CHECK(rtl_write_allowed(RTL_CMD, 1, 0x88) && rtl_write_allowed(RTL_CMD, 1, 0));
    CHECK(!rtl_write_allowed(RTL_CMD, 1, 0x04) && !rtl_write_allowed(RTL_CMD, 1, 0x0c));
    CHECK(!rtl_write_allowed(0x34, 4, 0x04000000u));   /* TE in the top byte of a 32-bit write */
    CHECK(rtl_write_allowed(0x34, 4, 0x08000004u));    /* bit 2 of INT_CFG0 is not TE */
    CHECK(!rtl_write_allowed(0x36, 2, 0x0400));
    /* the transmit configuration and the doorbell */
    CHECK(!rtl_write_allowed(RTL_TXCFG, 4, 0) && !rtl_write_allowed(0x43, 1, 0));
    CHECK(rtl_write_allowed(0x44, 4, 0x41000c0fu));   /* RXCFG */
    CHECK(!rtl_write_allowed(RTL_TXSTART, 2, 1) && !rtl_write_allowed(0x93, 1, 0));
    CHECK(rtl_write_allowed(0x8c, 4, 0) && !rtl_write_allowed(0x8e, 4, 0));
    CHECK(rtl_write_allowed(0x94, 4, 0));
    /* only widths the chip has */
    CHECK(!rtl_write_allowed(0x50, 3, 0) && !rtl_write_allowed(0x50, 0, 0));
    CHECK(!rtl_write_allowed(0x50, 8, 0));
    return true;
}

/* drv/rtl8125 with `word` (NULL: none) and only a channel for DR_SERVE:
 * it must end at once, by itself, with exit 0, and leave its job empty. */
static bool runs_and_leaves(const char *word)
{
    handle_t job, a, b, proc;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    const char *argv[] = { "drv/rtl8125", word };
    struct spawn_handle x = { SR_DRIVER(DR_SERVE), b };
    struct spawn_args sa = {
        .path = "drv/rtl8125", .argc = word ? 2 : 1, .argv = argv, .job = job,
        .extra = &x, .nextra = 1,
    };
    CHECK_ST(spawn(&sa, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(a), OK);
    struct job_info ji;
    CHECK_ST(jam_job_get_info(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("drv/rtl8125 (%s) left %lu units of kind %u", word ? word : "no word",
                 (unsigned long)ji.used[k], k);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* Without `netprobe` the driver touches nothing; with it but without its
 * hardware (no RTL8125: QEMU) it says so and ends. */
bool t_rtl8125_stays_off(void)
{
    return runs_and_leaves(NULL) && runs_and_leaves("netprobe");
}
