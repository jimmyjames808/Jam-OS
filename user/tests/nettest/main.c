/* nettest: finding the card, the session helpers and the run (nettest.h
 * says what each mode checks). */
#define CHECK_PROG "nettest"
#define CHECK_CUR  cur
#include <check.h>
#include <devmgr.h>
#include <idl/netdev.h>
#include <wants.h>
#include "nettest.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc devmgr-ctl\n");

#define LINK_WAIT (10 * NS_PER_S)

handle_t dm;                      /* devmgr's control channel */
handle_t nic;
const char *cur;
uint8_t mac[6];
static unsigned passed, failed;

static status_t map(handle_t vmo, void **out)
{
    uint64_t va = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, NETDEV_RING_BYTES,
                               VMAR_READ | VMAR_WRITE, &va);
    if (st == OK)
        *out = (void *)(uintptr_t)va;
    return st;
}

status_t sess_open(struct sess *s)
{
    *s = (struct sess){ 0 };
    status_t st = netdev_open_until(nic, soon(), &s->ch, &s->tx, &s->rx, &s->to_driver,
                                    &s->to_stack);
    if (st != OK)
        return st;
    if ((st = map(s->tx, &s->txm)) == OK)
        st = map(s->rx, &s->rxm);
    if (st == OK && (!netdev_end_attach(&s->txe, s->txm, NETDEV_RING_TX, true) ||
                     !netdev_end_attach(&s->rxe, s->rxm, NETDEV_RING_RX, false)))
        st = ERR_INTERNAL;   /* not the layout of <jam/netdev.h> */
    if (st != OK)
        sess_close(s);
    return st;
}

void sess_close(struct sess *s)
{
    handle_t self = startup_handle(SR_SELF_VMAR);
    if (s->txm)
        (void)jam_vmar_unmap(self, (uint64_t)(uintptr_t)s->txm, NETDEV_RING_BYTES);
    if (s->rxm)
        (void)jam_vmar_unmap(self, (uint64_t)(uintptr_t)s->rxm, NETDEV_RING_BYTES);
    handle_t hs[5] = { s->ch, s->tx, s->rx, s->to_driver, s->to_stack };
    for (int i = 0; i < 5; i++)
        if (hs[i])
            jam_handle_close(hs[i]);
    *s = (struct sess){ 0 };
}

bool get_stats(handle_t ch, struct netdev_stats *out)
{
    uint8_t b[NETDEV_STATS_SIZE];
    CHECK_ST(netdev_stats_until(ch, soon(), b), OK);
    memcpy(out, b, sizeof(*out));
    return true;
}

bool wait_link(void)
{
    uint64_t end = now() + LINK_WAIT;
    for (;;) {
        uint8_t chip[16];
        uint16_t vlan, mtu;
        uint32_t link = 0, speed, changes;
        CHECK_ST(netdev_info_until(nic, soon(), mac, &vlan, &mtu, &link, &speed, &changes, chip),
                 OK);
        if (link & NETDEV_LINK_UP)
            return true;
        if (now() > end)
            FAIL("no link in %lu s", (unsigned long)(LINK_WAIT / NS_PER_S));
        (void)jam_nanosleep(now() + 50 * NS_PER_MS);
    }
}

void frame_make(uint8_t *buf, uint32_t len, const char *tag, uint32_t seq)
{
    memset(buf, 0, len);
    memset(buf, 0xff, 6);
    memcpy(buf + 6, mac, 6);
    buf[12] = (uint8_t)(NT_ETHERTYPE >> 8);
    buf[13] = (uint8_t)NT_ETHERTYPE;
    uint8_t body[32];
    int n = snprintf((char *)body, sizeof(body), "%s %u", tag, seq);
    for (int i = 0; i <= n && 14u + (uint32_t)i < len; i++)
        buf[14 + i] = body[i];   /* the text and its NUL, as far as the frame goes */
}

void tx_kick(struct sess *s)
{
    (void)netdev_publish(&s->txe);
    (void)jam_event_signal(s->to_driver, 0, NETDEV_SIG_TX);
}

/* The card's service, from devmgr's control channel. */
static status_t find_nic(void)
{
    struct devmgr_rep r;
    uint32_t nh = 0;
    status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, E1K_VENDOR, E1K_DEVICE, 0, &r, &nic, 1,
                              &nh, soon());
    if (st == OK && nh != 1)
        st = ERR_INTERNAL;
    return st;
}

static void run(const char *name, bool (*fn)(void))
{
    cur = name;
    uint64_t t0 = now();
    if (fn()) {
        passed++;
        printf("nettest: %s ok (%lu ms)\n", name, (unsigned long)((now() - t0) / NS_PER_MS));
    } else {
        failed++;
    }
}

bool t_off(void)
{
    struct devmgr_rep r;
    uint32_t nh = 0;
    handle_t h = 0;
    status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, E1K_VENDOR, E1K_DEVICE, 0, &r, &h, 1, &nh,
                              soon());
    if (nh)
        jam_handle_close(h);
    CHECK_ST(st, ERR_BAD_STATE);   /* bound, but the driver finished: no VLAN */
    CHECK_ST(devmgr_call(dm, DEVMGR_SUPERVISION, E1K_VENDOR, E1K_DEVICE, 0, &r, NULL, 0, NULL,
                         soon()), OK);
    CHECK_EQ(r.a, DEVMGR_SUP_FINISHED);
    CHECK_EQ(r.b, 0);   /* never restarted */
    return true;
}

static int usage(void)
{
    printf("usage: nettest vlan | rx | off\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return usage();
    const char *mode = argv[1];
    bool off = !strcmp(mode, "off"), vlan = !strcmp(mode, "vlan"), rx = !strcmp(mode, "rx");
    if (!off && !vlan && !rx)
        return usage();
    char line[120];
    dm = svc_get(SVC_DEVMGR_CTL);
    status_t st = dm ? (off ? OK : find_nic()) : ERR_NOT_FOUND;
    if (st != OK) {
        int n = snprintf(line, sizeof(line), "nettest %s: no e1000e service (%s): FAILED", mode,
                         status_str(st));
        jam_debug_report(line, (uint64_t)n);
        return 1;
    }
    if (off) {
        run("off", t_off);
    } else if (vlan) {
        run("session", t_session);
        run("hostile", t_hostile);
    } else {
        run("rx_census", t_rx_census);
        run("rx_flood", t_rx_flood);
    }
    if (nic)
        jam_handle_close(nic);
    int n = failed ? snprintf(line, sizeof(line), "nettest %s: %u passed, %u FAILED", mode, passed,
                              failed)
                   : snprintf(line, sizeof(line), "nettest %s: %u passed", mode, passed);
    jam_debug_report(line, (uint64_t)n);
    return failed ? 1 : 0;
}
