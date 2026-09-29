/* The hid driver (drivers/hid, M7 Track B) in the kernel build: a kernel
 * process driving a mock boot keyboard served from here with the
 * generated usb_dispatch, reporting to a mock console read here. One key
 * press and release must come out as two input.key calls; DR_USB closing
 * (the unplug) must end it with 0; its job ends empty and no channel is
 * left behind. The full behaviour is tested on the process build in
 * utest (user/utest/hid.c); this proves the kernel build runs too. */
#include <jam/channel.h>
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/ktest.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <idl/input.h>
#include <idl/usb.h>

#define S         1000000000ull
#define CH_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL)

/* A boot keyboard's configuration: config, interface 0 (3/1/1), HID
 * descriptor (no report descriptor: the driver skips reading it),
 * endpoint 0x81. */
static const uint8_t kb_config[] = {
    9, 2, 34, 0, 1, 1, 0, 0xa0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0, 0,
    7, 5, 0x81, 3, 8, 0, 10,
};

struct kmock {
    struct channel *reports_ours, *reports_theirs;
    unsigned ctl_out;
};

static status_t k_info(void *ctx, uint16_t *vendor, uint16_t *product, uint8_t *speed,
                       uint8_t *iface, uint8_t *cls, uint8_t *sub, uint8_t *proto, uint8_t *nep,
                       uint8_t *alt, uint8_t *address)
{
    (void)ctx;
    *vendor = 0x0627;
    *product = 0x0001;
    *speed = 1;
    *iface = 0;
    *cls = 3;
    *sub = 1;
    *proto = 1;
    *nep = 1;
    *alt = 0;
    *address = 1;
    return OK;
}

static status_t k_get_descriptor(void *ctx, uint8_t type, uint8_t index, uint16_t lang,
                                 uint16_t length, uint8_t recip, uint16_t *actual,
                                 uint8_t data[1024])
{
    (void)ctx, (void)lang;
    if (type != 2 || index || recip)
        return ERR_NOT_SUPPORTED;
    uint16_t n = length < sizeof(kb_config) ? length : sizeof(kb_config);
    for (uint16_t i = 0; i < n; i++)
        data[i] = kb_config[i];
    *actual = n;
    return OK;
}

static status_t k_control_out(void *ctx, uint8_t type, uint8_t request, uint16_t value,
                              uint16_t index, uint16_t length, const uint8_t data[64])
{
    (void)type, (void)request, (void)value, (void)length, (void)data;
    ((struct kmock *)ctx)->ctl_out++;
    return index == 0 ? OK : ERR_ACCESS_DENIED;
}

#define REPORTS_PLACEHOLDER 0x7eu   /* swapped for the real endpoint when the reply goes */

static status_t k_open_interrupt_in(void *ctx, uint8_t endpoint, handle_t *out_reports,
                                    uint16_t *max_packet, uint8_t *interval_ms)
{
    struct kmock *m = ctx;
    if (endpoint != 0x81 || m->reports_ours)
        return ERR_INVALID_ARGS;
    status_t st = channel_create(&m->reports_ours, &m->reports_theirs);
    if (st != OK)
        return st;
    *out_reports = REPORTS_PLACEHOLDER;
    *max_packet = 8;
    *interval_ms = 10;
    return OK;
}

static const struct usb_ops kops = {
    .info = k_info,
    .get_descriptor = k_get_descriptor,
    .control_out = k_control_out,
    .open_interrupt_in = k_open_interrupt_in,
};

/* One usb request, if any: dispatched, answered. */
static bool serve_usb(struct channel *usb, struct kmock *m)
{
    uint8_t q[USB_REQ_MAX], r[USB_REP_MAX];
    uint32_t n = 0, nh = 0;
    if (channel_read(usb, q, sizeof(q), &n, NULL, 0, &nh) != OK)
        return false;
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = usb_dispatch(&kops, m, q, n, r, rhs, &rhn);
    struct khandle kh[1];
    uint32_t nk = 0;
    if (rhn == 1 && rhs[0] == REPORTS_PLACEHOLDER && m->reports_theirs) {
        kh[0] = khandle_from_new((struct kobject *)m->reports_theirs, CH_RIGHTS);
        m->reports_theirs = NULL;
        nk = 1;
    }
    KT_EQ(channel_write(usb, r, rn, kh, nk), OK);
    return true;
}

struct kev {
    uint16_t usage;
    uint8_t  state, mods;
    uint32_t cp;
};

static bool serve_input(struct channel *input, struct kev *ev, unsigned *nev)
{
    struct input_key_req q;
    uint32_t n = 0, nh = 0;
    if (channel_read(input, &q, sizeof(q), &n, NULL, 0, &nh) != OK)
        return false;
    KT_EQ(n, sizeof(q));
    KT_EQ(q.ordinal, INPUT_KEY);
    if (*nev < 4)
        ev[(*nev)++] = (struct kev){ q.usage, q.state, q.mods, q.codepoint };
    struct idl_rep_hdr r = { q.txid, OK };
    KT_EQ(channel_write(input, &r, sizeof(r), NULL, 0), OK);
    return true;
}

static struct job *fresh_job(void)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    job_unref(root);
    return j;
}

KTEST(hid_kernel_process_keyboard)
{
    driver_main_fn fn = driver_kernel_find("hid");
    KT_ASSERT(fn != NULL);
    struct job *j = fresh_job();
    uint64_t chans = channel_live_count();
    struct channel *usb, *usb_d, *input, *input_d;
    KT_EQ(channel_create(&usb, &usb_d), OK);
    KT_EQ(channel_create(&input, &input_d), OK);
    struct driver_kernel_handle hs[2] = {
        { DR_USB, khandle_from_new((struct kobject *)usb_d, CH_RIGHTS) },
        { DR_INPUT, khandle_from_new((struct kobject *)input_d, CH_RIGHTS) },
    };
    struct process *p;
    KT_EQ(driver_kernel_start("hid", fn, hs, 2, j, &p), OK);

    struct kmock m = { 0 };
    struct kev ev[4];
    unsigned nev = 0;
    bool typed = false;
    uint64_t deadline = uptime_ns() + 20 * S;
    while (nev < 2 && uptime_ns() < deadline) {
        bool busy = serve_usb(usb, &m);
        busy |= serve_input(input, ev, &nev);
        if (!typed && m.reports_ours && m.ctl_out >= 3) {   /* protocol, idle, LEDs */
            const uint8_t down[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 }, up[8] = { 0 };
            KT_EQ(channel_write(m.reports_ours, down, 8, NULL, 0), OK);
            KT_EQ(channel_write(m.reports_ours, up, 8, NULL, 0), OK);
            typed = true;
        }
        if (!busy)
            thread_sleep_ms(1);
    }
    KT_EQ(nev, 2);
    KT_ASSERT(ev[0].usage == 0x04 && ev[0].state == INPUT_KEY_DOWN && ev[0].cp == 'a');
    KT_ASSERT(ev[1].usage == 0x04 && ev[1].state == INPUT_KEY_UP && ev[1].cp == 'a');

    kobject_unref((struct kobject *)usb);   /* unplugged */
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 20 * S, NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.state, PROCESS_DEAD);
    KT_EQ(info.killed, 0);
    KT_EQ(info.exit_code, 0);
    kobject_unref(process_kobject(p));
    KT_EQ(object_wait_one((struct kobject *)input, SIG_PEER_CLOSED, uptime_ns() + S, NULL), OK);
    KT_EQ(object_wait_one((struct kobject *)m.reports_ours, SIG_PEER_CLOSED, uptime_ns() + S,
                          NULL), OK);
    kobject_unref((struct kobject *)input);
    kobject_unref((struct kobject *)m.reports_ours);
    for (uint32_t k = 1; k < JOB_LIMIT_COUNT; k++)
        if (job_used(j, k))
            panic("ktest %s: job kind %u still has %lu units", ktest_current, k, job_used(j, k));
    KT_EQ(channel_live_count(), chans);
    job_unref(j);
}
