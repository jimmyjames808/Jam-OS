/* utest: devmgr's supervision, with the crash-test driver (drv/crasher,
 * DEVMGR_TEST_DRIVER): a crash is restarted, restarts back off, too many
 * in a minute give up; and what a driver's handles can't do (with the
 * first PCI function that has MSI-X, DEVMGR_DRIVER_VIEW). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <devmgr.h>
#include <jam/driver.h>
#include <os.h>
#include "utest.h"

/* ---- supervision with the crash-test driver ----------------------------------------- */

#define TV DEVMGR_TEST_VENDOR
#define TD DEVMGR_TEST_DEVICE

/* PING: when this instance of the crash-test driver started. */
static status_t crasher_ping(handle_t ch, uint64_t deadline, uint64_t *started)
{
    uint32_t q[2] = { 0, CRASHER_PING };
    struct {
        uint32_t txid;       /* ours */
        int32_t  status;     /* OK */
        uint64_t started;    /* when this instance of the driver started */
    } __attribute__((packed)) rep;
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = ch, .wn = sizeof(q), .wbytes = (uint64_t)(uintptr_t)q, .rcap = sizeof(rep),
        .rbytes = (uint64_t)(uintptr_t)&rep, .ractual = (uint64_t)(uintptr_t)&n,
        .deadline_ns = deadline,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    if (n < 8 || (rep.status == OK && n < sizeof(rep)))
        return ERR_INTERNAL;
    *started = rep.started;
    return rep.status;
}

/* The crash-test driver's channel and process, or false (with a skip or a
 * failure line). */
static bool crasher(handle_t dm, handle_t *ch, handle_t *proc, bool *skip)
{
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    *skip = false;
    status_t st = dm_call(dm, DEVMGR_TEST_DRIVER, 0, 0, &r, NULL, NULL);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no drv/crasher: skipped\n", utest_cur);
        *skip = true;
        return false;
    }
    CHECK_ST(st, OK);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), OK);
    *ch = hs[0];
    uint64_t started = 0;
    /* up, whenever it started */
    CHECK_ST(crasher_ping(*ch, now() + 10 * NS_PER_S, &started), OK);
    CHECK_ST(dm_call(dm, DEVMGR_GET_DRIVER, TV, TD, &r, hs, &nh), OK);
    CHECK_EQ(nh, 2);   /* process and job: no hardware */
    *proc = hs[0];
    CHECK_ST(jam_handle_close(hs[1]), OK);
    /* No function behind it: DRIVER_VIEW must not open PCI function 0. */
    uint32_t vh = 0;
    CHECK_ST(dm_call(dm, DEVMGR_DRIVER_VIEW, TV, TD, &r, hs, &vh), ERR_NOT_FOUND);
    CHECK_EQ(vh, 0);
    return true;
}

/* Crash it through ch (it answers nothing), and see it die. *t: when the
 * crash was asked for. */
static bool crash_it(handle_t ch, handle_t proc, uint64_t *t)
{
    uint32_t q[2] = { 0x77, CRASHER_CRASH };
    *t = now();
    CHECK_ST(jam_channel_write(ch, q, sizeof(q), NULL, 0), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(info.killed);   /* a crash: the kernel killed it */
    uint64_t started;
    CHECK_ST(crasher_ping(ch, now() + 5 * NS_PER_S, &started), ERR_PEER_CLOSED);
    return true;
}

/* Crash once, reconnect: *delay_ms from the crash to the new instance's
 * start (the backoff, and a start). */
static bool crash_and_reconnect(handle_t dm, handle_t *ch, handle_t *proc, uint64_t *delay_ms)
{
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    uint64_t t = 0, started = 0;
    if (!crash_it(*ch, *proc, &t))
        return false;
    CHECK_ST(jam_handle_close(*ch), OK);
    CHECK_ST(jam_handle_close(*proc), OK);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), OK);   /* the new channel */
    *ch = hs[0];
    CHECK_ST(crasher_ping(*ch, now() + 15 * NS_PER_S, &started), OK);   /* waits for the restart */
    CHECK(started > t);
    *delay_ms = (started - t) / NS_PER_MS;
    CHECK_ST(dm_call(dm, DEVMGR_GET_DRIVER, TV, TD, &r, hs, &nh), OK);
    *proc = hs[0];
    CHECK_ST(jam_handle_close(hs[1]), OK);
    return true;
}

static uint32_t sup_restarts0;   /* the crash-test driver's restarts before these tests */

/* A driver that crashes comes back by itself; its clients reconnect. */
bool t_supervised_restart(void)
{
    handle_t dm = devmgr(), ch, proc;
    struct devmgr_rep sup;
    bool skip;
    if (!dm)
        return true;
    if (!crasher(dm, &ch, &proc, &skip))
        return skip;
    if (!supervision(dm, TV, TD, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    sup_restarts0 = sup.b;
    uint64_t ms = 0;
    if (!crash_and_reconnect(dm, &ch, &proc, &ms))
        return false;
    printf("utest: %s: crashed, restarted and answering %lu ms later\n", utest_cur, (unsigned long)ms);
    CHECK(ms >= 100 && ms < 3000);   /* the first backoff is 100 ms */
    if (!supervision(dm, TV, TD, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    CHECK_EQ(sup.b, sup_restarts0 + 1);
    CHECK_EQ(sup.c, 100);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* Each restart within a minute doubles the backoff: 200, 400, 800, 1600 ms. */
bool t_supervised_backoff(void)
{
    handle_t dm = devmgr(), ch, proc;
    struct devmgr_rep sup;
    bool skip;
    if (!dm)
        return true;
    if (!crasher(dm, &ch, &proc, &skip))
        return skip;
    for (uint32_t k = 1; k <= 4; k++) {
        uint64_t ms = 0, want = 100ull << k;
        if (!crash_and_reconnect(dm, &ch, &proc, &ms))
            return false;
        if (!supervision(dm, TV, TD, &sup))
            return false;
        printf("utest: %s: restart %u after %lu ms (backoff %u ms)\n", utest_cur, k + 1,
               (unsigned long)ms, sup.c);
        CHECK_EQ(sup.c, want);
        CHECK(ms >= want && ms < want + 3000);
        CHECK_EQ(sup.b, sup_restarts0 + 1 + k);
    }
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* The 6th death within a minute: devmgr gives up (GET_SERVICE says
 * ERR_BAD_STATE); TEST_DRIVER starts it afresh; a driver that exits 0 by
 * itself is finished, not restarted. */
bool t_supervised_give_up(void)
{
    handle_t dm = devmgr(), ch, proc, hs[DEVMGR_MAX_HANDLES];
    struct devmgr_rep sup, r;
    uint32_t nh = 0;
    bool skip;
    if (!dm)
        return true;
    if (!crasher(dm, &ch, &proc, &skip))
        return skip;
    uint64_t t = 0;
    if (!crash_it(ch, proc, &t))
        return false;
    uint64_t until = now() + 5 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, TV, TD, &sup))
            return false;
        if (sup.a == DEVMGR_SUP_GAVE_UP || now() > until)
            break;
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    printf("utest: %s: after %u restarts: state %u (3 = gave up)\n", utest_cur,
           sup.b - sup_restarts0, sup.a);
    CHECK_EQ(sup.a, DEVMGR_SUP_GAVE_UP);
    CHECK_EQ(sup.b, sup_restarts0 + 5);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), ERR_BAD_STATE);
    jam_nanosleep(now() + 300 * NS_PER_MS);   /* no restart comes */
    if (!supervision(dm, TV, TD, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_GAVE_UP);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);

    /* Started again on request, with a fresh history; exit 0 = finished. */
    if (!crasher(dm, &ch, &proc, &skip))
        return false;
    uint32_t q[3] = { 0x78, CRASHER_EXIT, 0 };
    CHECK_ST(jam_channel_write(ch, q, sizeof(q), NULL, 0), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(!info.killed && info.exit_code == 0);
    until = now() + 5 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, TV, TD, &sup))
            return false;
        if (sup.a == DEVMGR_SUP_FINISHED || now() > until)
            break;
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    CHECK_EQ(sup.a, DEVMGR_SUP_FINISHED);
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, TV, TD, &r, hs, &nh), ERR_BAD_STATE);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    return true;
}

/* What a driver's handles can't do, with a function that has MSI-X (the
 * same handles devmgr gives its driver, minus the interrupt and the
 * dma_cap): map its MSI-X table or PBA page, turn on bus mastering, make a
 * dma_cap or an interrupt object, write its MSI-X capability, reach any
 * other function (no RES_PCI, no slicing), get DMA memory or pin without
 * a dma_cap. */
bool t_driver_handle_limits(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES], x, v;
    uint32_t nh = 0;
    struct devmgr_rep r;
    if (!dm)
        return true;
    /* The first function with MSI-X whose table and PBA BARs a driver
     * could get (on the PC some BARs stay unsized and are never handed
     * out, e.g. the VMD controller's). */
    status_t st = ERR_NOT_FOUND;
    uint32_t cap = 0;
    for (uint32_t inst = 0; inst < 32; inst++) {
        nh = 0;
        st = devmgr_call(dm, DEVMGR_DRIVER_VIEW, 0xffff, 0xffff, inst, &r, hs, DEVMGR_MAX_HANDLES,
                         &nh, now() + 30 * NS_PER_S);
        if (st == ERR_NOT_FOUND)
            break;
        uint32_t t = 0, p = 0;
        if (st == OK && nh >= 2 && (cap = pci_find_cap(hs[0], 0x11)) &&
            jam_pci_config_read(hs[0], cap + 4, 4, &t) == OK &&
            jam_pci_config_read(hs[0], cap + 8, 4, &p) == OK &&
            (r.a & (1u << (t & 7))) && (r.a & (1u << (p & 7))))
            break;
        printf("utest: %s: MSI-X function #%u not usable (%s), trying the next\n", utest_cur, inst,
               st == OK ? "table BAR not handed out" : status_str(st));
        for (uint32_t k = 0; k < nh; k++)
            jam_handle_close(hs[k]);
        st = ERR_NOT_FOUND;
    }
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no usable function with MSI-X: skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    handle_t dev = hs[0];
    uint32_t mask = r.a, tab = 0, pba = 0, ctl = 0, cmd = 0, id = 0;
    CHECK_ST(jam_pci_config_read(dev, 0, 4, &id), OK);
    printf("utest: %s: using %04x:%04x\n", utest_cur, id & 0xffff, id >> 16);
    CHECK_ST(jam_pci_config_read(dev, cap + 4, 4, &tab), OK);
    CHECK_ST(jam_pci_config_read(dev, cap + 8, 4, &pba), OK);
    CHECK_ST(jam_pci_config_read(dev, cap + 2, 2, &ctl), OK);
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    /* The MSI-X table and PBA pages, in whichever BAR each lives. */
    const uint32_t where[2] = { tab, pba };
    for (int i = 0; i < 2; i++) {
        uint32_t bir = where[i] & 7, page = where[i] & ~0xfffu;
        CHECK(bir < 6 && (mask & (1u << bir)));
        handle_t bar = hs[1 + __builtin_popcount(mask & ((1u << bir) - 1))];
        CHECK_ST(jam_vmo_create_physical(bar, page, 4096, VMO_CACHE_UC, &x), ERR_ACCESS_DENIED);
        /* The rest of the BAR is the driver's: page 0, unless it holds the
         * table or the PBA itself. */
        bool zero_protected = page == 0 || ((where[1 - i] & 7) == bir &&
                                            (where[1 - i] & ~0xfffu) == 0);
        if (!zero_protected) {
            CHECK_ST(jam_vmo_create_physical(bar, 0, 4096, VMO_CACHE_UC, &x), OK);
            CHECK_ST(jam_handle_close(x), OK);
        }
        CHECK_ST(jam_resource_create(bar, RES_MMIO, 0, 4096, &x), ERR_ACCESS_DENIED);
    }
    /* Config: the kernel's bits and capabilities are read-only. */
    CHECK_ST(jam_pci_config_write(dev, 0x04, 2, cmd ^ CMD_BME), ERR_ACCESS_DENIED);
    CHECK_ST(jam_pci_config_write(dev, cap + 2, 2, ctl), ERR_ACCESS_DENIED);
    CHECK_ST(jam_pci_config_write(dev, 0x10, 4, 0xffffffffu), ERR_ACCESS_DENIED);   /* BAR 0 */
    /* No RIGHT_MANAGE. */
    CHECK_ST(jam_pci_bus_master(dev, 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_dma_cap_create(dev, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_interrupt_create_msi(dev, 0, IRQ_MSIX, &x), ERR_ACCESS_DENIED);
    /* No other function: its handle is one function, and can't be sliced. */
    struct pci_dev_info info;
    CHECK_ST(jam_pci_enum(dev, 0, &info), ERR_WRONG_TYPE);
    CHECK_ST(jam_pci_device_open(dev, 0, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_resource_create(dev, RES_PCI, 0, 0, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_pci_bar_resource(dev, tab & 7, &x), ERR_ACCESS_DENIED);
    /* Nothing to pass on: the function and its BARs can't be
     * duplicated or sent (DR_SERVE is a channel like this one), nor can
     * the registers as a VMO. */
    handle_t ca, cb;
    CHECK_ST(jam_channel_create(&ca, &cb), OK);
    CHECK_ST(jam_handle_duplicate(dev, RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_channel_write(ca, "x", 1, &dev, 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_duplicate(hs[1], RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_channel_write(ca, "x", 1, &hs[1], 1), ERR_ACCESS_DENIED);
    /* A register page of the lowest BAR (hs[1]) that isn't the table or
     * the PBA. */
    uint32_t low = (uint32_t)__builtin_ctz(mask), pg = 0;
    while (((tab & 7) == low && (tab & ~0xfffu) == pg) ||
           ((pba & 7) == low && (pba & ~0xfffu) == pg))
        pg += 4096;
    if (jam_vmo_create_physical(hs[1], pg, 4096, VMO_CACHE_UC, &x) == OK) {
        CHECK_ST(jam_handle_duplicate(x, RIGHT_SAME, &v), ERR_ACCESS_DENIED);
        CHECK_ST(jam_channel_write(ca, "x", 1, &x, 1), ERR_ACCESS_DENIED);
        CHECK_ST(jam_handle_close(x), OK);
    }
    CHECK_ST(jam_handle_close(ca), OK);
    CHECK_ST(jam_handle_close(cb), OK);
    /* No dma_cap: no DMA memory, no pins. */
    CHECK_ST(jam_vmo_create(4096, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, HANDLE_INVALID, &x),
             ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_create(4096, DRV_VMO_DMA32, dev, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_create(4096, 0, HANDLE_INVALID, &v), OK);
    uint64_t addr = 0, pin = 0;
    CHECK_ST(jam_vmo_pin(v, HANDLE_INVALID, 0, 4096, &addr, &pin), ERR_BAD_HANDLE);
    CHECK_ST(jam_vmo_pin(v, dev, 0, 4096, &addr, &pin), ERR_WRONG_TYPE);
    CHECK_ST(jam_handle_close(v), OK);
    for (uint32_t i = 0; i < nh; i++)
        CHECK_ST(jam_handle_close(hs[i]), OK);
    return true;
}
