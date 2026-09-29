/* init: the first user process, started by the kernel's userboot with the
 * root job and the bootfs image.
 *
 * It runs the programs listed in init.cfg one after another, each as a
 * real child process in a job of its own (a child of init's job), waits
 * for each to finish and reports how it ended: the lines go into the
 * kernel's RESULTS box. init exits 0 if every program exited 0. */
#include <os.h>

#define MAX_WORDS     16
#define RUN_TIMEOUT_S 240   /* per program */

/* Split one init.cfg line into words (in place). Returns how many. */
static int split(char *line, char **words)
{
    int n = 0;
    for (char *p = line; *p;) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;
        if (n == MAX_WORDS)
            return -1;
        words[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return n;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jam_debug_report(buf, (uint64_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

/* Run one program to completion. Returns true if it exited 0. */
static bool run(int argc, char **argv)
{
    handle_t job, proc;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        say("init: %s: no job (%s)", argv[0], status_str(st));
        return false;
    }
    struct spawn_args a = {
        .path = argv[0], .argc = argc, .argv = (const char *const *)argv, .job = job,
    };
    uint64_t t0 = (uint64_t)jam_clock_get();
    st = spawn(&a, &proc);
    if (st != OK) {
        say("init: %s: could not start (%s)", argv[0], status_str(st));
        jam_handle_close(job);
        return false;
    }
    struct process_info info;
    st = spawn_wait(proc, RUN_TIMEOUT_S * 1000000000ull, &info);
    if (st == ERR_TIMED_OUT) {
        say("init: %s: still running after %d s, killing it", argv[0], RUN_TIMEOUT_S);
        jam_process_kill(proc);
        st = spawn_wait(proc, 10000000000ull, &info);
    }
    uint64_t ms = ((uint64_t)jam_clock_get() - t0) / 1000000;
    bool ok = false;
    if (st != OK)
        say("init: %s: lost track of it (%s)", argv[0], status_str(st));
    else if (info.killed)
        say("init: %s was killed after %lu ms", argv[0], (unsigned long)ms);
    else {
        say("init: %s exited with code %ld after %lu ms", argv[0], (long)info.exit_code,
            (unsigned long)ms);
        ok = info.exit_code == 0;
    }
    jam_handle_close(proc);
    jam_handle_close(job);
    return ok;
}

/* init.cfg: one program per line, "<path in bootfs> [args...]"; blank lines
 * and lines starting with '#' are ignored. */
static bool run_config(const char *cfg, uint64_t len)
{
    char *text = malloc(len + 1);
    if (!text) {
        say("init: no memory for init.cfg");
        return false;
    }
    memcpy(text, cfg, len);
    text[len] = '\0';

    bool ok = true;
    int lineno = 0;
    for (char *line = text; line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        lineno++;
        char *words[MAX_WORDS + 1];
        int n = line[0] == '#' ? 0 : split(line, words);
        if (n < 0) {
            say("init: init.cfg:%d: more than %d words", lineno, MAX_WORDS);
            ok = false;
        } else if (n > 0) {
            words[n] = NULL;
            ok &= run(n, words);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    return ok;
}

int main(int argc, char **argv)
{
    printf("init: hello from ring 3 (%d arg%s:", argc, argc == 1 ? "" : "s");
    for (int i = 0; i < argc; i++)
        printf(" %s", argv[i]);
    printf(")\n");
    for (unsigned i = 0; i < startup_handle_count(); i++) {
        uint32_t role;
        handle_t h = startup_handle_at(i, &role);
        printf("init: handle %u = %#x (%s)\n", i, h, startup_role_name(role));
    }

    const struct bootfs_view *fs;
    status_t st = bootfs_default(&fs);
    if (st != OK) {
        say("init: can't map bootfs (%s)", status_str(st));
        return 1;
    }
    const void *cfg;
    uint64_t len;
    st = bootfs_lookup(fs, "init.cfg", &cfg, &len);
    if (st != OK) {
        say("init: no init.cfg in bootfs (%s)", status_str(st));
        return 1;
    }
    return run_config(cfg, len) ? 0 : 1;
}
