/* utest: the HID driver (drivers/hid, M7 Track B) as a process, against a
 * mock usb-bus and a mock console, both served from here.
 *
 * The mock usb-bus serves abi/idl/usb.idl on the driver's DR_USB channel
 * for a fake device made of recorded descriptors (a QEMU-style boot
 * keyboard, a boot mouse with a wheel, and a composite keyboard like the
 * PC's Sino Wealth 258a:0033: a boot keyboard interface plus a
 * report-protocol one with consumer and system control collections). It
 * records every control request, refuses one addressed to another
 * interface (as usb-bus does), and hands the driver a reports channel on
 * open_interrupt_in whose other end the tests write reports into. The mock
 * console serves abi/idl/input.idl on DR_INPUT and records each event with
 * the time it arrived.
 *
 * Everything runs on this one thread: pump() serves both channels until a
 * condition holds (a port wakes it when either has a request, or the
 * driver dies). Each test starts the driver in a job of its own and ends
 * with the driver exited 0 by itself, every one of our channel ends seeing
 * PEER_CLOSED (the driver closed everything), and its job empty. */
#include <os.h>
#include <idl/input.h>
#include <idl/usb.h>
#include "utest.h"


static const char *hcur;

#define FAIL(...)                                                   \
    do {                                                            \
        printf("utest: %s: FAILED at line %d: ", hcur, __LINE__);   \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
        return false;                                               \
    } while (0)
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c))                                                   \
            FAIL("%s", #c);                                         \
    } while (0)
#define CHECK_ST(expr, want)                                        \
    do {                                                            \
        status_t _s = (expr), _w = (want);                          \
        if (_s != _w)                                               \
            FAIL("%s is %s, want %s", #expr, status_str(_s), status_str(_w)); \
    } while (0)
#define CHECK_EQ(a, b)                                              \
    do {                                                            \
        int64_t _a = (int64_t)(a), _b = (int64_t)(b);               \
        if (_a != _b)                                               \
            FAIL("%s == %s: %ld vs %ld", #a, #b, (long)_a, (long)_b); \
    } while (0)

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

struct mock_if {
    uint8_t        num, cls, subclass, protocol;
    uint8_t        ep;         /* interrupt IN address */
    uint8_t        maxp;
    const uint8_t *rdesc;
    uint16_t       rlen;
};

struct mock_dev {
    uint16_t              vendor, product;
    const struct mock_if *ifs;
    unsigned              nifs;
};

#define IF(num, cls, sub, proto, ep, maxp, rd) { num, cls, sub, proto, ep, maxp, rd, sizeof(rd) }

static const struct mock_if kbd_ifs[] = { IF(0, 3, 1, 1, 0x81, 8, rd_keyboard) };
static const struct mock_if mouse_ifs[] = { IF(0, 3, 1, 2, 0x81, 4, rd_mouse) };
static const struct mock_if combo_ifs[] = {
    IF(0, 3, 1, 1, 0x81, 8, rd_keyboard),
    IF(1, 3, 0, 0, 0x82, 16, rd_media),
    IF(2, 8, 6, 0x50, 0x83, 64, rd_media),   /* not HID at all (a mass storage one) */
};
static const struct mock_dev dev_kbd = { 0x0627, 0x0001, kbd_ifs, 1 };      /* QEMU usb-kbd */
static const struct mock_dev dev_mouse = { 0x0627, 0x0001, mouse_ifs, 1 };
static const struct mock_dev dev_combo = { 0x258a, 0x0033, combo_ifs, 3 };  /* the PC's */

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

struct ctl {
    uint8_t  type, request;
    uint16_t value, index, length;
    uint8_t  data0;
};

#define EV_KEY   1
#define EV_MOUSE 2

struct ev {
    uint8_t  kind;
    uint8_t  state, mods, buttons;
    uint16_t usage;
    int16_t  dx, dy;
    int8_t   wheel;
    uint32_t cp;
    uint64_t t;
};

#define MAX_CTL 32
#define MAX_EV  512

struct mock {
    const struct mock_dev *dev;
    const struct mock_if  *itf;
    uint8_t   config[256];
    uint16_t  config_len;
    handle_t  job, proc, port;
    handle_t  usb, input;       /* our ends: we serve usb and input */
    handle_t  reports;          /* usb-bus's end of the reports channel */
    bool      usb_closed_by_peer, input_closed_by_peer;
    unsigned  report_desc_reads, refused, opens;
    struct ctl ctl[MAX_CTL];
    unsigned  nctl;
    struct ev ev[MAX_EV];
    unsigned  nev;
};

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

#define KEY_USB   1
#define KEY_INPUT 2
#define KEY_PROC  3

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
static bool pump(struct mock *m, uint64_t deadline, bool (*done)(struct mock *, unsigned),
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

static void pump_for(struct mock *m, uint64_t ns)
{
    pump(m, now() + ns, NULL, 0);
}

static bool have_events(struct mock *m, unsigned n)
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
static bool start(struct mock *m, const struct mock_dev *dev, unsigned ifn, bool with_console,
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
    if (wait_ready && !pump(m, now() + 20 * NS_PER_S, ready, m->itf->protocol == 1))
        FAIL("the driver never opened its endpoint (%u control requests, dead %d)", m->nctl,
             dead(m));
    return true;
}

/* Close what we still hold of the unplugged device: DR_USB and/or reports. */
static void unplug(struct mock *m, bool usb, bool reports)
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
static bool finish(struct mock *m, int code)
{
    if (!pump(m, now() + 20 * NS_PER_S, is_dead, 0))
        FAIL("the driver is still running");
    struct process_info info;
    CHECK_ST(spawn_wait(m->proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, code);
    serve_all(m);
    CHECK(peer_closed(m->usb));
    CHECK(peer_closed(m->input));
    CHECK(peer_closed(m->reports));
    unplug(m, true, true);
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

/* ---- typing ------------------------------------------------------------------ */

#define LSHIFT 0x02
#define LCTRL  0x01
#define LALT   0x04

static bool report(struct mock *m, const uint8_t *r, uint32_t n)
{
    CHECK_ST(jam_channel_write(m->reports, r, n, NULL, 0), OK);
    return true;
}

static bool keys(struct mock *m, uint8_t mods, uint8_t k0, uint8_t k1)
{
    const uint8_t r[8] = { mods, 0, k0, k1, 0, 0, 0, 0 };
    return report(m, r, 8);
}

/* The usage and Shift for a character of the US layout (0: none). */
static uint8_t usage_of(char c, bool *shift)
{
    static const char plain[] = "1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static const char shifted[] = "!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    *shift = false;
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(0x04 + c - 'a');
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        return (uint8_t)(0x04 + c - 'A');
    }
    for (unsigned i = 0; plain[i]; i++) {
        if (i == 20)
            continue;   /* 0x32 (non-US #): the same key as 0x31 in the US layout */
        if (plain[i] == c)
            return (uint8_t)(0x1e + i);
        if (shifted[i] == c) {
            *shift = true;
            return (uint8_t)(0x1e + i);
        }
    }
    return 0;
}

/* Wanted key events, in order. */
struct want {
    uint16_t usage;
    uint8_t  state, mods;
    uint32_t cp;
};

static unsigned count_not_repeat(struct mock *m, unsigned from)
{
    unsigned n = 0;
    for (unsigned i = from; i < m->nev; i++)
        n += m->ev[i].state != INPUT_KEY_REPEAT || m->ev[i].kind != EV_KEY;
    return n;
}

static bool have_non_repeats(struct mock *m, unsigned arg)
{
    return count_not_repeat(m, arg >> 16) >= (arg & 0xffff);
}

/* The events from *at on, REPEATs left out (the repeat test checks those;
 * elsewhere a slow run may add some), must be w[0..n) exactly; with
 * `exact`, nothing else may follow for 30 ms. *at moves past them. */
static bool expect_x(struct mock *m, unsigned *at, const struct want *w, unsigned n, bool exact)
{
    if (!pump(m, now() + 10 * NS_PER_S, have_non_repeats, *at << 16 | n))
        FAIL("%u event(s), want %u", count_not_repeat(m, *at), n);
    unsigned i = *at;
    for (unsigned k = 0; k < n; k++, i++) {
        while (m->ev[i].kind == EV_KEY && m->ev[i].state == INPUT_KEY_REPEAT)
            i++;
        const struct ev *e = &m->ev[i];
        if (e->kind != EV_KEY || e->usage != w[k].usage || e->state != w[k].state ||
            e->mods != w[k].mods || e->cp != w[k].cp)
            FAIL("event %u: kind %u usage %#x state %u mods %#x cp %#x, want usage %#x state %u "
                 "mods %#x cp %#x", k, e->kind, e->usage, e->state, e->mods, e->cp, w[k].usage,
                 w[k].state, w[k].mods, w[k].cp);
    }
    *at = i;
    if (exact) {
        pump_for(m, 30 * NS_PER_MS);
        if (count_not_repeat(m, i))
            FAIL("%u event(s) more than wanted", count_not_repeat(m, i));
    }
    return true;
}

static bool expect(struct mock *m, unsigned *at, const struct want *w, unsigned n)
{
    return expect_x(m, at, w, n, true);
}

/* The LED bytes the driver sent, in order, into out; how many. */
static unsigned leds(struct mock *m, uint8_t *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; i < m->nctl && n < cap; i++)
        if (m->ctl[i].type == 0x21 && m->ctl[i].request == 0x09 && m->ctl[i].value == 0x0200 &&
            m->ctl[i].length == 1)
            out[n++] = m->ctl[i].data0;
    return n;
}

#define D INPUT_KEY_DOWN
#define U INPUT_KEY_UP
#define R INPUT_KEY_REPEAT

/* "Hello, World!\n" typed on a boot keyboard: the start-up requests, then
 * the exact events of 'H' and the text of every DOWN. */
bool t_hid_typing(void)
{
    hcur = "hid_typing";
    static struct mock m;
    if (!start(&m, &dev_kbd, 0, true, true))
        return false;
    /* SET_PROTOCOL(boot), SET_IDLE(0) (refused: it goes on), LEDs Num Lock. */
    CHECK(m.nctl >= 3);
    CHECK(m.ctl[0].type == 0x21 && m.ctl[0].request == 0x0b && m.ctl[0].value == 0);
    CHECK(m.ctl[1].type == 0x21 && m.ctl[1].request == 0x0a && m.ctl[1].value == 0);
    uint8_t l[8];
    CHECK_EQ(leds(&m, l, 8), 1);
    CHECK_EQ(l[0], 0x01);
    CHECK_EQ(m.report_desc_reads, 1);
    CHECK_EQ(m.refused, 0);
    CHECK_EQ(m.opens, 1);

    const char *text = "Hello, World!\n";
    unsigned nwant = 0;
    for (const char *p = text; *p; p++) {
        bool shift;
        uint8_t u = usage_of(*p, &shift);
        CHECK(u != 0);
        if (!keys(&m, shift ? LSHIFT : 0, u, 0) || !keys(&m, 0, 0, 0))
            return false;
        nwant += shift ? 4 : 2;
    }
    const struct want h[] = {
        { 0xe1, D, LSHIFT, 0 }, { 0x0b, D, LSHIFT, 'H' }, { 0x0b, U, LSHIFT, 'H' },
        { 0xe1, U, 0, 0 }, { 0x08, D, 0, 'e' }, { 0x08, U, 0, 'e' },
    };
    unsigned at = 0;
    if (!expect_x(&m, &at, h, 6, false))
        return false;
    if (!pump(&m, now() + 10 * NS_PER_S, have_events, nwant))
        FAIL("%u events, want %u", m.nev, nwant);
    pump_for(&m, 30 * NS_PER_MS);
    CHECK_EQ(m.nev, nwant);
    char got[32];
    unsigned n = 0, downs = 0, ups = 0;
    for (unsigned i = 0; i < m.nev; i++) {
        CHECK(m.ev[i].state != R);   /* typed fast: nothing repeats */
        if (m.ev[i].state == D) {
            downs++;
            if (m.ev[i].cp && n < sizeof(got) - 1)
                got[n++] = (char)m.ev[i].cp;
        } else {
            ups++;
        }
    }
    got[n] = 0;
    if (strcmp(got, text) != 0)
        FAIL("typed \"%s\"", got);
    CHECK_EQ(downs, ups);
    unplug(&m, true, true);
    return finish(&m, 0);
}

/* Shift and Caps Lock (with its LED), Num Lock and the keypad, Ctrl and
 * Alt leaving the codepoint alone, Ctrl+Alt+Delete as a plain key. */
bool t_hid_modifiers(void)
{
    hcur = "hid_modifiers";
    static struct mock m;
    if (!start(&m, &dev_kbd, 0, true, true))
        return false;
    unsigned at = 0;
#define STEP(mods, k0, k1) \
    do { if (!keys(&m, mods, k0, k1)) return false; } while (0)
#define EXPECT(...)                                                     \
    do {                                                                \
        const struct want w_[] = { __VA_ARGS__ };                       \
        if (!expect(&m, &at, w_, sizeof(w_) / sizeof(w_[0])))           \
            return false;                                               \
    } while (0)

    STEP(0, 0x39, 0);                       /* Caps Lock */
    STEP(0, 0, 0);
    EXPECT({ 0x39, D, 0, 0 }, { 0x39, U, 0, 0 });
    STEP(0, 0x04, 0);                       /* a -> A */
    STEP(0, 0, 0);
    STEP(LSHIFT, 0x04, 0);                  /* Shift+a -> a */
    STEP(0, 0, 0);
    STEP(0, 0x1e, 0);                       /* 1 stays 1 */
    STEP(LSHIFT, 0x1e, 0);                  /* Shift while 1 is held: nothing new for 1 */
    STEP(LSHIFT, 0, 0);
    STEP(0, 0, 0);
    EXPECT({ 0x04, D, 0, 'A' }, { 0x04, U, 0, 'A' },
           { 0xe1, D, LSHIFT, 0 }, { 0x04, D, LSHIFT, 'a' }, { 0x04, U, LSHIFT, 'a' },
           { 0xe1, U, 0, 0 },
           { 0x1e, D, 0, '1' }, { 0xe1, D, LSHIFT, 0 }, { 0x1e, U, LSHIFT, '1' },
           { 0xe1, U, 0, 0 });
    STEP(0, 0x39, 0);                       /* Caps Lock off */
    STEP(0, 0, 0);
    STEP(0x20, 0x2f, 0);                    /* right Shift + [ -> { */
    STEP(0, 0, 0);
    STEP(LSHIFT, 0x34, 0);                  /* Shift + ' -> " */
    STEP(0, 0, 0);
    EXPECT({ 0x39, D, 0, 0 }, { 0x39, U, 0, 0 },
           { 0xe5, D, 0x20, 0 }, { 0x2f, D, 0x20, '{' }, { 0x2f, U, 0x20, '{' },
           { 0xe5, U, 0, 0 },
           { 0xe1, D, LSHIFT, 0 }, { 0x34, D, LSHIFT, '"' }, { 0x34, U, LSHIFT, '"' },
           { 0xe1, U, 0, 0 });
    uint8_t l[8];
    CHECK_EQ(leds(&m, l, 8), 3);            /* start, Caps on, Caps off */
    CHECK_EQ(l[1], 0x03);
    CHECK_EQ(l[2], 0x01);

    /* Keypad: Num Lock is on at start; off, KP8 is Up. */
    STEP(0, 0x60, 0);
    STEP(0, 0, 0);
    STEP(0, 0x57, 0);                       /* KP + */
    STEP(0, 0, 0);
    STEP(0, 0x53, 0);                       /* Num Lock off */
    STEP(0, 0, 0);
    STEP(0, 0x60, 0);
    STEP(0, 0, 0);
    STEP(0, 0x5d, 0);                       /* KP5: no key printed on it */
    STEP(0, 0, 0);
    EXPECT({ 0x60, D, 0, '8' }, { 0x60, U, 0, '8' }, { 0x57, D, 0, '+' }, { 0x57, U, 0, '+' },
           { 0x53, D, 0, 0 }, { 0x53, U, 0, 0 },
           { 0x52, D, 0, 0 }, { 0x52, U, 0, 0 }, { 0x5d, D, 0, 0 }, { 0x5d, U, 0, 0 });
    CHECK_EQ(leds(&m, l, 8), 4);
    CHECK_EQ(l[3], 0x00);
    STEP(0, 0x53, 0);                       /* Num Lock back on */
    STEP(0, 0, 0);
    EXPECT({ 0x53, D, 0, 0 }, { 0x53, U, 0, 0 });

    /* Ctrl+C keeps 'c'; Ctrl+Alt+Delete is a key like any other. */
    STEP(LCTRL, 0x06, 0);
    STEP(0, 0, 0);
    STEP(LCTRL | LALT, 0x4c, 0);
    STEP(0, 0, 0);
    STEP(0, 0x28, 0x2a);                    /* Enter and Backspace together */
    STEP(0, 0, 0);
    STEP(0, 0x3a, 0x29);                    /* F1, Escape */
    STEP(0, 0, 0);
    EXPECT({ 0xe0, D, LCTRL, 0 }, { 0x06, D, LCTRL, 'c' }, { 0x06, U, LCTRL, 'c' },
           { 0xe0, U, 0, 0 },
           { 0xe0, D, LCTRL, 0 }, { 0xe2, D, LCTRL | LALT, 0 },
           { 0x4c, D, LCTRL | LALT, 0 }, { 0x4c, U, LCTRL | LALT, 0 },
           { 0xe0, U, LALT, 0 }, { 0xe2, U, 0, 0 },
           { 0x28, D, 0, '\n' }, { 0x2a, D, 0, '\b' }, { 0x2a, U, 0, '\b' },
           { 0x28, U, 0, '\n' },
           { 0x3a, D, 0, 0 }, { 0x29, D, 0, 0x1b }, { 0x29, U, 0, 0x1b }, { 0x3a, U, 0, 0 });
    /* One report letting go of Shift and pressing a key: the key is
     * unshifted; one pressing Shift and a key: shifted. */
    STEP(LSHIFT, 0, 0);
    STEP(0, 0x04, 0);
    STEP(LSHIFT, 0x04, 0x05);
    STEP(0, 0, 0);
    EXPECT({ 0xe1, D, LSHIFT, 0 }, { 0xe1, U, 0, 0 }, { 0x04, D, 0, 'a' },
           { 0xe1, D, LSHIFT, 0 }, { 0x05, D, LSHIFT, 'B' },
           { 0x05, U, LSHIFT, 'B' }, { 0x04, U, LSHIFT, 'a' }, { 0xe1, U, 0, 0 });
    CHECK_EQ(m.refused, 0);
    unplug(&m, true, true);
    return finish(&m, 0);
}

/* Phantom state (ErrorRollOver in every slot) is ignored whole, and keys
 * held across it are neither released nor pressed again. */
bool t_hid_rollover(void)
{
    hcur = "hid_rollover";
    static struct mock m;
    if (!start(&m, &dev_kbd, 0, true, true))
        return false;
    unsigned at = 0;
    STEP(0, 0x04, 0);
    EXPECT({ 0x04, D, 0, 'a' });
    const uint8_t phantom[8] = { LSHIFT, 0, 1, 1, 1, 1, 1, 1 };
    if (!report(&m, phantom, 8))
        return false;
    const uint8_t undefined[8] = { 0, 0, 0x04, 0x03, 0, 0, 0, 0 };
    if (!report(&m, undefined, 8))
        return false;
    pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(count_not_repeat(&m, at), 0);  /* nothing: not even the Shift */
    STEP(0, 0x04, 0x05);
    STEP(0, 0, 0);
    EXPECT({ 0x05, D, 0, 'b' }, { 0x05, U, 0, 'b' }, { 0x04, U, 0, 'a' });
    /* Six keys at once, then a report with a key repeated in two slots. */
    const uint8_t six[8] = { 0, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    if (!report(&m, six, 8))
        return false;
    const uint8_t dup[8] = { 0, 0, 0x0a, 0x0a, 0, 0, 0, 0 };
    if (!report(&m, dup, 8))
        return false;
    STEP(0, 0, 0);
    EXPECT({ 0x04, D, 0, 'a' }, { 0x05, D, 0, 'b' }, { 0x06, D, 0, 'c' }, { 0x07, D, 0, 'd' },
           { 0x08, D, 0, 'e' }, { 0x09, D, 0, 'f' },
           { 0x09, U, 0, 'f' }, { 0x08, U, 0, 'e' }, { 0x07, U, 0, 'd' }, { 0x06, U, 0, 'c' },
           { 0x05, U, 0, 'b' }, { 0x04, U, 0, 'a' }, { 0x0a, D, 0, 'g' }, { 0x0a, U, 0, 'g' });
    /* Too short to be a report: ignored. */
    const uint8_t runt[2] = { LSHIFT, 0 };
    if (!report(&m, runt, 2))
        return false;
    pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(count_not_repeat(&m, at), 0);
    unplug(&m, true, true);
    return finish(&m, 0);
}

/* Key repeat: the first REPEAT 500 ms after the DOWN, then ~30 a second;
 * Shift pressed meanwhile changes what repeats; nothing after the UP. */
bool t_hid_repeat(void)
{
    hcur = "hid_repeat";
    static struct mock m;
    if (!start(&m, &dev_kbd, 0, true, true))
        return false;
    if (!keys(&m, 0, 0x1b, 0))              /* x */
        return false;
    if (!pump(&m, now() + 5 * NS_PER_S, have_events, 1))
        FAIL("no DOWN");
    uint64_t t_down = m.ev[0].t;
    pump(&m, t_down + 900 * NS_PER_MS, NULL, 0);
    if (!keys(&m, LSHIFT, 0x1b, 0))
        return false;
    pump(&m, t_down + 1400 * NS_PER_MS, NULL, 0);
    if (!keys(&m, 0, 0, 0))
        return false;
    pump_for(&m, 300 * NS_PER_MS);

    unsigned reps = 0, shifted = 0, up_at = 0, first_rep = 0, shift_at = 0;
    for (unsigned i = 1; i < m.nev; i++) {
        const struct ev *e = &m.ev[i];
        CHECK(e->kind == EV_KEY);
        if (e->usage == 0xe1 && e->state == D)
            shift_at = i;
        if (e->usage == 0x1b && e->state == U)
            up_at = i;
        if (e->state != R)
            continue;
        CHECK(!up_at);                      /* nothing after the UP */
        CHECK_EQ(e->usage, 0x1b);
        if (!reps++)
            first_rep = i;
        if (shift_at) {
            CHECK_EQ(e->cp, 'X');
            CHECK_EQ(e->mods, LSHIFT);
            shifted++;
        } else {
            CHECK_EQ(e->cp, 'x');
            CHECK_EQ(e->mods, 0);
        }
    }
    CHECK(up_at && shift_at);
    CHECK(first_rep);
    uint64_t delay = m.ev[first_rep].t - t_down;
    uint64_t held = m.ev[up_at].t - t_down;
    printf("utest: %s: first repeat after %lu ms, %u repeats in %lu ms held (%u shifted)\n",
           hcur, (unsigned long)(delay / NS_PER_MS), reps, (unsigned long)(held / NS_PER_MS),
           shifted);
    if (delay < 480 * NS_PER_MS || delay > 1200 * NS_PER_MS)
        FAIL("the first repeat came %lu ms after the DOWN", (unsigned long)(delay / NS_PER_MS));
    /* Held ~1.4 s: ~27 at 30 Hz. A slow QEMU may lose some; a burst of
     * catch-up repeats after a late wakeup would show up as too many. */
    unsigned most = (unsigned)((held + 50 * NS_PER_MS - 500 * NS_PER_MS) / (NS_PER_S / 30)) + 2;
    if (reps < 8 || reps > most)
        FAIL("%u repeats (at most %u)", reps, most);
    CHECK(shifted >= 2);
    unplug(&m, true, true);
    return finish(&m, 0);
}

/* A boot mouse: buttons, deltas, the wheel; unchanged reports send nothing;
 * the reports channel closing alone ends it with 5 (restart me);
 * a 3-byte report has no wheel. */
bool t_hid_mouse(void)
{
    hcur = "hid_mouse";
    static struct mock m;
    if (!start(&m, &dev_mouse, 0, true, true))
        return false;
    for (unsigned i = 0; i < m.nctl; i++)
        CHECK(m.ctl[i].request != 0x0a && m.ctl[i].request != 0x09);   /* no SET_IDLE, LEDs */
    CHECK(m.nctl == 1 && m.ctl[0].request == 0x0b && m.ctl[0].value == 0);
    const uint8_t r1[4] = { 0x01, 5, (uint8_t)-3, 0 };
    const uint8_t r2[4] = { 0x01, 0, 0, 0 };            /* nothing changed: no event */
    const uint8_t r3[4] = { 0x00, 0, 0, 0 };
    const uint8_t r4[4] = { 0x00, 0, 0, (uint8_t)-1 };
    const uint8_t r5[3] = { 0x06, (uint8_t)-128, 127 }; /* no wheel byte */
    const uint8_t r6[8] = { 0x04, 1, 1, 2, 0x55, 0x55, 0x55, 0x55 };   /* padded */
    if (!report(&m, r1, 4) || !report(&m, r2, 4) || !report(&m, r3, 4) || !report(&m, r4, 4) ||
        !report(&m, r5, 3) || !report(&m, r6, 8))
        return false;
    if (!pump(&m, now() + 10 * NS_PER_S, have_events, 5))
        FAIL("%u mouse events, want 5", m.nev);
    pump_for(&m, 30 * NS_PER_MS);
    CHECK_EQ(m.nev, 5);
    const struct { int16_t dx, dy; int8_t wheel; uint8_t buttons; } w[5] = {
        { 5, -3, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, -1, 0 }, { -128, 127, 0, 6 }, { 1, 1, 2, 4 },
    };
    for (unsigned i = 0; i < 5; i++) {
        const struct ev *e = &m.ev[i];
        if (e->kind != EV_MOUSE || e->dx != w[i].dx || e->dy != w[i].dy ||
            e->wheel != w[i].wheel || e->buttons != w[i].buttons)
            FAIL("mouse event %u: %d %d %d %#x", i, e->dx, e->dy, e->wheel, e->buttons);
    }
    /* The reports channel alone closing (usb-bus gave the endpoint up after
     * errors) with DR_USB still open: not "device gone" but exit 5, so
     * devmgr restarts hid instead of leaving the mouse dead (M7 review). */
    unplug(&m, false, true);
    return finish(&m, 5);
}

/* A composite keyboard: the boot interface types, the report-protocol one
 * is skipped (exit 0, its report descriptor read, no request sent), and a
 * non-HID interface handed to hid ends with 2. */
bool t_hid_composite(void)
{
    hcur = "hid_composite";
    static struct mock m;
    if (!start(&m, &dev_combo, 1, true, false))
        return false;
    if (!finish(&m, 0))
        return false;
    CHECK_EQ(m.nctl, 0);
    CHECK_EQ(m.opens, 0);
    CHECK_EQ(m.report_desc_reads, 1);
    CHECK_EQ(m.nev, 0);

    if (!start(&m, &dev_combo, 2, true, false) || !finish(&m, 2))
        return false;
    CHECK_EQ(m.nctl, 0);
    CHECK_EQ(m.opens, 0);

    if (!start(&m, &dev_combo, 0, true, true))
        return false;
    CHECK_EQ(m.refused, 0);                 /* its own endpoint 0x81, its own interface */
    unsigned at = 0;
    STEP(0, 0x0b, 0);
    STEP(0, 0, 0);
    EXPECT({ 0x0b, D, 0, 'h' }, { 0x0b, U, 0, 'h' });
    unplug(&m, true, true);
    return finish(&m, 0);
}

/* Unplug while a key is held and repeating: DR_USB closing alone ends it
 * with 0 ("device gone"); so does the console closing ("console gone").
 * Without a console at all, keys go to the log and unplug still ends it. */
bool t_hid_unplug_and_console_gone(void)
{
    hcur = "hid_unplug_and_console_gone";
    static struct mock m;
    if (!start(&m, &dev_kbd, 0, true, true))
        return false;
    unsigned at = 0;
    STEP(0, 0x04, 0);
    EXPECT({ 0x04, D, 0, 'a' });
    pump_for(&m, 600 * NS_PER_MS);                 /* repeating now */
    CHECK(m.nev > 1);
    unplug(&m, true, false);                /* DR_USB only */
    if (!finish(&m, 0))
        return false;

    if (!start(&m, &dev_kbd, 0, true, true))
        return false;
    at = 0;
    STEP(0, 0x04, 0);
    EXPECT({ 0x04, D, 0, 'a' });
    jam_port_unbind(m.port, m.input, KEY_INPUT);
    CHECK_ST(jam_handle_close(m.input), OK);   /* the console restarts */
    m.input = 0;
    if (!finish(&m, 0))
        return false;

    if (!start(&m, &dev_mouse, 0, false, true))
        return false;
    const uint8_t click[4] = { 1, 0, 0, 0 };
    if (!report(&m, click, 4))
        return false;
    pump_for(&m, 50 * NS_PER_MS);
    unplug(&m, true, true);
    return finish(&m, 0);
}
#undef STEP
#undef EXPECT
