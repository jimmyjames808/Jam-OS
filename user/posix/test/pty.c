/* posixtest's pty and session sections: pseudo-terminals (posix_openpt,
 * grantpt, unlockpt, ptsname, openpty: /dev/ptmx and /dev/pts/N) and
 * init's sessions (<jamos/session.h>), from the master's side
 * (user/posix/sys/pty.c; docs/M13-PLAN.md "X1 notes").
 *
 * pty: a master and its slave: the names, stat, the sizes both ways
 * (TIOCSWINSZ on either), bytes both ways through the slave's line
 * discipline; a child on the slave (its 0 to 2, by posix_spawn's dup2):
 * the slave is its terminal (TIOCSCTTY, tcgetpgrp, tcgetsid, tcsetpgrp's
 * EPERM), Ctrl+C typed into the master is its SIGINT, and closing the
 * master is its SIGHUP; PTY_MAX masters at once and the next refused; a
 * slave that isn't ours; openpty, and the master's read ending in EIO once
 * the last slave has closed.
 *
 * session (only when asked: it needs /svc/session, a shell-mode init's,
 * which posixtest's list doesn't ask for: utest's session_posix starts it
 * with it):
 * an interactive session (the prompt, echo, a resize and TERM as a program
 * on it sees them, the master closed: waitpid says 2); a command (its
 * output read to the end, its status); kill's SIGTERM hangs one up, any
 * other signal does nothing.
 *
 * The child modes: --child-pty (on the slave), --child-winsize (prints its
 * terminal's size and TERM), --child-hup (writes /data/x1-hup.txt on its
 * terminal's SIGHUP: utest's session_hangup). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/session.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PTYS        8                    /* the layer's PTY_MAX */
#define WAIT_MS     10000
#define OUT_CAP     8192
#define PROMPT      "\033[0m> "          /* the end of the native shell's prompt */
#define HUP_FILE    "/data/x1-hup.txt"

extern char **environ;

/* ---- the child modes --------------------------------------------------------------- */

static volatile sig_atomic_t got_int, got_hup;

static void on_sig(int sig)
{
    if (sig == SIGINT)
        got_int++;
    else if (sig == SIGHUP)
        got_hup++;
}

static void say(const char *s)
{
    (void)write(1, s, strlen(s));
}

/* On the slave: it is our terminal; Ctrl+C is SIGINT; the master's close
 * SIGHUP (exit 42). */
int pt_child_pty(void)
{
    struct sigaction sa = { .sa_handler = on_sig };   /* no SA_RESTART: EINTR */
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGINT, &sa, NULL) == 0 && sigaction(SIGHUP, &sa, NULL) == 0);
    CHECK(isatty(0) && isatty(1) && isatty(2));
    CHECK(ioctl(0, TIOCSCTTY, 0) == 0);   /* it is already */
    CHECK(tcgetpgrp(0) == getpgrp() && tcgetsid(0) == getsid(0));
    errno = 0;
    CHECK(tcsetpgrp(0, 999999) == -1 && errno == EPERM);
    CHECK(tcsetpgrp(0, getpgrp()) == 0);
    say(pt_failures() ? "pty-failed\n" : "pty-ready\n");
    char b[16];
    errno = 0;
    CHECK(read(0, b, sizeof(b)) == -1 && errno == EINTR && got_int == 1);
    say("pty-int\n");
    while (!got_hup && read(0, b, sizeof(b)) > 0)
        ;
    while (!got_hup)   /* a read that saw the end before the signal came */
        pause();
    return pt_failures() ? 1 : 42;
}

int pt_child_winsize(void)
{
    struct winsize w = { 0 };
    CHECK(ioctl(1, TIOCGWINSZ, &w) == 0);
    const char *term = getenv("TERM");
    printf("winsize %ux%u TERM=%s\n", w.ws_col, w.ws_row, term ? term : "(none)");
    return pt_failures() ? 1 : 0;
}

int pt_child_hup(void)
{
    struct sigaction sa = { .sa_handler = on_sig };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGHUP, &sa, NULL) == 0);
    say("hup-ready\n");
    while (!got_hup)
        pause();
    int fd = open(HUP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)write(fd, "hup\n", 4);
        close(fd);
    }
    return 0;
}

/* ---- the master's side ------------------------------------------------------------- */

static struct {
    char   out[OUT_CAP];
    size_t len, from;
} term;

static void term_reset(void)
{
    term.len = term.from = 0;
}

/* Read the master into term.out for up to WAIT_MS: true once needle
 * came (after the last match; NULL: never), and *ended once the master's
 * read ended (EIO). */
static bool shows_on(int m, const char *needle, bool *ended)
{
    size_t nl = needle ? strlen(needle) : 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        for (size_t i = term.from; needle && i + nl <= term.len; i++)
            if (!memcmp(term.out + i, needle, nl)) {
                term.from = i + nl;
                return true;
            }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long left = WAIT_MS - ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd p = { .fd = m, .events = POLLIN };
        if (left <= 0 || poll(&p, 1, (int)left) != 1)
            break;
        if (term.len == OUT_CAP) {   /* keep the newer half */
            memmove(term.out, term.out + OUT_CAP / 2, OUT_CAP / 2);
            term.len = OUT_CAP / 2;
            term.from = term.from > OUT_CAP / 2 ? term.from - OUT_CAP / 2 : 0;
        }
        ssize_t n = read(m, term.out + term.len, OUT_CAP - term.len);
        if (n <= 0) {
            if (ended)
                *ended = n < 0 && errno == EIO;
            break;
        }
        term.len += (size_t)n;
    }
    if (needle) {
        printf("posixtest: pty: \"%s\" not shown (%zu bytes came), the last of them: \"",
               needle, term.len);
        for (size_t i = term.len > 200 ? term.len - 200 : 0; i < term.len; i++) {
            unsigned char c = (unsigned char)term.out[i];
            printf(c >= 0x20 && c < 0x7f ? "%c" : "<%02x>", c);
        }
        printf("\"\n");
    }
    return false;
}

static bool shows(int m, const char *needle)
{
    return shows_on(m, needle, NULL);
}

/* The master's reads end (EIO) within WAIT_MS. */
static bool ends(int m)
{
    bool ended = false;
    (void)shows_on(m, NULL, &ended);
    if (!ended)
        printf("posixtest: pty: the master's reads didn't end in EIO\n");
    return ended;
}

static void typed(int m, const char *s)
{
    CHECK(write(m, s, strlen(s)) == (ssize_t)strlen(s));
}

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}

/* ---- pty: a master and its slave ----------------------------------------------------- */

static void names(int m, int s, const char *name)
{
    char buf[32];
    CHECK(ptsname_r(m, buf, 3) == ERANGE);
    CHECK(isatty(s) && isatty(m));
    CHECK(ttyname_r(s, buf, sizeof(buf)) == 0 && !strcmp(buf, name));
    unsigned n = 99;
    CHECK(ioctl(m, TIOCGPTN, &n) == 0 && n == (unsigned)(name[9] - '0'));
    struct stat st;
    CHECK(fstat(m, &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(stat("/dev/ptmx", &st) == 0 && S_ISCHR(st.st_mode));
    CHECK(stat(name, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 136);
    CHECK(fstat(s, &st) == 0 && major(st.st_rdev) == 136 && minor(st.st_rdev) == n);
    CHECK(stat("/dev/pts", &st) == 0 && S_ISDIR(st.st_mode));
}

static void sizes(int m, int s)
{
    struct winsize w = { .ws_row = 30, .ws_col = 100 }, r = { 0 };
    CHECK(ioctl(m, TIOCSWINSZ, &w) == 0);
    CHECK(ioctl(s, TIOCGWINSZ, &r) == 0 && r.ws_col == 100 && r.ws_row == 30);
    w = (struct winsize){ .ws_row = 20, .ws_col = 90 };
    CHECK(ioctl(s, TIOCSWINSZ, &w) == 0);
    CHECK(ioctl(m, TIOCGWINSZ, &r) == 0 && r.ws_col == 90 && r.ws_row == 20);
    CHECK(ioctl(s, TIOCGWINSZ, &r) == 0 && r.ws_col == 90 && r.ws_row == 20);
    w.ws_col = 0;
    errno = 0;
    CHECK(ioctl(m, TIOCSWINSZ, &w) == -1 && errno == EINVAL);
}

static void bytes(int m, int s)
{
    CHECK(write(s, "out\n", 4) == 4);
    CHECK(shows(m, "out\r\n"));   /* ONLCR */
    typed(m, "in\r");
    char b[8] = { 0 };
    CHECK(read(s, b, sizeof(b)) == 3 && !memcmp(b, "in\n", 3));   /* ICRNL, a line */
    CHECK(shows(m, "in\r\n"));   /* its echo */
}

/* A child with the slave as its 0 to 2. */
static void child_on_slave(int m, int s)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (int fd = 0; fd < 3; fd++)
        posix_spawn_file_actions_adddup2(&fa, s, fd);
    char *argv[] = { "posixtest", "--child-pty", NULL };
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, "/boot/bin/posixtest", &fa, NULL, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    close(s);
    CHECK(shows(m, "pty-ready\r\n"));
    nap_ms(200);   /* into its read */
    typed(m, "\x03");
    CHECK(shows(m, "pty-int\r\n"));
    nap_ms(100);
    close(m);   /* the hang-up: SIGHUP */
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42);
}

static void one_pty(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0);
    const char *name = m >= 0 ? ptsname(m) : NULL;
    CHECK(name && !strncmp(name, "/dev/pts/", 9));
    if (m < 0 || !name)
        return;
    char copy[16];
    snprintf(copy, sizeof(copy), "%s", name);
    int s = open(copy, O_RDWR | O_NOCTTY);
    CHECK(s >= 0);
    if (s < 0)
        return;
    names(m, s, copy);
    sizes(m, s);
    term_reset();
    bytes(m, s);
    child_on_slave(m, s);
}

/* PTY_MAX masters, the next refused; a slave that isn't ours. */
static void limits(void)
{
    int ms[PTYS];
    for (int i = 0; i < PTYS; i++)
        CHECK((ms[i] = posix_openpt(O_RDWR | O_NOCTTY)) >= 0);
    errno = 0;
    CHECK(posix_openpt(O_RDWR | O_NOCTTY) == -1 && errno == EAGAIN);
    for (int i = 0; i < PTYS; i++)
        if (ms[i] >= 0)
            close(ms[i]);
    errno = 0;
    CHECK(open("/dev/pts/5", O_RDWR | O_NOCTTY) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(open("/dev/pts/x", O_RDWR | O_NOCTTY) == -1 && errno == ENOENT);
}

/* openpty, and the master's end once the last slave has closed. */
static void last_slave(void)
{
    int m = -1, s = -1;
    char name[20];
    struct winsize w = { .ws_row = 24, .ws_col = 80 }, r = { 0 };
    CHECK(openpty(&m, &s, name, NULL, &w) == 0);
    if (m < 0 || s < 0)
        return;
    CHECK(ioctl(s, TIOCGWINSZ, &r) == 0 && r.ws_col == 80 && r.ws_row == 24);
    CHECK(write(s, "bye\n", 4) == 4);
    term_reset();
    CHECK(shows(m, "bye\r\n"));
    close(s);   /* the last slave: the console ends, the master's reads with it */
    CHECK(ends(m));
    close(m);
}

void pt_pty(char **argv)
{
    (void)argv;
    one_pty();
    limits();
    last_slave();
}

/* ---- session: init's ------------------------------------------------------------------ */

static void interactive(void)
{
    struct winsize w = { .ws_row = 30, .ws_col = 100 };
    int m = -1;
    pid_t pid = -1;
    CHECK(jamos_session_open(&w, "xterm-256color", NULL, &m, &pid) == 0);
    if (m < 0)
        return;
    CHECK(fcntl(m, F_GETFD) == FD_CLOEXEC);
    term_reset();
    CHECK(shows(m, PROMPT));
    typed(m, "echo hi-pty\r");
    CHECK(shows(m, "\r\nhi-pty\r\n"));
    CHECK(shows(m, PROMPT));
    w = (struct winsize){ .ws_row = 40, .ws_col = 120 };
    CHECK(ioctl(m, TIOCSWINSZ, &w) == 0);
    struct winsize r = { 0 };
    CHECK(ioctl(m, TIOCGWINSZ, &r) == 0 && r.ws_col == 120);
    typed(m, "posixtest --child-winsize\r");
    CHECK(shows(m, "winsize 120x40 TERM=xterm-256color"));
    CHECK(shows(m, PROMPT));
    close(m);   /* the hang-up */
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 2);
}

static void command(void)
{
    int m = -1;
    pid_t pid = -1;
    CHECK(jamos_session_open(NULL, NULL, "echo cmd-out; exit 3", &m, &pid) == 0);
    if (m < 0)
        return;
    term_reset();
    CHECK(shows(m, "cmd-out\r\n"));
    CHECK(ends(m));
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 3);
    close(m);
    errno = 0;
    CHECK(jamos_session_open(NULL, "bad term", NULL, &m, &pid) == -1 && errno == EINVAL);
}

static void killed(void)
{
    int m = -1;
    pid_t pid = -1;
    CHECK(jamos_session_open(NULL, "", "", &m, &pid) == 0);
    if (m < 0)
        return;
    term_reset();
    CHECK(shows(m, PROMPT));
    CHECK(kill(pid, SIGUSR1) == 0);   /* accepted: nothing happens */
    CHECK(kill(pid, SIGTERM) == 0);   /* the hang-up */
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 2);
    CHECK(ends(m));
    close(m);
}

void pt_session(char **argv)
{
    (void)argv;
    interactive();
    command();
    killed();
}
