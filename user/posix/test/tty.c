/* posixtest's tty section: a POSIX program on a terminal (the console's
 * POSIX side, docs/M13-PLAN.md "The terminal layer"; user/posix/sys/tty.c).
 *
 * This process starts a connection console (bin/console "conn") and is
 * its remote terminal: it types into the master and reads what the
 * terminal shows. The program on the terminal is this program again, in
 * a child mode:
 *   - --child-tty, started with a console channel (SR_CONSOLE, a PROGRAM
 *     client) and no hand-over, as the shell starts a program: 0 to 2 are
 *     the terminal; isatty, ttyname, tcgetattr's defaults, TIOCGWINSZ;
 *     a canonical read of a typed line (its echo shown); raw mode by
 *     tcsetattr (Ctrl+C is the byte 3), poll for writing and for the
 *     typing; MIN 0 and TIME 2 (a read that
 *     ends with nothing after 0.2 s); O_NONBLOCK's EAGAIN; canonical mode
 *     back after a drain (TCSADRAIN);
 *   - --child-tty-handover, given a terminal as descriptor 0 in a
 *     hand-over (POSIXFDS_TTY, the two handles this process got from
 *     console.open_tty): it writes through it.
 * Not here yet: a resize as SIGWINCH (the size event reaches libc, whose
 * signal thread is B4's: tty.h's tty_take_signals). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/console.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <posixfds.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termconn.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define WAIT_NS   (10 * NS_PER_S)
#define OUT_CAP   8192
#define PROGRAM   2   /* console.new_client's PROGRAM level */

/* ---- the child: a program on the terminal -------------------------------------------- */

static void say(const char *s)
{
    (void)write(1, s, strlen(s));
}

static int64_t ms_since(const struct timespec *a)
{
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}

static void child_queries(void)
{
    CHECK(isatty(0) && isatty(1) && isatty(2));
    char name[32];
    CHECK(ttyname_r(0, name, sizeof(name)) == 0 && !strcmp(name, "/dev/tty"));
    CHECK(ttyname_r(0, name, 3) == ERANGE);
    struct termios t;
    CHECK(tcgetattr(0, &t) == 0);
    CHECK((t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));
    CHECK(t.c_cc[VINTR] == 3 && t.c_cc[VMIN] == 1 && (t.c_oflag & ONLCR));
    CHECK(cfgetospeed(&t) == B38400);
    struct winsize w;
    CHECK(ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col == 90 && w.ws_row == 20);
}

static void child_canonical(void)
{
    char line[64] = { 0 };
    say("ready-canon\n");
    ssize_t n = read(0, line, sizeof(line) - 1);
    CHECK(n == 11 && !strcmp(line, "typed line\n"));
    char out[96];
    snprintf(out, sizeof(out), "got:%.*s|\n", n > 0 ? (int)n - 1 : 0, line);
    say(out);
    /* A read shorter than the line: the rest comes next. */
    say("ready-short\n");
    CHECK(read(0, line, 3) == 3 && !memcmp(line, "abc", 3));
    CHECK(read(0, line, sizeof(line)) == 4 && !memcmp(line, "def\n", 4));
}

static void child_raw(struct termios *saved)
{
    struct termios t = *saved;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 3;
    t.c_cc[VTIME] = 0;
    CHECK(tcsetattr(0, TCSANOW, &t) == 0);
    struct pollfd p = { .fd = 1, .events = POLLOUT };
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT));   /* the data channel has room */
    p = (struct pollfd){ .fd = 0, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);   /* nothing typed yet */
    say("ready-raw\n");
    CHECK(poll(&p, 1, 10000) == 1 && (p.revents & POLLIN));   /* woken by the typing */
    unsigned char b[8];
    CHECK(read(0, b, sizeof(b)) == 3 && b[0] == 'q' && b[1] == 3 && b[2] == 'w');
    /* MIN 0, TIME 2: nothing typed, the read ends after 0.2 s with 0. */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 2;
    CHECK(tcsetattr(0, TCSANOW, &t) == 0);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(read(0, b, sizeof(b)) == 0);
    CHECK(ms_since(&t0) >= 150);
    int fl = fcntl(0, F_GETFL);
    CHECK(fcntl(0, F_SETFL, fl | O_NONBLOCK) == 0);
    errno = 0;
    CHECK(read(0, b, 1) == -1 && errno == EAGAIN);
    CHECK(fcntl(0, F_SETFL, fl) == 0);
    CHECK(tcsetattr(0, TCSADRAIN, saved) == 0);
    struct termios back;
    CHECK(tcgetattr(0, &back) == 0 && (back.c_lflag & ICANON));
    CHECK(tcdrain(1) == 0 && tcflush(0, TCIFLUSH) == 0);
}

int pt_child_tty(void)
{
    struct termios saved;
    child_queries();
    CHECK(tcgetattr(0, &saved) == 0);
    child_canonical();
    child_raw(&saved);
    say(pt_failures() ? "child-failed\n" : "child-done\n");
    return pt_failures() ? 1 : 0;
}

int pt_child_tty_handover(void)
{
    CHECK(isatty(0) && !isatty(1));
    CHECK(write(0, "via-handover\n", 13) == 13);
    return pt_failures() ? 1 : 0;
}

/* ---- this process: the console and its remote terminal ---------------------------------- */

static struct {
    handle_t job, proc, admin, master, ctl;
    char     out[OUT_CAP];
    size_t   len, from;
} con;

static bool start_console(void)
{
    handle_t a, m, c;
    if (jam_job_create(startup_handle(SR_JOB), 0, &con.job) != OK ||
        jam_channel_create(&con.admin, &a) != OK || jam_channel_create(&con.master, &m) != OK ||
        jam_channel_create(&con.ctl, &c) != OK)
        return false;
    const char *argv[] = { "console", "conn", "cols=90", "rows=20" };
    struct spawn_handle x[] = { { SR_USER + 0, a }, { CONN_MASTER_ROLE, m }, { CONN_CTL_ROLE, c } };
    struct spawn_args sa = { .path = "bin/console", .name = "console-tty", .argc = 4,
                             .argv = argv, .job = con.job, .extra = x, .nextra = 3 };
    return spawn(&sa, &con.proc) == OK;
}

static void stop_console(void)
{
    jam_job_kill(con.job);
    jam_handle_close(con.proc);
    jam_handle_close(con.admin);
    jam_handle_close(con.master);
    jam_handle_close(con.ctl);
    jam_handle_close(con.job);
}

/* What the terminal shows, read until needle comes (after the last match). */
static bool shows(const char *needle)
{
    size_t nl = strlen(needle);
    uint64_t until = now() + WAIT_NS;
    for (;;) {
        for (size_t i = con.from; i + nl <= con.len; i++)
            if (!memcmp(con.out + i, needle, nl)) {
                con.from = i + nl;
                return true;
            }
        signals_t seen;
        if (jam_object_wait_one(con.master, SIG_READABLE, until, &seen) != OK)
            break;
        static uint8_t msg[CONN_OUT_MSG_MAX];
        uint32_t n = 0, nh = 0;
        struct channel_read_args r = {
            .h = con.master, .rd_cap = sizeof(msg), .rd_bytes = (uint64_t)(uintptr_t)msg,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        if (jam_channel_read(&r) != OK || con.len + n > sizeof(con.out))
            break;
        memcpy(con.out + con.len, msg, n);
        con.len += n;
    }
    printf("posixtest: tty: \"%s\" not shown; the terminal showed %zu bytes\n", needle, con.len);
    return false;
}

static void typed(const char *s, size_t n)
{
    CHECK(jam_channel_write(con.master, s, (uint32_t)n, NULL, 0) == OK);
}

static handle_t start_child(const char *mode, handle_t console, const struct posixfds_out *ho)
{
    const char *argv[] = { "posixtest", mode, NULL };
    struct spawn_handle x = { SR_CONSOLE, console };
    struct spawn_args a = {
        .path = "bin/posixtest", .argc = 2, .argv = argv, .job = startup_handle(SR_JOB),
        .extra = console ? &x : NULL, .nextra = console ? 1 : 0, .handover = ho,
    };
    handle_t proc = HANDLE_INVALID;
    CHECK(spawn(&a, &proc) == OK);
    return proc;
}

static int child_end(handle_t proc)
{
    struct process_info info;
    status_t st = spawn_wait(proc, WAIT_NS, &info);
    CHECK(st == OK);
    jam_handle_close(proc);
    return st == OK && !info.killed ? (int)info.exit_code : -1;
}

static void drive_child(void)
{
    handle_t program = HANDLE_INVALID;
    CHECK(console_new_client_within(con.admin, WAIT_NS, PROGRAM, &program) == OK);
    handle_t proc = start_child("--child-tty", program, NULL);
    CHECK(shows("ready-canon\r\n"));
    typed("typed line\r", 11);
    CHECK(shows("typed line\r\n"));   /* the echo */
    CHECK(shows("got:typed line|\r\n"));
    CHECK(shows("ready-short\r\n"));
    typed("abcdef\r", 7);
    CHECK(shows("ready-raw\r\n"));
    typed("q\x03w", 3);
    CHECK(shows("child-done\r\n"));
    CHECK(child_end(proc) == 0);
}

static void handover_child(void)
{
    handle_t hs[2];
    CHECK(console_open_tty_within(con.admin, WAIT_NS, &hs[0], &hs[1]) == OK);
    const struct posixfds_desc fds[] = {
        { .fd = 0, .kind = POSIXFDS_TTY, .status = O_RDWR, .handles = hs },
    };
    const struct posixfds_out ho = { .cwd = "/", .fds = fds, .nfds = 1 };
    handle_t proc = start_child("--child-tty-handover", HANDLE_INVALID, &ho);
    CHECK(shows("via-handover\r\n"));
    CHECK(child_end(proc) == 0);
}

void pt_tty(char **argv)
{
    (void)argv;
    if (!start_console()) {
        CHECK(!"bin/console conn started");
        return;
    }
    drive_child();
    handover_child();
    stop_console();
}
