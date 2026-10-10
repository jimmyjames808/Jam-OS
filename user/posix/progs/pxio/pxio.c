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
 *
 * Writing is write(2) alone, so nothing is buffered between two modes'
 * lines. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
                                         "SHELL" };
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
    char buf[4096];
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
    put(2, "usage: pxio env|cwd|fds|stack|out T|err T|both O E|seq N|cat|count|exit N\n");
    return 2;
}
