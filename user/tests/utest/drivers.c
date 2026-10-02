/* utest: drivers as processes. The null and drvtest drivers run as
 * processes started from here and talk through <idl/null.h>; with devmgr
 * (its control channel, SR_DEVMGR_CTL): the edu driver process it bound,
 * called through <idl/edu.h>, what the query channel may not do (hda's
 * channel included: it is the mixer's), a device channel (scoped to edu),
 * and edu
 * killed in the middle of a DMA (the device's pages quarantined, the
 * driver restarted). Tests that need devmgr or the edu device (QEMU's)
 * skip themselves without it. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <devmgr.h>
#include <idl/hda.h>
#include <idl/null.h>
#include <os.h>
#include "edu_check.h"
#include "utest.h"

/* ---- drivers as processes ------------------------------------------------------- */

#define DRVTEST_NULL 0x40   /* drvtest's role for its channel to a null server */

/* Start bootfs driver drv/<name> in job, handing it h under driver role
 * `role` (h is consumed). drvtest also gets the args "alpha beta". */
static status_t driver(const char *name, handle_t job, uint32_t role, handle_t h,
                       handle_t *proc)
{
    char path[32];
    snprintf(path, sizeof(path), "drv/%s", name);
    const char *argv[] = { path, "alpha", "beta" };
    struct spawn_handle x = { SR_DRIVER(role), h };
    struct spawn_args a = {
        .path = path, .argc = strcmp(name, "drvtest") ? 1 : 3, .argv = argv, .job = job,
        .extra = &x, .nextra = 1,
    };
    return spawn(&a, proc);
}

static bool job_is_empty(handle_t job)
{
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("the drivers' job still has %lu units of kind %u", (unsigned long)ji.used[k], k);
    return true;
}

/* The null driver as a process, called through the generated client from
 * here, then by the drvtest driver (a process too), which checks the whole
 * driver.h surface in the process build. */
bool t_driver_processes(void)
{
    handle_t job, a, b, srv, cli;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(driver("null", job, DR_SERVE, b, &srv), OK);
    uint64_t v = 0;
    uint32_t sum = 0;
    CHECK_ST(null_ping(a, 7, &v), OK);
    CHECK_EQ(v, 7);
    CHECK_ST(null_add(a, 40, 2, &sum), OK);
    CHECK_EQ(sum, 42);
    CHECK_ST(driver("drvtest", job, DRVTEST_NULL, a, &cli), OK);   /* our end goes to it */
    struct process_info info;
    CHECK_ST(spawn_wait(cli, 30 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);   /* every drvtest check passed */
    CHECK_ST(spawn_wait(srv, 10 * NS_PER_S, &info), OK);   /* its client is gone: it returns */
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(cli), OK);
    CHECK_ST(jam_handle_close(srv), OK);
    if (!job_is_empty(job))
        return false;
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* Killing a driver process that is waiting for requests. */
bool t_driver_killed(void)
{
    handle_t job, a, b, srv;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(driver("null", job, DR_SERVE, b, &srv), OK);
    uint8_t data[16], rev[16];
    for (int i = 0; i < 16; i++)
        data[i] = (uint8_t)i;
    CHECK_ST(null_reverse(a, data, rev), OK);
    CHECK(rev[0] == 15 && rev[15] == 0);
    CHECK_ST(jam_process_kill(srv), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(srv, 10 * NS_PER_S, &info), OK);
    CHECK(info.killed);
    uint64_t v;
    CHECK_ST(null_ping(a, 1, &v), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(srv), OK);
    if (!job_is_empty(job))
        return false;
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

bool t_startup_message(void)
{
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("startup", "hello", job, HANDLE_INVALID, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* ---- devmgr and the edu driver process ------------------------------------------------ */

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8

/* devmgr's control channel (the tests kill, rebind and look at
 * drivers' handles; init and the shell's `utest` hand it to us), or 0
 * (with a line saying the test is skipped). */
handle_t devmgr(void)
{
    handle_t dm = svc_get(SVC_DEVMGR_CTL);
    if (!dm)
        printf("utest: %s: no devmgr control channel (not started by init or the shell's "
               "utest?): skipped\n", utest_cur);
    return dm;
}

status_t dm_call(handle_t dm, uint32_t op, uint16_t vendor, uint16_t device,
                        struct devmgr_rep *r, handle_t *hs, uint32_t *nh)
{
    return devmgr_call(dm, op, vendor, device, 0, r, hs, hs ? DEVMGR_MAX_HANDLES : 0, nh,
                       now() + 30 * NS_PER_S);
}

/* The edu protocol end to end: utest -> devmgr's edu driver process. */
bool t_edu_process(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    struct devmgr_rep r;
    if (!dm)
        return true;
    status_t st = dm_call(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    CHECK_EQ(nh, 1);
    struct edu_check_result res;
    CHECK_ST(edu_check(hs[0], now() + 60 * NS_PER_S, &res), OK);
    char line[160];
    int n = snprintf(line, sizeof(line),
                     "edu (process): factorial(10)=%u ok, DMA 4 KiB round trip ok in %lu us, "
                     "MSI -> driver in %lu us",
                     res.fact10, (unsigned long)(res.dma_ns / 1000),
                     (unsigned long)(res.msi_ns / 1000));
    jam_debug_report(line, (uint64_t)n);
    CHECK_ST(jam_handle_close(hs[0]), OK);
    /* What devmgr must refuse. */
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, 0x1234, 0x0bad, &r, hs, &nh), ERR_NOT_FOUND);
    CHECK_ST(dm_call(dm, 0x00030063u, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh), ERR_NOT_SUPPORTED);
    CHECK_ST(dm_call(dm, DEVMGR_STATUS, 0, 0, &r, NULL, NULL), OK);
    CHECK(r.a >= 1);   /* edu at least */
    return true;
}

/* devmgr's query channel (SR_DEVMGR) answers the queries and
 * refuses everything that changes something or hands out hardware. */
bool t_devmgr_query_channel(void)
{
    handle_t q = svc_get(SVC_DEVMGR);
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    if (!q || !devmgr())
        return true;
    CHECK_ST(dm_call(q, DEVMGR_STATUS, 0, 0, &r, NULL, NULL), OK);
    static const uint32_t refused[] = { DEVMGR_KILL, DEVMGR_REBIND, DEVMGR_DRIVER_VIEW,
                                        DEVMGR_TEST_DRIVER, DEVMGR_SET_CONSOLE, 0x00030063u };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        nh = 0;
        CHECK_ST(dm_call(q, refused[i], 0xffff, 0xffff, &r, hs, &nh), ERR_ACCESS_DENIED);
        CHECK_EQ(nh, 0);
    }
    /* SET_CONSOLE with its handle: refused, the handle closed. */
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct devmgr_req qr = { 0, DEVMGR_SET_CONSOLE, 0, 0, 0 };
    uint32_t n = 0, got = 0;
    struct channel_call_args ca = {
        .h = q, .wn = sizeof(qr), .wbytes = (uint64_t)(uintptr_t)&qr,
        .wh = (uint64_t)(uintptr_t)&b, .whn = 1, .rcap = sizeof(r),
        .rbytes = (uint64_t)(uintptr_t)&r, .ractual = (uint64_t)(uintptr_t)&n,
        .rhactual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + 10 * NS_PER_S,
    };
    CHECK_ST(jam_channel_call(&ca), OK);
    CHECK(n >= DEVMGR_REP_HDR);
    CHECK_ST(r.status, ERR_ACCESS_DENIED);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(a, SIG_PEER_CLOSED, now() + 5 * NS_PER_S, &seen), OK);
    jam_handle_close(a);
    return true;
}

/* hda's service (its one output stream is the mixer's) is refused on the
 * query channel, whichever way it is named: init holds the sound card's
 * device channel (in shell mode the mixer's, in this run init's own),
 * and the query channel never hands out a device that has one. The
 * control channel (the tests') hands it out, and the query channel still
 * hands out the others'. Skipped without an hda driver. */
bool t_devmgr_query_refuses_hda(void)
{
    handle_t q = svc_get(SVC_DEVMGR), dm = devmgr();
    struct devmgr_rep r;
    unsigned hdas = 0;
    if (!q || !dm)
        return true;
    for (uint32_t n = 0; n < 32; n++) {
        handle_t ch = HANDLE_INVALID;
        uint32_t nh = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                                  now() + 10 * NS_PER_S);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK)
            continue;
        uint32_t codec, pin, dac, pcm, formats, amp, jack, count;
        uint8_t nodes[8], text[240];
        st = hda_info_until(ch, now() + 10 * NS_PER_S, &codec, &pin, &dac, &pcm, &formats, &amp,
                            &jack, &count, nodes, text);
        CHECK_ST(jam_handle_close(ch), OK);
        bool hda = st != ERR_NOT_SUPPORTED;   /* another driver's service */
        hdas += hda;
        nh = 0;
        ch = HANDLE_INVALID;
        CHECK_ST(devmgr_call(q, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                             now() + 10 * NS_PER_S), hda ? ERR_ACCESS_DENIED : OK);
        CHECK_EQ(nh, hda ? 0u : 1u);
        if (ch)
            CHECK_ST(jam_handle_close(ch), OK);
    }
    /* By its ids too: QEMU's intel-hda (ICH6) and ich9-intel-hda. */
    static const uint16_t ids[][2] = { { 0x8086, 0x2668 }, { 0x8086, 0x293e } };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        handle_t ch = HANDLE_INVALID;
        uint32_t nh = 0;
        status_t st = devmgr_call(q, DEVMGR_GET_SERVICE, ids[i][0], ids[i][1], 0, &r, &ch, 1,
                                  &nh, now() + 10 * NS_PER_S);
        CHECK(st == ERR_NOT_FOUND || st == ERR_ACCESS_DENIED);
        CHECK_EQ(nh, 0);
    }
    if (!hdas)
        printf("utest: %s: no hda driver: only the ids checked\n", utest_cur);
    return true;
}

/* One devmgr call on ch naming (vendor, device, instance); OK with the
 * one handle it carried into *out (closed if out is NULL). */
static status_t dm_one(handle_t ch, uint32_t op, uint16_t vendor, uint16_t device,
                       uint32_t instance, handle_t *out)
{
    struct devmgr_rep r;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    status_t st = devmgr_call(ch, op, vendor, device, instance, &r, hs, DEVMGR_MAX_HANDLES, &nh,
                              now() + 10 * NS_PER_S);
    for (uint32_t i = 0; i < nh; i++)
        if (i || !out || st != OK)
            jam_handle_close(hs[i]);
    if (st == OK && out)
        *out = nh ? hs[0] : HANDLE_INVALID;
    return st;
}

/* The query channel's answer for edu's service settles to `want` within
 * 5 s (a device channel's end reaches devmgr in its own time). */
static bool edu_from_query(handle_t q, status_t want)
{
    uint64_t until = now() + 5 * NS_PER_S;
    status_t st;
    while ((st = dm_one(q, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, 0, NULL)) != want &&
           now() < until)
        (void)jam_nanosleep(now() + 20 * NS_PER_MS);
    CHECK_ST(st, want);
    return true;
}

/* A device channel (DEVMGR_DEVICE_CHANNEL) for edu, named by its class
 * (QEMU's edu is class 00 ff 00, PCI_CLASS_OTHERS): it answers about edu alone,
 * there is one at a time, and while it has a client the query channel
 * refuses edu's service; once it is closed the query channel hands it
 * out again. Skipped without devmgr or edu. */
bool t_devmgr_device_channel(void)
{
    handle_t q = svc_get(SVC_DEVMGR), dm = devmgr(), d = HANDLE_INVALID, ch = HANDLE_INVALID;
    if (!q || !dm)
        return true;
    if (dm_one(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, 0, NULL) != OK) {
        printf("utest: %s: no edu device: skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(devmgr_device_channel(q, DEVMGR_PCI_CLASS, 0, 0x00ff00u, now() + 10 * NS_PER_S, &d),
             ERR_ACCESS_DENIED);   /* the control channel's to make */
    CHECK_ST(devmgr_device_channel(dm, DEVMGR_PCI_CLASS, 0, 0x00ff00u, now() + 10 * NS_PER_S,
                                   &d), OK);
    handle_t again = HANDLE_INVALID;
    CHECK_ST(devmgr_device_channel(dm, EDU_VENDOR, EDU_DEVICE, 0, now() + 10 * NS_PER_S, &again),
             ERR_BAD_STATE);
    CHECK_ST(devmgr_device_channel(dm, 0x1234, 0x0bad, 0, now() + 10 * NS_PER_S, &again),
             ERR_NOT_FOUND);
    /* Its own device, by all-zero fields or by its ids: edu's driver. */
    CHECK_ST(dm_one(d, DEVMGR_GET_SERVICE, 0, 0, 0, &ch), OK);
    uint32_t f = 0;
    CHECK_ST(edu_factorial_until(ch, now() + 10 * NS_PER_S, 5, &f), OK);
    CHECK_EQ(f, 120u);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(dm_one(d, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, 0, NULL), OK);
    CHECK_ST(dm_one(d, DEVMGR_SUPERVISION, 0, 0, 0, NULL), OK);
    CHECK_ST(dm_one(d, DEVMGR_GET_DRIVER, 0, 0, 0, NULL), OK);
    /* Nothing else: another device (by ids or by class; one that doesn't
     * exist too), or a call that isn't about its device. */
    CHECK_ST(dm_one(d, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, 1, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(dm_one(d, DEVMGR_GET_SERVICE, 0x1234, 0x0bad, 0, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(dm_one(d, DEVMGR_GET_SERVICE, DEVMGR_TEST_VENDOR, DEVMGR_TEST_DEVICE, 0, NULL),
             ERR_ACCESS_DENIED);
    CHECK_ST(dm_one(d, DEVMGR_GET_SERVICE, DEVMGR_PCI_CLASS, 0, 0x0c0330u, NULL),
             ERR_ACCESS_DENIED);   /* xHCI: usb-bus */
    CHECK_ST(dm_one(d, DEVMGR_SUPERVISION, DEVMGR_PCI_CLASS, 0, 0x0c0330u, NULL),
             ERR_ACCESS_DENIED);
    static const uint32_t refused[] = { DEVMGR_STATUS, DEVMGR_KILL, DEVMGR_REBIND,
                                        DEVMGR_DRIVER_VIEW, DEVMGR_DEVICE_CHANNEL,
                                        DEVMGR_TEST_DRIVER };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        CHECK_ST(dm_one(d, refused[i], 0, 0, 0, NULL), ERR_ACCESS_DENIED);
    /* The query channel: not while d has a client; again once it is gone. */
    if (!edu_from_query(q, ERR_ACCESS_DENIED))
        return false;
    CHECK_ST(dm_one(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, 0, NULL), OK);   /* control */
    CHECK_ST(jam_handle_close(d), OK);
    return edu_from_query(q, OK);
}

/* devmgr's supervision view of a device. */
bool supervision(handle_t dm, uint16_t vendor, uint16_t device, struct devmgr_rep *r)
{
    CHECK_ST(dm_call(dm, DEVMGR_SUPERVISION, vendor, device, r, NULL, NULL), OK);
    return true;
}

/* Wait (up to 10 s) for edu's DMA quarantine to be empty; *sup: the
 * supervision view then. */
static bool quarantine_released(handle_t dm, struct devmgr_rep *sup)
{
    uint64_t until = now() + 10 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, EDU_VENDOR, EDU_DEVICE, sup))
            return false;
        if (!sup->d || now() > until)
            return true;
        jam_nanosleep(now() + 20 * NS_PER_MS);
    }
}

/* Killing the edu driver process while its DMA runs, and supervision
 * bringing it back: Bus Master Enable goes off, its pinned buffer is
 * quarantined (still charged to its job), its MSI vector is free; devmgr
 * restarts it at once (a KILL is a death like a crash) with a new vector
 * and dma_cap; the client reconnects through GET_SERVICE and factorial and
 * DMA work; once the new driver has turned bus mastering on, the
 * quarantine lets go (a grace period later) with no page written while it
 * held them, and the dead driver's job is empty. */
bool t_edu_killed_mid_dma(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    struct devmgr_rep r, sup;
    if (!dm)
        return true;
    status_t st = dm_call(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    handle_t ch = hs[0];
    CHECK_ST(dm_call(dm, DEVMGR_GET_DRIVER, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh), OK);
    CHECK_EQ(nh, 3);
    handle_t proc = hs[0], job = hs[1], dev = hs[2];
    if (!supervision(dm, EDU_VENDOR, EDU_DEVICE, &sup))
        return false;
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    uint32_t restarts0 = sup.b, changed0 = sup.e;

    uint64_t addr = 0;
    uint32_t cmd = 0, f = 0;
    CHECK_ST(edu_dma_start_until(ch, now() + 10 * NS_PER_S, 4096, &addr), OK);   /* running now */
    CHECK(addr && addr < (1ull << 32));
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    CHECK(cmd & CMD_BME);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    CHECK(ji.used[JOB_LIMIT_PAGES] > 0);
    uint64_t t0 = now();
    CHECK_ST(dm_call(dm, DEVMGR_KILL, EDU_VENDOR, EDU_DEVICE, &r, NULL, NULL), OK);
    printf("utest: %s: killed mid-DMA, dead %lu us later\n", utest_cur,
           (unsigned long)((now() - t0) / 1000));
    struct process_info info;
    CHECK_ST(jam_process_get_info(proc, &info), OK);
    CHECK_EQ(info.state, PROCESS_DEAD);
    CHECK(info.killed);
    /* Its pinned buffer is quarantined, still charged to its job. */
    if (!supervision(dm, EDU_VENDOR, EDU_DEVICE, &sup))
        return false;
    CHECK(sup.a == DEVMGR_SUP_RESTARTING || sup.a == DEVMGR_SUP_RUNNING);
    CHECK(sup.d >= 2);
    CHECK_ST(info_of(job, &ji), OK);
    CHECK(ji.used[JOB_LIMIT_PAGES] > 0);
    CHECK_ST(edu_factorial_until(ch, now() + 5 * NS_PER_S, 3, &f), ERR_PEER_CLOSED);
    CHECK_ST(jam_pci_config_write(dev, 0x3c, 1, 0), ERR_ACCESS_DENIED);   /* a read-only view */
    CHECK_ST(jam_process_kill(proc), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);

    /* The reconnect rule: ask devmgr again; the call waits for the restart. */
    CHECK_ST(dm_call(dm, DEVMGR_GET_SERVICE, EDU_VENDOR, EDU_DEVICE, &r, hs, &nh), OK);
    CHECK_ST(edu_factorial_until(hs[0], now() + 10 * NS_PER_S, 10, &f), OK);
    CHECK_EQ(f, 3628800);
    printf("utest: %s: restarted and answering %lu ms after the kill\n", utest_cur,
           (unsigned long)((now() - t0) / NS_PER_MS));
    CHECK_ST(edu_dma_roundtrip_until(hs[0], now() + 10 * NS_PER_S, 4096), OK);
    CHECK_ST(jam_handle_close(hs[0]), OK);
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    CHECK(cmd & CMD_BME);
    /* The quarantine lets the dead driver's pages go a grace period (1 s)
     * after the new driver turned bus mastering on. Its count drops only
     * once the pages are back, so then the job is empty (pins, VMOs,
     * threads: gone), and nothing wrote them meanwhile. */
    if (!quarantine_released(dm, &sup) || !job_is_empty(job))
        return false;
    printf("utest: %s: restarts %u, quarantine %u page(s) left, %u stale page(s)\n", utest_cur,
           sup.b - restarts0, sup.d, sup.e - changed0);
    CHECK_EQ(sup.a, DEVMGR_SUP_RUNNING);
    CHECK_EQ(sup.b, restarts0 + 1);
    CHECK_EQ(sup.d, 0);
    CHECK_EQ(sup.e, changed0);   /* 0 stale bytes */
    CHECK_ST(jam_handle_close(job), OK);
    CHECK_ST(jam_handle_close(dev), OK);
    return true;
}
