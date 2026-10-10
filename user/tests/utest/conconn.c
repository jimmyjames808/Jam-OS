/* utest: the console's connection back end (bin/console started with the
 * argument "conn": user/services/console/conn.c and connin.c, <termconn.h>).
 * Each test starts a console as init's session service will: its ADMIN
 * channel (SR_USER + 0), the master channel (CONN_MASTER_ROLE) and the
 * control channel (CONN_CTL_ROLE) are ours; the "remote terminal" is
 * conrig.c, typing into the master and reading what it shows.
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
 * waits) and, once the master is read, every byte arrives, none dropped.
 *
 * t_con_conn_stop: once rig_stop returns, the console has ended and our
 * job holds the handles, threads and message bytes it held before
 * rig_start: nothing of it is left for the next test to count. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/conn_ctl.h>
#include <idl/console.h>
#include <os.h>
#include <termconn.h>
#include "conrig.h"
#include "utest.h"

/* ---- 1. a shell on it --------------------------------------------------------------- */

#define PROMPT "\033[0m> "   /* the end of the shell's prompt (user/services/shell/main.c) */

static bool sizes(void)
{
    CHECK(rig_size_is(rig.admin, 100, 30));   /* the arguments' */
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 120, 40), OK);
    CHECK(rig_size_is(rig.admin, 120, 40));
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 0, 5), ERR_INVALID_ARGS);
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 5, 0), ERR_INVALID_ARGS);
    CHECK(rig_size_is(rig.admin, 120, 40));   /* unchanged */
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 60000, 60000), OK);
    CHECK(rig_size_is(rig.admin, CONN_COLS_MAX, CONN_ROWS_MAX));   /* clamped */
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 100, 30), OK);
    return rig_size_is(rig.admin, 100, 30);
}

static bool start_shell(handle_t *proc)
{
    handle_t client;
    CHECK_ST(console_new_client_within(rig.admin, CON_WAIT, SHELL_LEVEL, &client), OK);
    const char *argv[] = { "bin/shell" };
    struct spawn_handle x = { SR_CONSOLE, client };
    struct spawn_args a = { .path = "bin/shell", .argc = 1, .argv = argv, .job = rig.job,
                            .extra = &x, .nextra = 1, .ns = NS_ALL };
    CHECK_ST(spawn(&a, proc), OK);
    return true;
}

/* The shell runs line and ends with its prompt again; want is in what it printed. */
static bool runs(const char *line, const char *want)
{
    CHECK(rig_typed(line, strlen(line)));
    CHECK(rig_typed("\r", 1));
    CHECK(rig_expect(want, STEP_MS));
    return rig_expect(PROMPT, STEP_MS);
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
    CHECK(rig_typed(paste, sizeof(paste) - 1));
    rig_settle(500);
    CHECK(!rig_saw("\r\nheld-line\r\n"));
    CHECK(rig_typed("\r", 1));
    CHECK(rig_expect("\r\nheld-line\r\n", STEP_MS));
    CHECK(rig_expect(PROMPT, STEP_MS));
    /* Ctrl+C ends a command that would run for 100 s. */
    CHECK(rig_typed("sleep 100\r", 10));
    rig_settle(300);
    uint64_t t0 = now();
    CHECK(rig_typed("\x03", 1));
    CHECK(rig_expect("^C", STEP_MS));
    CHECK(rig_expect(PROMPT, STEP_MS));
    CHECK(now() - t0 < 5 * NS_PER_S);
    return true;
}

static bool shell_flow(void)
{
    handle_t shell;
    CHECK(sizes());
    CHECK(start_shell(&shell));
    CHECK(rig_expect(PROMPT, STEP_MS));
    bool ok = typing() && paste_and_interrupt();
    if (ok) {   /* the terminal hangs up: the console and the shell end */
        jam_handle_close(rig.master);
        rig.master = HANDLE_INVALID;
        ok = rig_exit_code_is(rig.proc, 0) && rig_exit_code_is(shell, 2);
    }
    jam_handle_close(shell);
    return ok;
}

bool t_con_conn_shell(void)
{
    const char *argv[] = { "console", "conn", "cols=100", "rows=30" };
    bool ok = rig_start(argv, 4, false) && shell_flow();
    return rig_stop(ok) && ok;
}

/* ---- 2. the kernel log -------------------------------------------------------------- */

static bool log_flow(void)
{
    handle_t client;
    static const uint8_t any[32];   /* every process's lines */
    printf("utest: conn-log-before\n");   /* logged before show_log: not shown */
    CHECK_ST(console_new_client_within(rig.admin, CON_WAIT, SHELL_LEVEL, &client), OK);
    CHECK_ST(console_show_log_within(client, CON_WAIT, 1, any), OK);
    printf("utest: conn-log-marker \033[31mred\033[0m end\n");
    CHECK(rig_expect("conn-log-marker ", STEP_MS));
    CHECK(rig_expect("?[31mred?[0m end\r\n", STEP_MS));
    CHECK(!rig_saw("conn-log-before"));
    for (size_t i = 0; i < rig.len; i++)
        CHECK(rig.out[i] != 0x1b);   /* an escape sequence in a log line never reaches the terminal */
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
    bool ok = rig_start(argv, 2, true) && log_flow();
    return rig_stop(ok) && ok;
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
        CHECK_ST(jam_channel_write(rig.master, buf, m & 1 ? (uint32_t)sizeof(buf) : 777, NULL, 0), OK);
    }
    static const char unfinished[] = "\033[\033[\033[200\033[2\033";   /* sequences without an end */
    CHECK(rig_typed(unfinished, sizeof(unfinished) - 1));
    handle_t ev;
    CHECK_ST(jam_event_create(&ev), OK);
    /* A message with a handle: the console closes it and goes on (the
     * kernel may also refuse to carry it). */
    (void)jam_channel_write(rig.master, "x", 1, &ev, 1);
    return rig_size_is(rig.admin, 80, 24);
}

static bool hostile_ctl(void)
{
    static const uint8_t garbage[3][9] = {
        { 1, 2, 3 },                                   /* shorter than a header */
        { 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 1 },     /* an ordinal of nothing */
        { 7, 0, 0, 0, 1, 0, 42, 0, 9 },                /* txid 7, resize, a short body */
    };
    for (unsigned i = 0; i < 3; i++)
        CHECK_ST(jam_channel_write(rig.ctl, garbage[i], i ? 9 : 3, NULL, 0), OK);
    CHECK_ST(conn_ctl_resize_within(rig.ctl, CON_WAIT, 90, 25), OK);   /* it still answers */
    CHECK(rig_size_is(rig.admin, 90, 25));
    jam_handle_close(rig.ctl);   /* its owner lets it go: the size stays */
    rig.ctl = HANDLE_INVALID;
    return rig_size_is(rig.admin, 90, 25);
}

bool t_con_conn_hostile(void)
{
    const char *argv[] = { "console", "conn" };
    bool ok = rig_start(argv, 2, false) && hostile_master() && hostile_ctl();
    if (ok) {   /* the junk was typed to nobody: the console still hangs up on request */
        jam_handle_close(rig.master);
        rig.master = HANDLE_INVALID;
        ok = rig_exit_code_is(rig.proc, 0);
    }
    return rig_stop(ok) && ok;
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
        if (jam_object_wait_one(rig.master, SIG_READABLE, until, &seen) != OK)
            break;
        uint32_t n = 0, nh = 0;
        if (drv_channel_read(rig.master, msg, sizeof(msg), &n, NULL, 0, &nh) != OK)
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
    CHECK_ST(console_new_client_within(rig.admin, CON_WAIT, SHELL_LEVEL, &other), OK);
    uint64_t done = 0;
    status_t st = OK;
    for (unsigned i = 0; i < WRITES && st == OK; i++) {
        st = console_write_within(rig.admin, 300 * NS_PER_MS, buf, sizeof(buf));
        done += st == OK;
    }
    CHECK_ST(st, ERR_TIMED_OUT);   /* the console stopped serving: the writer waits */
    CHECK(done > 1000);
    /* Nothing is read yet, so a client of another channel waits too. */
    uint16_t c, r;
    CHECK_ST(console_size_within(other, 200 * NS_PER_MS, &c, &r), ERR_TIMED_OUT);
    /* The timed-out write was taken too: done + 1 writes in all. */
    CHECK(drained((done + 1) * WRITE_BYTES));
    CHECK(rig_size_is(other, 80, 24));   /* serving again */
    jam_handle_close(other);
    return true;
}

bool t_con_conn_backpressure(void)
{
    const char *argv[] = { "console", "conn" };
    bool ok = rig_start(argv, 2, false) && flood();
    return rig_stop(ok) && ok;
}

/* ---- 5. stopping ---------------------------------------------------------------------- */

bool t_con_conn_stop(void)
{
    static const unsigned kinds[] = { JOB_LIMIT_HANDLES, JOB_LIMIT_THREADS, JOB_LIMIT_MSG_BYTES };
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    const char *argv[] = { "console", "conn" };
    bool ok = rig_start(argv, 2, false) && rig_size_is(rig.admin, 80, 24);
    CHECK(rig_stop(ok) && ok);
    CHECK_ST(info_of(own_job(), &after), OK);
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        CHECK_EQ(after.used[kinds[i]], before.used[kinds[i]]);
    return true;
}
