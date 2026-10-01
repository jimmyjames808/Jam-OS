/* contest: `run contest mouse`: the mouse on a console key channel, as
 * <jam/abi.h> "input" describes it. tools/shell-tests/mouse.txt runs it
 * with a USB mouse in QEMU: at each "contest: mouse: step N" the script
 * moves the mouse and clicks through QEMU's monitor, then types a letter
 * over the serial port, which ends the step (a key always arrives).
 *
 *   1  a client that didn't ask gets no mouse message: the key comes first
 *   2  after a struct input_want it gets struct input_mouse_event messages
 *      (12 bytes, kind INPUT_EVENT_MOUSE) that add up to what the mouse did,
 *      the wheel included (+: a notch away from the user), and keys still
 *      come as 8 bytes
 *   3  a newer focus that didn't ask gets none, and the older one that did
 *      gets none either: the mouse goes with the keys
 *   4  an input_want without the mouse bit stops them
 *   5  anything else written on the channel loses it: the console closes it
 * Exit code 0 when all hold. */
#define CHECK_PROG "contest"
#define CHECK_CUR  "mouse"
#include <check.h>
#include <idl/console.h>
#include <os.h>
#include "contest.h"

union msg {
    struct input_key_event   key;
    struct input_mouse_event mouse;
    uint8_t                  bytes[32];
};

/* What a step's messages added up to, up to the key that ended it. */
struct seen {
    int      reports;        /* mouse messages */
    int      dx, dy;         /* their movement, summed */
    int      wheel;          /* their wheel notches, summed */
    uint8_t  pressed;        /* every button seen down */
    uint8_t  last;           /* the buttons in the last one */
    bool     wellformed;     /* each had the right kind and a zero reserved field */
    uint32_t key;            /* the codepoint of the key */
};

/* The next message on k within 20 s; its size, 0 if none came. */
static uint32_t next_msg(handle_t k, union msg *m)
{
    signals_t seen;
    if (jam_object_wait_one(k, SIG_READABLE, now() + 20000 * NS_PER_MS, &seen) != OK)
        return 0;
    uint32_t n = 0;
    struct channel_read_args a = {
        .h = k, .bytes_cap = sizeof(*m), .bytes = (uint64_t)(uintptr_t)m,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    return jam_channel_read(&a) == OK ? n : 0;
}

/* Say the step, then gather k's messages until a key goes down. */
static bool step(handle_t k, int number, struct seen *s)
{
    memset(s, 0, sizeof(*s));
    s->wellformed = true;
    printf("contest: mouse: step %d\n", number);
    for (;;) {
        union msg m;
        uint32_t n = next_msg(k, &m);
        if (n == sizeof(m.key) && m.key.state == INPUT_KEY_DOWN) {
            s->key = m.key.codepoint;
            return true;
        }
        if (n == sizeof(m.key))
            continue;
        if (n != sizeof(m.mouse))
            FAIL("step %d: a message of %u bytes", number, n);
        s->reports++;
        s->dx += m.mouse.dx;
        s->dy += m.mouse.dy;
        s->wheel += m.mouse.wheel;
        s->pressed |= m.mouse.buttons;
        s->last = m.mouse.buttons;
        s->wellformed &= m.mouse.kind == INPUT_EVENT_MOUSE && !m.mouse.reserved;
    }
}

static bool want(handle_t k, uint32_t events)
{
    struct input_want w = { .events = events };
    CHECK_ST(jam_channel_write(k, &w, sizeof(w), NULL, 0), OK);
    return true;
}

static bool empty(handle_t k)
{
    signals_t seen = 0;
    jam_object_wait_one(k, SIG_READABLE, now() + 50 * NS_PER_MS, &seen);
    return !(seen & SIG_READABLE);
}

static bool mouse_checks(handle_t con)
{
    handle_t k, k2;
    struct seen s;
    CHECK_ST(console_open_keys(con, &k), OK);
    CHECK(step(k, 1, &s));
    CHECK(s.reports == 0 && s.key == 'a');

    CHECK(want(k, INPUT_WANT_MOUSE));
    CHECK(step(k, 2, &s));
    CHECK(s.reports >= 2 && s.wellformed && s.key == 'b');
    CHECK_EQ(s.dx, 7);
    CHECK_EQ(s.dy, -3);
    CHECK_EQ(s.wheel, 1);   /* two notches away from the user, one back */
    CHECK(s.pressed == (INPUT_BTN_LEFT | INPUT_BTN_RIGHT) && s.last == 0);

    CHECK_ST(console_open_keys(con, &k2), OK);
    CHECK(step(k2, 3, &s));
    CHECK(s.reports == 0 && s.key == 'c' && empty(k));
    jam_handle_close(k2);

    CHECK(want(k, 0));
    CHECK(step(k, 4, &s));
    CHECK(s.reports == 0 && s.key == 'd');

    /* Half a request: the console drops the channel at the next report. */
    uint32_t junk = INPUT_WANT_MOUSE;
    CHECK_ST(jam_channel_write(k, &junk, sizeof(junk), NULL, 0), OK);
    printf("contest: mouse: step 5\n");
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(k, SIG_PEER_CLOSED, now() + 20000 * NS_PER_MS, &seen), OK);
    jam_handle_close(k);
    return true;
}

int contest_mouse(handle_t con)
{
    bool ok = mouse_checks(con);
    printf("contest: mouse: %s\n", ok ? "all held" : "FAILED");
    return ok ? 0 : 1;
}
