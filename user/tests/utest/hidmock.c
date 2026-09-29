/* utest: the mock usb-bus and mock console the hid driver runs against
 * (hid.c has the tests; hidmock.h the model).
 *
 * The mock usb-bus serves abi/idl/usb.idl on the driver's DR_USB channel
 * for a fake device made of recorded descriptors. It records every
 * control request, refuses one addressed to another interface (as usb-bus
 * does), and hands the driver a reports channel on open_interrupt_in
 * whose other end the tests write reports into. The mock console serves
 * abi/idl/input.idl on DR_INPUT and records each event with the time it
 * arrived. mock_pump() serves both on the caller's thread until a
 * condition holds (a port wakes it when either has a request, or the
 * driver dies). */
#define CHECK_PROG "utest"
#define CHECK_CUR  hcur
#include <check.h>
#include <idl/input.h>
#include <idl/usb.h>
#include <os.h>
#include "hidmock.h"

/* ---- recorded devices ------------------------------------------------------ */

/* The boot keyboard report descriptor (HID 1.11 appendix B.1, as QEMU's
 * usb-kbd and most keyboards' boot interfaces have it). */
static const uint8_t rd_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01,
    0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};
/* A boot mouse with a wheel (3 buttons, x, y, wheel). */
static const uint8_t rd_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0,
};
/* A gaming keyboard's second interface: system control (id 2), consumer
 * control (id 3) and a vendor collection (id 6). */
static const uint8_t rd_media[] = {
    0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85, 0x02, 0x19, 0x81, 0x29, 0x83, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x19, 0x00, 0x2a, 0x3c, 0x02, 0x15,
    0x00, 0x26, 0x3c, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xc0,
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x06, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x07, 0x81, 0x02, 0xc0,
};

#define IF(num, cls, sub, proto, ep, maxp, rd) { num, cls, sub, proto, ep, maxp, rd, sizeof(rd) }

static const struct mock_if kbd_ifs[] = { IF(0, 3, 1, 1, 0x81, 8, rd_keyboard) };
static const struct mock_if mouse_ifs[] = { IF(0, 3, 1, 2, 0x81, 4, rd_mouse) };
static const struct mock_if combo_ifs[] = {
    IF(0, 3, 1, 1, 0x81, 8, rd_keyboard),
    IF(1, 3, 0, 0, 0x82, 16, rd_media),
    IF(2, 8, 6, 0x50, 0x83, 64, rd_media),   /* not HID at all (a mass storage one) */
};
const struct mock_dev dev_kbd = { 0x0627, 0x0001, kbd_ifs, 1 };      /* QEMU usb-kbd */
const struct mock_dev dev_mouse = { 0x0627, 0x0001, mouse_ifs, 1 };
const struct mock_dev dev_combo = { 0x258a, 0x0033, combo_ifs, 3 };  /* the PC's */

/* The configuration descriptor of dev: config, then per interface its
 * interface, HID (for class 3) and endpoint descriptors. */
static uint16_t build_config(const struct mock_dev *dev, uint8_t *o)
{
    uint16_t n = 9;
    for (unsigned i = 0; i < dev->nifs; i++) {
        const struct mock_if *f = &dev->ifs[i];
        const uint8_t itf[9] = { 9, 4, f->num, 0, 1, f->cls, f->subclass, f->protocol, 0 };
        memcpy(o + n, itf, 9);
        n += 9;
        if (f->cls == 3) {
            const uint8_t hid[9] = { 9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)f->rlen,
                                     (uint8_t)(f->rlen >> 8) };
            memcpy(o + n, hid, 9);
            n += 9;
        }
        const uint8_t ep[7] = { 7, 5, f->ep, 3, f->maxp, 0, 10 };
        memcpy(o + n, ep, 7);
        n += 7;
    }
    const uint8_t cfg[9] = { 9, 2, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)dev->nifs, 1, 0,
                             0xa0, 50 };
    memcpy(o, cfg, 9);
    return n;
}

/* ---- the mocks --------------------------------------------------------------- */

static status_t m_info(void *ctx, uint16_t *vendor, uint16_t *product, uint8_t *speed,
                       uint8_t *iface, uint8_t *cls, uint8_t *sub, uint8_t *proto, uint8_t *nep,
                       uint8_t *alt, uint8_t *address)
{
    struct mock *m = ctx;
    *vendor = m->dev->vendor;
    *product = m->dev->product;
    *speed = 1;
    *iface = m->itf->num;
    *cls = m->itf->cls;
    *sub = m->itf->subclass;
    *proto = m->itf->protocol;
    *nep = 1;
    *alt = 0;
    *address = 3;
    return OK;
}

static status_t m_get_descriptor(void *ctx, uint8_t type, uint8_t index, uint16_t lang,
                                 uint16_t length, uint8_t recip, uint16_t *actual,
                                 uint8_t data[1024])
{
    struct mock *m = ctx;
    const uint8_t *src = NULL;
    uint16_t n = 0;
    (void)lang;
    if (length > 1024)
        return ERR_INVALID_ARGS;
    if (type == 2 && index == 0 && !recip) {
        src = m->config;
        n = m->config_len;
    } else if (type == 0x22 && recip) {
        src = m->itf->rdesc;
        n = m->itf->rlen;
        m->report_desc_reads++;
    } else {
        return ERR_NOT_SUPPORTED;   /* a STALL */
    }
    *actual = n < length ? n : length;
    memcpy(data, src, *actual);
    return OK;
}

static status_t m_control_in(void *ctx, uint8_t type, uint8_t request, uint16_t value,
                             uint16_t index, uint16_t length, uint16_t *actual,
                             uint8_t data[1024])
{
    (void)ctx, (void)type, (void)request, (void)value, (void)index, (void)length, (void)data;
    *actual = 0;
    return ERR_NOT_SUPPORTED;
}

static status_t m_control_out(void *ctx, uint8_t type, uint8_t request, uint16_t value,
                              uint16_t index, uint16_t length, const uint8_t data[64])
{
    struct mock *m = ctx;
    if ((type & 0x1f) != 1 || index != m->itf->num || length > 64) {
        m->refused++;   /* usb-bus: not this interface */
        return ERR_ACCESS_DENIED;
    }
    if (m->nctl < MAX_CTL)
        m->ctl[m->nctl++] = (struct ctl){ type, request, value, index, length,
                                          length ? data[0] : 0 };
    if (request == 0x0a)
        return ERR_NOT_SUPPORTED;   /* SET_IDLE stalls, as on many real keyboards */
    return OK;
}

static status_t m_open_interrupt_in(void *ctx, uint8_t endpoint, handle_t *out_reports,
                                    uint16_t *max_packet, uint8_t *interval_ms)
{
    struct mock *m = ctx;
    if (endpoint != m->itf->ep || m->reports) {
        m->refused++;
        return ERR_INVALID_ARGS;
    }
    handle_t a, b;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    m->reports = a;
    m->opens++;
    *out_reports = b;
    *max_packet = m->itf->maxp;
    *interval_ms = 10;
    return OK;
}

static status_t m_endpoint_stats(void *ctx, uint8_t endpoint, uint64_t *reports,
                                 uint64_t *dropped, uint64_t *errors, uint8_t *open)
{
    (void)ctx, (void)endpoint;
    *reports = *dropped = *errors = 0;
    *open = 0;
    return OK;
}

static const struct usb_ops mock_usb_ops = {
    .info = m_info,
    .get_descriptor = m_get_descriptor,
    .control_in = m_control_in,
    .control_out = m_control_out,
    .open_interrupt_in = m_open_interrupt_in,
    .endpoint_stats = m_endpoint_stats,
};

static struct ev *record(struct mock *m, uint8_t kind)
{
    if (m->nev == MAX_EV)
        return NULL;
    struct ev *e = &m->ev[m->nev++];
    *e = (struct ev){ .kind = kind, .t = now() };
    return e;
}

static status_t m_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    struct ev *e = record(ctx, EV_KEY);
    if (!e)
        return ERR_NO_RESOURCES;
    e->usage = usage;
    e->state = state;
    e->mods = mods;
    e->cp = cp;
    return OK;
}

static status_t m_mouse(void *ctx, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    struct ev *e = record(ctx, EV_MOUSE);
    if (!e)
        return ERR_NO_RESOURCES;
    e->dx = dx;
    e->dy = dy;
    e->wheel = wheel;
    e->buttons = buttons;
    return OK;
}

static const struct input_ops mock_input_ops = { .key = m_key, .mouse = m_mouse };

static void serve_all(struct mock *m)
{
    status_t st;
    if (m->usb && !m->usb_closed_by_peer) {
        while ((st = usb_serve_one(m->usb, &mock_usb_ops, m)) == OK)
            ;
        if (st == ERR_PEER_CLOSED)
            m->usb_closed_by_peer = true;
    }
    if (m->input && !m->input_closed_by_peer) {
        while ((st = input_serve_one(m->input, &mock_input_ops, m)) == OK)
            ;
        if (st == ERR_PEER_CLOSED)
            m->input_closed_by_peer = true;
    }
}

static bool dead(struct mock *m)
{
    return jam_object_wait_one(m->proc, SIG_TERMINATED, 0, NULL) == OK;
}

/* Serve the driver until done(m, arg) holds (true), the deadline passes or
 * the driver is dead without it (false). */
bool mock_pump(struct mock *m, uint64_t deadline, bool (*done)(struct mock *, unsigned),
                 unsigned arg)
{
    for (;;) {
        serve_all(m);
        if (done && done(m, arg))
            return true;
        if (done && dead(m)) {   /* it may have died just after done() looked */
            serve_all(m);
            return done(m, arg);
        }
        if (now() >= deadline)
            return false;
        struct port_packet p;
        jam_port_wait(m->port, deadline, &p);
    }
}

void mock_pump_for(struct mock *m, uint64_t ns)
{
    mock_pump(m, now() + ns, NULL, 0);
}

bool mock_have_events(struct mock *m, unsigned n)
{
    return m->nev >= n;
}

static bool is_dead(struct mock *m, unsigned unused)
{
    (void)unused;
    return dead(m);
}

/* Ready: the endpoint is open, and a keyboard has set its LEDs. */
static bool ready(struct mock *m, unsigned want_leds)
{
    if (!m->reports)
        return false;
    if (!want_leds)
        return true;
    for (unsigned i = 0; i < m->nctl; i++)
        if (m->ctl[i].request == 0x09)
            return true;
    return false;
}

/* Start drv/hid on interface `ifn` of dev, with a console unless
 * !with_console, and wait until it serves reports (unless !wait_ready). */
bool mock_start(struct mock *m, const struct mock_dev *dev, unsigned ifn, bool with_console,
                  bool wait_ready)
{
    *m = (struct mock){ .dev = dev, .itf = &dev->ifs[ifn] };
    m->config_len = build_config(dev, m->config);
    handle_t usb_drv, input_drv = 0;
    CHECK_ST(jam_job_create(startup_handle(SR_JOB), 0, &m->job), OK);
    CHECK_ST(jam_channel_create(&m->usb, &usb_drv), OK);
    if (with_console)
        CHECK_ST(jam_channel_create(&m->input, &input_drv), OK);
    CHECK_ST(jam_port_create(&m->port), OK);
    CHECK_ST(jam_port_bind(m->port, m->usb, KEY_USB, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT), OK);
    if (with_console)
        CHECK_ST(jam_port_bind(m->port, m->input, KEY_INPUT, SIG_READABLE | SIG_PEER_CLOSED,
                               PORT_BIND_PERSISTENT), OK);
    const char *argv[] = { "drv/hid" };
    struct spawn_handle x[2] = { { SR_DRIVER(DR_USB), usb_drv },
                                 { SR_DRIVER(DR_INPUT), input_drv } };
    struct spawn_args a = {
        .path = "drv/hid", .argc = 1, .argv = argv, .job = m->job, .extra = x,
        .nextra = with_console ? 2 : 1,
    };
    CHECK_ST(spawn(&a, &m->proc), OK);
    CHECK_ST(jam_port_bind(m->port, m->proc, KEY_PROC, SIG_TERMINATED, PORT_BIND_ONCE), OK);
    if (wait_ready && !mock_pump(m, now() + 20 * NS_PER_S, ready, m->itf->protocol == 1))
        FAIL("the driver never opened its endpoint (%u control requests, dead %d)", m->nctl,
             dead(m));
    return true;
}

/* Close what we still hold of the unplugged device: DR_USB and/or reports. */
void mock_unplug(struct mock *m, bool usb, bool reports)
{
    if (usb && m->usb) {
        jam_port_unbind(m->port, m->usb, KEY_USB);
        jam_handle_close(m->usb);
        m->usb = 0;
    }
    if (reports && m->reports) {
        jam_handle_close(m->reports);
        m->reports = 0;
    }
}

static bool peer_closed(handle_t h)
{
    return !h || jam_object_wait_one(h, SIG_PEER_CLOSED, 0, NULL) == OK;
}

/* The driver must end by itself with exit code `code`; every channel end
 * we still hold sees it gone; then its job is empty. */
bool mock_finish(struct mock *m, int code)
{
    if (!mock_pump(m, now() + 20 * NS_PER_S, is_dead, 0))
        FAIL("the driver is still running");
    struct process_info info;
    CHECK_ST(spawn_wait(m->proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, code);
    serve_all(m);
    CHECK(peer_closed(m->usb));
    CHECK(peer_closed(m->input));
    CHECK(peer_closed(m->reports));
    mock_unplug(m, true, true);
    if (m->input)
        CHECK_ST(jam_handle_close(m->input), OK);
    CHECK_ST(jam_handle_close(m->port), OK);
    CHECK_ST(jam_handle_close(m->proc), OK);
    struct job_info ji;
    CHECK_ST(jam_job_get_info(m->job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("the driver's job still has %lu units of kind %u", (unsigned long)ji.used[k],
                 k);
    CHECK_ST(jam_handle_close(m->job), OK);
    return true;
}
