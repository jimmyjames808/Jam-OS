/* The shell's command layer: variables, aliases, parsing, ; && || and
 * pipes, the command table with its help, tab completion (see sh.h).
 *
 * Pipes: every stage runs in turn in this process. While a stage runs,
 * everything it prints (sh_put, and main.c's say/put) is appended to a
 * memory buffer instead of the screen (sh_capture); the next stage gets
 * that buffer as its input (sh_stdin), and the last stage prints to
 * wherever the whole pipeline prints. So `dmesg | grep usb | tail -3`
 * works with any builtin, at the cost of each stage finishing before the
 * next starts (fine for text this size; a pipe holds at most 4 MiB). A
 * program started with `run` inside a pipe gets an SR_STDOUT channel,
 * which libos's printf writes to; the shell copies what arrives into the
 * pipe (sh_run in cmds_shell.c). Kernel commands (ktest, bench, stress,
 * kill) print into the kernel log, so their text can't be piped. */
#include "sh.h"

#define PIPE_MAX   (4u << 20)
#define MAX_VARS   64
#define MAX_ALIAS  32
#define MAX_WORDS  64
#define MAX_DEPTH  8     /* aliases in aliases, watch 'a | b' in a line, ... */

/* ---- small helpers ------------------------------------------------------------ */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

bool sh_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (UINT64_MAX - 9) / 10)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

const char *sh_human(uint64_t b, char *buf, size_t cap)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    unsigned u = 0;
    uint64_t scale = 1;
    while (u < 4 && b >= scale * 1024) {
        scale *= 1024;
        u++;
    }
    if (!u) {
        snprintf(buf, cap, "%lu B", (unsigned long)b);
    } else {
        uint64_t tenths = (b * 10 + scale / 2) / scale;
        snprintf(buf, cap, "%lu.%lu %s", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10),
                 units[u]);
    }
    return buf;
}

static bool name_char(char c, bool first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (!first && c >= '0' && c <= '9');
}

/* ---- output and pipes ---------------------------------------------------------- */

struct buf {
    char  *p;
    size_t n, cap;
    bool   full;
};

static struct buf *capture;       /* where output goes now; NULL = the screen */
static const char *in_data;       /* this stage's input */
static size_t in_len;
static bool have_in;

static void buf_add(struct buf *b, const char *s, size_t n)
{
    if (b->n + n > PIPE_MAX) {
        b->full = true;
        n = PIPE_MAX - b->n;
    }
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n)
            cap *= 2;
        char *p = malloc(cap);
        if (!p) {
            b->full = true;
            return;
        }
        if (b->n)
            memcpy(p, b->p, b->n);
        free(b->p);
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

bool sh_capture(const char *s, size_t n)
{
    if (!capture)
        return false;
    buf_add(capture, s, n);
    return true;
}

bool sh_piped(void)
{
    return capture != NULL;
}

void sh_put(const char *s, size_t n)
{
    sh_put_raw(s, n);
}

static void vsay(const char *fmt, va_list ap)
{
    char buf[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n >= (int)sizeof(buf)) {
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            sh_put(big, (size_t)n);
            free(big);
            n = 0;
        } else {
            n = sizeof(buf) - 1;
        }
    }
    va_end(ap2);
    if (n > 0)
        sh_put(buf, (size_t)n);
}

void sh_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

void sh_tty(const char *fmt, ...)
{
    struct buf *saved = capture;
    capture = NULL;
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
    capture = saved;
}

bool sh_stdin(const char **data, size_t *len)
{
    if (!have_in)
        return false;
    *data = in_data ? in_data : "";
    *len = in_len;
    return true;
}

bool sh_input(const char *who, int argc, char **argv, int i, const char **data, size_t *len)
{
    if (i < argc) {
        char abs[SH_PATH_MAX];
        const void *d;
        uint64_t n;
        bool dir = false;
        if (!sh_resolve(argv[i], abs, sizeof(abs)) || sh_stat(abs, &dir, &n) != OK) {
            sh_tty("%s: %s: no such file\n", who, argv[i]);
            return false;
        }
        if (dir || sh_read(abs, &d, &n) != OK) {
            sh_tty("%s: %s: is a directory\n", who, argv[i]);
            return false;
        }
        *data = d;
        *len = (size_t)n;
        return true;
    }
    if (sh_stdin(data, len))
        return true;
    sh_tty("%s: no input (give a file, or pipe into it: dmesg | %s)\n", who, who);
    return false;
}

/* ---- keys ------------------------------------------------------------------------ */

static bool interrupted;

int sh_poll_key(uint64_t deadline)
{
    sh_flush();
    struct input_key_event ev;
    while (sh_get_key(&ev, deadline)) {
        if (sh_is_ctrl(&ev, 'c')) {
            if (!interrupted)
                sh_tty("^C\n");
            interrupted = true;
            return 3;
        }
        if (ev.codepoint)
            return (int)ev.codepoint;
    }
    return -1;
}

bool sh_interrupted(void)
{
    if (!interrupted)
        sh_poll_key(0);
    return interrupted;
}

bool sh_sleep(uint64_t ns)
{
    uint64_t deadline = (uint64_t)jam_clock_get() + ns;
    while (!interrupted && (uint64_t)jam_clock_get() < deadline)
        sh_poll_key(deadline);
    return !interrupted;
}

/* ---- variables and aliases --------------------------------------------------------- */

struct var {
    char  name[32];
    char *value;   /* NULL: free slot */
    bool  exported;
};
static struct var vars[MAX_VARS];

static struct var *find_var(const char *name)
{
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].value && !strcmp(vars[i].name, name))
            return &vars[i];
    return NULL;
}

const char *sh_getvar(const char *name)
{
    struct var *v = find_var(name);
    return v ? v->value : NULL;
}

void sh_setvar(const char *name, const char *value, int exported)
{
    if (strlen(name) >= sizeof(vars[0].name))
        return;
    struct var *v = find_var(name);
    if (!v) {
        for (int i = 0; i < MAX_VARS && !v; i++)
            if (!vars[i].value)
                v = &vars[i];
        if (!v) {
            sh_tty("set: too many variables (%d)\n", MAX_VARS);
            return;
        }
        memcpy(v->name, name, strlen(name) + 1);
        v->exported = false;
    }
    char *d = dup_str(value);
    if (!d)
        return;
    free(v->value);
    v->value = d;
    if (exported >= 0)
        v->exported = exported;
}

void sh_unsetvar(const char *name)
{
    struct var *v = find_var(name);
    if (v) {
        free(v->value);
        v->value = NULL;
    }
}

/* For `run`: "NAME=value" of every exported variable, NULL-terminated
 * (freed by sh_free_env). */
char **sh_make_env(void)
{
    char **env = calloc(MAX_VARS + 1, sizeof(char *));
    int n = 0;
    for (int i = 0; env && i < MAX_VARS; i++) {
        if (!vars[i].value || !vars[i].exported)
            continue;
        size_t l = strlen(vars[i].name) + strlen(vars[i].value) + 2;
        char *s = malloc(l);
        if (!s)
            continue;
        snprintf(s, l, "%s=%s", vars[i].name, vars[i].value);
        env[n++] = s;
    }
    return env;
}

void sh_free_env(char **env)
{
    for (int i = 0; env && env[i]; i++)
        free(env[i]);
    free(env);
}

struct alias {
    char  name[32];
    char *value;
};
static struct alias aliases[MAX_ALIAS];

static struct alias *find_alias(const char *name)
{
    for (int i = 0; i < MAX_ALIAS; i++)
        if (aliases[i].value && !strcmp(aliases[i].name, name))
            return &aliases[i];
    return NULL;
}

static bool set_alias(const char *name, const char *value)
{
    if (!*name || strlen(name) >= sizeof(aliases[0].name))
        return false;
    struct alias *a = find_alias(name);
    for (int i = 0; i < MAX_ALIAS && !a; i++)
        if (!aliases[i].value)
            a = &aliases[i];
    if (!a)
        return false;
    char *d = dup_str(value);
    if (!d)
        return false;
    memcpy(a->name, name, strlen(name) + 1);
    free(a->value);
    a->value = d;
    return true;
}

/* ---- the command table --------------------------------------------------------------- */

enum { C_INFO, C_FILES, C_TEXT, C_SHELL, C_SYSTEM, C_TESTS, C_COUNT };
static const char *const cat_names[C_COUNT] = {
    "Information", "Files (/boot is the boot image, read-only)", "Text (read a file or a pipe)",
    "Shell", "System", "Tests",
};

struct sh_cmd {
    const char *name;
    int       (*fn)(int argc, char **argv);   /* NULL: one of main.c's */
    uint8_t     cat;
    const char *usage;
    const char *help;
};

#define C(n, cat, usage, help) { #n, shc_##n, cat, usage, help }
#define M(n, cat, usage, help) { n, NULL, cat, usage, help }

static const struct sh_cmd cmds[] = {
    C(help, C_SHELL, "help [command]", "the commands by category, or one command's usage"),
    C(uname, C_INFO, "uname [-a]", "the system's name (-a: with version, machine and CPU)"),
    C(version, C_INFO, "version", "the Jam OS version"),
    C(uptime, C_INFO, "uptime", "the time, how long since boot, CPU use since boot"),
    C(date, C_INFO, "date [-u] [-r] [-d @secs]",
      "the date and time from the real-time clock, in $TZ (default Australia/Sydney).\n"
      "  -u: UTC. -r: the raw clock and how it was read. -d @secs: that Unix time.\n"
      "  TZ: Australia/Sydney (AEST/AEDT),\n"
      "  UTC, or an offset like +10, +9:30, -5. RTC=local (default: the clock keeps\n"
      "  local time, as Windows sets it) or RTC=utc. Example: TZ=UTC date"),
    C(lscpu, C_INFO, "lscpu [-e]",
      "the CPU: model, P-cores, E-cores, threads (-e: one line per CPU)"),
    C(free, C_INFO, "free", "memory: total, used, free"),
    C(ps, C_INFO, "ps [-k]",
      "processes: id, threads, CPU time, memory of its job, name (indented by job).\n"
      "  -k: the kernel's own listing (jobs with pages, handles, threads) in the log"),
    C(top, C_INFO, "top [-d seconds] [-n frames]",
      "live CPU use per CPU and per process, and memory; q or Ctrl+C quits"),
    C(whoami, C_INFO, "whoami", "the user (there is one: jam)"),
    C(hostname, C_INFO, "hostname", "this machine's name"),
    C(dmesg, C_INFO, "dmesg", "the whole kernel log (64 KiB); pipe it: dmesg | grep usb"),
    C(history, C_SHELL, "history", "the lines typed (up/down recall them)"),
    M("devices", C_SYSTEM, "devices", "PCI functions and the drivers devmgr bound (alias lspci)"),
    M("usb", C_SYSTEM, "usb", "USB devices from usb-bus (alias lsusb)"),
    M("log", C_INFO, "log [lines]", "the last lines of the kernel log (default 20)"),
    M("mem", C_SYSTEM, "mem", "physical memory from the kernel, and the shell's job"),
    M("kill", C_SYSTEM, "kill <name>", "kill the first process with that name (see ps)"),
    M("clear", C_SHELL, "clear", "clear the screen (also Ctrl+L)"),
    M("reboot", C_SYSTEM, "reboot", "restart the machine"),
    C(run, C_SYSTEM, "run <prog> [args]",
      "start /boot/bin/<prog> (or a path), wait, say how it ended; Ctrl+C kills it.\n"
      "  Typing a program's name does the same. Exported variables are its environment;\n"
      "  in a pipe its printf output goes down the pipe: run utest | grep passed"),
    M("ktest", C_TESTS, "ktest [prefix]", "kernel tests (as the boot menu's All tests)"),
    M("bench", C_TESTS, "bench", "kernel benchmark"),
    M("stress", C_TESTS, "stress <seconds>", "stress test (1..600)"),
    M("panic", C_TESTS, "panic", "panic the kernel (a test: its screen must show)"),
    C(pwd, C_FILES, "pwd", "the current directory"),
    C(cd, C_FILES, "cd [dir]", "change directory (no argument: $HOME)"),
    C(ls, C_FILES, "ls [-l] [path...]", "list a directory (-l: sizes)"),
    C(find, C_FILES, "find [dir] [-name text]", "every file below dir (names containing text)"),
    C(cat, C_TEXT, "cat [file...]", "print files (or the pipe)"),
    C(hexdump, C_TEXT, "hexdump [-s offset] [-n bytes] [file]", "hex and ASCII, 16 bytes a line (also hd)"),
    C(wc, C_TEXT, "wc [-l|-w|-c] [file]", "count lines, words, bytes"),
    C(head, C_TEXT, "head [-n N] [file]", "the first N lines (default 10)"),
    C(tail, C_TEXT, "tail [-n N] [file]", "the last N lines (default 10)"),
    C(grep, C_TEXT, "grep [-i] [-v] [-c] [-n] <pattern> [file]",
      "lines matching pattern: text, with ^ $ . * as in a regular expression.\n"
      "  -i any case, -v the others, -c count them, -n with line numbers"),
    C(sort, C_TEXT, "sort [-r] [-n] [-u] [file]", "sort lines (-r reverse, -n numeric, -u unique)"),
    C(uniq, C_TEXT, "uniq [-c] [file]", "drop repeated adjacent lines (-c: count them)"),
    C(seq, C_TEXT, "seq [first] last", "the numbers first..last, one a line"),
    C(echo, C_SHELL, "echo [-n] [-e] [text...]", "print the words ($NAME expanded; -n no newline, -e \\n \\t)"),
    C(set, C_SHELL, "set [NAME value]",
      "set a shell variable (or NAME=value); no arguments: list them"),
    C(unset, C_SHELL, "unset NAME...", "remove variables"),
    C(export, C_SHELL, "export [NAME[=value]...]", "variables given to programs `run` starts"),
    C(env, C_SHELL, "env", "the exported variables (a program's environment)"),
    C(alias, C_SHELL, "alias [name[=text]]", "list, show or define aliases: alias ll='ls -l'"),
    C(unalias, C_SHELL, "unalias name...", "remove aliases"),
    C(type, C_SHELL, "type name...", "what a name is: alias, builtin or program (also which)"),
    C(time, C_SHELL, "time <command...>", "run a command and say how long it took"),
    C(sleep, C_SHELL, "sleep <seconds>", "wait (Ctrl+C stops it; 0.5 works)"),
    C(repeat, C_SHELL, "repeat <n> <command...>", "run a command n times (Ctrl+C stops)"),
    C(watch, C_SHELL, "watch [-n seconds] <command...>",
      "run a command every n seconds (default 2) until Ctrl+C; quote pipes:\n"
      "  watch -n 1 'ps | grep hid'"),
    C(true, C_SHELL, "true", "status 0"),
    C(false, C_SHELL, "false", "status 1"),
};

static const struct sh_cmd *find_cmd(const char *name)
{
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        if (!strcmp(cmds[i].name, name))
            return &cmds[i];
    return NULL;
}

/* Extra names for commands (not user aliases: `type` says builtin). */
static const char *const synonyms[][2] = {
    { "hd", "hexdump" }, { "which", "type" }, { "printenv", "env" }, { "?", "help" },
};

static const char *canonical(const char *name)
{
    for (size_t i = 0; i < sizeof(synonyms) / sizeof(synonyms[0]); i++)
        if (!strcmp(synonyms[i][0], name))
            return synonyms[i][1];
    return name;
}

bool sh_is_builtin(const char *name)
{
    return find_cmd(canonical(name)) != NULL;
}

const char *sh_alias_of(const char *name)
{
    struct alias *a = find_alias(name);
    return a ? a->value : NULL;
}

SH_CMD(help)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            const char *a = sh_alias_of(argv[i]);
            const struct sh_cmd *c = find_cmd(canonical(argv[i]));
            if (c) {
                sh_say("usage: %s\n  %s\n", c->usage, c->help);
            } else if (a) {
                sh_say("%s: an alias for: %s\n", argv[i], a);
            } else {
                sh_say("help: no command %s\n", argv[i]);
                return 1;
            }
        }
        return 0;
    }
    for (int k = 0; k < C_COUNT; k++) {
        sh_say("\033[1m%s\033[0m\n", cat_names[k]);
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
            if (cmds[i].cat != k)
                continue;
            /* The usage, then the first line of the help. */
            const char *h = cmds[i].help;
            size_t hl = strchr(h, '\n') ? (size_t)(strchr(h, '\n') - h) : strlen(h);
            sh_say("  %-28s %.*s\n", cmds[i].usage, (int)hl, h);
        }
    }
    sh_say("Programs in /boot/bin run by name (utest, contest, ...). Aliases: lspci lsusb ll.\n"
           "Lines: a ; b   a && b   a || b   a | b | c   NAME=value   $NAME   'quotes'  # comment\n"
           "Keys: Tab completes, left/right/home/end, backspace/delete, up/down history,\n"
           "Ctrl+C cancel, Ctrl+L clear, Shift+PageUp/PageDown scroll back. help <cmd>: details.\n");
    return 0;
}

/* ---- words: quotes, escapes, $ ----------------------------------------------------- */

static int last_status;
static int depth;

int sh_status(void)
{
    return last_status;
}

struct words {
    int   argc;
    char *argv[MAX_WORDS + 1];
    bool  quoted[MAX_WORDS];
};

static void words_free(struct words *w)
{
    for (int i = 0; i < w->argc; i++)
        free(w->argv[i]);
    w->argc = 0;
}

/* Append the value of the $ reference at *pp (just after '$') to b. */
static void expand_var(const char **pp, struct buf *b)
{
    const char *p = *pp;
    char name[32];
    size_t n = 0;
    if (*p == '?') {
        char num[16];
        snprintf(num, sizeof(num), "%d", last_status);
        buf_add(b, num, strlen(num));
        *pp = p + 1;
        return;
    }
    bool brace = *p == '{';
    if (brace)
        p++;
    while (name_char(*p, n == 0) && n < sizeof(name) - 1)
        name[n++] = *p++;
    name[n] = '\0';
    if (brace) {
        if (*p != '}') {   /* not a reference after all */
            buf_add(b, "${", 2);
            *pp = *pp + 1;
            return;
        }
        p++;
    }
    if (!n) {
        buf_add(b, "$", 1);   /* a lone $ */
        *pp = brace ? *pp + 1 : p;
        return;
    }
    const char *v = sh_getvar(name);
    if (v)
        buf_add(b, v, strlen(v));
    *pp = p;
}

/* Split one simple command's text into words. false on a syntax error
 * (said). */
static bool split_words(const char *s, struct words *w)
{
    w->argc = 0;
    const char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return true;
        if (w->argc == MAX_WORDS) {
            sh_tty("sh: more than %d words\n", MAX_WORDS);
            return false;
        }
        struct buf b = { 0 };
        bool quoted = false;
        while (*p && *p != ' ' && *p != '\t') {
            char c = *p;
            if (c == '\\' && p[1]) {
                buf_add(&b, p + 1, 1);
                p += 2;
                quoted = true;
            } else if (c == '\'') {
                const char *e = strchr(p + 1, '\'');
                if (!e) {
                    sh_tty("sh: no closing '\n");
                    free(b.p);
                    return false;
                }
                buf_add(&b, p + 1, (size_t)(e - p - 1));
                p = e + 1;
                quoted = true;
            } else if (c == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        buf_add(&b, p + 1, 1);
                        p += 2;
                    } else if (*p == '$') {
                        p++;
                        expand_var(&p, &b);
                    } else {
                        buf_add(&b, p++, 1);
                    }
                }
                if (*p != '"') {
                    sh_tty("sh: no closing \"\n");
                    free(b.p);
                    return false;
                }
                p++;
                quoted = true;
            } else if (c == '$') {
                p++;
                expand_var(&p, &b);
            } else {
                buf_add(&b, p++, 1);
            }
        }
        if (!b.n && !quoted) {   /* $UNSET alone: no word at all */
            free(b.p);
            continue;
        }
        char *word = malloc(b.n + 1);
        if (!word) {
            free(b.p);
            return false;
        }
        if (b.n)
            memcpy(word, b.p, b.n);
        word[b.n] = '\0';
        free(b.p);
        w->quoted[w->argc] = quoted;
        w->argv[w->argc++] = word;
        w->argv[w->argc] = NULL;
    }
}

/* "NAME=value" (NAME a valid name): the length of NAME, else 0. */
static size_t assignment(const char *word)
{
    size_t n = 0;
    while (name_char(word[n], n == 0))
        n++;
    return n && word[n] == '=' ? n : 0;
}

/* ---- running ------------------------------------------------------------------------- */

static int main_status;   /* set by sh_unknown while main.c runs a line */
static int exec_line(const char *line);

/* argv as a command: the table, else main.c (which calls sh_unknown for
 * what it doesn't know either). */
static int exec_argv(int argc, char **argv)
{
    const struct sh_cmd *c = find_cmd(canonical(argv[0]));
    if (c && c->fn)
        return c->fn(argc, argv);
    char line[256];
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (n + l + 2 > sizeof(line)) {
            sh_tty("%s: line too long\n", argv[0]);
            return 2;
        }
        if (i)
            line[n++] = ' ';
        memcpy(line + n, argv[i], l);
        n += l;
    }
    line[n] = '\0';
    main_status = 0;
    sh_main_command(line);
    return main_status;
}

/* 'word' with ' as '\'' : re-quoted, for an alias's arguments. */
static void quote_into(struct buf *b, const char *word)
{
    buf_add(b, " '", 2);
    for (const char *p = word; *p; p++) {
        if (*p == '\'')
            buf_add(b, "'\\''", 4);
        else
            buf_add(b, p, 1);
    }
    buf_add(b, "'", 1);
}

static int exec_simple(const char *text)
{
    struct words w;
    if (!split_words(text, &w))
        return 2;
    if (!w.argc)
        return last_status;
    int st = 0;
    /* Leading NAME=value words: on their own they set variables; before a
     * command they hold only while it runs (TZ=UTC date). */
    int na = 0;
    while (na < w.argc && assignment(w.argv[na]))
        na++;
    struct { char name[32]; char *old; bool had; } saved[8];
    int nsaved = 0;
    for (int i = 0; i < na; i++) {
        size_t l = assignment(w.argv[i]);
        char name[32];
        if (l >= sizeof(name))
            continue;
        memcpy(name, w.argv[i], l);
        name[l] = '\0';
        if (na < w.argc && nsaved < 8) {
            const char *old = sh_getvar(name);
            memcpy(saved[nsaved].name, name, l + 1);
            saved[nsaved].had = old != NULL;
            saved[nsaved].old = old ? dup_str(old) : NULL;
            nsaved++;
        }
        sh_setvar(name, w.argv[i] + l + 1, -1);
    }
    if (na < w.argc) {
        const char *a = w.quoted[na] ? NULL : sh_alias_of(w.argv[na]);
        if (a && depth < MAX_DEPTH) {
            struct buf b = { 0 };
            buf_add(&b, a, strlen(a));
            for (int i = na + 1; i < w.argc; i++)
                quote_into(&b, w.argv[i]);
            buf_add(&b, "", 1);
            st = b.p ? exec_line(b.p) : 2;
            free(b.p);
        } else {
            st = exec_argv(w.argc - na, w.argv + na);
        }
    }
    for (int i = 0; i < nsaved; i++) {
        if (saved[i].had)
            sh_setvar(saved[i].name, saved[i].old, -1);
        else
            sh_unsetvar(saved[i].name);
        free(saved[i].old);
    }
    words_free(&w);
    return st;
}

enum { OP_END, OP_PIPE, OP_SEMI, OP_AND, OP_OR };

struct seg {
    char *text;
    int   op;   /* what follows it */
};

#define MAX_SEGS 32

/* Cut a line at | ; && || outside quotes (and drop a # comment). The
 * texts point into `line`, which is modified. -1 on a syntax error. */
static int segments(char *line, struct seg *segs)
{
    int n = 0;
    char *start = line, *p = line;
    char q = 0;
    bool word_start = true;
    for (;;) {
        char c = *p;
        if (q) {
            if (c == '\\' && q == '"' && p[1]) {
                p += 2;
                continue;
            }
            if (!c)
                break;   /* split_words reports the missing quote */
            if (c == q)
                q = 0;
            p++;
            continue;
        }
        if (c == '\\' && p[1]) {
            p += 2;
            word_start = false;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            p++;
            word_start = false;
            continue;
        }
        if (c == '#' && word_start)
            *p = c = '\0';
        int op = !c ? OP_END : c == ';' ? OP_SEMI : c == '|' ? (p[1] == '|' ? OP_OR : OP_PIPE)
               : c == '&' ? (p[1] == '&' ? OP_AND : -1) : -2;
        if (op == -1) {
            sh_tty("sh: & (running in the background) is not supported\n");
            return -1;
        }
        if (op == -2) {
            word_start = c == ' ' || c == '\t';
            p++;
            continue;
        }
        if (n == MAX_SEGS) {
            sh_tty("sh: too many commands on one line\n");
            return -1;
        }
        *p = '\0';
        segs[n].text = start;
        segs[n].op = op;
        n++;
        if (op == OP_END)
            break;
        p += op == OP_AND || op == OP_OR ? 2 : 1;
        start = p;
        word_start = true;
    }
    if (q) {
        segs[n].text = start;
        segs[n].op = OP_END;
        n++;
    }
    /* An empty command next to | && || is a mistake; next to ; it's fine. */
    for (int i = 0; i < n; i++) {
        const char *t = segs[i].text;
        while (*t == ' ' || *t == '\t')
            t++;
        bool empty = !*t;
        bool needs = segs[i].op == OP_PIPE || segs[i].op == OP_AND || segs[i].op == OP_OR ||
                     (i > 0 && (segs[i - 1].op == OP_PIPE || segs[i - 1].op == OP_AND ||
                                segs[i - 1].op == OP_OR));
        if (empty && needs) {
            sh_tty("sh: a command is missing next to | && or ||\n");
            return -1;
        }
    }
    return n;
}

static int exec_pipeline(struct seg *s, int n)
{
    struct buf *out = capture;
    const char *saved_in = in_data;
    size_t saved_len = in_len;
    bool saved_have = have_in;
    struct buf bufs[2] = { { 0 }, { 0 } };
    int st = 0;
    for (int k = 0; k < n && !interrupted; k++) {
        struct buf *mine = k < n - 1 ? &bufs[k % 2] : NULL;
        if (mine) {
            mine->n = 0;
            mine->full = false;
        }
        capture = mine ? mine : out;
        if (k) {
            struct buf *prev = &bufs[(k - 1) % 2];
            in_data = prev->p;
            in_len = prev->n;
            have_in = true;
        }
        st = exec_simple(s[k].text);
        if (mine && mine->full)
            sh_tty("sh: pipe full: output past %u MiB dropped\n", PIPE_MAX >> 20);
    }
    free(bufs[0].p);
    free(bufs[1].p);
    capture = out;
    in_data = saved_in;
    in_len = saved_len;
    have_in = saved_have;
    return st;
}

static int exec_line(const char *line)
{
    if (depth >= MAX_DEPTH) {
        sh_tty("sh: nested too deep (an alias using itself?)\n");
        return 2;
    }
    char *copy = dup_str(line);
    if (!copy)
        return 2;
    depth++;
    struct seg segs[MAX_SEGS];
    int n = segments(copy, segs);
    int st = n < 0 ? 2 : last_status;
    int prev_op = OP_SEMI;
    for (int i = 0; i < n && !interrupted;) {
        int j = i;
        while (segs[j].op == OP_PIPE && j + 1 < n)
            j++;
        bool run = prev_op == OP_SEMI || (prev_op == OP_AND && st == 0) ||
                   (prev_op == OP_OR && st != 0);
        if (run) {
            st = exec_pipeline(segs + i, j - i + 1);
            last_status = st;
        }
        prev_op = segs[j].op;
        i = j + 1;
    }
    depth--;
    free(copy);
    return st;
}

int sh_run_words(int argc, char **argv)
{
    if (argc == 1)
        return exec_line(argv[0]);
    return exec_argv(argc, argv);
}

void sh_line(char *line)
{
    interrupted = false;
    last_status = exec_line(line);
    if (interrupted)
        last_status = 130;
}

int sh_run_program(int argc, char **argv);   /* cmds_shell.c */

void sh_unknown(int argc, char **argv)
{
    char path[SH_PATH_MAX];
    const void *d;
    uint64_t n;
    if (!strchr(argv[0], '/')) {
        snprintf(path, sizeof(path), "/boot/bin/%s", argv[0]);
        if (sh_read(path, &d, &n) == OK) {
            main_status = sh_run_program(argc, argv);
            return;
        }
    }
    sh_tty("%s: unknown command (try help)\n", argv[0]);
    main_status = 127;
}

void sh_init(void)
{
    sh_setvar("USER", "jam", 1);
    sh_setvar("HOME", "/boot", 1);
    sh_setvar("PATH", "/boot/bin", 1);
    sh_setvar("TZ", "Australia/Sydney", 1);
    sh_setvar("RTC", "local", 0);
    sh_setvar("HOSTNAME", "jamos", 0);
    set_alias("lspci", "devices");
    set_alias("lsusb", "usb");
    set_alias("ll", "ls -l");
}

/* ---- builtins that need this file's state ---------------------------------------------- */

SH_CMD(set)
{
    if (argc == 1) {
        for (int i = 0; i < MAX_VARS; i++)
            if (vars[i].value)
                sh_say("%s%s=%s\n", vars[i].exported ? "export " : "", vars[i].name,
                       vars[i].value);
        return 0;
    }
    if (argc == 3 && !assignment(argv[1])) {
        for (const char *p = argv[1]; *p; p++)
            if (!name_char(*p, p == argv[1])) {
                sh_tty("set: %s: not a variable name\n", argv[1]);
                return 1;
            }
        sh_setvar(argv[1], argv[2], -1);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        size_t l = assignment(argv[i]);
        if (!l || l >= 32) {
            sh_tty("usage: set NAME value, or set NAME=value\n");
            return 1;
        }
        char name[32];
        memcpy(name, argv[i], l);
        name[l] = '\0';
        sh_setvar(name, argv[i] + l + 1, -1);
    }
    return 0;
}

SH_CMD(unset)
{
    for (int i = 1; i < argc; i++)
        sh_unsetvar(argv[i]);
    return 0;
}

SH_CMD(export)
{
    if (argc == 1)
        return shc_env(argc, argv);
    for (int i = 1; i < argc; i++) {
        size_t l = assignment(argv[i]);
        char name[32];
        if (l && l < sizeof(name)) {
            memcpy(name, argv[i], l);
            name[l] = '\0';
            sh_setvar(name, argv[i] + l + 1, 1);
        } else if (find_var(argv[i])) {
            find_var(argv[i])->exported = true;
        } else if (!l) {
            sh_setvar(argv[i], "", 1);
        }
    }
    return 0;
}

SH_CMD(env)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < MAX_VARS; i++)
        if (vars[i].value && vars[i].exported)
            sh_say("%s=%s\n", vars[i].name, vars[i].value);
    return 0;
}

SH_CMD(alias)
{
    if (argc == 1) {
        for (int i = 0; i < MAX_ALIAS; i++)
            if (aliases[i].value)
                sh_say("alias %s='%s'\n", aliases[i].name, aliases[i].value);
        return 0;
    }
    int st = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *a = sh_alias_of(argv[i]);
            if (a) {
                sh_say("alias %s='%s'\n", argv[i], a);
            } else {
                sh_tty("alias: %s: not found\n", argv[i]);
                st = 1;
            }
            continue;
        }
        *eq = '\0';
        if (!set_alias(argv[i], eq + 1)) {
            sh_tty("alias: can't define %s\n", argv[i]);
            st = 1;
        }
    }
    return st;
}

SH_CMD(unalias)
{
    int st = 0;
    for (int i = 1; i < argc; i++) {
        struct alias *a = find_alias(argv[i]);
        if (a) {
            free(a->value);
            a->value = NULL;
        } else {
            sh_tty("unalias: %s: not found\n", argv[i]);
            st = 1;
        }
    }
    return st;
}

/* ---- tab completion ---------------------------------------------------------------------- */

#define MAX_CAND 128

struct cands {
    char     names[MAX_CAND][64];
    bool     dir[MAX_CAND];
    unsigned n;
};

static void cand_add(struct cands *c, const char *name, bool dir)
{
    for (unsigned i = 0; i < c->n; i++)
        if (!strcmp(c->names[i], name))
            return;
    if (c->n < MAX_CAND && strlen(name) < sizeof(c->names[0])) {
        memcpy(c->names[c->n], name, strlen(name) + 1);
        c->dir[c->n++] = dir;
    }
}

bool sh_complete(char *line, unsigned *len, unsigned *pos, unsigned cap)
{
    /* The word before the cursor, and whether it is the command's first. */
    unsigned ws = *pos;
    while (ws > 0 && line[ws - 1] != ' ')
        ws--;
    unsigned k = ws;
    while (k > 0 && line[k - 1] == ' ')
        k--;
    bool first = k == 0 || line[k - 1] == '|' || line[k - 1] == ';' || line[k - 1] == '&';
    char word[SH_PATH_MAX];
    unsigned wl = *pos - ws;
    if (wl >= sizeof(word))
        return false;
    memcpy(word, line + ws, wl);
    word[wl] = '\0';

    struct cands *c = calloc(1, sizeof(*c));
    if (!c)
        return false;
    const char *stem = word;   /* the part the candidates complete */
    if (first && !strchr(word, '/')) {
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
            if (!strncmp(cmds[i].name, word, wl))
                cand_add(c, cmds[i].name, false);
        for (int i = 0; i < MAX_ALIAS; i++)
            if (aliases[i].value && !strncmp(aliases[i].name, word, wl))
                cand_add(c, aliases[i].name, false);
        struct sh_dirent ents[64];
        int n = sh_readdir("/boot/bin", ents, 64);
        for (int i = 0; i < n; i++)
            if (!strncmp(ents[i].name, word, wl))
                cand_add(c, ents[i].name, false);
    } else {
        /* A path: list the directory part, match the last part. */
        char *slash = NULL;
        for (char *p = word; *p; p++)
            if (*p == '/')
                slash = p;
        char dir[SH_PATH_MAX], abs[SH_PATH_MAX];
        if (slash) {
            size_t dl = (size_t)(slash - word) + 1;
            memcpy(dir, word, dl);
            dir[dl] = '\0';
            stem = slash + 1;
        } else {
            memcpy(dir, ".", 2);
        }
        if (sh_resolve(dir, abs, sizeof(abs))) {
            struct sh_dirent *ents = calloc(256, sizeof(*ents));
            int n = ents ? sh_readdir(abs, ents, 256) : -1;
            for (int i = 0; i < n; i++)
                if (!strncmp(ents[i].name, stem, strlen(stem)))
                    cand_add(c, ents[i].name, ents[i].dir);
            free(ents);
        }
    }
    bool redraw = false;
    size_t sl = strlen(stem);
    if (c->n) {
        /* The longest common prefix of the candidates. */
        size_t common = strlen(c->names[0]);
        for (unsigned i = 1; i < c->n; i++) {
            size_t j = 0;
            while (j < common && c->names[i][j] == c->names[0][j])
                j++;
            common = j;
        }
        char add[80];
        size_t na = 0;
        if (common > sl) {
            na = common - sl;
            memcpy(add, c->names[0] + sl, na);
        }
        if (c->n == 1)
            add[na++] = c->dir[0] ? '/' : ' ';
        if (na && *len + na <= cap) {
            memmove(line + *pos + na, line + *pos, *len - *pos);
            memcpy(line + *pos, add, na);
            *len += (unsigned)na;
            *pos += (unsigned)na;
            redraw = true;
        } else if (c->n > 1) {
            /* Nothing to add: show the choices, then the line again. */
            sh_put("\r\n", 2);
            unsigned col = 0;
            for (unsigned i = 0; i < c->n; i++) {
                sh_say("%s%s%s", c->names[i], c->dir[i] ? "/" : "", "  ");
                col += (unsigned)strlen(c->names[i]) + 2 + c->dir[i];
                if (col > 70) {
                    sh_put("\r\n", 2);
                    col = 0;
                }
            }
            if (col)
                sh_put("\r\n", 2);
            redraw = true;
        }
    }
    free(c);
    return redraw;
}
