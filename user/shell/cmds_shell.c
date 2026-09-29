/* Shell commands: echo true false type time sleep repeat watch run. (set,
 * unset, export, env, alias, unalias and help are in sh_exec.c, next to
 * the tables they use.) */
#include "sh.h"

char **sh_make_env(void);          /* sh_exec.c */
void   sh_free_env(char **env);
bool   sh_is_builtin(const char *name);
const char *sh_alias_of(const char *name);

SH_CMD(echo)
{
    bool newline = true, esc = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        bool ok = true;
        for (const char *p = argv[i] + 1; *p; p++)
            ok &= *p == 'n' || *p == 'e';
        if (!ok)
            break;   /* an ordinary word that starts with - */
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n')
                newline = false;
            else
                esc = true;
        }
    }
    for (int k = i; k < argc; k++) {
        if (k > i)
            sh_put(" ", 1);
        const char *s = argv[k];
        if (!esc) {
            sh_put(s, strlen(s));
            continue;
        }
        for (; *s; s++) {
            char c = *s;
            if (c == '\\' && s[1]) {
                s++;
                c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'e' ? '\033' : *s;
            }
            sh_put(&c, 1);
        }
    }
    if (newline)
        sh_put("\n", 1);
    return 0;
}

SH_CMD(true)
{
    (void)argc;
    (void)argv;
    return 0;
}

SH_CMD(false)
{
    (void)argc;
    (void)argv;
    return 1;
}

SH_CMD(type)
{
    int st = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = sh_alias_of(argv[i]);
        char path[SH_PATH_MAX];
        const void *d;
        uint64_t n;
        snprintf(path, sizeof(path), "/boot/bin/%s", argv[i]);
        if (a)
            sh_say("%s is an alias for %s\n", argv[i], a);
        else if (sh_is_builtin(argv[i]))
            sh_say("%s is a shell builtin\n", argv[i]);
        else if (!strchr(argv[i], '/') && sh_read(path, &d, &n) == OK)
            sh_say("%s is %s\n", argv[i], path);
        else {
            sh_say("%s: not found\n", argv[i]);
            st = 1;
        }
    }
    return st;
}

/* "1.5" seconds -> ns; false if it isn't a number. */
static bool parse_seconds(const char *s, uint64_t *ns)
{
    uint64_t whole = 0, frac = 0, scale = SH_S;
    const char *p = s;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; p++, any = true) {
        if (whole > 1000000)
            return false;
        whole = whole * 10 + (uint64_t)(*p - '0');
    }
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, any = true)
            if (scale >= 10) {
                scale /= 10;
                frac += (uint64_t)(*p - '0') * scale;
            }
    if (*p || !any)
        return false;
    *ns = whole * SH_S + frac;
    return true;
}

SH_CMD(sleep)
{
    uint64_t ns;
    if (argc != 2 || !parse_seconds(argv[1], &ns)) {
        sh_tty("usage: sleep <seconds>\n");
        return 2;
    }
    return sh_sleep(ns) ? 0 : 130;
}

SH_CMD(time)
{
    if (argc < 2) {
        sh_tty("usage: time <command...>\n");
        return 2;
    }
    uint64_t t0 = (uint64_t)jam_clock_get();
    int st = sh_run_words(argc - 1, argv + 1);
    uint64_t us = ((uint64_t)jam_clock_get() - t0) / 1000;
    sh_tty("time: %lu.%06lu s (status %d)\n", (unsigned long)(us / 1000000),
           (unsigned long)(us % 1000000), st);
    return st;
}

SH_CMD(repeat)
{
    uint64_t n;
    if (argc < 3 || !sh_parse_u64(argv[1], &n)) {
        sh_tty("usage: repeat <n> <command...>\n");
        return 2;
    }
    int st = 0;
    for (uint64_t i = 0; i < n && !sh_interrupted(); i++)
        st = sh_run_words(argc - 2, argv + 2);
    return st;
}

SH_CMD(watch)
{
    uint64_t every = 2 * SH_S;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        if (!parse_seconds(argv[i + 1], &every) || every < 100 * SH_MS) {
            sh_tty("watch: -n: at least 0.1 seconds\n");
            return 2;
        }
        i += 2;
    }
    if (i >= argc) {
        sh_tty("usage: watch [-n seconds] <command...>\n");
        return 2;
    }
    int st = 0;
    bool screen = !sh_piped();
    while (!sh_interrupted()) {
        if (screen)
            sh_say("\033[2J\033[H");
        sh_say("Every %lu.%lus: ", (unsigned long)(every / SH_S),
               (unsigned long)(every % SH_S / (100 * SH_MS)));
        for (int k = i; k < argc; k++)
            sh_say("%s%s", argv[k], k + 1 < argc ? " " : "\n");
        st = sh_run_words(argc - i, argv + i);
        if (!sh_sleep(every))
            break;
    }
    return st;
}

/* ---- run: start a program from /boot ------------------------------------------------ */

/* Copy what the program wrote to its SR_STDOUT channel into our output. */
static void drain(handle_t out)
{
    char buf[4096];
    for (;;) {
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = out, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        if (jam_channel_read(&a) != OK)
            return;
        sh_put(buf, n);
    }
}

int sh_run_program(int argc, char **argv)
{
    /* The bootfs path: "utest" -> bin/utest; "bin/x" as it is (as before);
     * else a path through the cwd ("/boot/bin/x", "../drv/x"). */
    char path[SH_PATH_MAX];
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK) {
        sh_tty("run: no bootfs\n");
        return 127;
    }
    if (!strchr(argv[0], '/')) {
        snprintf(path, sizeof(path), "bin/%s", argv[0]);
    } else if (bootfs_lookup(fs, argv[0], &data, &size) == OK) {
        snprintf(path, sizeof(path), "%s", argv[0]);
    } else {
        char abs[SH_PATH_MAX];
        const char *name = sh_resolve(argv[0], abs, sizeof(abs)) ? sh_bootfs_name(abs) : NULL;
        snprintf(path, sizeof(path), "%s", name ? name : argv[0]);
    }
    if (bootfs_lookup(fs, path, &data, &size) != OK) {
        sh_tty("run: no %s in bootfs\n", path);
        return 127;
    }
    handle_t job, proc, out_r = HANDLE_INVALID, out_w = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        sh_tty("run: no job (%s)\n", status_str(st));
        return 126;
    }
    struct spawn_handle x[3];
    unsigned nx = 0;
    handle_t h;
    if (sh_devmgr() && jam_handle_duplicate(sh_devmgr(), RIGHT_SAME, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_DEVMGR, h };
    if (jam_handle_duplicate(sh_console(), RIGHT_SAME, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_CONSOLE, h };
    /* In a pipe: its printf goes down a channel to us (libos printf.c). */
    if (sh_piped() && jam_channel_create(&out_r, &out_w) == OK)
        x[nx++] = (struct spawn_handle){ SR_STDOUT, out_w };
    char **env = sh_make_env();
    const char *args[20];
    int n = 0;
    args[n++] = path;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    struct spawn_args a = {
        .path = path, .argc = n, .argv = args, .job = job, .extra = x, .nextra = nx,
        .envp = (const char *const *)env,
    };
    uint64_t t0 = (uint64_t)jam_clock_get();
    st = spawn(&a, &proc);
    sh_free_env(env);
    if (st != OK) {
        sh_tty("run: can't start %s (%s)\n", path, status_str(st));
        if (out_r)
            jam_handle_close(out_r);
        jam_handle_close(job);
        return 126;
    }
    sh_tty("run: %s started (Ctrl+C kills it)\n", path);
    sh_flush();
    struct process_info info;
    bool killed = false;
    while ((st = spawn_wait(proc, 50 * SH_MS, &info)) == ERR_TIMED_OUT) {
        if (out_r)
            drain(out_r);
        if (sh_interrupted() && !killed) {
            sh_tty("^C: killing %s\n", path);
            sh_flush();
            jam_job_kill(job);
            killed = true;
        }
    }
    if (out_r) {
        drain(out_r);
        jam_handle_close(out_r);
    }
    uint64_t ms = ((uint64_t)jam_clock_get() - t0) / SH_MS;
    int code;
    if (st != OK) {
        sh_tty("run: lost track of %s (%s)\n", path, status_str(st));
        code = 126;
    } else if (info.killed) {
        sh_tty("run: %s was killed after %lu ms\n", path, (unsigned long)ms);
        code = 137;
    } else {
        sh_tty("run: %s exited with code %ld after %lu ms\n", path, (long)info.exit_code,
               (unsigned long)ms);
        code = info.exit_code < 0 ? 1 : info.exit_code > 255 ? 255 : (int)info.exit_code;
    }
    struct job_info ji;
    if (jam_job_get_info(job, &ji) == OK) {
        bool clean = true;
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            clean &= ji.used[k] == 0;
        if (!clean)
            sh_tty("run: its job still holds %lu pages, %lu handles, %lu threads\n",
                   (unsigned long)ji.used[JOB_LIMIT_PAGES], (unsigned long)ji.used[JOB_LIMIT_HANDLES],
                   (unsigned long)ji.used[JOB_LIMIT_THREADS]);
    }
    jam_handle_close(proc);
    jam_handle_close(job);
    return code;
}

SH_CMD(run)
{
    if (argc < 2) {
        sh_tty("usage: run <prog> [args]\n");
        return 2;
    }
    return sh_run_program(argc - 1, argv + 1);
}
