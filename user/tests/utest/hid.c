/* utest: the HID driver (drivers/hid) as a process, against the mock
 * usb-bus and mock compositor of hidmock.c: a boot keyboard types (usages
 * and modifiers, no character and no repeats: M12's Q15 and review W6;
 * lock keys as keys and the lights as the compositor says: W7; rollover),
 * events kept while the compositor's queue is full, none lost (W6), a
 * boot mouse moves and clicks, a composite device's other interfaces are
 * skipped, and the driver ends right when the device or the compositor
 * goes.
 * Each test starts the driver in a job of its own and ends with the
 * driver exited by itself with the code it should, every one of our
 * channel ends seeing PEER_CLOSED (the driver closed everything), and its
 * job empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  hcur
#include <check.h>
#include <idl/input.h>
#include <os.h>
#include "hidmock.h"
#include "utest.h"

const char *hcur;   /* the test running */

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
    uint16_t usage;          /* key usage */
    uint8_t  state, mods;    /* INPUT_KEY_*, modifiers */
};

static unsigned count_from(const struct mock *m, unsigned from)
{
    return m->nev > from ? m->nev - from : 0;
}

static bool have_from(struct mock *m, unsigned arg)
{
    return count_from(m, arg >> 16) >= (arg & 0xffff);
}

/* The events from *at on must be w[0..n) exactly; with `exact`, nothing
 * else may follow for 30 ms. *at moves past them. */
static bool expect_x(struct mock *m, unsigned *at, const struct want *w, unsigned n, bool exact)
{
    if (!mock_pump(m, now() + 10 * NS_PER_S, have_from, *at << 16 | n))
        FAIL("%u event(s), want %u", count_from(m, *at), n);
    unsigned i = *at;
    for (unsigned k = 0; k < n; k++, i++) {
        const struct ev *e = &m->ev[i];
        if (e->kind != EV_KEY || e->usage != w[k].usage || e->state != w[k].state ||
            e->mods != w[k].mods)
            FAIL("event %u: kind %u usage %#x state %u mods %#x, want usage %#x state %u "
                 "mods %#x", k, e->kind, e->usage, e->state, e->mods, w[k].usage, w[k].state,
                 w[k].mods);
    }
    *at = i;
    if (exact) {
        mock_pump_for(m, 30 * NS_PER_MS);
        if (count_from(m, i))
            FAIL("%u event(s) more than wanted", count_from(m, i));
    }
    return true;
}

static bool expect(struct mock *m, unsigned *at, const struct want *w, unsigned n)
{
    return expect_x(m, at, w, n, true);
}

/* The LED bytes the driver sent, in order, into out; how many. */
static unsigned leds(const struct mock *m, uint8_t *out, unsigned cap)
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

/* "Hello, World!\n" typed on a boot keyboard: the start-up requests (the
 * lights dark: the compositor says what to light), then the exact events
 * of 'H' and the usage of every DOWN, which is all a key event says. */
bool t_hid_typing(void)
{
    hcur = "hid_typing";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    /* SET_PROTOCOL(boot), SET_IDLE(0) (refused: it goes on), LEDs dark. */
    CHECK(m.nctl >= 3);
    CHECK(m.ctl[0].type == 0x21 && m.ctl[0].request == 0x0b && m.ctl[0].value == 0);
    CHECK(m.ctl[1].type == 0x21 && m.ctl[1].request == 0x0a && m.ctl[1].value == 0);
    uint8_t l[8];
    CHECK_EQ(leds(&m, l, 8), 1);
    CHECK_EQ(l[0], 0x00);
    CHECK_EQ(m.report_desc_reads, 1);
    CHECK_EQ(m.refused, 0);
    CHECK_EQ(m.opens, 1);

    const char *text = "Hello, World!\n";
    unsigned nwant = 0, nkeys = 0;
    uint8_t typed[32];
    for (const char *p = text; *p; p++) {
        bool shift;
        uint8_t u = usage_of(*p, &shift);
        CHECK(u != 0);
        if (!keys(&m, shift ? LSHIFT : 0, u, 0) || !keys(&m, 0, 0, 0))
            return false;
        nwant += shift ? 4 : 2;
        typed[nkeys++] = u;
    }
    const struct want h[] = {
        { 0xe1, D, LSHIFT }, { 0x0b, D, LSHIFT }, { 0x0b, U, LSHIFT },
        { 0xe1, U, 0 }, { 0x08, D, 0 }, { 0x08, U, 0 },
    };
    unsigned at = 0;
    if (!expect_x(&m, &at, h, 6, false))
        return false;
    if (!mock_pump(&m, now() + 10 * NS_PER_S, mock_have_events, nwant))
        FAIL("%u events, want %u", m.nev, nwant);
    mock_pump_for(&m, 30 * NS_PER_MS);
    CHECK_EQ(m.nev, nwant);
    unsigned n = 0, downs = 0, ups = 0;
    for (unsigned i = 0; i < m.nev; i++) {
        CHECK(m.ev[i].state == D || m.ev[i].state == U);   /* nothing else, ever */
        if (m.ev[i].state == D) {
            downs++;
            if (m.ev[i].usage < 0xe0 && n < nkeys && m.ev[i].usage != typed[n++])
                FAIL("DOWN %u: usage %#x, want %#x", n - 1, m.ev[i].usage, typed[n - 1]);
        } else {
            ups++;
        }
    }
    CHECK_EQ(n, nkeys);
    CHECK_EQ(downs, ups);
    CHECK_EQ(m.readies, 1);
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* Shift, the lock keys as plain keys (no LED of hid's own: review W7),
 * the keypad as itself (Num Lock is the compositor's to apply), Ctrl and
 * Alt as modifiers, Ctrl+Alt+Delete as a plain key. */
bool t_hid_modifiers(void)
{
    hcur = "hid_modifiers";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
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
    EXPECT({ 0x39, D, 0 }, { 0x39, U, 0 });
    STEP(0, 0x04, 0);                       /* a: no layout here */
    STEP(0, 0, 0);
    STEP(LSHIFT, 0x04, 0);                  /* Shift+a */
    STEP(0, 0, 0);
    STEP(0, 0x1e, 0);                       /* 1 */
    STEP(LSHIFT, 0x1e, 0);                  /* Shift while 1 is held: nothing new for 1 */
    STEP(LSHIFT, 0, 0);
    STEP(0, 0, 0);
    EXPECT({ 0x04, D, 0 }, { 0x04, U, 0 },
           { 0xe1, D, LSHIFT }, { 0x04, D, LSHIFT }, { 0x04, U, LSHIFT }, { 0xe1, U, 0 },
           { 0x1e, D, 0 }, { 0xe1, D, LSHIFT }, { 0x1e, U, LSHIFT }, { 0xe1, U, 0 });
    STEP(0x20, 0x2f, 0);                    /* right Shift + [ */
    STEP(0, 0, 0);
    EXPECT({ 0xe5, D, 0x20 }, { 0x2f, D, 0x20 }, { 0x2f, U, 0x20 }, { 0xe5, U, 0 });
    /* The keypad as itself, Num Lock a key like Caps Lock. */
    STEP(0, 0x60, 0);
    STEP(0, 0, 0);
    STEP(0, 0x53, 0);                       /* Num Lock */
    STEP(0, 0, 0);
    STEP(0, 0x60, 0);                       /* KP8: still KP8 */
    STEP(0, 0, 0);
    STEP(0, 0x47, 0);                       /* Scroll Lock */
    STEP(0, 0, 0);
    EXPECT({ 0x60, D, 0 }, { 0x60, U, 0 }, { 0x53, D, 0 }, { 0x53, U, 0 },
           { 0x60, D, 0 }, { 0x60, U, 0 }, { 0x47, D, 0 }, { 0x47, U, 0 });
    uint8_t l[8];
    CHECK_EQ(leds(&m, l, 8), 1);            /* the dark start only: the locks aren't hid's */

    /* Ctrl+C; Ctrl+Alt+Delete is a key like any other. */
    STEP(LCTRL, 0x06, 0);
    STEP(0, 0, 0);
    STEP(LCTRL | LALT, 0x4c, 0);
    STEP(0, 0, 0);
    STEP(0, 0x28, 0x2a);                    /* Enter and Backspace together */
    STEP(0, 0, 0);
    EXPECT({ 0xe0, D, LCTRL }, { 0x06, D, LCTRL }, { 0x06, U, LCTRL }, { 0xe0, U, 0 },
           { 0xe0, D, LCTRL }, { 0xe2, D, LCTRL | LALT },
           { 0x4c, D, LCTRL | LALT }, { 0x4c, U, LCTRL | LALT },
           { 0xe0, U, LALT }, { 0xe2, U, 0 },
           { 0x28, D, 0 }, { 0x2a, D, 0 }, { 0x2a, U, 0 }, { 0x28, U, 0 });
    /* One report letting go of Shift and pressing a key: the key is
     * unshifted; one pressing Shift and a key: shifted. */
    STEP(LSHIFT, 0, 0);
    STEP(0, 0x04, 0);
    STEP(LSHIFT, 0x04, 0x05);
    STEP(0, 0, 0);
    EXPECT({ 0xe1, D, LSHIFT }, { 0xe1, U, 0 }, { 0x04, D, 0 },
           { 0xe1, D, LSHIFT }, { 0x05, D, LSHIFT },
           { 0x05, U, LSHIFT }, { 0x04, U, LSHIFT }, { 0xe1, U, 0 });
    CHECK_EQ(m.refused, 0);
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* The lights are the compositor's (review W7): input.leds lights them
 * (SET_REPORT with that byte), the same lights again change nothing, only
 * the three LED bits go to the keyboard. */
bool t_hid_leds(void)
{
    hcur = "hid_leds";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    uint8_t l[8];
    CHECK_EQ(leds(&m, l, 8), 1);            /* dark at start */
    CHECK_ST(input_leds(m.input, INPUT_LED_NUM | INPUT_LED_CAPS), OK);
    CHECK_ST(input_leds(m.input, INPUT_LED_NUM | INPUT_LED_CAPS), OK);   /* no change */
    CHECK_ST(input_leds(m.input, 0xf8 | INPUT_LED_NUM), OK);            /* bits it lacks */
    CHECK_ST(input_leds(m.input, INPUT_LED_SCROLL), OK);
    uint64_t until = now() + 10 * NS_PER_S;
    while (leds(&m, l, 8) < 4 && now() < until)
        mock_pump_for(&m, 10 * NS_PER_MS);
    mock_pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(leds(&m, l, 8), 4);
    CHECK_EQ(l[1], 0x03);
    CHECK_EQ(l[2], 0x01);
    CHECK_EQ(l[3], 0x04);
    CHECK_EQ(m.nev, 0);                     /* lights are no keys */
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* More lights than hid reads a turn (64), queued while it waits for the
 * keyboard to take the first: the last one is lit. DR_INPUT is bound
 * PERSISTENT, which fires when the channel turns readable, not while it
 * stays so: hid read 64 and never the rest, nor any light after them. */
bool t_hid_leds_storm(void)
{
    hcur = "hid_leds_storm";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    uint8_t l[8];
    CHECK_EQ(leds(&m, l, 8), 1);   /* dark at start */
    /* Nobody serves DR_USB meanwhile: the first one's SET_REPORT waits,
     * and the rest (the same lights: no request) queue behind it. */
    for (unsigned i = 0; i < 99; i++)
        CHECK_ST(input_leds(m.input, INPUT_LED_NUM), OK);
    CHECK_ST(input_leds(m.input, INPUT_LED_CAPS), OK);
    uint64_t until = now() + 10 * NS_PER_S;
    while (leds(&m, l, 8) < 3 && now() < until)
        mock_pump_for(&m, 10 * NS_PER_MS);
    mock_pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(leds(&m, l, 8), 3);
    CHECK_EQ(l[1], 0x01);
    CHECK_EQ(l[2], 0x02);
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* The compositor slow (its queue for this keyboard full: the mock stops
 * reading): hid keeps the events it can't send and sends them, in order,
 * once there is room; no key event is lost (review W6: the old calls gave
 * up after 2 s and dropped the event, a lost UP leaving the key held). */
bool t_hid_queue_full(void)
{
    hcur = "hid_queue_full";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    mock_pump_for(&m, 50 * NS_PER_MS);      /* its ready event read */
    m.hold_input = true;
    enum { PRESSES = 600 };                 /* 1200 events: more than a channel's queue holds */
    for (unsigned i = 0; i < PRESSES; i++) {
        uint8_t u = (uint8_t)(0x04 + i % 26);
        for (unsigned half = 0; half < 2; half++) {
            const uint8_t r[8] = { 0, 0, half ? 0 : u, 0, 0, 0, 0, 0 };
            status_t st;
            while ((st = jam_channel_write(m.reports, r, 8, NULL, 0)) == ERR_SHOULD_WAIT)
                jam_sleep_until(now() + NS_PER_MS);
            CHECK_ST(st, OK);
        }
    }
    jam_sleep_until(now() + 300 * NS_PER_MS);   /* hid has read every report meanwhile */
    CHECK_EQ(m.nev, 0);
    m.hold_input = false;
    if (!mock_pump(&m, now() + 20 * NS_PER_S, mock_have_events, 2 * PRESSES))
        FAIL("%u events, want %u", m.nev, 2 * PRESSES);
    mock_pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(m.nev, 2 * PRESSES);
    for (unsigned i = 0; i < 2 * PRESSES; i++) {
        const struct ev *e = &m.ev[i];
        uint8_t u = (uint8_t)(0x04 + i / 2 % 26);
        if (e->kind != EV_KEY || e->usage != u || e->state != (i % 2 ? U : D))
            FAIL("event %u: usage %#x state %u, want %#x %u", i, e->usage, e->state, u,
                 i % 2 ? U : D);
    }
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* Phantom state (ErrorRollOver in every slot) is ignored whole, and keys
 * held across it are neither released nor pressed again. */
bool t_hid_rollover(void)
{
    hcur = "hid_rollover";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    unsigned at = 0;
    STEP(0, 0x04, 0);
    EXPECT({ 0x04, D, 0 });
    const uint8_t phantom[8] = { LSHIFT, 0, 1, 1, 1, 1, 1, 1 };
    if (!report(&m, phantom, 8))
        return false;
    const uint8_t undefined[8] = { 0, 0, 0x04, 0x03, 0, 0, 0, 0 };
    if (!report(&m, undefined, 8))
        return false;
    mock_pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(count_from(&m, at), 0);  /* nothing: not even the Shift */
    STEP(0, 0x04, 0x05);
    STEP(0, 0, 0);
    EXPECT({ 0x05, D, 0 }, { 0x05, U, 0 }, { 0x04, U, 0 });
    /* Six keys at once, then a report with a key repeated in two slots. */
    const uint8_t six[8] = { 0, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    if (!report(&m, six, 8))
        return false;
    const uint8_t dup[8] = { 0, 0, 0x0a, 0x0a, 0, 0, 0, 0 };
    if (!report(&m, dup, 8))
        return false;
    STEP(0, 0, 0);
    EXPECT({ 0x04, D, 0 }, { 0x05, D, 0 }, { 0x06, D, 0 }, { 0x07, D, 0 },
           { 0x08, D, 0 }, { 0x09, D, 0 },
           { 0x09, U, 0 }, { 0x08, U, 0 }, { 0x07, U, 0 }, { 0x06, U, 0 },
           { 0x05, U, 0 }, { 0x04, U, 0 }, { 0x0a, D, 0 }, { 0x0a, U, 0 });
    /* Too short to be a report: ignored. */
    const uint8_t runt[2] = { LSHIFT, 0 };
    if (!report(&m, runt, 2))
        return false;
    mock_pump_for(&m, 50 * NS_PER_MS);
    CHECK_EQ(count_from(&m, at), 0);
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* No repeats (review W6: Wayland clients repeat keys themselves): a key
 * held 1.4 s, Shift pressed meanwhile, sends its DOWN and UP and Shift's,
 * nothing more. */
bool t_hid_repeat(void)
{
    hcur = "hid_repeat";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    if (!keys(&m, 0, 0x1b, 0))              /* x */
        return false;
    if (!mock_pump(&m, now() + 5 * NS_PER_S, mock_have_events, 1))
        FAIL("no DOWN");
    uint64_t t_down = m.ev[0].t;
    mock_pump(&m, t_down + 900 * NS_PER_MS, NULL, 0);
    if (!keys(&m, LSHIFT, 0x1b, 0))
        return false;
    mock_pump(&m, t_down + 1400 * NS_PER_MS, NULL, 0);
    if (!keys(&m, 0, 0, 0))
        return false;
    unsigned at = 0;
    const struct want w[] = { { 0x1b, D, 0 }, { 0xe1, D, LSHIFT }, { 0x1b, U, LSHIFT },
                              { 0xe1, U, 0 } };
    if (!expect_x(&m, &at, w, 4, false))
        return false;
    mock_pump_for(&m, 300 * NS_PER_MS);
    CHECK_EQ(m.nev, 4);
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* A mouse like QEMU's (its report descriptor has a wheel, so it is put in
 * the report protocol; its report is laid out as the boot one with the
 * wheel in a fourth byte): buttons, deltas, the wheel; unchanged reports
 * send nothing; the reports channel closing alone ends it with 5 (restart
 * me); a 3-byte report has no wheel. hidmouse.c has the other mice. */
bool t_hid_mouse(void)
{
    hcur = "hid_mouse";
    static struct mock m;
    if (!mock_start(&m, &dev_mouse, 0, true, true))
        return false;
    for (unsigned i = 0; i < m.nctl; i++)
        CHECK(m.ctl[i].request != 0x0a && m.ctl[i].request != 0x09);   /* no SET_IDLE, LEDs */
    CHECK(m.nctl == 1 && m.ctl[0].request == 0x0b && m.ctl[0].value == 1);   /* report */
    const uint8_t r1[4] = { 0x01, 5, (uint8_t)-3, 0 };
    const uint8_t r2[4] = { 0x01, 0, 0, 0 };            /* nothing changed: no event */
    const uint8_t r3[4] = { 0x00, 0, 0, 0 };
    const uint8_t r4[4] = { 0x00, 0, 0, (uint8_t)-1 };
    const uint8_t r5[3] = { 0x06, (uint8_t)-128, 127 }; /* no wheel byte */
    const uint8_t r6[8] = { 0x04, 1, 1, 2, 0x55, 0x55, 0x55, 0x55 };   /* padded */
    if (!report(&m, r1, 4) || !report(&m, r2, 4) || !report(&m, r3, 4) || !report(&m, r4, 4) ||
        !report(&m, r5, 3) || !report(&m, r6, 8))
        return false;
    if (!mock_pump(&m, now() + 10 * NS_PER_S, mock_have_events, 5))
        FAIL("%u mouse events, want 5", m.nev);
    mock_pump_for(&m, 30 * NS_PER_MS);
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
     * devmgr restarts hid instead of leaving the mouse dead. */
    mock_unplug(&m, false, true);
    return mock_finish(&m, 5);
}

/* A composite keyboard: the boot interface types, the report-protocol one
 * is skipped (exit 0, its report descriptor read, no request sent), and a
 * non-HID interface handed to hid ends with 2. */
bool t_hid_composite(void)
{
    hcur = "hid_composite";
    static struct mock m;
    if (!mock_start(&m, &dev_combo, 1, true, false))
        return false;
    if (!mock_finish(&m, 0))
        return false;
    CHECK_EQ(m.nctl, 0);
    CHECK_EQ(m.opens, 0);
    CHECK_EQ(m.report_desc_reads, 1);
    CHECK_EQ(m.nev, 0);

    if (!mock_start(&m, &dev_combo, 2, true, false) || !mock_finish(&m, 2))
        return false;
    CHECK_EQ(m.nctl, 0);
    CHECK_EQ(m.opens, 0);

    if (!mock_start(&m, &dev_combo, 0, true, true))
        return false;
    CHECK_EQ(m.refused, 0);                 /* its own endpoint 0x81, its own interface */
    unsigned at = 0;
    STEP(0, 0x0b, 0);
    STEP(0, 0, 0);
    EXPECT({ 0x0b, D, 0 }, { 0x0b, U, 0 });
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}

/* Unplug while a key is held: DR_USB closing alone ends it with 0
 * ("device gone"); so does the compositor closing ("console gone"). Without
 * one at all, keys go to the log and unplug still ends it. */
bool t_hid_unplug_and_console_gone(void)
{
    hcur = "hid_unplug_and_console_gone";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    unsigned at = 0;
    STEP(0, 0x04, 0);
    EXPECT({ 0x04, D, 0 });
    mock_pump_for(&m, 600 * NS_PER_MS);                 /* held */
    CHECK_EQ(m.nev, 1);
    mock_unplug(&m, true, false);                /* DR_USB only */
    if (!mock_finish(&m, 0))
        return false;

    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    at = 0;
    STEP(0, 0x04, 0);
    EXPECT({ 0x04, D, 0 });
    jam_port_unbind(m.port, m.input, KEY_INPUT);
    CHECK_ST(jam_handle_close(m.input), OK);   /* the compositor restarts */
    m.input = 0;
    if (!mock_finish(&m, 0))
        return false;

    if (!mock_start(&m, &dev_mouse, 0, false, true))
        return false;
    const uint8_t click[4] = { 1, 0, 0, 0 };
    if (!report(&m, click, 4))
        return false;
    mock_pump_for(&m, 50 * NS_PER_MS);
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}
#undef STEP
#undef EXPECT
