/* Running a line: its lists (; && || &), each list's pipeline, each
 * stage's NAME=value words and alias, and then the command: one in the
 * table, else a program in /boot/bin. A command before a single & is a
 * program started in the background (sh_jobs.c): a program's name or
 * `run`; a shell command, an alias or a pipeline before & is refused. */
#include "sh_core.h"

#define MAX_DEPTH 8   /* aliases in aliases, watch 'a | b' in a line, ... */

static int last_status;
static int depth;

static int exec_line(const char *line);

int sh_status(void)
{
    return last_status;
}

/* Not in the table: /boot/bin/<name> if there is one (bg: started in the
 * background). */
static int exec_unknown(int argc, char **argv, bool bg)
{
    char path[SH_PATH_MAX];
    bool dir;
    uint64_t n;
    if (!strchr(argv[0], '/')) {
        snprintf(path, sizeof(path), "/boot/bin/%s", argv[0]);
        if (sh_stat(path, &dir, &n) == OK && !dir)
            return bg ? sh_start_background(argc, argv) : sh_run_program(argc, argv);
    }
    sh_tty("%s: unknown command (try help)\n", argv[0]);
    return 127;
}

/* The command argv; bg: before a &, so only a program (its name, or
 * `run prog`), started in the background. */
static int exec_argv(int argc, char **argv, bool bg)
{
    const struct sh_cmd *c = sh_find_cmd(argv[0]);
    if (!bg)
        return c ? c->fn(argc, argv) : exec_unknown(argc, argv, false);
    if (!c)
        return exec_unknown(argc, argv, true);
    if (c->fn == shc_run)
        return argc > 1 ? sh_start_background(argc - 1, argv + 1) : c->fn(argc, argv);
    sh_tty("sh: only a program runs in the background (& after %s, a shell command): "
           "e.g. run utest &\n", argv[0]);
    return 2;
}

/* 'word' with ' as '\'' : re-quoted, for an alias's arguments. */
static void quote_into(struct sh_buf *b, const char *word)
{
    sh_buf_add(b, " '", 2);
    for (const char *p = word; *p; p++) {
        if (*p == '\'')
            sh_buf_add(b, "'\\''", 4);
        else
            sh_buf_add(b, p, 1);
    }
    sh_buf_add(b, "'", 1);
}

/* The alias's text with the arguments after it, run as a line. */
static int exec_alias(const char *alias, int argc, char **argv)
{
    struct sh_buf b = { 0 };
    sh_buf_add(&b, alias, strlen(alias));
    for (int i = 1; i < argc; i++)
        quote_into(&b, argv[i]);
    sh_buf_add(&b, "", 1);
    int st = b.p ? exec_line(b.p) : 2;
    free(b.p);
    return st;
}

struct saved_var {
    char  name[SH_NAME_MAX];   /* NAME of a NAME=value prefix */
    char *old;                 /* its value before (malloc'd copy) */
    bool  had;                 /* it was set before */
};

#define MAX_SAVED 8

/* Leading NAME=value words: on their own they set variables; before a
 * command they hold only while it runs (TZ=UTC date), so the old values
 * go into saved. The number of them; *nsaved how many were saved. */
static int assign_leading(struct sh_words *w, struct saved_var *saved, int *nsaved)
{
    int na = 0;
    while (na < w->argc && sh_assignment(w->argv[na]))
        na++;
    for (int i = 0; i < na; i++) {
        size_t l = sh_assignment(w->argv[i]);
        char name[SH_NAME_MAX];
        if (l >= sizeof(name))
            continue;
        memcpy(name, w->argv[i], l);
        name[l] = '\0';
        if (na < w->argc && *nsaved < MAX_SAVED) {
            const char *old = sh_getvar(name);
            struct saved_var *s = &saved[(*nsaved)++];
            memcpy(s->name, name, l + 1);
            s->had = old != NULL;
            s->old = old ? sh_strdup(old) : NULL;
        }
        sh_setvar(name, w->argv[i] + l + 1, -1);
    }
    return na;
}

static void restore_vars(struct saved_var *saved, int nsaved)
{
    for (int i = 0; i < nsaved; i++) {
        if (saved[i].had)
            sh_setvar(saved[i].name, saved[i].old, -1);
        else
            sh_unsetvar(saved[i].name);
        free(saved[i].old);
    }
}

static int exec_simple(const char *text, bool bg)
{
    struct sh_words w;
    if (!sh_split_words(text, &w))
        return 2;
    if (!w.argc)
        return last_status;
    int st = 0;
    struct saved_var saved[MAX_SAVED];
    int nsaved = 0;
    int na = assign_leading(&w, saved, &nsaved);
    if (na < w.argc) {
        const char *a = w.quoted[na] ? NULL : sh_alias_of(w.argv[na]);
        if (a && bg) {
            sh_tty("sh: & doesn't follow aliases (%s): type the program's name\n",
                   w.argv[na]);
            st = 2;
        } else if (a && depth < MAX_DEPTH) {
            st = exec_alias(a, w.argc - na, w.argv + na);
        } else {
            st = exec_argv(w.argc - na, w.argv + na, bg);
        }
    }
    restore_vars(saved, nsaved);
    sh_words_free(&w);
    return st;
}

/* Stages a | b | c in turn: each one's output is captured for the next,
 * the last prints where the pipeline prints. */
static int exec_pipeline(struct sh_seg *s, int n)
{
    struct sh_stdio outer = sh_stdio_get();
    struct sh_buf bufs[2] = { { 0 }, { 0 } };
    int st = 0;
    for (int k = 0; k < n && !sh_cancelled(); k++) {
        struct sh_buf *mine = k < n - 1 ? &bufs[k % 2] : NULL;
        if (mine) {
            mine->n = 0;
            mine->full = false;
        }
        struct sh_stdio io = sh_stdio_get();
        io.out = mine ? mine : outer.out;
        if (k) {
            struct sh_buf *prev = &bufs[(k - 1) % 2];
            io.in = prev->p;
            io.in_len = prev->n;
            io.have_in = true;
        }
        sh_stdio_set(io);
        st = exec_simple(s[k].text, false);
        if (mine && mine->full)
            sh_tty("sh: pipe full: output past %u MiB dropped\n", SH_PIPE_MAX >> 20);
    }
    free(bufs[0].p);
    free(bufs[1].p);
    sh_stdio_set(outer);
    return st;
}

/* Every stage of a pipeline would have to run at once to put one in the
 * background, and the shell runs them in turn. */
static int refuse_bg_pipe(void)
{
    sh_tty("sh: a pipeline can't run in the background (| before &): start one program "
           "with &\n");
    return 2;
}

static int exec_line(const char *line)
{
    if (depth >= MAX_DEPTH) {
        sh_tty("sh: nested too deep (an alias using itself?)\n");
        return 2;
    }
    char *copy = sh_strdup(line);
    if (!copy)
        return 2;
    depth++;
    struct sh_seg segs[SH_MAX_SEGS];
    int n = sh_segments(copy, segs);
    int st = n < 0 ? 2 : last_status;
    int prev_op = SH_OP_SEMI;
    for (int i = 0; i < n && !sh_cancelled();) {
        int j = i;
        while (segs[j].op == SH_OP_PIPE && j + 1 < n)
            j++;
        bool run = prev_op == SH_OP_SEMI || prev_op == SH_OP_BG ||
                   (prev_op == SH_OP_AND && st == 0) || (prev_op == SH_OP_OR && st != 0);
        if (run) {
            st = segs[j].op != SH_OP_BG ? exec_pipeline(segs + i, j - i + 1)
                 : j == i               ? exec_simple(segs[i].text, true)
                                        : refuse_bg_pipe();
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
    return exec_argv(argc, argv, false);
}

void sh_line(char *line)
{
    sh_io_new_line();
    last_status = exec_line(line);
    if (sh_cancelled())
        last_status = 130;
}

void sh_init(void)
{
    sh_handles_init();
    sh_setvar("USER", "jam", 1);
    sh_setvar("HOME", "/boot", 1);
    sh_setvar("PATH", "/boot/bin", 1);
    sh_setvar("HOSTNAME", "jamos", 0);
    sh_set_alias("lspci", "devices");
    sh_set_alias("lsusb", "usb");
    sh_set_alias("ll", "ls -l");
}
