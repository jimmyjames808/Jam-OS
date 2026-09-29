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
        /* Its whole job: whatever it started (even orphans) goes too, and
         * job_kill returns once all of it is dead. */
        say("init: %s: still running after %d s, killing its job", argv[0], RUN_TIMEOUT_S);
        if (jam_job_kill(job) != OK)
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

/* M6: userboot gives init the root resource (SR_RESOURCE). Check it is
 * one: slicing RES_PCI out of it works, a zero-sized MMIO slice doesn't.
 * (devmgr will get that RES_PCI in phase 2.) */
static bool check_root_resource(void)
{
    handle_t root = startup_handle(SR_RESOURCE), pci, bad;
    if (root == HANDLE_INVALID) {
        say("init: no root resource (SR_RESOURCE) in the startup message");
        return false;
    }
    status_t st = jam_resource_create(root, RES_PCI, 0, 0, &pci);
    if (st != OK) {
        say("init: can't slice RES_PCI from the root resource (%s)", status_str(st));
        return false;
    }
    jam_handle_close(pci);
    st = jam_resource_create(root, RES_MMIO, 0x100000000ull, 0, &bad);
    if (st != ERR_INVALID_ARGS) {
        say("init: a zero-sized MMIO slice gave %s, want ERR_INVALID_ARGS", status_str(st));
        if (st == OK)
            jam_handle_close(bad);
        return false;
    }
    return true;
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

    if (!check_root_resource())
        return 1;

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
