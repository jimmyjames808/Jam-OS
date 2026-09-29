/* contest: checks of the console process's protocols (M7 Track C), run
 * from the shell (`run contest`; the shell hands it SR_CONSOLE).
 *
 * It plays an input source and a focused client at once: connect_input
 * gives it an `input` channel, open_keys a key channel; what it feeds in
 * as a terminal (input.text) or a keyboard (input.key) must come out as
 * the right struct input_key_event. Then the source goes away (the console
 * must carry on and accept a new one: "survives restarts of its input
 * sources"), and the focus stack: a second open_keys takes the keys, and
 * closing it gives them back to the first. Exit code 0 when all hold. */
#include <os.h>
#include <idl/console.h>
#include <idl/input.h>

#define MS 1000000ull

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
    if (jam_object_wait_one(k, SIG_READABLE, (uint64_t)jam_clock_get() + 2000 * MS, &seen) != OK)
        return false;
    uint32_t n = 0;
    struct channel_read_args a = {
        .h = k, .bytes_cap = sizeof(*ev), .bytes = (uint64_t)(uintptr_t)ev,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    return jam_channel_read(&a) == OK && n == sizeof(*ev);
}

static bool expect(handle_t k, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    struct input_key_event ev;
    if (!next_key(k, &ev)) {
        printf("contest: no key (want usage %#x cp %u)\n", usage, cp);
        return false;
    }
    if (ev.usage != usage || ev.state != state || ev.mods != mods || ev.codepoint != cp) {
        printf("contest: got usage %#x state %u mods %#x cp %u, want %#x %u %#x %u\n", ev.usage,
               ev.state, ev.mods, ev.codepoint, usage, state, mods, cp);
        return false;
    }
    return true;
}

static status_t text(handle_t src, const char *s)
{
    uint8_t b[64] = { 0 };
    size_t n = strlen(s);
    memcpy(b, s, n);
    return input_text(src, (uint16_t)n, b);
}

static bool empty(handle_t k)
{
    signals_t seen = 0;
    jam_object_wait_one(k, SIG_READABLE, (uint64_t)jam_clock_get() + 50 * MS, &seen);
    return !(seen & SIG_READABLE);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t con = startup_handle(SR_CONSOLE), k, k2, src, src2;
    if (!con) {
        printf("contest: no console channel (SR_CONSOLE)\n");
        return 1;
    }
    uint16_t cols = 0, rows = 0;
    CHECK(console_size(con, &cols, &rows) == OK && cols >= 40 && rows >= 10);
    uint8_t line[2048] = "contest: \033[96mhello\033[0m from the console protocol\n";
    CHECK(console_write(con, (uint16_t)strlen((char *)line), line) == OK);
    CHECK(console_write(con, 3000, line) == ERR_INVALID_ARGS);

    CHECK(console_open_keys(con, &k) == OK);
    CHECK(console_connect_input(con, &src) == OK);
    /* A terminal: text, Enter, an arrow, Backspace (DEL), Ctrl+C. */
    CHECK(text(src, "ab\r\033[A\x7f\x03") == OK);
    CHECK(expect(k, 0, INPUT_KEY_DOWN, 0, 'a'));
    CHECK(expect(k, 0, INPUT_KEY_DOWN, 0, 'b'));
    CHECK(expect(k, 0x28, INPUT_KEY_DOWN, 0, '\n'));
    CHECK(expect(k, 0x52, INPUT_KEY_DOWN, 0, 0));
    CHECK(expect(k, 0x2a, INPUT_KEY_DOWN, 0, 8));
    CHECK(expect(k, 0, INPUT_KEY_DOWN, 0, 3));
    /* CR LF is one Enter; an escape split over two calls still parses. */
    CHECK(text(src, "\r\n\033[") == OK);
    CHECK(text(src, "3~") == OK);
    CHECK(expect(k, 0x28, INPUT_KEY_DOWN, 0, '\n'));
    CHECK(expect(k, 0x4c, INPUT_KEY_DOWN, 0, 0x7f));
    CHECK(empty(k));
    /* A keyboard: events pass through as they are (key-ups too). */
    CHECK(input_key(src, 0x04, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 'A') == OK);
    CHECK(input_key(src, 0x04, INPUT_KEY_UP, INPUT_MOD_LSHIFT, 'A') == OK);
    CHECK(expect(k, 0x04, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 'A'));
    CHECK(expect(k, 0x04, INPUT_KEY_UP, INPUT_MOD_LSHIFT, 'A'));
    CHECK(input_mouse(src, 3, -2, 0, 1) == OK);
    CHECK(text(src, "") == OK);
    uint8_t big[64] = { 0 };
    CHECK(input_text(src, 65, big) == ERR_INVALID_ARGS);

    /* The source goes away; the console carries on and takes a new one. */
    jam_handle_close(src);
    snprintf((char *)line, sizeof(line), "contest: source 1 gone\n");
    CHECK(console_write(con, (uint16_t)strlen((char *)line), line) == OK);
    CHECK(console_connect_input(con, &src2) == OK);
    CHECK(text(src2, "z") == OK);
    CHECK(expect(k, 0, INPUT_KEY_DOWN, 0, 'z'));

    /* Focus: the newest open_keys gets the keys; closing it gives them back. */
    CHECK(console_open_keys(con, &k2) == OK);
    CHECK(text(src2, "q") == OK);
    CHECK(expect(k2, 0, INPUT_KEY_DOWN, 0, 'q'));
    CHECK(empty(k));
    jam_handle_close(k2);
    CHECK(text(src2, "r") == OK);
    CHECK(expect(k, 0, INPUT_KEY_DOWN, 0, 'r'));
    jam_handle_close(src2);
    jam_handle_close(k);   /* the shell has the keys again */

    printf("contest: %d checks, %d failed\n", checks, failed);
    uint8_t done[2048];
    int n = snprintf((char *)done, sizeof(done), "contest: %d checks, %d failed\n", checks, failed);
    console_write(con, (uint16_t)n, done);
    return failed ? 1 : 0;
}
