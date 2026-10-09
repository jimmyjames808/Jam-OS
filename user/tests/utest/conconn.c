/* utest: the console's connection back end (bin/console started with the
 * argument "conn": user/services/console/conn.c and connin.c, <termconn.h>).
 * Each test starts a console as init's session service will: its ADMIN
 * channel (SR_USER + 0), the master channel (CONN_MASTER_ROLE) and the
 * control channel (CONN_CTL_ROLE) are ours; the "remote terminal" is this
 * file, typing into the master and reading what it shows.
 *
 * t_con_conn_shell: a native shell (bin/shell, on a SHELL client of the
 * console) runs on it: the size from the arguments and from the control
 * channel (a resize, a zero, a size past the limits); the prompt arrives
 * as bytes; `echo` (its output's LF sent as CR LF), `ls /boot` and Ctrl+C
 * on `sleep 100` driven by typing into the master; a bracketed paste with
 * a line break in it is held until Enter (the markers reach the shell, which
 * asked for them); closing the master ends the console with 0 and the
 * shell with 2.
 *
 * t_con_conn_log: a client's show_log gets the kernel log's lines from the
 * console's root (KLOG alone) as text, a control character in one shown as
 * '?', and no line from before the console started.
 *
 * t_con_conn_hostile: junk, a handle and escape sequences without an end on
 * the master, junk on the control channel, the control channel closed: the
 * console keeps answering; then the master closed.
 *
 * t_con_conn_backpressure: clients write more than the master's queue
 * holds while nobody reads it: the console stops serving them (a write
 * waits) and, once the master is read, every byte arrives, none dropped. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/conn_ctl.h>
#include <idl/console.h>
#include <os.h>
#include <termconn.h>
#include "utest.h"

#define CON_WAIT   (5 * NS_PER_S)
#define STEP_MS    10000u
#define SHELL_LEVEL 1   /* console.new_client: L_SHELL (user/services/console/console.h) */
#define OUT_CAP    (1u << 16)

struct conn {
    handle_t job, proc;       /* the console */
    handle_t admin;           /* its ADMIN client */
    handle_t master, ctl;     /* ours */
    char     out[OUT_CAP];    /* what the master gave us (the last OUT_CAP bytes) */
    size_t   len, from;       /* its length, and where the next expect looks from */
};

static struct conn k;

/* Start the console with argv (argc entries) and the kernel log if klog. */
static bool start(const char *const *argv, int argc, bool klog)
{
    handle_t admin_theirs, master_theirs, ctl_theirs, root = startup_handle(SR_RESOURCE);
    memset(&k, 0, sizeof(k));
    CHECK_ST(new_job(&k.job), OK);
    CHECK_ST(jam_channel_create(&k.admin, &admin_theirs), OK);
    CHECK_ST(jam_channel_create(&k.master, &master_theirs), OK);
    CHECK_ST(jam_channel_create(&k.ctl, &ctl_theirs), OK);
    struct spawn_handle x[4] = { { SR_USER + 0, admin_theirs }, { CONN_MASTER_ROLE, master_theirs },
                                 { CONN_CTL_ROLE, ctl_theirs } };
    unsigned nx = 3;
    handle_t klog_root;
    if (klog) {
        CHECK_ST(jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_ROOT_KLOG, &klog_root), OK);
        x[nx++] = (struct spawn_handle){ SR_RESOURCE, klog_root };
    }
    struct spawn_args a = { .path = "bin/console", .name = "console-conn", .argc = argc,
                            .argv = argv, .job = k.job, .extra = x, .nextra = nx };
    CHECK_ST(spawn(&a, &k.proc), OK);
    return true;
}

static void stop(bool ok)
{
    if (!ok)
        (void)jam_job_kill(k.job);
    jam_handle_close(k.proc);
    jam_handle_close(k.admin);
    jam_handle_close(k.master);
    jam_handle_close(k.ctl);
    jam_handle_close(k.job);
}

/* ---- the remote terminal ----------------------------------------------------------- */

/* One message from the master, appended to out (until deadline). */
static bool take(uint64_t until)
{
    signals_t seen = 0;
    status_t st = jam_object_wait_one(k.master, SIG_READABLE | SIG_PEER_CLOSED, until, &seen);
    if (st != OK || !(seen & SIG_READABLE))
        return false;
    static uint8_t msg[CONN_OUT_MSG_MAX + 16];
    uint32_t n = 0, nh = 0;
    if (drv_channel_read(k.master, msg, sizeof(msg), &n, NULL, 0, &nh) != OK)
        return false;
    if (k.len + n > OUT_CAP) {   /* keep the newest half */
        size_t keep = OUT_CAP / 2, drop = k.len - keep;
        memmove(k.out, k.out + drop, keep);
        k.len = keep;
        k.from = k.from > drop ? k.from - drop : 0;
    }
    memcpy(k.out + k.len, msg, n);
    k.len += n;
    return true;
}

static void dump_tail(void)
{
    size_t from = k.len > 300 ? k.len - 300 : 0;
    printf("utest: the master's last bytes: \"");
    for (size_t i = from; i < k.len; i++) {
        uint8_t c = (uint8_t)k.out[i];
        if (c >= 0x20 && c < 0x7f)
            printf("%c", c);
        else
            printf("\\x%02x", c);
    }
    printf("\"\n");
}

/* Wait until the bytes needle (n of them) come on the master, after the
 * last match. */
static bool expect_n(const char *needle, size_t nl, unsigned ms)
{
    uint64_t until = now() + (uint64_t)ms * NS_PER_MS;
    for (;;) {
        for (size_t i = k.from; i + nl <= k.len; i++)
            if (!memcmp(k.out + i, needle, nl)) {
                k.from = i + nl;
                return true;
            }
        if (!take(until)) {
            printf("utest: %s: not seen on the master within %u ms: \"%s\"\n", utest_cur, ms,
                   needle);
            dump_tail();
            return false;
        }
    }
}

static bool expect(const char *needle, unsigned ms)
{
    return expect_n(needle, strlen(needle), ms);
}

/* Read the master for ms milliseconds, whatever comes. */
static void settle(unsigned ms)
{
    uint64_t until = now() + (uint64_t)ms * NS_PER_MS;
    while (take(until))
        ;
}

static bool typed(const char *s, size_t n)
{
    CHECK_ST(jam_channel_write(k.master, s, (uint32_t)n, NULL, 0), OK);
    return true;
}

static bool saw(const char *needle)
{
    size_t nl = strlen(needle);
    for (size_t i = k.from; i + nl <= k.len; i++)
        if (!memcmp(k.out + i, needle, nl))
            return true;
    return false;
}

static bool size_is(handle_t client, uint16_t cols, uint16_t rows)
{
    uint16_t c = 0, r = 0;
    CHECK_ST(console_size_within(client, CON_WAIT, &c, &r), OK);
    CHECK_EQ(c, cols);
    CHECK_EQ(r, rows);
    return true;
}

static bool exit_code_is(handle_t proc, int code)
{
    struct process_info info;
    CHECK_ST(spawn_wait(proc, CON_WAIT, &info), OK);
    CHECK_EQ(info.killed, 0);
    CHECK_EQ(info.exit_code, code);
    return true;
}

/* ---- 1. a shell on it --------------------------------------------------------------- */

#define PROMPT "\033[0m> "   /* the end of the shell's prompt (user/services/shell/main.c) */

static bool sizes(void)
{
    CHECK(size_is(k.admin, 100, 30));   /* the arguments' */
    CHECK_ST(conn_ctl_resize_within(k.ctl, CON_WAIT, 120, 40), OK);
    CHECK(size_is(k.admin, 120, 40));
    CHECK_ST(conn_ctl_resize_within(k.ctl, CON_WAIT, 0, 5), ERR_INVALID_ARGS);
    CHECK_ST(conn_ctl_resize_within(k.ctl, CON_WAIT, 5, 0), ERR_INVALID_ARGS);
    CHECK(size_is(k.admin, 120, 40));   /* unchanged */
    CHECK_ST(conn_ctl_resize_within(k.ctl, CON_WAIT, 60000, 60000), OK);
    CHECK(size_is(k.admin, CONN_COLS_MAX, CONN_ROWS_MAX));   /* clamped */
    CHECK_ST(conn_ctl_resize_within(k.ctl, CON_WAIT, 100, 30), OK);
    return size_is(k.admin, 100, 30);
}

static bool start_shell(handle_t *proc)
{
    handle_t client;
    CHECK_ST(console_new_client_within(k.admin, CON_WAIT, SHELL_LEVEL, &client), OK);
    const char *argv[] = { "bin/shell" };
    struct spawn_handle x = { SR_CONSOLE, client };
    struct spawn_args a = { .path = "bin/shell", .argc = 1, .argv = argv, .job = k.job,
                            .extra = &x, .nextra = 1, .ns = NS_ALL };
    CHECK_ST(spawn(&a, proc), OK);
    return true;
}

/* The shell runs line and ends with its prompt again; want is in what it printed. */
static bool runs(const char *line, const char *want)
{
    CHECK(typed(line, strlen(line)));
    CHECK(typed("\r", 1));
    CHECK(expect(want, STEP_MS));
    return expect(PROMPT, STEP_MS);
}

static bool typing(void)
{
    /* The output's LF comes as CR LF: the line after the echoed command. */
    CHECK(runs("echo hello-conn", "\r\nhello-conn\r\n"));
    return runs("ls /boot", "init.cfg");
}

static bool paste_and_interrupt(void)
{
    /* A paste with a line break, between the markers the shell asked for
     * (ESC [ ? 2004 h in its output): held on the line until Enter. */
    static const char paste[] = "\033[200~echo held-line\r\033[201~";
    CHECK(typed(paste, sizeof(paste) - 1));
    settle(500);
    CHECK(!saw("\r\nheld-line\r\n"));
    CHECK(typed("\r", 1));
    CHECK(expect("\r\nheld-line\r\n", STEP_MS));
    CHECK(expect(PROMPT, STEP_MS));
    /* Ctrl+C ends a command that would run for 100 s. */
    CHECK(typed("sleep 100\r", 10));
    settle(300);
    uint64_t t0 = now();
    CHECK(typed("\x03", 1));
    CHECK(expect("^C", STEP_MS));
    CHECK(expect(PROMPT, STEP_MS));
    CHECK(now() - t0 < 5 * NS_PER_S);
    return true;
}

static bool shell_flow(void)
{
    handle_t shell;
    CHECK(sizes());
    CHECK(start_shell(&shell));
    CHECK(expect(PROMPT, STEP_MS));
    bool ok = typing() && paste_and_interrupt();
    if (ok) {   /* the terminal hangs up: the console and the shell end */
        jam_handle_close(k.master);
        k.master = HANDLE_INVALID;
        ok = exit_code_is(k.proc, 0) && exit_code_is(shell, 2);
    }
    jam_handle_close(shell);
    return ok;
}

bool t_con_conn_shell(void)
{
    const char *argv[] = { "console", "conn", "cols=100", "rows=30" };
    bool ok = start(argv, 4, false) && shell_flow();
    stop(ok);
    return ok;
}

/* ---- 2. the kernel log -------------------------------------------------------------- */

static bool log_flow(void)
{
    handle_t client;
    static const uint8_t any[32];   /* every process's lines */
    printf("utest: conn-log-before\n");   /* logged before show_log: not shown */
    CHECK_ST(console_new_client_within(k.admin, CON_WAIT, SHELL_LEVEL, &client), OK);
    CHECK_ST(console_show_log_within(client, CON_WAIT, 1, any), OK);
    printf("utest: conn-log-marker \033[31mred\033[0m end\n");
    CHECK(expect("conn-log-marker ", STEP_MS));
    CHECK(expect("?[31mred?[0m end\r\n", STEP_MS));
    CHECK(!saw("conn-log-before"));
    for (size_t i = 0; i < k.len; i++)
        CHECK(k.out[i] != 0x1b);   /* an escape sequence in a log line never reaches the terminal */
    CHECK_ST(console_show_log_within(client, CON_WAIT, 0, any), OK);
    jam_handle_close(client);
    return true;
}

bool t_con_conn_log(void)
{
    handle_t root = startup_handle(SR_RESOURCE), probe;
    if (!root || jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_ROOT_KLOG, &probe) != OK) {
        printf("utest: %s: no RIGHT_ROOT_KLOG on our root: skipped\n", utest_cur);
        return true;
    }
    jam_handle_close(probe);
    const char *argv[] = { "console", "conn" };
    bool ok = start(argv, 2, true) && log_flow();
    stop(ok);
    return ok;
}

/* ---- 3. junk ------------------------------------------------------------------------- */

/* Pseudo-random bytes with plenty of ESC and '[' (a fixed seed: the same each run). */
static void junk(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        uint8_t b = (uint8_t)(seed >> 24);
        p[i] = b < 40 ? 0x1b : b < 80 ? '[' : b < 100 ? '2' : b < 120 ? '~' : b;
    }
}

static bool hostile_master(void)
{
    static uint8_t buf[1u << 16];
    for (uint32_t m = 0; m < 6; m++) {
        junk(buf, m & 1 ? sizeof(buf) : 777, 0x1234 + m);
        CHECK_ST(jam_channel_write(k.master, buf, m & 1 ? (uint32_t)sizeof(buf) : 777, NULL, 0), OK);
    }
    static const char unfinished[] = "\033[\033[\033[200\033[2\033";   /* sequences without an end */
    CHECK(typed(unfinished, sizeof(unfinished) - 1));
    handle_t ev;
    CHECK_ST(jam_event_create(&ev), OK);
    /* A message with a handle: the console closes it and goes on (the
     * kernel may also refuse to carry it). */
    (void)jam_channel_write(k.master, "x", 1, &ev, 1);
    return size_is(k.admin, 80, 24);
}

static bool hostile_ctl(void)
{
    static const uint8_t garbage[3][9] = {
        { 1, 2, 3 },                                   /* shorter than a header */
        { 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 1 },     /* an ordinal of nothing */
        { 7, 0, 0, 0, 1, 0, 42, 0, 9 },                /* txid 7, resize, a short body */
    };
    for (unsigned i = 0; i < 3; i++)
        CHECK_ST(jam_channel_write(k.ctl, garbage[i], i ? 9 : 3, NULL, 0), OK);
    CHECK_ST(conn_ctl_resize_within(k.ctl, CON_WAIT, 90, 25), OK);   /* it still answers */
    CHECK(size_is(k.admin, 90, 25));
    jam_handle_close(k.ctl);   /* its owner lets it go: the size stays */
    k.ctl = HANDLE_INVALID;
    return size_is(k.admin, 90, 25);
}

bool t_con_conn_hostile(void)
{
    const char *argv[] = { "console", "conn" };
    bool ok = start(argv, 2, false) && hostile_master() && hostile_ctl();
    if (ok) {   /* the junk was typed to nobody: the console still hangs up on request */
        jam_handle_close(k.master);
        k.master = HANDLE_INVALID;
        ok = exit_code_is(k.proc, 0);
    }
    stop(ok);
    return ok;
}

/* ---- 4. a master nobody reads -------------------------------------------------------- */

#define WRITE_BYTES 2048u    /* console.write's most */
#define WRITES      2600u    /* 5.3 MB: more than the master's 1024 messages of 4 KiB */

/* Reads the master until total bytes have come, all of them `x`; false if
 * fewer come in 5 s. */
static bool drained(uint64_t total)
{
    static uint8_t msg[CONN_OUT_MSG_MAX + 16];
    uint64_t got = 0, until = now() + CON_WAIT;
    while (got < total) {
        signals_t seen = 0;
        if (jam_object_wait_one(k.master, SIG_READABLE, until, &seen) != OK)
            break;
        uint32_t n = 0, nh = 0;
        if (drv_channel_read(k.master, msg, sizeof(msg), &n, NULL, 0, &nh) != OK)
            break;
        CHECK(n <= CONN_OUT_MSG_MAX);
        for (uint32_t i = 0; i < n; i++)
            CHECK(msg[i] == 'x');
        got += n;
        until = now() + CON_WAIT;
    }
    CHECK_EQ(got, total);
    return true;
}

static bool flood(void)
{
    static uint8_t buf[WRITE_BYTES];
    handle_t other;
    memset(buf, 'x', sizeof(buf));
    CHECK_ST(console_new_client_within(k.admin, CON_WAIT, SHELL_LEVEL, &other), OK);
    uint64_t done = 0;
    status_t st = OK;
    for (unsigned i = 0; i < WRITES && st == OK; i++) {
        st = console_write_within(k.admin, 300 * NS_PER_MS, buf, sizeof(buf));
        done += st == OK;
    }
    CHECK_ST(st, ERR_TIMED_OUT);   /* the console stopped serving: the writer waits */
    CHECK(done > 1000);
    /* Nothing is read yet, so a client of another channel waits too. */
    uint16_t c, r;
    CHECK_ST(console_size_within(other, 200 * NS_PER_MS, &c, &r), ERR_TIMED_OUT);
    /* The timed-out write was taken too: done + 1 writes in all. */
    CHECK(drained((done + 1) * WRITE_BYTES));
    CHECK(size_is(other, 80, 24));   /* serving again */
    jam_handle_close(other);
    return true;
}

bool t_con_conn_backpressure(void)
{
    const char *argv[] = { "console", "conn" };
    bool ok = start(argv, 2, false) && flood();
    stop(ok);
    return ok;
}
