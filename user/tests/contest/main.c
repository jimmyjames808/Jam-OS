/* contest: checks of the console process's protocols, run from the shell
 * (`run contest`; the shell hands it SR_CONSOLE).
 *
 * The shell hands a program a PROGRAM-level console channel (console.idl
 * new_client), so contest checks that it can't connect an input source or
 * make channels, then the focus stack with real keys: it
 * prints "contest: type <c> now" and the test script types it over the
 * serial port (serialin, a real source): its open_keys gets it; a second
 * open_keys takes the keys, and closing it gives them back to the first.
 * (What sources feed in -- terminal text and escapes, keyboard events --
 * is tested by every shell test typing over serial and usbkeys over USB.)
 * `contest trap` holds the keys forever: Ctrl+C must still reach the shell,
 * which kills it. Exit code 0 when all hold. */
#include <devmgr.h>
#include <idl/console.h>
#include <idl/input.h>
#include <os.h>


static int checks, failed;

#define CHECK(c)                                                          \
    do {                                                                  \
        checks++;                                                         \
        if (!(c)) {                                                       \
            failed++;                                                     \
            printf("contest: FAILED at line %d: %s\n", __LINE__, #c);     \
        }                                                                 \
    } while (0)

static bool next_key(handle_t k, struct input_key_event *ev)
{
    signals_t seen;
    if (jam_object_wait_one(k, SIG_READABLE, now() + 2000 * NS_PER_MS, &seen) != OK)
        return false;
    uint32_t n = 0;
    struct channel_read_args a = {
        .h = k, .bytes_cap = sizeof(*ev), .bytes = (uint64_t)(uintptr_t)ev,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    return jam_channel_read(&a) == OK && n == sizeof(*ev);
}

/* A key typed by the test script after our prompt. */
static bool typed(handle_t k, char c)
{
    printf("contest: type %c now\n", c);
    struct input_key_event ev;
    uint64_t end = now() + 20000 * NS_PER_MS;
    while (now() < end)
        if (next_key(k, &ev) && ev.state == INPUT_KEY_DOWN)
            return ev.codepoint == (uint32_t)c;
    printf("contest: no key (want %c)\n", c);
    return false;
}

static bool empty(handle_t k)
{
    signals_t seen = 0;
    jam_object_wait_one(k, SIG_READABLE, now() + 50 * NS_PER_MS, &seen);
    return !(seen & SIG_READABLE);
}

/* ---- probe `run contest steal`: a `run` program posing as the console. If
 * it could send devmgr DEVMGR_SET_CONSOLE, the USB class drivers devmgr
 * (re)starts afterwards would connect their keys to it
 * (tools/shell-tests/review-steal.txt checks that they don't: `run`
 * programs get no devmgr channel, and the query channel refuses it). */
static handle_t steal_src;

static status_t st_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    (void)ctx;
    if (state == INPUT_KEY_DOWN)
        printf("contest: steal: got key usage %#x mods %#x cp %u\n", usage, mods, cp);
    return OK;
}
static status_t st_mouse(void *ctx, int16_t dx, int16_t dy, int8_t w, uint8_t b)
{
    (void)ctx, (void)dx, (void)dy, (void)w, (void)b;
    return OK;
}
static status_t st_text(void *ctx, uint16_t n, const uint8_t bytes[64])
{
    (void)ctx, (void)n, (void)bytes;
    return OK;
}
static const struct input_ops st_input = { st_key, st_mouse, st_text };

static status_t sc_write(void *ctx, uint16_t n, const uint8_t t[2048])
{
    (void)ctx, (void)n, (void)t;
    return OK;
}
static status_t sc_size(void *ctx, uint16_t *c, uint16_t *r)
{
    (void)ctx;
    *c = 80;
    *r = 25;
    return OK;
}
static status_t sc_clear(void *ctx)
{
    (void)ctx;
    return OK;
}
static status_t sc_open_keys(void *ctx, handle_t *out)
{
    (void)ctx, (void)out;
    return ERR_NOT_SUPPORTED;
}
static status_t sc_connect_input(void *ctx, handle_t *out)
{
    (void)ctx;
    handle_t theirs;
    status_t st = jam_channel_create(&steal_src, &theirs);
    if (st == OK) {
        printf("contest: steal: a driver connected its input to us\n");
        *out = theirs;
    }
    return st;
}
static status_t sc_lend_screen(void *ctx, uint32_t *w, uint32_t *h, uint32_t *p, uint8_t *rs,
                               uint8_t *gs, uint8_t *bs, uint64_t *size, handle_t *screen,
                               handle_t *lease)
{
    (void)ctx, (void)w, (void)h, (void)p, (void)rs, (void)gs, (void)bs, (void)size;
    (void)screen, (void)lease;
    return ERR_NOT_SUPPORTED;
}
static status_t sc_new_client(void *ctx, uint8_t level, handle_t *out)
{
    (void)ctx, (void)level, (void)out;
    return ERR_NOT_SUPPORTED;
}
static const struct console_ops st_console = { sc_write, sc_size, sc_clear, sc_open_keys,
                                               sc_connect_input, sc_lend_screen,
                                               sc_new_client };

static int steal(void)
{
    handle_t dm = startup_handle(SR_DEVMGR), mine, theirs;
    if (!dm) {
        /* `run` programs get no devmgr channel. */
        printf("contest: steal: no devmgr channel: nothing to steal with\n");
        return 0;
    }
    if (jam_channel_create(&mine, &theirs) != OK)
        return 1;
    struct devmgr_req q = { 0, DEVMGR_SET_CONSOLE, 0, 0, 0 };
    struct devmgr_rep r;
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = dm, .wn = sizeof(q), .wbytes = (uint64_t)(uintptr_t)&q,
        .wh = (uint64_t)(uintptr_t)&theirs, .whn = 1, .rcap = sizeof(r),
        .rbytes = (uint64_t)(uintptr_t)&r, .ractual = (uint64_t)(uintptr_t)&n,
        .rhactual = (uint64_t)(uintptr_t)&got,
        .deadline_ns = now() + 5000 * NS_PER_MS,
    };
    status_t st = jam_channel_call(&a);
    printf("contest: steal: DEVMGR_SET_CONSOLE: %s, reply %d\n", status_str(st),
           n >= DEVMGR_REP_HDR ? r.status : -1);
    if (st == OK && n >= DEVMGR_REP_HDR && r.status == ERR_ACCESS_DENIED) {
        printf("contest: steal: refused (a query channel)\n");
        return 0;
    }
    uint64_t end = now() + 40000 * NS_PER_MS;
    while (now() < end) {
        while (console_serve_one(mine, &st_console, NULL) == OK)
            ;
        while (steal_src && input_serve_one(steal_src, &st_input, NULL) == OK)
            ;
        jam_nanosleep(now() + 20 * NS_PER_MS);
    }
    printf("contest: steal: done\n");
    return 0;
}

/* ---- probe `run contest flood`: write to the console flat out, keeping our
 * channel's queue full (requests sent without waiting for their replies),
 * until killed. The console serves a client a bounded round at a time, so
 * the keys still get through: Ctrl+C must reach the shell, which kills us
 * (tools/shell-tests/basic.txt). Empty writes, so nothing floods the log. */
static int flood(handle_t con)
{
    static struct console_write_req q = { .ordinal = CONSOLE_WRITE };
    if (!con)
        return 1;
    printf("contest: flood: writing flat out\n");
    for (;;) {
        while (jam_channel_write(con, &q, sizeof(q), NULL, 0) == OK)
            ;
        /* The queue is full: throw the replies away and fill it again. */
        _Alignas(8) uint8_t rep[64];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = con, .bytes_cap = sizeof(rep), .bytes = (uint64_t)(uintptr_t)rep,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        while (jam_channel_read(&a) == OK)
            ;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "steal"))
        return steal();
    if (argc > 1 && !strcmp(argv[1], "flood"))
        return flood(startup_handle(SR_CONSOLE));
    handle_t con = startup_handle(SR_CONSOLE), k, k2, src;

    if (!con) {
        printf("contest: no console channel (SR_CONSOLE)\n");
        return 1;
    }
    /* Probe (`run contest cad`): a program with no root resource, only the
     * console channel the shell hands it, tries to play a keyboard and send
     * Ctrl+Alt+Del. It must fail: that channel can't connect a source. */
    if (argc > 1 && !strcmp(argv[1], "cad")) {
        CHECK(!startup_handle(SR_RESOURCE));
        status_t st = console_connect_input(con, &src);
        printf("contest: cad: connect_input: %s\n", status_str(st));
        if (st != OK)
            return st == ERR_ACCESS_DENIED && !failed ? 0 : 1;
        printf("contest: cad: sending Ctrl+Alt+Del as an input source\n");
        CHECK(input_key(src, 0x4c, INPUT_KEY_DOWN, INPUT_MOD_LCTRL | INPUT_MOD_LALT, 0) == OK);
        jam_nanosleep(now() + 2000 * NS_PER_MS);
        printf("contest: cad: still here\n");
        return 1;
    }
    /* `contest trap`: hold the keys and never let go (`run shell` would):
     * Ctrl+C must still reach the shell that ran us. */
    if (argc > 1 && !strcmp(argv[1], "trap")) {
        CHECK(console_open_keys(con, &k) == OK);
        printf("contest: trap: holding the keys\n");
        for (;;) {
            struct input_key_event ev;
            if (next_key(k, &ev) && ev.state == INPUT_KEY_DOWN)
                printf("contest: trap: got key usage %#x cp %u\n", ev.usage, ev.codepoint);
        }
    }
    uint16_t cols = 0, rows = 0;
    CHECK(console_size(con, &cols, &rows) == OK && cols >= 40 && rows >= 10);
    uint8_t line[2048] = "contest: \033[96mhello\033[0m from the console protocol\n";
    CHECK(console_write(con, (uint16_t)strlen((char *)line), line) == OK);
    CHECK(console_write(con, 3000, line) == ERR_INVALID_ARGS);

    /* A program's channel: no input sources, no new channels. */
    handle_t extra;
    CHECK(console_connect_input(con, &src) == ERR_ACCESS_DENIED);
    CHECK(console_new_client(con, 2, &extra) == ERR_ACCESS_DENIED);
    CHECK(console_new_client(con, 1, &extra) == ERR_ACCESS_DENIED);
    CHECK(console_new_client(con, 3, &extra) == ERR_INVALID_ARGS);

    /* Focus, with keys the test script types: the newest open_keys gets
     * them; closing it gives them back. */
    CHECK(console_open_keys(con, &k) == OK);
    CHECK(typed(k, 'q'));
    CHECK(console_open_keys(con, &k2) == OK);
    CHECK(typed(k2, 'r'));
    CHECK(empty(k));
    jam_handle_close(k2);
    CHECK(typed(k, 's'));
    jam_handle_close(k);   /* the shell has the keys again */

    printf("contest: %d checks, %d failed\n", checks, failed);
    uint8_t done[2048];
    int n = snprintf((char *)done, sizeof(done), "contest: %d checks, %d failed\n", checks, failed);
    console_write(con, (uint16_t)n, done);
    return failed ? 1 : 0;
}
