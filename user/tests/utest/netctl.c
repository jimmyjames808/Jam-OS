/* utest: netstack's control channel (abi/idl/netctl.idl). First in-process:
 * ctl.c serves a channel of the test's own (requests written without
 * waiting, ctl_serve run, the replies read), over the same core
 * user/tests/utest/netstack.c drives; then bin/netstack as a process,
 * started the way init will start it (its control channel at SR_USER + 0),
 * called with the blocking client and killed.
 *
 * Covered: set_ipv4 and info agree; every kind of address set_ipv4 must
 * refuse is refused and changes nothing; set_dns likewise; clear forgets
 * the address and the DNS servers; stats answers; the process serves
 * with no device (link down, nothing sent) and leaves its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/netctl.h>
#include <os.h>
#include "ctl.h"
#include "utest.h"

#define A_ADDR  0x0a021505u   /* 10.2.21.5 */
#define A_GW    0x0a021501u   /* 10.2.21.1 */
#define A_MASK  0xffffff00u   /* /24 */

/* Our end and ctl.c's end of an in-process control channel. */
struct pair {
    handle_t cli, srv;
};

/* Serve what is queued on the server end and read the one reply. */
static status_t pump(const struct pair *p, void *rep, uint32_t cap, struct idl_msg *m)
{
    status_t st = ctl_serve(p->srv);
    if (st != ERR_SHOULD_WAIT)
        return st == OK ? ERR_INTERNAL : st;   /* one request can't spend the budget */
    return idl_reply_read(p->cli, rep, cap, m);
}

static status_t set_ipv4(const struct pair *p, uint32_t a, uint32_t mask, uint32_t gw)
{
    _Alignas(8) uint8_t rep[NETCTL_REP_MAX];
    struct idl_msg m;
    status_t st = netctl_set_ipv4_send(p->cli, 1, a, mask, gw);
    if (st == OK)
        st = pump(p, rep, sizeof(rep), &m);
    return st == OK ? netctl_set_ipv4_result(rep, &m) : st;
}

static status_t set_dns(const struct pair *p, uint32_t first, uint32_t second)
{
    _Alignas(8) uint8_t rep[NETCTL_REP_MAX];
    struct idl_msg m;
    status_t st = netctl_set_dns_send(p->cli, 2, first, second);
    if (st == OK)
        st = pump(p, rep, sizeof(rep), &m);
    return st == OK ? netctl_set_dns_result(rep, &m) : st;
}

static status_t clear(const struct pair *p)
{
    _Alignas(8) uint8_t rep[NETCTL_REP_MAX];
    struct idl_msg m;
    status_t st = netctl_clear_send(p->cli, 3);
    if (st == OK)
        st = pump(p, rep, sizeof(rep), &m);
    return st == OK ? netctl_clear_result(rep, &m) : st;
}

/* netctl.info's answer. */
struct info {
    uint32_t address, mask, gateway, dns1, dns2;
    uint8_t  mac[6], device, link;
};

static status_t info(const struct pair *p, struct info *i)
{
    _Alignas(8) uint8_t rep[NETCTL_REP_MAX];
    struct idl_msg m;
    status_t st = netctl_info_send(p->cli, 4);
    if (st == OK)
        st = pump(p, rep, sizeof(rep), &m);
    if (st != OK)
        return st;
    return netctl_info_result(rep, &m, &i->address, &i->mask, &i->gateway, &i->dns1, &i->dns2,
                              i->mac, &i->device, &i->link);
}

/* Each refused, and the address it had stays. */
static bool refusals(const struct pair *p)
{
    static const uint32_t bad[][3] = {
        { A_ADDR, 0xffff00ffu, A_GW },     /* a mask with a hole */
        { A_ADDR, 0, 0 },                  /* /0 */
        { A_ADDR, 0xfffffffeu, 0 },        /* /31 */
        { A_ADDR, 0xffffffffu, 0 },        /* /32 */
        { 0x0a021500u, A_MASK, A_GW },     /* the subnet's own address */
        { 0x0a0215ffu, A_MASK, A_GW },     /* its broadcast */
        { 0x7f000001u, 0xff000000u, 0 },   /* loopback */
        { 0x00000005u, 0xff000000u, 0 },   /* 0.0.0.0/8 */
        { 0xe0000005u, 0xff000000u, 0 },   /* multicast */
        { 0xf0000005u, 0xff000000u, 0 },   /* 240.0.0.0/4 */
        { A_ADDR, A_MASK, 0x0a021601u },   /* a gateway outside the subnet */
        { A_ADDR, A_MASK, A_ADDR },        /* the gateway is us */
        { A_ADDR, A_MASK, 0x0a0215ffu },   /* the gateway is the broadcast */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (set_ipv4(p, bad[i][0], bad[i][1], bad[i][2]) != ERR_INVALID_ARGS)
            FAIL("set_ipv4 %08x/%08x gw %08x was taken", bad[i][0], bad[i][1], bad[i][2]);
    CHECK_ST(set_dns(p, 0x7f000001u, 0), ERR_INVALID_ARGS);
    CHECK_ST(set_dns(p, 0x08080808u, 0xffffffffu), ERR_INVALID_ARGS);
    struct info i;
    CHECK_ST(info(p, &i), OK);
    CHECK_EQ(i.address, A_ADDR);
    CHECK_EQ(i.mask, A_MASK);
    CHECK_EQ(i.gateway, A_GW);
    CHECK_EQ(i.dns1, 0x0a021501u);
    return true;
}

bool t_netctl_set_and_clear(void)
{
    struct pair p;
    stack_stop();
    CHECK_ST(stack_start(&stack_no_device), OK);
    CHECK_ST(jam_channel_create(&p.cli, &p.srv), OK);
    CHECK_ST(set_ipv4(&p, A_ADDR, A_MASK, A_GW), OK);
    CHECK_ST(set_dns(&p, 0x0a021501u, 0x01010101u), OK);
    struct info i;
    CHECK_ST(info(&p, &i), OK);
    CHECK_EQ(i.address, A_ADDR);
    CHECK_EQ(i.mask, A_MASK);
    CHECK_EQ(i.gateway, A_GW);
    CHECK_EQ(i.dns1, 0x0a021501u);
    CHECK_EQ(i.dns2, 0x01010101u);
    CHECK_EQ(i.device, 0);
    CHECK_EQ(i.link, 0);
    CHECK(refusals(&p));
    CHECK_ST(set_ipv4(&p, 0xc0a80002u, 0xfffffffcu, 0xc0a80001u), OK);   /* a /30 */
    CHECK_ST(set_ipv4(&p, A_ADDR, 0xff000000u, 0), OK);   /* /8, no gateway */
    CHECK_ST(clear(&p), OK);
    CHECK_ST(info(&p, &i), OK);
    CHECK(!i.address && !i.mask && !i.gateway && !i.dns1 && !i.dns2);
    CHECK_ST(clear(&p), OK);   /* twice is fine */
    /* A request of the wrong size is refused, not taken. */
    uint32_t junk[2] = { 0, NETCTL_SET_IPV4 };
    CHECK_ST(jam_channel_write(p.cli, junk, sizeof(junk), NULL, 0), OK);
    _Alignas(8) uint8_t rep[NETCTL_REP_MAX];
    struct idl_msg m;
    CHECK_ST(pump(&p, rep, sizeof(rep), &m), OK);
    CHECK_EQ(((const struct idl_rep_hdr *)rep)->status, ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(p.cli), OK);
    CHECK_ST(ctl_serve(p.srv), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(p.srv), OK);
    stack_stop();
    return true;
}

/* Its job empty, waiting (bounded) for a killed process's pages. */
static bool job_drained(handle_t job)
{
    struct job_info ji;
    uint64_t end = now() + 5 * NS_PER_S;
    for (;;) {
        CHECK_ST(info_of(job, &ji), OK);
        unsigned k = 1;
        while (k < JOB_LIMIT_COUNT && !ji.used[k])
            k++;
        if (k == JOB_LIMIT_COUNT)
            return true;
        if (now() > end)
            FAIL("netstack's job still has %lu units of kind %u", (unsigned long)ji.used[k], k);
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
}

bool t_netctl_process(void)
{
    const struct bootfs_view *b;
    const void *data;
    uint64_t size;
    if (bootfs_default(&b) != OK || bootfs_lookup(b, "bin/netstack", &data, &size) != OK) {
        printf("utest: %s: no bin/netstack: skipped\n", utest_cur);
        return true;
    }
    handle_t job, proc, cli, srv;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&cli, &srv), OK);
    const char *argv[] = { "bin/netstack" };
    struct spawn_handle x = { SR_USER + 0, srv };
    struct spawn_args a = {
        .path = "bin/netstack", .argc = 1, .argv = argv, .job = job, .extra = &x, .nextra = 1,
    };
    CHECK_ST(spawn(&a, &proc), OK);   /* consumes srv */
    CHECK_ST(netctl_set_ipv4(cli, A_ADDR, A_MASK, A_GW), OK);
    CHECK_ST(netctl_set_ipv4(cli, A_ADDR, 0xfffffffeu, 0), ERR_INVALID_ARGS);
    uint32_t addr, mask, gw, d1, d2;
    uint8_t mac[6], device, link;
    CHECK_ST(netctl_info(cli, &addr, &mask, &gw, &d1, &d2, mac, &device, &link), OK);
    CHECK_EQ(addr, A_ADDR);
    CHECK_EQ(mask, A_MASK);
    CHECK_EQ(gw, A_GW);
    CHECK_EQ(device, 0);
    CHECK_EQ(link, 0);
    uint64_t rx, refused, tx, txd, echo, errs, limited;
    uint32_t l, ar, ip, ic, ud, bad, bufs, heap;
    CHECK_ST(netctl_stats(cli, &rx, &refused, &tx, &txd, &echo, &errs, &limited, &l, &ar, &ip,
                          &ic, &ud, &bad, &bufs, &heap), OK);
    CHECK_EQ(rx, 0);
    CHECK_EQ(tx, 0);
    CHECK_EQ(bufs, 0);
    CHECK_ST(netctl_clear(cli), OK);
    CHECK_ST(netctl_info(cli, &addr, &mask, &gw, &d1, &d2, mac, &device, &link), OK);
    CHECK_EQ(addr, 0);
    CHECK_ST(jam_process_kill(proc), OK);
    struct process_info pi;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &pi), OK);
    CHECK(pi.killed);
    CHECK_ST(netctl_info(cli, &addr, &mask, &gw, &d1, &d2, mac, &device, &link), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(cli), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK(job_drained(job));
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}
