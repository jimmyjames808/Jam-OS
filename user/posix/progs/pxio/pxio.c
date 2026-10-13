/* pxio: a small POSIX program for the shell's tests
 * (tools/shell-tests/posix.txt): it shows what the shell gave a POSIX
 * program (its descriptors, environment, current directory and stack) and
 * does the little I/O a redirection or a pipe is checked with. Its output
 * lines start "pxio: ", so a test can tell them from the shell's.
 *
 *   pxio env             PWD HOME PATH TERM TZ USER LOGNAME SHELL, one a line
 *   pxio cwd             the current directory
 *   pxio fds             what descriptors 0, 1 and 2 are: tty, file, pipe
 *   pxio stack           uses 7 MiB of its stack
 *   pxio out TEXT        TEXT and a newline on descriptor 1
 *   pxio err TEXT        the same on descriptor 2
 *   pxio both OUT ERR    OUT on 1, then ERR on 2, one write each
 *   pxio seq N           "line 1" .. "line N" on descriptor 1
 *   pxio cat             descriptor 0 to descriptor 1, as it comes
 *   pxio count           "pxio: N lines, M bytes" of descriptor 0
 *   pxio exit N          exits with code N
 *   pxio sigint          "pxio: waiting", then waits for SIGINT (the shell's
 *                        Ctrl+C), says "pxio: got SIGINT" and exits with 3
 *   pxio yes             "y" lines for ever
 *   pxio head N          the first N lines of descriptor 0, then exits at once
 *                        (the rest unread: the writer gets SIGPIPE)
 *   pxio gen N           N MiB of 64-byte lines ("xxx...x" and a newline)
 *   pxio idle            "pxio: idle", then waits for ever (SIGTERM and Ctrl+C
 *                        end it by their default action)
 *   pxio ignore          "pxio: ignoring": the same with SIGINT, SIGQUIT and
 *                        SIGTERM ignored, so only its job's kill ends it
 *   pxio raise N         raises signal N (default action: the program is ended
 *                        by it)
 *   pxio size            "pxio: size COLSxROWS", its terminal's (TIOCGWINSZ
 *                        on 1: an SSH session's window, tools/ssh-test.sh)
 *
 * Writing is write(2) alone, so nothing is buffered between two modes'
 * lines. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0)
            exit(3);
        s += w;
        n -= (size_t)w;
    }
}

static int show_env(void)
{
    static const char *const names[] = { "PWD", "HOME", "PATH", "TERM", "TZ", "USER", "LOGNAME",
                                         "SHELL", "WAYLAND_DISPLAY" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *v = getenv(names[i]);
        char line[512];
        snprintf(line, sizeof(line), "pxio: %s=%s\n", names[i], v ? v : "(unset)");
        put(1, line);
    }
    return 0;
}

static int show_cwd(void)
{
    char dir[512], line[560];
    snprintf(line, sizeof(line), "pxio: cwd=%s\n", getcwd(dir, sizeof(dir)) ? dir : "(none)");
    put(1, line);
    return 0;
}

static int show_fds(void)
{
    for (int fd = 0; fd < 3; fd++) {
        struct stat st;
        const char *kind = "closed";
        if (fstat(fd, &st) == 0)
            kind = isatty(fd) ? "tty" : S_ISREG(st.st_mode) ? "file" : S_ISFIFO(st.st_mode)
                                                                         ? "pipe" : "other";
        char line[64];
        snprintf(line, sizeof(line), "pxio: fd%d %s\n", fd, kind);
        put(1, line);
    }
    return 0;
}

/* 64 KiB of stack a level, 112 levels: 7 MiB, each page touched. */
static int deep(int level)
{
    volatile char frame[64 * 1024];
    for (size_t i = 0; i < sizeof(frame); i += 4096)
        frame[i] = (char)level;
    int r = level < 111 ? deep(level + 1) : 0;
    return r + frame[0] - (char)level;
}

static int show_stack(void)
{
    if (deep(0))
        return 1;
    put(1, "pxio: stack 7 MiB ok\n");
    return 0;
}

static int seq(int n)
{
    for (int i = 1; i <= n; i++) {
        char line[32];
        snprintf(line, sizeof(line), "line %d\n", i);
        put(1, line);
    }
    return 0;
}

static int copy_stdin(bool count)
{
    static char buf[65536];
    long bytes = 0, lines = 0;
    for (ssize_t n; (n = read(0, buf, sizeof(buf))) > 0;) {
        bytes += n;
        for (ssize_t i = 0; i < n; i++)
            lines += buf[i] == '\n';
        for (ssize_t at = 0, w; !count && at < n; at += w)
            if ((w = write(1, buf + at, (size_t)(n - at))) <= 0)
                exit(3);
    }
    if (count) {
        char line[64];
        snprintf(line, sizeof(line), "pxio: %ld lines, %ld bytes\n", lines, bytes);
        put(1, line);
    }
    return 0;
}

static volatile sig_atomic_t got_int;

static void on_int(int sig)
{
    (void)sig;
    got_int = 1;
}

static int wait_sigint(void)
{
    signal(SIGINT, on_int);
    put(1, "pxio: waiting\n");
    while (!got_int)
        pause();
    put(1, "pxio: got SIGINT\n");
    return 3;
}

static int yes(void)
{
    static char block[65536];
    for (size_t i = 0; i < sizeof(block); i += 2) {
        block[i] = 'y';
        block[i + 1] = '\n';
    }
    for (;;)
        for (size_t at = 0, w; at < sizeof(block); at += w)
            if ((w = (size_t)write(1, block + at, sizeof(block) - at)) == (size_t)-1)
                exit(3);
}

static int head(int lines)
{
    char buf[4096];
    for (ssize_t n; lines > 0 && (n = read(0, buf, sizeof(buf))) > 0;) {
        ssize_t keep = 0;
        while (keep < n && lines > 0)
            if (buf[keep++] == '\n')
                lines--;
        for (ssize_t at = 0, w; at < keep; at += w)
            if ((w = write(1, buf + at, (size_t)(keep - at))) <= 0)
                exit(3);
    }
    return 0;
}

static int gen(int mib)
{
    static char block[65536];
    for (size_t i = 0; i < sizeof(block); i += 64) {
        memset(block + i, 'x', 63);
        block[i + 63] = '\n';
    }
    for (int i = 0; i < mib * 16; i++)
        for (size_t at = 0, w; at < sizeof(block); at += w)
            if ((w = (size_t)write(1, block + at, sizeof(block) - at)) == (size_t)-1)
                exit(3);
    return 0;
}

static int idle(bool ignore)
{
    if (ignore) {
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
    }
    put(1, ignore ? "pxio: ignoring\n" : "pxio: idle\n");
    for (;;)
        pause();
    return 0;
}

static int show_size(void)
{
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) < 0) {
        put(1, "pxio: size: no terminal\n");
        return 1;
    }
    char line[64];
    snprintf(line, sizeof(line), "pxio: size %ux%u\n", w.ws_col, w.ws_row);
    put(1, line);
    return 0;
}

int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "env"))
        return show_env();
    if (!strcmp(m, "cwd"))
        return show_cwd();
    if (!strcmp(m, "fds"))
        return show_fds();
    if (!strcmp(m, "stack"))
        return show_stack();
    if (!strcmp(m, "out") && argc == 3) {
        put(1, argv[2]);
        put(1, "\n");
        return 0;
    }
    if (!strcmp(m, "err") && argc == 3) {
        put(2, argv[2]);
        put(2, "\n");
        return 0;
    }
    if (!strcmp(m, "both") && argc == 4) {
        put(1, argv[2]);
        put(1, "\n");
        put(2, argv[3]);
        put(2, "\n");
        return 0;
    }
    if (!strcmp(m, "seq") && argc == 3)
        return seq(atoi(argv[2]));
    if (!strcmp(m, "cat"))
        return copy_stdin(false);
    if (!strcmp(m, "count"))
        return copy_stdin(true);
    if (!strcmp(m, "exit") && argc == 3)
        return atoi(argv[2]);
    if (!strcmp(m, "sigint"))
        return wait_sigint();
    if (!strcmp(m, "yes"))
        return yes();
    if (!strcmp(m, "head") && argc == 3)
        return head(atoi(argv[2]));
    if (!strcmp(m, "gen") && argc == 3)
        return gen(atoi(argv[2]));
    if (!strcmp(m, "idle"))
        return idle(false);
    if (!strcmp(m, "ignore"))
        return idle(true);
    if (!strcmp(m, "raise") && argc == 3) {
        raise(atoi(argv[2]));
        put(1, "pxio: raise returned\n");
        return 4;
    }
    if (!strcmp(m, "size"))
        return show_size();
    put(2, "usage: pxio env|cwd|fds|stack|out T|err T|both O E|seq N|cat|count|exit N|sigint|"
           "yes|head N|gen N|idle|ignore|raise N|size\n");
    return 2;
}
