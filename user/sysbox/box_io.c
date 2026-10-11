/* The shell's calls for a command that runs as a POSIX program (a box,
 * user/sysbox/): what sh.h declares and sh_io.c, main.c and sh_handles.c
 * answer inside the shell, answered over standard input and output and the
 * program's own handles. The command files and the shell's library files
 * (sh_kernel.c, sh_sysinfo.c, sh_num.c, sh_time.c, sh_lookup.c, sh_vfs.c)
 * are the shell's, compiled into the box as they are: one copy of every
 * command.
 *
 * What differs from the shell:
 *   output     sh_put and sh_say are standard output (a pipe or a file as
 *              the starter made it), sh_tty is standard error;
 *   input      sh_stdin reads standard input when it is no terminal;
 *   Ctrl+C     SIGINT is noted (sh_interrupted), not fatal, so a loop ends
 *              with its summary; the waits end with it (ERR_INTERRUPTED);
 *   handles    the root resource is the program's own SR_RESOURCE, with
 *              only the powers its list (wants-*) asked for, or none at
 *              all when a POSIX parent started it (the spawn passes the
 *              namespace on, not the root resource); every service is the
 *              one in its namespace (/svc/<name>);
 *   the log    a kernel debug command's output (sh_kcmd) is the log lines
 *              logged while it ran, printed when it is over: the shell
 *              shows them as they come (sh_show_log), a program has no
 *              console to ask.
 * A command that needs a handle the program lacks says so with an
 * error from the call (sh_sysinfo: "free: access denied"), as it does in
 * a shell without the right. */
#include <jamos/libjam.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "box.h"
#include "sh.h"

#define STDIN_MAX (4u << 20)   /* the most a command reads from a pipe, as the shell's pipe holds */
#define OUT_PIECE 2048         /* standard output's buffer, and the most sh_put writes at once: no
                                  write is bigger than the shell's pipe reader takes (4096) */

static volatile sig_atomic_t interrupted;

/* ---- output --------------------------------------------------------------------- */

void sh_put(const char *s, size_t n)
{
    while (n) {
        size_t k = n < OUT_PIECE ? n : OUT_PIECE;
        fwrite(s, 1, k, stdout);
        s += k;
        n -= k;
    }
}

void sh_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void sh_tty(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);   /* what was said before comes first */
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void sh_flush(void)
{
    fflush(stdout);
}

bool sh_piped(void)
{
    return !isatty(STDOUT_FILENO);
}

/* ---- input ---------------------------------------------------------------------- */

static char *piped;           /* standard input, read once */
static size_t piped_len;
static bool piped_read;

bool sh_stdin(const char **data, size_t *len)
{
    if (isatty(STDIN_FILENO))
        return false;
    if (!piped_read) {
        piped_read = true;
        size_t cap = 0;
        for (;;) {
            if (piped_len == cap) {
                if (cap >= STDIN_MAX)
                    break;
                cap = cap ? cap * 2 : 4096;
                char *more = realloc(piped, cap + 1);
                if (!more)
                    break;
                piped = more;
            }
            ssize_t n = read(STDIN_FILENO, piped + piped_len, cap - piped_len);
            if (n <= 0)
                break;
            piped_len += (size_t)n;
        }
        if (piped)
            piped[piped_len] = '\0';
    }
    *data = piped ? piped : "";
    *len = piped_len;
    return true;
}

bool sh_input(const char *who, int argc, char **argv, int i, const char **data, size_t *len)
{
    if (i < argc) {
        char abs[SH_PATH_MAX];
        uint64_t size;
        status_t st = sh_resolve(argv[i], abs, sizeof(abs)) ? sh_read(abs, (const void **)data, &size)
                                                            : ERR_NOT_FOUND;
        if (st != OK) {
            sh_tty("%s: %s: %s\n", who, argv[i], sh_why(st));
            return false;
        }
        *len = (size_t)size;
        return true;
    }
    if (sh_stdin(data, len))
        return true;
    sh_tty("%s: no file given and nothing piped in\n", who);
    return false;
}

/* ---- Ctrl+C --------------------------------------------------------------------- */

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
}

bool sh_interrupted(void)
{
    return interrupted;
}

int sh_poll_key(uint64_t deadline)
{
    (void)deadline;
    return -1;   /* no keyboard: a program reads its terminal as standard input */
}

void sh_drop_typeahead(void)
{
}

bool sh_sleep(uint64_t ns)
{
    struct timespec ts = { .tv_sec = (time_t)(ns / NS_PER_S), .tv_nsec = (long)(ns % NS_PER_S) };
    while (!interrupted && nanosleep(&ts, &ts) < 0 && errno == EINTR && !interrupted)
        ;
    return !interrupted;
}

/* ---- handles and variables ------------------------------------------------------ */

handle_t sh_root(void)
{
    return startup_handle(SR_RESOURCE);
}

handle_t sh_devmgr(void)
{
    return svc_get(SVC_DEVMGR);
}

const char *sh_getvar(const char *name)
{
    return getenv(name);
}

/* ---- the kernel's log while a debug command runs -------------------------------- */

static uint64_t log_from;   /* where the log ended when sh_show_log(true) was called */
static bool     log_open;   /* the log could be read then */

void sh_show_log(bool on, const char *only)
{
    (void)only;
    if (on) {
        handle_t r;
        log_open = jam_klog_open(sh_root(), &r) == OK;
        if (log_open) {
            jam_handle_close(r);
            log_from = sh_klog_end();
        }
        return;
    }
    if (!log_open)
        return;
    log_open = false;
    char *buf;
    size_t got;
    uint64_t first;
    if (sh_klog_read(log_from, &buf, &got, &first) == OK) {
        sh_put(buf, got);
        free(buf);
    }
}

/* ---- the start ------------------------------------------------------------------ */

/* At exit: what is buffered is written, and a terminal is waited for until
 * it has shown it all: the console frees a terminal at the program's end
 * with what it hasn't read yet, so the last lines of a long output are lost
 * otherwise (M13's X1 notes). */
static void finish(void)
{
    fflush(stdout);
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; fd++)
        if (isatty(fd))
            tcdrain(fd);
}

int box_init(const char *cmd, int needs_root)
{
    static char obuf[OUT_PIECE];
    setvbuf(stdout, obuf, isatty(STDOUT_FILENO) ? _IOLBF : _IOFBF, sizeof(obuf));
    atexit(finish);
    if (needs_root && !sh_root()) {
        sh_tty("%s: not given the kernel's information: the Jam shell passes it to a program it\n"
               "  starts (run %s); a program started by another program (a POSIX shell, env) "
               "has none\n",
               cmd, cmd);
        return 1;
    }
    struct sigaction sa = { .sa_handler = on_sigint };
    sigaction(SIGINT, &sa, NULL);
    char cwd[SH_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        sh_chdir(cwd);
    return 0;
}
