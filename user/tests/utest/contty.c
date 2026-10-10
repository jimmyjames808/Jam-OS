/* utest: the terminal's POSIX side in the console (console.open_tty, the
 * `tty` protocol: user/services/console/tty.c), on a connection console
 * (conrig.h): this file is the remote terminal, typing into the master and
 * reading what it shows, and the POSIX client, speaking the protocol and
 * reading and writing its data channel the way libc's terminal descriptor does.
 *
 * t_con_tty_canonical: a line typed is echoed and arrives as one message;
 * output's LF is shown as CR LF; erase; end of file as an empty message;
 * the settings' defaults; the size, and a resize as the size event;
 * Ctrl+C with no shell below as the signal event, the input flushed.
 *
 * t_con_tty_raw: settings changed now, after a drain (the output written
 * before is shown first) and after a flush; raw bytes (Ctrl+C among them)
 * as they came, no echo; flush of the input; drain; the settings outlive
 * the client (the next one sees them); a bad `when` and `queue`.
 *
 * t_con_tty_focus: a shell's keys below a program's tty (the shell's
 * SHELL client opened keys, its program's PROGRAM client a tty): the keys
 * go to the tty, ISIG's Ctrl+C to the shell's keys and not the tty; a line
 * typed ahead and not read, and a line half typed, reach the shell's keys
 * when the tty closes; an output message past TTY_OUT_MAX hangs the tty
 * up; nine ttys are one too many. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/conn_ctl.h>
#include <idl/console.h>
#include <ldisc.h>
#include <os.h>
#include "conrig.h"
#include "utest.h"

#define PROGRAM_LEVEL 2   /* console.new_client: L_PROGRAM */
#define MAX_TTYS_TEST 8   /* open_tty's clients at once (the console's MAX_TTYS) */

struct tty_client {
    handle_t ch, data;   /* the tty channel, the data channel */
};

static status_t open_tty(handle_t client, struct tty_client *t)
{
    return console_open_tty_within(client, CON_WAIT, &t->ch, &t->data);
}

static void close_tty(struct tty_client *t)
{
    jam_handle_close(t->ch);
    jam_handle_close(t->data);
}

/* The next input message (waiting up to ms): its length, -1 if none came. */
static long in_msg(const struct tty_client *t, char *buf, uint32_t cap, unsigned ms)
{
    signals_t seen;
    if (jam_object_wait_one(t->data, SIG_READABLE, now() + (uint64_t)ms * NS_PER_MS, &seen) != OK)
        return -1;
    uint32_t n = 0, nh = 0;
    if (drv_channel_read(t->data, buf, cap, &n, NULL, 0, &nh) != OK)
        return -1;
    return (long)n;
}

static bool in_is(const struct tty_client *t, const char *want)
{
    char buf[256];
    long n = in_msg(t, buf, sizeof(buf), STEP_MS);
    CHECK_EQ(n, (long)strlen(want));
    CHECK(!memcmp(buf, want, (size_t)n));
    return true;
}

static bool out_write(const struct tty_client *t, const char *s)
{
    CHECK_ST(jam_channel_write(t->data, s, (uint32_t)strlen(s), NULL, 0), OK);
    return true;
}

static bool get(const struct tty_client *t, struct ld_termios *s)
{
    CHECK_ST(tty_get_attr_within(t->ch, CON_WAIT, &s->iflag, &s->oflag, &s->cflag, &s->lflag,
                                 s->cc, &s->ispeed, &s->ospeed), OK);
    return true;
}

static status_t set(const struct tty_client *t, uint8_t when, const struct ld_termios *s)
{
    return tty_set_attr_within(t->ch, CON_WAIT, when, s->iflag, s->oflag, s->cflag, s->lflag,
                               s->cc, s->ispeed, s->ospeed);
}

/* The events the tty channel brought. */
struct seen_events {
    unsigned sizes, signals;
    uint16_t cols, rows;
    uint8_t  sig;
};

static void ev_size(void *ctx, uint16_t cols, uint16_t rows)
{
    struct seen_events *e = ctx;
    e->sizes++;
    e->cols = cols;
    e->rows = rows;
}

static void ev_signal(void *ctx, uint8_t sig)
{
    struct seen_events *e = ctx;
    e->signals++;
    e->sig = sig;
}

/* Wait up to ms for an event, then read all that came. */
static void events(const struct tty_client *t, struct seen_events *e, unsigned ms)
{
    static const struct tty_events tab = { .size = ev_size, .signal = ev_signal };
    signals_t seen;
    memset(e, 0, sizeof(*e));
    if (jam_object_wait_one(t->ch, SIG_READABLE, now() + (uint64_t)ms * NS_PER_MS, &seen) != OK)
        return;
    while (tty_read_event(t->ch, &tab, e) == OK)
        ;
}

/* ---- 1. canonical mode ------------------------------------------------------------- */

static bool canonical_flow(void)
{
    struct tty_client t;
    CHECK_ST(open_tty(rig.admin, &t), OK);
    struct ld_termios s;
    CHECK(get(&t, &s));
    CHECK(s.lflag & LD_ICANON && s.lflag & LD_ECHO && s.lflag & LD_ISIG);
    CHECK(s.oflag & LD_ONLCR && s.iflag & LD_ICRNL);
    CHECK_EQ(s.cc[LD_VINTR], 3);
    CHECK(rig_typed("hellx\x7fo\r", 8));
    CHECK(rig_expect("hellx\b \bo\r\n", STEP_MS));
    CHECK(in_is(&t, "hello\n"));
    CHECK(out_write(&t, "two\nlines\n"));
    CHECK(rig_expect("two\r\nlines\r\n", STEP_MS));
    CHECK(rig_typed("\x04", 1));   /* Ctrl+D at the start of a line */
    CHECK(in_is(&t, ""));
    uint16_t c = 0, r = 0;
    CHECK_ST(tty_get_size_within(t.ch, CON_WAIT, &c, &r), OK);
    CHECK(c == 100 && r == 30);
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 132, 43), OK);
    struct seen_events e;
    events(&t, &e, STEP_MS);
    CHECK(e.sizes == 1 && e.cols == 132 && e.rows == 43);
    /* Ctrl+C: no shell's keys below this ADMIN client's tty, so it is told. */
    CHECK(rig_typed("doomed\x03", 7));
    CHECK(rig_expect("doomed^C", STEP_MS));
    events(&t, &e, STEP_MS);
    CHECK(e.signals == 1 && e.sig == TTY_SIG_INT);
    CHECK(rig_typed("after\r", 6));
    CHECK(in_is(&t, "after\n"));   /* "doomed" was flushed */
    close_tty(&t);
    return true;
}

bool t_con_tty_canonical(void)
{
    const char *argv[] = { "console", "conn", "cols=100", "rows=30" };
    bool ok = rig_start(argv, 4, false) && canonical_flow();
    return rig_stop(ok) && ok;
}

/* ---- 2. raw mode, drain and flush ------------------------------------------------------ */

static bool raw_settings(const struct tty_client *t)
{
    struct ld_termios s;
    CHECK(get(t, &s));
    s.lflag &= ~(LD_ICANON | LD_ECHO | LD_ISIG);
    s.cc[LD_VMIN] = 1;
    CHECK_ST(set(t, TTY_WHEN_NOW, &s), OK);
    CHECK_ST(set(t, 3, &s), ERR_INVALID_ARGS);
    CHECK_ST(tty_flush_within(t->ch, CON_WAIT, 3), ERR_INVALID_ARGS);
    CHECK(rig_typed("a\x03" "b", 3));
    char got[8];
    size_t n = 0;
    while (n < 3) {
        long k = in_msg(t, got + n, (uint32_t)(sizeof(got) - n), STEP_MS);
        CHECK(k > 0);
        n += (size_t)k;
    }
    CHECK(n == 3 && !memcmp(got, "a\x03" "b", 3));   /* Ctrl+C is the byte 3, no signal */
    struct seen_events e;
    events(t, &e, 200);
    CHECK_EQ(e.signals, 0u);
    rig_settle(100);
    CHECK(!rig_saw("a"));   /* no echo */
    return true;
}

static bool drain_and_flush(const struct tty_client *t)
{
    /* After a drain, the output written before it has been shown. */
    CHECK(out_write(t, "before-drain"));
    struct ld_termios s;
    CHECK(get(t, &s));
    CHECK_ST(set(t, TTY_WHEN_DRAIN, &s), OK);
    CHECK(rig_expect("before-drain", STEP_MS));
    CHECK(out_write(t, "and-more"));
    CHECK_ST(tty_drain_within(t->ch, CON_WAIT), OK);
    CHECK(rig_expect("and-more", STEP_MS));
    /* Input typed and not read, thrown away. */
    CHECK(rig_typed("zz", 2));
    signals_t seen;
    CHECK_ST(jam_object_wait_one(t->data, SIG_READABLE, now() + CON_WAIT, &seen), OK);
    CHECK_ST(tty_flush_within(t->ch, CON_WAIT, TTY_QUEUE_IN), OK);
    char buf[8];
    CHECK_EQ(in_msg(t, buf, sizeof(buf), 100), -1);
    /* TCSAFLUSH: the same, after the drain. */
    CHECK(rig_typed("yy", 2));
    CHECK_ST(jam_object_wait_one(t->data, SIG_READABLE, now() + CON_WAIT, &seen), OK);
    CHECK_ST(set(t, TTY_WHEN_FLUSH, &s), OK);
    CHECK_EQ(in_msg(t, buf, sizeof(buf), 100), -1);
    return true;
}

static bool raw_flow(void)
{
    struct tty_client t, next;
    CHECK_ST(open_tty(rig.admin, &t), OK);
    bool ok = raw_settings(&t) && drain_and_flush(&t);
    close_tty(&t);
    CHECK(ok);
    /* The settings are the terminal's: the next client finds raw mode. */
    CHECK_ST(open_tty(rig.admin, &next), OK);
    struct ld_termios s;
    CHECK(get(&next, &s));
    CHECK(!(s.lflag & LD_ICANON) && !(s.lflag & LD_ECHO));
    close_tty(&next);
    return true;
}

bool t_con_tty_raw(void)
{
    const char *argv[] = { "console", "conn" };
    bool ok = rig_start(argv, 2, false) && raw_flow();
    return rig_stop(ok) && ok;
}

/* ---- 3. the focus, with a shell below -------------------------------------------------- */

struct keys_seen {
    char     text[64];   /* the characters (codepoints below 0x80), in order */
    unsigned n, ctrl_c;
};

static void on_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    struct keys_seen *k = ctx;
    (void)usage;
    (void)state;
    (void)mods;
    if (cp == 3)
        k->ctrl_c++;
    else if (cp && cp < 0x80 && k->n + 1 < sizeof(k->text))
        k->text[k->n++] = (char)cp;
}

/* The keys that come on ch within ms, until want characters came. */
static void keys_until(handle_t ch, struct keys_seen *k, unsigned want, unsigned ms)
{
    static const struct keys_events tab = { .key = on_key };
    uint64_t until = now() + (uint64_t)ms * NS_PER_MS;
    memset(k, 0, sizeof(*k));
    while (k->n < want || (!want && !k->ctrl_c)) {
        signals_t seen;
        if (jam_object_wait_one(ch, SIG_READABLE, until, &seen) != OK)
            return;
        while (keys_read_event(ch, &tab, k) == OK)
            ;
    }
}

static bool shell_below(handle_t shell_keys, handle_t program)
{
    struct tty_client t;
    CHECK_ST(open_tty(program, &t), OK);
    CHECK(rig_typed("x\r", 2));
    CHECK(in_is(&t, "x\n"));
    /* ISIG's Ctrl+C: the shell's keys get it; the tty no signal. */
    CHECK(rig_typed("\x03", 1));
    struct keys_seen k;
    keys_until(shell_keys, &k, 0, STEP_MS);
    CHECK_EQ(k.ctrl_c, 1u);
    CHECK_EQ(k.n, 0u);   /* the "x" was the tty's */
    struct seen_events e;
    events(&t, &e, 200);
    CHECK_EQ(e.signals, 0u);
    /* Typed ahead and not read, and half typed: the shell's once the tty goes. */
    CHECK(rig_typed("ls\rab", 5));
    signals_t seen;
    CHECK_ST(jam_object_wait_one(t.data, SIG_READABLE, now() + CON_WAIT, &seen), OK);
    close_tty(&t);
    keys_until(shell_keys, &k, 5, STEP_MS);
    CHECK(k.n == 5 && !memcmp(k.text, "ls\nab", 5));
    return true;
}

static bool hostile(handle_t program)
{
    struct tty_client t;
    CHECK_ST(open_tty(program, &t), OK);
    static char big[TTY_OUT_MAX + 1];
    memset(big, 'q', sizeof(big));
    CHECK_ST(jam_channel_write(t.data, big, sizeof(big), NULL, 0), OK);
    signals_t seen;
    CHECK_ST(jam_object_wait_one(t.ch, SIG_PEER_CLOSED, now() + CON_WAIT, &seen), OK);
    close_tty(&t);
    /* Eight at once; the ninth is refused. */
    struct tty_client many[MAX_TTYS_TEST + 1];
    for (unsigned i = 0; i < MAX_TTYS_TEST; i++)
        CHECK_ST(open_tty(rig.admin, &many[i]), OK);
    CHECK_ST(open_tty(rig.admin, &many[MAX_TTYS_TEST]), ERR_NO_RESOURCES);
    for (unsigned i = 0; i < MAX_TTYS_TEST; i++)
        close_tty(&many[i]);
    return rig_size_is(rig.admin, 80, 24);   /* still serving */
}

static bool focus_flow(void)
{
    handle_t shell, program, shell_keys;
    CHECK_ST(console_new_client_within(rig.admin, CON_WAIT, SHELL_LEVEL, &shell), OK);
    CHECK_ST(console_open_keys_within(shell, CON_WAIT, &shell_keys), OK);
    CHECK_ST(console_new_client_within(shell, CON_WAIT, PROGRAM_LEVEL, &program), OK);
    bool ok = shell_below(shell_keys, program) && hostile(program);
    jam_handle_close(program);
    jam_handle_close(shell_keys);
    jam_handle_close(shell);
    return ok;
}

bool t_con_tty_focus(void)
{
    const char *argv[] = { "console", "conn" };
    bool ok = rig_start(argv, 2, false) && focus_flow();
    return rig_stop(ok) && ok;
}
