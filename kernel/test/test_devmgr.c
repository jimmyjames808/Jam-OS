/* The real devmgr (bin/devmgr, "nousb") and a real driver process
 * (drv/edu), started from here the way init starts devmgr, looked at from
 * the kernel side: what the driver holds, and what is left once it is
 * gone. QEMU only (they need the edu device); skipped on the PC and from
 * the shell (the live devmgr has the device).
 *
 *   devmgr_driver_handles_stay
 *       A bound driver's hardware handles (dma_cap, interrupt, function
 *       and BARs) have neither RIGHT_DUPLICATE nor RIGHT_TRANSFER.
 *   devmgr_driver_clean_exit
 *       A driver that exits by itself (its client gone, exit 0, not a
 *       kill) leaves its function with Bus Master Enable and MSI off, its
 *       vector freed, nothing pinned or quarantined, and no interrupt
 *       object alive.
 *   devmgr_refused_start_leaves_nothing
 *       A driver whose job can't pay for its start (devmgr's job at its
 *       thread, handle or page limit, stepping the page and handle
 *       headroom up one unit at a time so every point where a start can
 *       fail is hit) is refused, and each refusal leaves the job exactly as
 *       before, no interrupt object, Bus Master Enable and MSI off, nothing
 *       quarantined; with the limits gone the device binds again. */
#include <jam/channel.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/interrupt_test.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/startup.h>
#include <jam/time.h>
#include <jam/userboot.h>

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11e8
#define CMD_BME    0x04
#define MSI_ENABLE 0x0001
#define PASS_ON    (RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define CHAN_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL)

/* devmgr's protocol, as user/include/devmgr.h defines it (the kernel can't
 * include user headers): u32 txid, u32 ordinal, u16 vendor, u16 device,
 * u32 instance -> u32 txid, i32 status, u32 a..e (+ handles). */
#define DM_STATUS      0x00030001u
#define DM_GET_DRIVER  0x00030003u
#define DM_REBIND      0x00030005u
#define DM_MAX_HANDLES 8u

struct dm_req {
    uint32_t txid, ordinal;    /* channel txid; DM_* request */
    uint16_t vendor, device;   /* the device's PCI ids */
    uint32_t instance;         /* which instance of it */
} __attribute__((packed));

struct dm_rep {
    uint32_t txid;            /* the request's txid */
    int32_t  status;          /* OK or ERR_* */
    uint32_t a, b, c, d, e;   /* the reply's values, by request */
} __attribute__((packed));

/* A devmgr of our own. */
struct dm {
    struct job     *job;    /* devmgr's job; its drivers' jobs are below it */
    struct process *proc;   /* devmgr (a reference) */
    struct channel *ctl;    /* our end of its control channel */
    bool            was_managed;   /* the edu function's driver_managed before */
};

static struct pci_dev *edu(void)
{
    struct pci_dev *d = pci_find(EDU_VENDOR, EDU_DEVICE, 0);
    if (!d)
        kprintf("ktest %s: no edu device (QEMU with -device edu only), skipped\n",
                ktest_current);
    return d;
}

static bool bme(struct pci_dev *d)
{
    return pci_cfg_read(d, 0x04, 2) & CMD_BME;
}

static bool msi_on(struct pci_dev *d)
{
    return d->cap_msi && (pci_cfg_read(d, d->cap_msi + 2, 2) & MSI_ENABLE);
}

/* One call; handles that come back go into hs (up to DM_MAX_HANDLES; NULL:
 * none expected). Returns the reply's status. */
static status_t dm_call(const struct dm *m, uint32_t op, struct dm_rep *r, struct khandle *hs,
                        uint32_t *nh)
{
    struct dm_req q = { 0, op, op == DM_STATUS ? 0 : EDU_VENDOR, op == DM_STATUS ? 0 : EDU_DEVICE,
                        0 };
    uint32_t n = 0, got = 0;
    *r = (struct dm_rep){ 0 };
    status_t st = channel_call(m->ctl, &q, sizeof(q), NULL, 0, r, sizeof(*r), &n, hs,
                               hs ? DM_MAX_HANDLES : 0, &got, uptime_ns() + 30 * NS_PER_S);
    if (nh)
        *nh = got;
    if (st != OK)
        return st;
    return n >= 8 ? r->status : ERR_INTERNAL;
}

/* Start devmgr in a fresh job, as init does (RES_PCI, its control
 * channel), and wait for its first binding pass: edu bound. */
static void dm_start(struct dm *m, const struct pci_dev *d)
{
    m->was_managed = d->driver_managed;
    m->job = kt_fresh_job();
    struct kobject *root = resource_root(), *pci;
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    kobject_unref(root);
    struct channel *srv;
    KT_EQ(channel_create(&m->ctl, &srv), OK);
    const char *argv[] = { "bin/devmgr", "nousb" };   /* on QEMU: edu is all it binds */
    struct userboot_handle x[2] = {
        { SR_RESOURCE, khandle_from_new(pci, RES_RIGHTS) },
        { SR_DEVMGR_CTL, khandle_from_new((struct kobject *)srv, CHAN_RIGHTS) },
    };
    KT_EQ(userboot_spawn("bin/devmgr", argv, 2, m->job, x, 2, NULL, &m->proc), OK);
    struct dm_rep r;
    KT_EQ(dm_call(m, DM_STATUS, &r, NULL, NULL), OK);
    KT_EQ(r.a, 1);   /* bound */
    KT_EQ(r.b, 0);   /* failed */
}

/* Wait (bounded) until the driver has set up: it turns bus mastering on
 * last, after taking every handle out of its startup message. */
static void wait_driver_up(struct pci_dev *d)
{
    uint64_t until = uptime_ns() + 10 * NS_PER_S;
    while (!bme(d) && uptime_ns() < until)
        thread_sleep_ms(5);
    KT_ASSERT(bme(d));
}

/* The bound driver's process (a reference), once it has set up. */
static struct process *dm_driver(const struct dm *m, struct pci_dev *d)
{
    struct khandle hs[DM_MAX_HANDLES];
    uint32_t nh = 0;
    wait_driver_up(d);
    KT_EQ(dm_call(m, DM_GET_DRIVER, &(struct dm_rep){ 0 }, hs, &nh), OK);
    KT_EQ(nh, 3);   /* process, job, function */
    struct process *p = process_from_kobject(hs[0].obj);
    KT_ASSERT(p != NULL);
    kobject_ref(process_kobject(p));
    for (uint32_t i = 0; i < nh; i++)
        khandle_release(&hs[i]);
    return p;
}

/* Close our end of the control channel: devmgr stops its drivers (their
 * client ends close) and exits. Returns its exit code. */
static int64_t dm_stop(struct dm *m, struct pci_dev *d)
{
    kobject_unref((struct kobject *)m->ctl);
    KT_EQ(object_wait_one(process_kobject(m->proc), SIG_TERMINATED,
                          uptime_ns() + 60 * NS_PER_S, NULL),
          OK);
    struct process_info info;
    process_get_info(m->proc, &info);
    kobject_unref(process_kobject(m->proc));
    d->driver_managed = m->was_managed;   /* devmgr made it sticky */
    return info.exit_code;
}

/* Once the test holds nothing of theirs either: devmgr's job (and every
 * driver job below it) must be charged for nothing. */
static void dm_job_empty(const struct dm *m)
{
    kt_job_is_empty(m->job);
    job_unref(m->job);
}

KTEST(devmgr_driver_handles_stay)
{
    KT_SKIP_LIVE("runs a devmgr of its own; the live one has the devices");
    struct pci_dev *d = edu();
    if (!d)
        return;
    struct dm m;
    dm_start(&m, d);
    struct process *p = dm_driver(&m, d);
    static const struct { enum obj_type type; const char *what; } kinds[] = {
        { OBJ_DMA_CAP, "dma_cap" }, { OBJ_INTERRUPT, "interrupt" },
        { OBJ_RESOURCE, "function and BAR" },
    };
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        rights_t r[8];
        uint32_t cnt = handle_table_rights(process_handles(p), kinds[k].type, r, 8);
        kprintf("ktest %s: the driver holds %u %s handle(s)\n", ktest_current, cnt,
                kinds[k].what);
        KT_ASSERT(cnt >= 1 && cnt <= 8);
        for (uint32_t i = 0; i < cnt; i++)
            KT_EQ(r[i] & PASS_ON, 0);
    }
    kobject_unref(process_kobject(p));
    KT_EQ(dm_stop(&m, d), 0);
    dm_job_empty(&m);
}

KTEST(devmgr_driver_clean_exit)
{
    KT_SKIP_LIVE("runs a devmgr of its own; the live one has the devices");
    struct pci_dev *d = edu();
    if (!d)
        return;
    uint64_t irqs = interrupt_live_count();
    struct dma_quarantine_stats q0, q;
    dma_quarantine_stats(d, &q0);
    struct dm m;
    dm_start(&m, d);
    struct process *p = dm_driver(&m, d);
    struct kobject *irq, *cap;
    KT_EQ(handle_table_find(process_handles(p), OBJ_INTERRUPT, &irq), OK);
    KT_EQ(handle_table_find(process_handles(p), OBJ_DMA_CAP, &cap), OK);
    uint32_t cpu;
    uint8_t vec;
    KT_ASSERT(interrupt_vector_of(irq, &cpu, &vec));
    KT_ASSERT(msi_on(d));
    KT_GLOBAL_EQ(interrupt_live_count(), irqs + 1);

    KT_EQ(dm_stop(&m, d), 0);   /* every driver ended cleanly, its job at zero */
    struct process_info info;
    process_get_info(p, &info);
    kobject_unref(process_kobject(p));
    kprintf("ktest %s: the driver exited %ld (%s); BME %s, MSI %s, vector %s\n", ktest_current,
            info.exit_code, info.killed ? "killed" : "by itself", bme(d) ? "ON" : "off",
            msi_on(d) ? "ON" : "off", interrupt_vector_of(irq, &cpu, &vec) ? "HELD" : "freed");
    KT_EQ(info.state, PROCESS_DEAD);
    KT_EQ(info.killed, 0);
    KT_EQ(info.exit_code, 0);
    KT_ASSERT(!bme(d));
    KT_ASSERT(!msi_on(d));
    KT_ASSERT(!interrupt_vector_of(irq, &cpu, &vec));
    KT_EQ(dma_cap_pin_count(cap), 0);
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, q0.pins);   /* a clean driver unpins: nothing quarantined */
    kobject_unref(irq);
    kobject_unref(cap);
    dm_job_empty(&m);
    KT_GLOBAL_EQ(interrupt_live_count(), irqs);
}

/* ---- refused starts ---------------------------------------------------------- */

struct refusal_ref {
    uint64_t used[JOB_LIMIT_COUNT];   /* devmgr's job with no driver running */
    uint64_t irqs;                    /* interrupt objects alive before devmgr */
    uint64_t quarantined;             /* the function's quarantined pins before */
};

/* After a refused start: nothing of the driver is left. */
static void check_nothing_left(const struct dm *m, struct pci_dev *d, const struct refusal_ref *ref,
                               uint32_t kind, uint64_t headroom)
{
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(m->job, k) != ref->used[k])
            panic("ktest %s: a start refused at job kind %u + %lu left kind %u at %lu (was %lu)",
                  ktest_current, kind, headroom, k, job_used(m->job, k), ref->used[k]);
    KT_GLOBAL_EQ(interrupt_live_count(), ref->irqs);
    KT_ASSERT(!bme(d));
    KT_ASSERT(!msi_on(d));
    struct dma_quarantine_stats q;
    dma_quarantine_stats(d, &q);
    KT_EQ(q.pins, ref->quarantined);
}

static struct job *lift_job;
static uint32_t lift_kind;

/* DBG_PROCESS_START: the driver's start has paid for everything, its
 * first thread included, and nothing of it has run. Lift the limit now,
 * so the driver doesn't go on to die of it (a crash devmgr would report).
 * Nothing but devmgr's REBIND starts a process while this is installed. */
static void lift_hook(void *arg)
{
    (void)arg;
    if (lift_job)
        (void)job_set_limit(lift_job, lift_kind, JOB_NO_LIMIT);   /* can't fail: a real job */
}

/* Give devmgr's job `headroom` more units of `kind` than it uses without
 * a driver and REBIND, one more unit each round, until a start gets
 * through; every refusal must leave nothing. Returns the rounds refused. */
static unsigned sweep(const struct dm *m, struct pci_dev *d, const struct refusal_ref *ref,
                      uint32_t kind)
{
    struct dm_rep r;
    for (uint64_t h = 0; h < 512; h++) {
        KT_EQ(job_set_limit(m->job, kind, ref->used[kind] + h), OK);
        lift_job = m->job;
        lift_kind = kind;
        dbg_hooks[DBG_PROCESS_START] = lift_hook;
        status_t st = dm_call(m, DM_REBIND, &r, NULL, NULL);
        dbg_hooks[DBG_PROCESS_START] = NULL;
        lift_job = NULL;
        KT_EQ(job_set_limit(m->job, kind, JOB_NO_LIMIT), OK);
        if (st == OK) {
            wait_driver_up(d);   /* and it works */
            return (unsigned)h;
        }
        KT_ASSERT(st == ERR_NO_RESOURCES || st == ERR_NO_MEMORY);
        check_nothing_left(m, d, ref, kind, h);
    }
    panic("ktest %s: job kind %u: 512 more units and still no start", ktest_current, kind);
}

KTEST(devmgr_refused_start_leaves_nothing)
{
    KT_SKIP_LIVE("runs a devmgr of its own; the live one has the devices");
    struct pci_dev *d = edu();
    if (!d)
        return;
    struct refusal_ref ref = { .irqs = interrupt_live_count() };
    struct dma_quarantine_stats q;
    dma_quarantine_stats(d, &q);
    ref.quarantined = q.pins;
    struct dm m;
    dm_start(&m, d);
    wait_driver_up(d);

    /* No thread to spare beyond devmgr's own: the running driver is
     * stopped and its successor can't get its first thread. */
    struct process_info info;
    process_get_info(m.proc, &info);
    struct dm_rep r;
    KT_EQ(job_set_limit(m.job, JOB_LIMIT_THREADS, info.threads), OK);
    status_t st = dm_call(&m, DM_REBIND, &r, NULL, NULL);
    KT_EQ(job_set_limit(m.job, JOB_LIMIT_THREADS, JOB_NO_LIMIT), OK);
    KT_EQ(st, ERR_NO_RESOURCES);
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        ref.used[k] = job_used(m.job, k);   /* devmgr alone */
    KT_EQ(ref.used[JOB_LIMIT_THREADS], info.threads);
    check_nothing_left(&m, d, &ref, JOB_LIMIT_THREADS, 0);

    /* Every point a start can fail at, for handles and for pages. Each
     * sweep ends with a driver running, so stop it first (the thread
     * limit again) to get back to devmgr alone. */
    unsigned hnd = sweep(&m, d, &ref, JOB_LIMIT_HANDLES);
    KT_EQ(job_set_limit(m.job, JOB_LIMIT_THREADS, info.threads), OK);
    KT_EQ(dm_call(&m, DM_REBIND, &r, NULL, NULL), ERR_NO_RESOURCES);
    KT_EQ(job_set_limit(m.job, JOB_LIMIT_THREADS, JOB_NO_LIMIT), OK);
    check_nothing_left(&m, d, &ref, JOB_LIMIT_THREADS, 0);
    unsigned pages = sweep(&m, d, &ref, JOB_LIMIT_PAGES);
    kprintf("ktest %s: starts refused with 0..%u spare handle units and 0..%u spare pages, "
            "none left anything\n", ktest_current, hnd ? hnd - 1 : 0, pages ? pages - 1 : 0);
    KT_ASSERT(hnd > 0 && pages > 0);
    KT_EQ(dm_stop(&m, d), 0);   /* no driver died: refused starts aren't problems */
    dm_job_empty(&m);
    KT_GLOBAL_EQ(interrupt_live_count(), ref.irqs);
}
