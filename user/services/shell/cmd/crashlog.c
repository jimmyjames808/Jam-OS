/* crashlog: the kernel panics logd saved (/data/logs/boot-NNNN-crash.txt,
 * <crashlog.h>): the newest one's report (its code, what happened and
 * where, the backtrace, the last log lines before it, the boot and the
 * build), `crashlog N` the N-th newest, `crashlog list` all of them,
 * newest first. A report can be MiBs of log: only its start and the part
 * around the panic are read (<crashinfo.h> reads them). The desktop's
 * "Jam OS restarted after a problem" notice runs it in a new terminal
 * (its Details). */
#include <crashinfo.h>
#include "sh.h"

#define LOGS        "/data/logs"
#define SUFFIX      "-crash.txt"
#define PANIC_READ  (16u << 10)   /* the panic's lines, from their start */
#define BEFORE_READ (4u << 10)    /* the log just before them */
#define BEFORE_LINES 8
#define LABEL       "%-10s "

static struct sh_dirent ents[SH_DIR_MAX];
static int order[SH_DIR_MAX];          /* the crash reports' entries, newest first */
static unsigned numbers[SH_DIR_MAX];   /* each entry's number (boot-0042: 42) */
static char intro[CRASH_INTRO_MAX], panic_text[PANIC_READ], before[BEFORE_READ];
static size_t panic_len;               /* panic_text's bytes read */

static bool is_report(const char *name)
{
    size_t len = strlen(name), sl = strlen(SUFFIX);
    return len > sl && !strcmp(name + len - sl, SUFFIX);
}

/* The crash reports in /data/logs into order[], newest (the biggest
 * number) first: how many (-1: no such directory). */
static int find_reports(void)
{
    int n = sh_readdir(LOGS, ents, SH_DIR_MAX), k = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].dir || !is_report(ents[i].name))
            continue;
        unsigned number = 0;
        for (const char *p = ents[i].name; *p; p++)
            if (*p >= '0' && *p <= '9' && number < 100000000u)
                number = number * 10 + (unsigned)(*p - '0');
        numbers[i] = number;
        int at = k++;
        for (; at > 0 && numbers[order[at - 1]] < number; at--)
            order[at] = order[at - 1];
        order[at] = i;
    }
    if (n == SH_DIR_MAX)
        sh_readdir_cut("crashlog", LOGS, n, SH_DIR_MAX);
    return n < 0 ? -1 : k;
}

/* Up to cap bytes of f at off: how many (0 on an error). */
static size_t read_at(struct jfile *f, uint64_t off, char *buf, size_t cap)
{
    size_t done = 0;
    return file_read(f, off, buf, cap, &done) == OK ? done : 0;
}

/* The report's intro and panic lines into r; false (said) if it can't. */
static bool read_report(const char *name, struct jfile *f, struct crash_report *r)
{
    char path[SH_PATH_MAX];
    if (!sh_join(LOGS, name, path, sizeof(path)))
        return false;
    status_t st = file_open(path, FS_READ, f);
    if (st != OK) {
        sh_say("crashlog: %s: %s\n", path, sh_why(st));
        return false;
    }
    size_t n = read_at(f, 0, intro, sizeof(intro));
    if (!crash_read_intro(intro, n, r)) {
        sh_say("crashlog: %s isn't a crash report\n", path);
        file_close(f);
        return false;
    }
    panic_len = read_at(f, r->text_at + r->panic_at, panic_text, sizeof(panic_text));
    crash_read_panic(panic_text, panic_len, r);
    return true;
}

/* Where: the backtrace's first frame's name (the faulting instruction, or
 * what called panic()), then the CPU and the thread. */
static void where(const struct crash_report *r)
{
    char first[120];
    size_t at = 0;
    const char *name = NULL;
    if (crash_next_frame(panic_text, panic_len, &at, first, sizeof(first)) && first[0] == '#')
        name = strrchr(first, ' ');
    if (!name && !r->where[0])
        return;
    sh_say(LABEL "%s%s%s\n", "where", name ? name + 1 : "", name && r->where[0] ? ", " : "",
           r->where);
}

static void backtrace(void)
{
    char line[120];
    size_t at = 0;
    bool first = true;
    while (crash_next_frame(panic_text, panic_len, &at, line, sizeof(line))) {
        sh_say(LABEL "%s\n", first ? "backtrace" : "", line);
        first = false;
    }
    if (first)
        sh_say(LABEL "(none in the report)\n", "backtrace");
}

/* The last BEFORE_LINES whole lines of the log before the panic. */
static void last_lines(struct jfile *f, const struct crash_report *r)
{
    uint64_t from = r->panic_at > BEFORE_READ ? r->panic_at - BEFORE_READ : 0;
    size_t n = read_at(f, r->text_at + from, before, (size_t)(r->panic_at - from));
    size_t start = n;
    unsigned lines = 0;
    while (start > 0 && !(before[start - 1] == '\n' && start != n && ++lines == BEFORE_LINES))
        start--;
    if (start == 0 && from > 0)   /* began mid-line: from the next one */
        while (start < n && before[start++] != '\n')
            ;
    sh_say(LABEL "the last lines before the panic:\n", "log");
    struct sh_lines ls = { before + start, before + n };
    const char *s;
    size_t len;
    while (sh_next_line(&ls, &s, &len))
        sh_say(LABEL "\033[90m%.*s\033[0m\n", "", (int)len, s);
}

static int show(unsigned nth, unsigned count)
{
    struct jfile f;
    struct crash_report r;
    const char *name = ents[order[nth - 1]].name;
    if (!read_report(name, &f, &r))
        return 1;
    sh_say("Crash report " LOGS "/%s, ", name);
    if (nth == 1)
        sh_say("the newest of %u\n", count);
    else
        sh_say("%u of %u from the newest\n", nth, count);
    if (r.code[0])
        sh_say(LABEL "%s%s%s\n", "code", r.code, r.what[0] ? ": " : "", r.what);
    else
        sh_say(LABEL "(none: from a kernel before the panic codes)\n", "code");
    sh_say(LABEL "%s\n", "what", r.message[0] ? r.message : "(no message)");
    where(&r);
    backtrace();
    last_lines(&f, &r);
    sh_say(LABEL "%s\n", "boot", r.boot);
    sh_say(LABEL "%s\n", "build", r.build[0] ? r.build : "(not in the report)");
    file_close(&f);
    return 0;
}

static int list(unsigned count)
{
    sh_say("Crash reports in " LOGS ", newest first (crashlog N shows one):\n");
    for (unsigned i = 0; i < count && !sh_interrupted(); i++) {
        struct jfile f;
        struct crash_report r;
        const char *name = ents[order[i]].name;
        if (!read_report(name, &f, &r))
            continue;
        file_close(&f);
        sh_say("%3u  %s  %-12s %s\n", i + 1, name, r.code[0] ? r.code : "-", r.message);
    }
    return 0;
}

SH_CMD(crashlog)
{
    uint64_t nth = 1;
    bool all = argc == 2 && !strcmp(argv[1], "list");
    if (argc > 2 || (argc == 2 && !all && !sh_parse_u64(argv[1], &nth))) {
        sh_tty("usage: crashlog [N | list]\n");
        return 2;
    }
    int count = find_reports();
    if (count < 0) {
        sh_say("crashlog: no " LOGS " (is the Jam OS stick in?)\n");
        return 1;
    }
    if (count == 0) {
        sh_say("crashlog: no crash reports in " LOGS ": no panic was saved\n");
        return all ? 0 : 1;
    }
    if (all)
        return list((unsigned)count);
    if (nth < 1 || nth > (uint64_t)count) {
        sh_tty("crashlog: there %s %d crash report%s (crashlog list)\n", count == 1 ? "is" : "are",
               count, count == 1 ? "" : "s");
        return 1;
    }
    return show((unsigned)nth, (unsigned)count);
}
