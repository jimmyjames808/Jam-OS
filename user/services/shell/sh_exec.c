/* Running a line: its lists (; && || &), each list's pipeline, each
 * stage's NAME=value words and alias, and then the command: one in the
 * table, else a program in a directory of $PATH (/boot/bin and
 * /boot/usr/bin to begin with); a POSIX program takes the stage's
 * redirections (< > >> 2>&1: sh_posix.c). The POSIX programs of a pipeline
 * run at once, joined by pipes; the shell's commands run in turn (see
 * exec_pipeline). A command before a single & is a
 * program started in the background (sh_jobs.c): a program's name, `run`
 * or `jamjar` (a command that is only its program); another shell
 * command, an alias or a pipeline before & is refused. */
#include "sh_core.h"

#define MAX_DEPTH 8   /* aliases in aliases, watch 'a | b' in a line, ... */

static int last_status;
static int depth;

static int exec_line(const char *line);

int sh_status(void)
{
    return last_status;
}

/* The program called name (no '/') in the first directory of $PATH that
 * has one (SH_PATH_DEFAULT when PATH is unset), as an absolute path into
 * path. false if there is none. */
static bool path_find(const char *name, char *path, size_t cap)
{
    const char *dirs = sh_getvar("PATH");
    if (!dirs)
        dirs = SH_PATH_DEFAULT;
    while (*dirs) {
        const char *end = strchr(dirs, ':');
        size_t l = end ? (size_t)(end - dirs) : strlen(dirs);
        char dir[SH_PATH_MAX], abs[SH_PATH_MAX];
        bool is_dir;
        uint64_t n;
        if (l && l < sizeof(dir)) {
            memcpy(dir, dirs, l);
            dir[l] = '\0';
            if (sh_resolve(dir, abs, sizeof(abs)) && sh_join(abs, name, path, cap) &&
                sh_stat(path, &is_dir, &n) == OK && !is_dir)
                return true;
        }
        dirs += end ? l + 1 : l;
    }
    return false;
}

/* Not in the table: a program of that name in a directory of $PATH if
 * there is one (bg: started in the background). One in /boot/bin keeps its
 * bare name (what it is called as, and said); one found elsewhere is
 * started by its path. */
static int exec_unknown(int argc, char **argv, bool bg)
{
    char path[SH_PATH_MAX], std[SH_PATH_MAX];
    if (!strchr(argv[0], '/') && path_find(argv[0], path, sizeof(path))) {
        snprintf(std, sizeof(std), "/boot/bin/%s", argv[0]);
        char *by_path[SH_MAX_WORDS + 1];
        if (strcmp(path, std)) {
            memcpy(by_path, argv, (size_t)(argc + 1) * sizeof(argv[0]));
            by_path[0] = path;
            argv = by_path;
        }
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
    if (c && c->fn != shc_run && sh_redir_count()) {
        sh_tty("sh: %s is a shell command: < and > work for POSIX programs only\n", argv[0]);
        return 2;
    }
    if (!bg)
        return c ? c->fn(argc, argv) : exec_unknown(argc, argv, false);
    if (!c)
        return exec_unknown(argc, argv, true);
    if (c->fn == shc_run)
        return argc > 1 ? sh_start_background(argc - 1, argv + 1) : c->fn(argc, argv);
    if (c->fn == shc_jamjar)   /* `run jamjar`, as the command itself is */
        return sh_start_background(argc, argv);
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
    if (!w.argc) {
        sh_words_free(&w);   /* a redirection alone makes no file here */
        return last_status;
    }
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
        } else if (a && w.nredir) {
            sh_tty("sh: < and > don't follow aliases (%s): type the program's name\n",
                   w.argv[na]);
            st = 2;
        } else if (a && depth < MAX_DEPTH) {
            st = exec_alias(a, w.argc - na, w.argv + na);
        } else {
            sh_redir_set(w.redir, w.nredir);
            st = exec_argv(w.argc - na, w.argv + na, bg);
            sh_redir_set(NULL, 0);
        }
    }
    restore_vars(saved, nsaved);
    sh_words_free(&w);
    return st;
}

/* Whether the stage's text is a POSIX program, by its first word as
 * exec_argv and exec_unknown would find it (a builtin, an alias, a name
 * with a '/' or a NAME=value before it: no). Looked at without a word
 * said; it runs nothing. */
static bool stage_is_posix(const char *text)
{
    struct sh_words w;
    sh_quiet(true);
    bool ok = sh_split_words(text, &w);
    sh_quiet(false);
    if (!ok)
        return false;
    bool posix = false;
    if (w.argc && !sh_assignment(w.argv[0]) && !strchr(w.argv[0], '/') &&
        !sh_find_cmd(w.argv[0]) && (w.quoted[0] || !sh_alias_of(w.argv[0]))) {
        char path[SH_PATH_MAX], std[SH_PATH_MAX];
        if (path_find(w.argv[0], path, sizeof(path))) {
            snprintf(std, sizeof(std), "/boot/bin/%s", w.argv[0]);
            posix = sh_program_is_posix(strcmp(path, std) ? path : w.argv[0]);
        }
    }
    sh_words_free(&w);
    return posix;
}

/* "sh: pipe full ..." once for each buffer that overflowed, after the
 * stage that wrote it has ended. */
static void say_if_full(struct sh_buf *b)
{
    if (!b->full)
        return;
    b->full = false;
    sh_tty("sh: pipe full: output past %u MiB dropped\n", SH_PIPE_MAX >> 20);
}

/* Stages a | b | c. The shell's own commands run in turn: each one's
 * output is captured for the next, the last prints where the pipeline
 * prints. A POSIX program starts and is left running: when the next stage
 * is one too, its stdout is a pipe the next program reads (the shell takes
 * no part in the bytes), so a chain of them runs at once, like any shell's.
 * Before a stage that reads a captured buffer (a builtin, or a program
 * after one) the programs before it are waited for (sh_pipe_settle: it
 * copies what a program wrote for a builtin, and sends Ctrl+C), and again
 * at the end; the status is the last stage's. */
static int exec_pipeline(struct sh_seg *s, int n)
{
    struct sh_stdio outer = sh_stdio_get();
    struct sh_buf bufs[2] = { { 0 }, { 0 } };
    handle_t carry = HANDLE_INVALID;   /* the read end of the last program's pipe */
    bool last_deferred = false;
    int st = 0;
    for (int k = 0; k < n && !sh_cancelled(); k++) {
        struct sh_buf *mine = k < n - 1 ? &bufs[k % 2] : NULL;
        if (mine) {
            mine->n = 0;
            mine->full = false;
        }
        if (k && carry == HANDLE_INVALID) {
            (void)sh_pipe_settle();   /* the buffer it reads is whole now */
            say_if_full(&bufs[(k - 1) % 2]);
        }
        struct sh_stdio io = outer;
        io.out = mine ? mine : outer.out;
        io.defer = n > 1;
        io.direct_out = mine && stage_is_posix(s[k + 1].text);
        io.direct_in = carry;
        io.direct_next = HANDLE_INVALID;
        if (k) {
            struct sh_buf *prev = &bufs[(k - 1) % 2];
            io.in = prev->p;
            io.in_len = prev->n;
            io.have_in = true;
        }
        carry = HANDLE_INVALID;
        sh_stdio_set(io);
        unsigned before = sh_pipe_running();
        st = exec_simple(s[k].text, false);
        last_deferred = sh_pipe_running() > before;
        io = sh_stdio_get();
        if (io.direct_in != HANDLE_INVALID)
            jam_handle_close(io.direct_in);   /* not taken: the stage was no program */
        carry = io.direct_next;
        if (mine && !last_deferred)
            say_if_full(mine);
    }
    int ps = sh_pipe_settle();
    if (last_deferred && ps >= 0)
        st = ps;
    say_if_full(&bufs[0]);
    say_if_full(&bufs[1]);
    if (carry != HANDLE_INVALID)
        jam_handle_close(carry);
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
    sh_setvar("HOME", "/", 1);
    sh_setvar("PATH", SH_PATH_DEFAULT, 1);
    sh_setvar("HOSTNAME", "jamos", 0);
    sh_set_alias("lspci", "devices");
    sh_set_alias("lsusb", "usb");
    sh_set_alias("ll", "ls -l");
}
