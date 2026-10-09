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
#include <idl/devmgr.h>
#include <idl/hda.h>
#include <idl/netdev.h>
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

#define TENS (10 * NS_PER_S)

/* edu's address: its PCI ids, the first of them. */
#define EDU_ADDR DEVMGR_KIND_PCI_IDS, EDU_VENDOR, EDU_DEVICE, 0

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

/* devmgr's supervision view of the device (kind, a, b, n). */
bool supervision(handle_t dm, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                 struct dm_sup *s)
{
    CHECK_ST(devmgr_supervision_until(dm, now() + 30 * NS_PER_S, kind, a, b, n, &s->state,
                                      &s->restarts, &s->backoff_ms, &s->held, &s->changed), OK);
    return true;
}

/* A request to devmgr that no method of the protocol has (protocol id << 16
 * | a method nobody defined): the status it is answered with. */
static status_t forged(handle_t ch, uint32_t protocol_id)
{
    uint32_t q[2] = { 0, protocol_id << 16 | 0x63u };
    struct { uint32_t txid; int32_t status; } r = { 0, OK };
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = ch, .wr_len = sizeof(q), .wr_bytes = (uint64_t)(uintptr_t)q, .rd_cap = sizeof(r),
        .rd_bytes = (uint64_t)(uintptr_t)&r, .rd_actual = (uint64_t)(uintptr_t)&n,
        .deadline_ns = now() + TENS,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    return n >= sizeof(r) ? r.status : ERR_INTERNAL;
}

/* devmgr.get_service of the address (kind, a, b, n) on ch; OK with the
 * channel into *out (closed if out is NULL). */
static status_t service_of(handle_t ch, uint8_t kind, uint32_t a, uint32_t b, uint32_t n,
                           handle_t *out)
{
    handle_t h = HANDLE_INVALID;
    status_t st = devmgr_get_service_until(ch, now() + TENS, kind, a, b, n, &h);
    if (st == OK && out)
        *out = h;
    else if (st == OK)
        jam_handle_close(h);
    return st;
}

/* The edu protocol end to end: utest -> devmgr's edu driver process. */
bool t_edu_process(void)
{
    handle_t dm = devmgr(), svc;
    if (!dm)
        return true;
    status_t st = devmgr_get_service_until(dm, now() + 30 * NS_PER_S, EDU_ADDR, &svc);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    struct edu_check_result res;
    CHECK_ST(edu_check(svc, now() + 60 * NS_PER_S, &res), OK);
    char line[160];
    int n = snprintf(line, sizeof(line),
                     "edu (process): factorial(10)=%u ok, DMA 4 KiB round trip ok in %lu us, "
                     "MSI -> driver in %lu us",
                     res.fact10, (unsigned long)(res.dma_ns / 1000),
                     (unsigned long)(res.msi_ns / 1000));
    jam_debug_report(line, (uint64_t)n);
    CHECK_ST(jam_handle_close(svc), OK);
    /* What devmgr must refuse. */
    CHECK_ST(service_of(dm, DEVMGR_KIND_PCI_IDS, 0x1234, 0x0bad, 0, NULL), ERR_NOT_FOUND);
    CHECK_ST(forged(dm, DEVMGR_PROTOCOL_ID), ERR_NOT_SUPPORTED);
    CHECK_ST(forged(dm, DEVMGRCTL_PROTOCOL_ID), ERR_NOT_SUPPORTED);
    uint32_t bound, failed, skipped;
    CHECK_ST(devmgr_status_until(dm, now() + TENS, &bound, &failed, &skipped), OK);
    CHECK(bound >= 1);   /* edu at least */
    return true;
}

/* devmgr's query channel (SR_DEVMGR) answers the queries and
 * refuses everything that changes something or hands out hardware. */
bool t_devmgr_query_channel(void)
{
    handle_t q = svc_get(SVC_DEVMGR), h = HANDLE_INVALID;
    if (!q || !devmgr())
        return true;
    uint32_t bound, failed, skipped, mask;
    uint64_t koid;
    CHECK_ST(devmgr_status_until(q, now() + TENS, &bound, &failed, &skipped), OK);
    /* devmgrctl is the control channel's: every method of it, with an
     * address that would name a driver, is refused. */
    status_t ctl[] = {
        devmgrctl_kill_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0, &koid),
        devmgrctl_rebind_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0),
        devmgrctl_release_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0),
        devmgrctl_release_usb_until(q, now() + TENS, 0, 0, &h),
        devmgrctl_driver_view_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0, &h, &mask),
        devmgrctl_driver_bar_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0, 0, &h),
        devmgrctl_test_driver_until(q, now() + TENS),
        devmgrctl_device_channel_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0, &h),
        devmgrctl_shutdown_until(q, now() + TENS),
        devmgrctl_kill_process_until(q, now() + TENS, 1),
        devmgrctl_disk_storage_until(q, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0, &h),
        forged(q, DEVMGRCTL_PROTOCOL_ID),
    };
    for (unsigned i = 0; i < sizeof(ctl) / sizeof(ctl[0]); i++)
        CHECK_ST(ctl[i], ERR_ACCESS_DENIED);
    CHECK_EQ(h, HANDLE_INVALID);
    /* set_input and set_volumes with their handle: refused, the handle gone
     * with the request. */
    for (int which = 0; which < 2; which++) {
        handle_t a, b;
        CHECK_ST(jam_channel_create(&a, &b), OK);
        CHECK_ST(which ? devmgrctl_set_volumes_until(q, now() + TENS, b)
                       : devmgrctl_set_input_until(q, now() + TENS, b), ERR_ACCESS_DENIED);
        signals_t seen = 0;
        CHECK_ST(jam_object_wait_one(a, SIG_PEER_CLOSED, now() + 5 * NS_PER_S, &seen), OK);
        jam_handle_close(a);
    }
    return true;
}

/* hda's service (its one output stream is the mixer's) is refused on the
 * query channel, whichever way it is named: init holds the sound card's
 * device channel (in shell mode the mixer's, in this run init's own),
 * and the query channel never hands out a device that has one. The
 * control channel (the tests') hands it out, and the query channel still
 * hands out the others'. A network card's (QEMU_NET) is held by init the
 * same way, for netstack: refused too. Skipped without an hda driver. */
bool t_devmgr_query_refuses_hda(void)
{
    handle_t q = svc_get(SVC_DEVMGR), dm = devmgr();
    unsigned hdas = 0;
    if (!q || !dm)
        return true;
    for (uint32_t n = 0; n < 32; n++) {
        handle_t ch = HANDLE_INVALID;
        status_t st = service_of(dm, DEVMGR_KIND_PCI_BOUND, 0, 0, n, &ch);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK)
            continue;
        uint32_t codec, pin, dac, pcm, formats, amp, jack, nodes_len, text_len;
        uint8_t nodes[8];
        char text[240];
        st = hda_info_until(ch, now() + TENS, &codec, &pin, &dac, &pcm, &formats, &amp,
                            &jack, nodes, &nodes_len, text, &text_len);
        bool hda = st != ERR_NOT_SUPPORTED;   /* another driver's service */
        uint8_t mac[6];
        char chip[16 + 1];
        uint16_t vlan, mtu;
        uint32_t link, speed, changes, chip_len;
        bool nic = !hda && netdev_info_until(ch, now() + TENS, mac, &vlan, &mtu, &link,
                                             &speed, &changes, chip, &chip_len) !=
                               ERR_NOT_SUPPORTED;
        CHECK_ST(jam_handle_close(ch), OK);
        hdas += hda;
        bool claimed = hda || nic;   /* init holds a network card's device channel too */
        ch = HANDLE_INVALID;
        CHECK_ST(service_of(q, DEVMGR_KIND_PCI_BOUND, 0, 0, n, &ch),
                 claimed ? ERR_ACCESS_DENIED : OK);
        CHECK_EQ(ch != HANDLE_INVALID, !claimed);
        if (ch)
            CHECK_ST(jam_handle_close(ch), OK);
    }
    /* By its ids too: QEMU's intel-hda (ICH6) and ich9-intel-hda. */
    static const uint16_t ids[][2] = { { 0x8086, 0x2668 }, { 0x8086, 0x293e } };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        status_t st = service_of(q, DEVMGR_KIND_PCI_IDS, ids[i][0], ids[i][1], 0, NULL);
        CHECK(st == ERR_NOT_FOUND || st == ERR_ACCESS_DENIED);
    }
    if (!hdas)
        printf("utest: %s: no hda driver: only the ids checked\n", utest_cur);
    return true;
}

/* The query channel's answer for edu's service settles to `want` within
 * 5 s (a device channel's end reaches devmgr in its own time). */
static bool edu_from_query(handle_t q, status_t want)
{
    uint64_t until = now() + 5 * NS_PER_S;
    status_t st;
    while ((st = service_of(q, EDU_ADDR, NULL)) != want && now() < until)
        (void)jam_sleep_until(now() + 20 * NS_PER_MS);
    CHECK_ST(st, want);
    return true;
}

/* A device channel (devmgrctl.device_channel) for edu, named by its class
 * (QEMU's edu is class 00 ff 00, PCI_CLASS_OTHERS): it answers about edu alone,
 * there is one at a time, and while it has a client the query channel
 * refuses edu's service; once it is closed the query channel hands it
 * out again. Skipped without devmgr or edu. */
bool t_devmgr_device_channel(void)
{
    handle_t q = svc_get(SVC_DEVMGR), dm = devmgr(), d = HANDLE_INVALID, ch = HANDLE_INVALID;
    if (!q || !dm)
        return true;
    if (service_of(dm, EDU_ADDR, NULL) != OK) {
        printf("utest: %s: no edu device: skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(devmgrctl_device_channel_until(q, now() + TENS, DEVMGR_KIND_PCI_CLASS, 0x00ff00u, 0,
                                            0, &d), ERR_ACCESS_DENIED);   /* the control channel's */
    CHECK_ST(devmgrctl_device_channel_until(dm, now() + TENS, DEVMGR_KIND_PCI_CLASS, 0x00ff00u, 0,
                                            0, &d), OK);
    handle_t again = HANDLE_INVALID;
    CHECK_ST(devmgrctl_device_channel_until(dm, now() + TENS, EDU_ADDR, &again), ERR_BAD_STATE);
    CHECK_ST(devmgrctl_device_channel_until(dm, now() + TENS, DEVMGR_KIND_PCI_IDS, 0x1234,
                                            0x0bad, 0, &again), ERR_NOT_FOUND);
    /* Its own device, by KIND_SELF or by its ids: edu's driver. */
    CHECK_ST(service_of(d, DEVMGR_KIND_SELF, 0, 0, 0, &ch), OK);
    uint32_t f = 0;
    CHECK_ST(edu_factorial_until(ch, now() + TENS, 5, &f), OK);
    CHECK_EQ(f, 120u);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(service_of(d, EDU_ADDR, NULL), OK);
    struct dm_sup sup;
    CHECK(supervision(d, DEVMGR_KIND_SELF, 0, 0, 0, &sup));
    handle_t proc, job;
    uint32_t idx;
    CHECK_ST(devmgr_get_driver_until(d, now() + TENS, DEVMGR_KIND_SELF, 0, 0, 0, &proc, &job,
                                     &idx), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    CHECK_ST(devmgr_get_function_until(d, now() + TENS, DEVMGR_KIND_SELF, 0, 0, 0, &ch), OK);
    CHECK_ST(jam_handle_close(ch), OK);
    /* Nothing else: another device (by ids or by class; one that doesn't
     * exist too), or a call that isn't about its device. */
    CHECK_ST(service_of(d, DEVMGR_KIND_PCI_IDS, EDU_VENDOR, EDU_DEVICE, 1, NULL),
             ERR_ACCESS_DENIED);
    CHECK_ST(service_of(d, DEVMGR_KIND_PCI_IDS, 0x1234, 0x0bad, 0, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(service_of(d, DEVMGR_KIND_TEST, 0, 0, 0, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(service_of(d, DEVMGR_KIND_PCI_CLASS, 0x0c0330u, 0, 0, NULL),
             ERR_ACCESS_DENIED);   /* xHCI: usb-bus */
    CHECK_ST(devmgr_supervision_until(d, now() + TENS, DEVMGR_KIND_PCI_CLASS, 0x0c0330u, 0, 0,
                                      &sup.state, &sup.restarts, &sup.backoff_ms, &sup.held,
                                      &sup.changed), ERR_ACCESS_DENIED);
    uint32_t bound, failed, skipped;
    CHECK_ST(devmgr_status_until(d, now() + TENS, &bound, &failed, &skipped), ERR_ACCESS_DENIED);
    uint64_t koid;
    CHECK_ST(devmgrctl_kill_until(d, now() + TENS, DEVMGR_KIND_SELF, 0, 0, 0, &koid),
             ERR_ACCESS_DENIED);
    CHECK_ST(devmgrctl_rebind_until(d, now() + TENS, DEVMGR_KIND_SELF, 0, 0, 0),
             ERR_ACCESS_DENIED);
    CHECK_ST(devmgrctl_driver_view_until(d, now() + TENS, DEVMGR_KIND_SELF, 0, 0, 0, &ch,
                                         &bound), ERR_ACCESS_DENIED);
    CHECK_ST(devmgrctl_device_channel_until(d, now() + TENS, DEVMGR_KIND_SELF, 0, 0, 0, &ch),
             ERR_ACCESS_DENIED);
    CHECK_ST(devmgrctl_test_driver_until(d, now() + TENS), ERR_ACCESS_DENIED);
    /* The query channel: not while d has a client; again once it is gone. */
    if (!edu_from_query(q, ERR_ACCESS_DENIED))
        return false;
    CHECK_ST(service_of(dm, EDU_ADDR, NULL), OK);   /* control */
    CHECK_ST(jam_handle_close(d), OK);
    return edu_from_query(q, OK);
}

/* /svc/devmgr hands each opener a query channel of its own (svc.connect):
 * a request written on one is answered on that one only, the shared
 * channel's other holders never see it, and a closed opener's slot is
 * free again (more opens, one after another, than devmgr has slots). */
bool t_devmgr_openers(void)
{
    handle_t a = HANDLE_INVALID, b = HANDLE_INVALID;
    if (svc_open(SVC_DEVMGR, &a) != OK)
        return true;   /* no devmgr: skipped */
    CHECK_ST(svc_open(SVC_DEVMGR, &b), OK);
    CHECK_ST(devmgr_status_send(a, 7), OK);   /* asked, not waited for */
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(a, SIG_READABLE, now() + TENS, &seen), OK);
    CHECK_ST(jam_object_wait_one(b, SIG_READABLE, 0, &seen), ERR_TIMED_OUT);
    _Alignas(8) uint8_t rep[DEVMGR_REP_MAX];
    struct idl_msg m;
    uint32_t bound, failed, skipped;
    CHECK_ST(idl_reply_read(a, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, 7);
    CHECK_ST(devmgr_status_result(rep, &m, &bound, &failed, &skipped), OK);
    /* The opener's channel is a query channel like the shared one. */
    CHECK_ST(devmgr_status_until(b, now() + 30 * NS_PER_S, &bound, &failed, &skipped), OK);
    uint64_t koid;
    CHECK_ST(devmgrctl_kill_until(b, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, 0, &koid),
             ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    for (unsigned i = 0; i < 40; i++) {
        CHECK_ST(svc_open(SVC_DEVMGR, &a), OK);
        CHECK_ST(devmgr_status_until(a, now() + 30 * NS_PER_S, &bound, &failed, &skipped), OK);
        CHECK_ST(jam_handle_close(a), OK);
    }
    return true;
}

/* /svc/devmgr-ctl hands each opener a control channel of its own too
 * (svc.connect): init keeps the end it started devmgr with for itself, reads
 * the answers to the calls it sends without waiting there, and so a request
 * and its answer on one opener's channel are no other opener's. Each is a
 * control channel (it answers devmgrctl), and a closed one's slot is free
 * again. */
bool t_devmgr_ctl_openers(void)
{
    handle_t a = HANDLE_INVALID, b = HANDLE_INVALID;
    if (svc_open(SVC_DEVMGR_CTL, &a) != OK)
        return true;   /* no devmgr: skipped */
    CHECK_ST(svc_open(SVC_DEVMGR_CTL, &b), OK);
    CHECK_ST(devmgr_status_send(a, 9), OK);   /* asked, not waited for */
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(a, SIG_READABLE, now() + TENS, &seen), OK);
    CHECK_ST(jam_object_wait_one(b, SIG_READABLE, 0, &seen), ERR_TIMED_OUT);
    _Alignas(8) uint8_t rep[DEVMGR_REP_MAX];
    struct idl_msg m;
    uint32_t bound, failed, skipped;
    CHECK_ST(idl_reply_read(a, rep, sizeof(rep), &m), OK);
    CHECK_EQ(m.txid, 9);
    CHECK_ST(devmgr_status_result(rep, &m, &bound, &failed, &skipped), OK);
    /* Both are control channels. */
    CHECK_ST(devmgrctl_kill_process_until(a, now() + TENS, 0), ERR_NOT_FOUND);
    CHECK_ST(devmgrctl_kill_process_until(b, now() + TENS, 0), ERR_NOT_FOUND);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    for (unsigned i = 0; i < 40; i++) {
        CHECK_ST(svc_open(SVC_DEVMGR_CTL, &a), OK);
        CHECK_ST(devmgrctl_kill_process_until(a, now() + TENS, 0), ERR_NOT_FOUND);
        CHECK_ST(jam_handle_close(a), OK);
    }
    return true;
}

/* Wait (up to 10 s) for edu's DMA quarantine to be empty; *sup: the
 * supervision view then. */
static bool quarantine_released(handle_t dm, struct dm_sup *sup)
{
    uint64_t until = now() + 10 * NS_PER_S;
    for (;;) {
        if (!supervision(dm, EDU_ADDR, sup))
            return false;
        if (!sup->held || now() > until)
            return true;
        jam_sleep_until(now() + 20 * NS_PER_MS);
    }
}

/* Does an IOMMU translate DMA (SYSINFO_IOMMU: iommu=on)? Then a dead
 * driver's pins are freed as soon as the IOMMU took them away, not
 * quarantined. */
static bool iommu_on(void)
{
    handle_t h;
    struct sys_info si;
    if (jam_handle_duplicate(startup_handle(SR_RESOURCE), RIGHT_ROOT_SYSINFO, &h) != OK)
        return false;
    status_t st = jam_sys_info(h, &si);
    jam_handle_close(h);
    return st == OK && (si.flags & SYSINFO_IOMMU);
}

/* The address-based methods: every kind of address names the device it
 * should (edu: by its ids, its class, its place among the bound functions,
 * its bus address), one that isn't an address is ERR_INVALID_ARGS, and
 * get_driver's pci_index and get_function agree on which function it is. */
bool t_devmgr_addresses(void)
{
    handle_t dm = devmgr();
    if (!dm)
        return true;
    handle_t proc, job, fn;
    uint32_t idx = 0, idx2 = 0;
    status_t st = devmgr_get_driver_until(dm, now() + TENS, EDU_ADDR, &proc, &job, &idx);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    struct process_info pi, pi2;
    CHECK_ST(jam_process_get_info(proc, &pi), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    /* By its class: the first function of class 00 ff 00 that has a driver. */
    CHECK_ST(devmgr_get_driver_until(dm, now() + TENS, DEVMGR_KIND_PCI_CLASS, 0x00ff00u, 0, 0,
                                     &proc, &job, &idx2), OK);
    CHECK_ST(jam_process_get_info(proc, &pi2), OK);
    CHECK_EQ(pi2.koid, pi.koid);
    CHECK_EQ(idx2, idx);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    /* By its place among the functions that have a driver (some number
     * below 32 is edu), and by its address on the bus (bus 0, QEMU's). */
    bool by_bound = false, by_bdf = false;
    for (uint32_t n = 0; n < 32 && !by_bound; n++) {
        if (devmgr_get_driver_until(dm, now() + TENS, DEVMGR_KIND_PCI_BOUND, 0, 0, n, &proc,
                                    &job, &idx2) != OK)
            continue;
        by_bound = idx2 == idx;
        CHECK_ST(jam_handle_close(proc), OK);
        CHECK_ST(jam_handle_close(job), OK);
    }
    CHECK(by_bound);
    for (uint32_t bdf = 0; bdf < 256 && !by_bdf; bdf++) {   /* bus 0, devices 0-31, functions 0-7 */
        if (devmgr_get_driver_until(dm, now() + TENS, DEVMGR_KIND_PCI_BDF, bdf, 0, 0, &proc,
                                    &job, &idx2) != OK)
            continue;
        by_bdf = idx2 == idx;
        CHECK_ST(jam_handle_close(proc), OK);
        CHECK_ST(jam_handle_close(job), OK);
        if (by_bdf) {   /* its config space says whose it is */
            uint32_t id = 0;
            CHECK_ST(devmgr_get_function_until(dm, now() + TENS, DEVMGR_KIND_PCI_BDF, bdf, 0, 0,
                                               &fn), OK);
            CHECK_ST(jam_pci_config_read(fn, 0, 4, &id), OK);
            CHECK_EQ(id, (uint32_t)(EDU_DEVICE << 16 | EDU_VENDOR));
            CHECK_ST(jam_pci_config_write(fn, 0x3c, 1, 0), ERR_ACCESS_DENIED);   /* read-only */
            CHECK_ST(jam_handle_close(fn), OK);
        }
    }
    CHECK(by_bdf);
    /* Not an address, or an address of nothing. */
    handle_t h = HANDLE_INVALID;
    CHECK_ST(service_of(dm, 99, 0, 0, 0, &h), ERR_INVALID_ARGS);
    CHECK_ST(service_of(dm, DEVMGR_KIND_SELF, 0, 0, 0, &h),
             ERR_INVALID_ARGS);   /* only a device channel has a device of its own */
    CHECK_ST(service_of(dm, DEVMGR_KIND_PCI_BDF, 0x10000, 0, 0, &h), ERR_INVALID_ARGS);
    CHECK_ST(service_of(dm, DEVMGR_KIND_PCI_CLASS, 0x00ff00u, 1, 0, &h), ERR_INVALID_ARGS);
    CHECK_ST(service_of(dm, DEVMGR_KIND_PCI_BOUND, 1, 0, 0, &h), ERR_INVALID_ARGS);
    CHECK_ST(service_of(dm, DEVMGR_KIND_USB_IFACE, 0xdead, 256, 0, &h), ERR_INVALID_ARGS);
    CHECK_ST(service_of(dm, DEVMGR_KIND_USB_IFACE, 0xdead, 0, 0, &h), ERR_NOT_FOUND);
    CHECK_ST(service_of(dm, DEVMGR_KIND_PCI_BDF, 0xffff, 0, 0, &h), ERR_NOT_FOUND);
    CHECK_EQ(h, HANDLE_INVALID);
    return true;
}

/* kill_process: the driver whose process has this koid, wherever devmgr
 * has it. The old koid is gone afterwards (the restart is a new process),
 * and a koid devmgr doesn't run is ERR_NOT_FOUND. */
bool t_devmgr_kill_process(void)
{
    handle_t dm = devmgr(), proc, job;
    if (!dm)
        return true;
    uint32_t idx;
    status_t st = devmgr_get_driver_until(dm, now() + TENS, EDU_ADDR, &proc, &job, &idx);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    struct process_info pi;
    CHECK_ST(jam_process_get_info(proc, &pi), OK);
    CHECK_ST(jam_handle_close(job), OK);
    struct dm_sup before, after;
    if (!supervision(dm, EDU_ADDR, &before))
        return false;
    CHECK_ST(devmgrctl_kill_process_until(dm, now() + 30 * NS_PER_S, pi.koid), OK);
    struct process_info dead;
    CHECK_ST(jam_process_get_info(proc, &dead), OK);
    CHECK_EQ(dead.state, PROCESS_DEAD);   /* answered once it was dead */
    CHECK(dead.killed);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(devmgrctl_kill_process_until(dm, now() + TENS, pi.koid), ERR_NOT_FOUND);
    CHECK_ST(devmgrctl_kill_process_until(dm, now() + TENS, 0), ERR_NOT_FOUND);
    handle_t ch;
    uint32_t f = 0;
    CHECK_ST(service_of(dm, EDU_ADDR, &ch), OK);   /* the reconnect rule */
    CHECK_ST(edu_factorial_until(ch, now() + TENS, 4, &f), OK);
    CHECK_EQ(f, 24u);
    CHECK_ST(jam_handle_close(ch), OK);
    if (!supervision(dm, EDU_ADDR, &after))
        return false;
    CHECK_EQ(after.restarts, before.restarts + 1);   /* a death like any: restarted */
    return true;
}

/* Killing the edu driver process while its DMA runs, and supervision
 * bringing it back: Bus Master Enable goes off, its pinned buffer is
 * quarantined (still charged to its job; with the IOMMU, freed as soon as
 * its domain is gone instead), its MSI vector is free; devmgr
 * restarts it at once (a kill is a death like a crash) with a new vector
 * and dma_cap; the client reconnects through get_service and factorial and
 * DMA work; once the new driver has turned bus mastering on, the
 * quarantine lets go (a grace period later) with no page written while it
 * held them, and the dead driver's job is empty. */
bool t_edu_killed_mid_dma(void)
{
    handle_t dm = devmgr(), ch, proc, job, dev;
    struct dm_sup sup;
    if (!dm)
        return true;
    status_t st = devmgr_get_service_until(dm, now() + 30 * NS_PER_S, EDU_ADDR, &ch);
    if (st == ERR_NOT_FOUND) {
        printf("utest: %s: no edu device (not QEMU?): skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(st, OK);
    uint32_t idx;
    CHECK_ST(devmgr_get_driver_until(dm, now() + 30 * NS_PER_S, EDU_ADDR, &proc, &job, &idx),
             OK);
    CHECK_ST(devmgr_get_function_until(dm, now() + 30 * NS_PER_S, EDU_ADDR, &dev), OK);
    if (!supervision(dm, EDU_ADDR, &sup))
        return false;
    CHECK_EQ(sup.state, DEVMGR_SUP_RUNNING);
    uint32_t restarts0 = sup.restarts, changed0 = sup.changed;

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
    uint64_t koid = 0;
    CHECK_ST(devmgrctl_kill_until(dm, now() + 30 * NS_PER_S, EDU_ADDR, &koid), OK);
    printf("utest: %s: killed mid-DMA, dead %lu us later\n", utest_cur,
           (unsigned long)((now() - t0) / 1000));
    struct process_info info;
    CHECK_ST(jam_process_get_info(proc, &info), OK);
    CHECK_EQ(info.state, PROCESS_DEAD);
    CHECK_EQ(info.koid, koid);   /* kill says whose process it was */
    CHECK(info.killed);
    /* Its pinned buffer is quarantined, still charged to its job (with
     * the IOMMU it may be freed already: the kernel's dma_iommu_* tests
     * check that order). */
    if (!supervision(dm, EDU_ADDR, &sup))
        return false;
    CHECK(sup.state == DEVMGR_SUP_RESTARTING || sup.state == DEVMGR_SUP_RUNNING);
    if (!iommu_on()) {
        CHECK(sup.held >= 2);
        CHECK_ST(info_of(job, &ji), OK);
        CHECK(ji.used[JOB_LIMIT_PAGES] > 0);
    }
    CHECK_ST(edu_factorial_until(ch, now() + 5 * NS_PER_S, 3, &f), ERR_PEER_CLOSED);
    CHECK_ST(jam_pci_config_write(dev, 0x3c, 1, 0), ERR_ACCESS_DENIED);   /* a read-only view */
    CHECK_ST(jam_process_kill(proc), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(ch), OK);
    CHECK_ST(jam_handle_close(proc), OK);

    /* The reconnect rule: ask devmgr again; the call waits for the restart. */
    handle_t again;
    CHECK_ST(devmgr_get_service_until(dm, now() + 30 * NS_PER_S, EDU_ADDR, &again), OK);
    CHECK_ST(edu_factorial_until(again, now() + 10 * NS_PER_S, 10, &f), OK);
    CHECK_EQ(f, 3628800);
    printf("utest: %s: restarted and answering %lu ms after the kill\n", utest_cur,
           (unsigned long)((now() - t0) / NS_PER_MS));
    CHECK_ST(edu_dma_roundtrip_until(again, now() + 10 * NS_PER_S, 4096), OK);
    CHECK_ST(jam_handle_close(again), OK);
    CHECK_ST(jam_pci_config_read(dev, 0x04, 2, &cmd), OK);
    CHECK(cmd & CMD_BME);
    /* The quarantine lets the dead driver's pages go a grace period (1 s)
     * after the new driver turned bus mastering on. Its count drops only
     * once the pages are back, so then the job is empty (pins, VMOs,
     * threads: gone), and nothing wrote them meanwhile. */
    if (!quarantine_released(dm, &sup) || !job_is_empty(job))
        return false;
    printf("utest: %s: restarts %u, quarantine %u page(s) left, %u stale page(s)\n", utest_cur,
           sup.restarts - restarts0, sup.held, sup.changed - changed0);
    CHECK_EQ(sup.state, DEVMGR_SUP_RUNNING);
    CHECK_EQ(sup.restarts, restarts0 + 1);
    CHECK_EQ(sup.held, 0);
    CHECK_EQ(sup.changed, changed0);   /* 0 stale bytes */
    CHECK_ST(jam_handle_close(job), OK);
    CHECK_ST(jam_handle_close(dev), OK);
    return true;
}
