/* utest: the hid driver's mice in report protocol, against the mock
 * usb-bus and console of hidmock.c, and its report descriptor parser's
 * self-test. A mouse whose descriptor has a wheel is put in the report
 * protocol (SET_PROTOCOL(report)) and its reports are decoded with the
 * descriptor's layout: 16-bit and 12-bit X/Y, the wheel, report ids (only
 * the mouse's own reports count). A mouse without a wheel, any mouse with
 * the arg "hidboot", and one that refuses SET_PROTOCOL(report) stay in
 * the boot protocol. Each test ends like hid.c's: the driver exits by
 * itself with the right code and leaves its job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  hcur
#include <check.h>
#include <os.h>
#include "hidmock.h"
#include "utest.h"

#define SET_PROTOCOL 0x0b

/* A wanted mouse event. */
struct want_move {
    int16_t dx, dy;      /* movement */
    int8_t  wheel;       /* wheel */
    uint8_t buttons;     /* buttons */
};

static bool report(struct mock *m, const uint8_t *r, uint32_t n)
{
    CHECK_ST(jam_channel_write(m->reports, r, n, NULL, 0), OK);
    return true;
}

/* The events must be w[0..n) exactly, and nothing more for 30 ms. */
static bool expect_moves(struct mock *m, const struct want_move *w, unsigned n)
{
    if (!mock_pump(m, now() + 10 * NS_PER_S, mock_have_events, n))
        FAIL("%u mouse events, want %u", m->nev, n);
    mock_pump_for(m, 30 * NS_PER_MS);
    CHECK_EQ(m->nev, n);
    for (unsigned i = 0; i < n; i++) {
        const struct ev *e = &m->ev[i];
        if (e->kind != EV_MOUSE || e->dx != w[i].dx || e->dy != w[i].dy ||
            e->wheel != w[i].wheel || e->buttons != w[i].buttons)
            FAIL("mouse event %u: %d %d wheel %d buttons %#x, want %d %d wheel %d buttons %#x",
                 i, e->dx, e->dy, e->wheel, e->buttons, w[i].dx, w[i].dy, w[i].wheel,
                 w[i].buttons);
    }
    return true;
}

/* The SET_PROTOCOL values sent, in order, must be want[0..n). */
static bool protocols(const struct mock *m, const uint16_t *want, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < m->nctl; i++) {
        if (m->ctl[i].request != SET_PROTOCOL)
            continue;
        if (k == n || m->ctl[i].value != want[k])
            FAIL("SET_PROTOCOL %u: value %u, want %d", k, m->ctl[i].value,
                 k < n ? want[k] : -1);
        k++;
    }
    CHECK_EQ(k, n);
    return true;
}

static bool finish(struct mock *m)
{
    mock_unplug(m, true, true);
    return mock_finish(m, 0);
}

/* `drv/hid selftest`: the parser against its fixtures, cut and fuzzed. */
bool t_hid_report_parser(void)
{
    hcur = "hid_report_parser";
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    const char *argv[] = { "drv/hid", "selftest" };
    struct spawn_args a = { .path = "drv/hid", .argc = 2, .argv = argv, .job = job };
    CHECK_ST(spawn(&a, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 60 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);   /* 6: a check failed (the log says which) */
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* A gaming mouse: report protocol; 16-bit X/Y, the wheel both ways; a
 * report with nothing new sends nothing; a short one is dropped. */
bool t_hid_mouse_report_protocol(void)
{
    hcur = "hid_mouse_report_protocol";
    static struct mock m;
    if (!mock_start(&m, &dev_gaming, 0, true, true))
        return false;
    const uint16_t report_protocol[] = { 1 };
    if (!protocols(&m, report_protocol, 1))
        return false;
    const uint8_t r1[8] = { 0x01, 0x00, 0x2c, 0x01, 0x18, 0xfc, 0x01, 0x00 };
    const uint8_t r2[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05 };   /* pan only */
    const uint8_t r3[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00 };
    const uint8_t r4[3] = { 0x01, 0x05, 0x05 };                                 /* short */
    const uint8_t r5[8] = { 0x04, 0x00, 0x00, 0x80, 0xff, 0x7f, 0x00, 0x00 };
    if (!report(&m, r1, 8) || !report(&m, r2, 8) || !report(&m, r3, 8) || !report(&m, r4, 3) ||
        !report(&m, r5, 8))
        return false;
    const struct want_move w[] = {
        { 300, -1000, 1, 1 }, { 0, 0, -1, 0 }, { -32768, 32767, 0, 4 },
    };
    if (!expect_moves(&m, w, 3))
        return false;
    return finish(&m);
}

/* A receiver's mouse interface with report ids: only id 2 is the mouse's;
 * the keyboard's (id 1) and consumer control's (id 3) are not events. */
bool t_hid_mouse_report_ids(void)
{
    hcur = "hid_mouse_report_ids";
    static struct mock m;
    if (!mock_start(&m, &dev_receiver, 0, true, true))
        return false;
    const uint16_t report_protocol[] = { 1 };
    if (!protocols(&m, report_protocol, 1))
        return false;
    const uint8_t kbd[9] = { 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t mouse[8] = { 0x02, 0x02, 0x00, 0xfb, 0xcf, 0x12, 0x01, 0x00 };
    const uint8_t media[5] = { 0x03, 0xe9, 0x00, 0x00, 0x00 };
    const uint8_t mouse2[8] = { 0x02, 0x00, 0x00, 0x01, 0xf8, 0x7f, 0xff, 0x00 };
    if (!report(&m, kbd, 9) || !report(&m, mouse, 8) || !report(&m, media, 5) ||
        !report(&m, mouse2, 8))
        return false;
    const struct want_move w[] = { { -5, 300, 1, 2 }, { -2047, 2047, -1, 0 } };
    if (!expect_moves(&m, w, 2))
        return false;
    return finish(&m);
}

/* The boot protocol stays: a mouse without a wheel; the gaming mouse with
 * "hidboot"; one that stalls SET_PROTOCOL(report) (then SET_PROTOCOL(boot)).
 * All three decode boot reports, a fourth byte as the wheel. */
bool t_hid_mouse_boot_kept(void)
{
    hcur = "hid_mouse_boot_kept";
    static struct mock m;
    const uint8_t r[4] = { 0x03, 0x05, 0xfd, 0x02 };
    const struct want_move w[] = { { 5, -3, 2, 3 } };
    const uint16_t boot[] = { 0 }, report_then_boot[] = { 1, 0 };

    if (!mock_start(&m, &dev_plain_mouse, 0, true, true) || !protocols(&m, boot, 1) ||
        !report(&m, r, 4) || !expect_moves(&m, w, 1) || !finish(&m))
        return false;
    if (!mock_start_arg(&m, &dev_gaming, 0, "hidboot") || !protocols(&m, boot, 1) ||
        !report(&m, r, 4) || !expect_moves(&m, w, 1) || !finish(&m))
        return false;
    if (!mock_start(&m, &dev_gaming_stubborn, 0, true, true) ||
        !protocols(&m, report_then_boot, 2) || !report(&m, r, 4) || !expect_moves(&m, w, 1))
        return false;
    return finish(&m);
}
