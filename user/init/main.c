/* init: the first user process, started by the kernel's userboot with the
 * root job, the root resource and the bootfs image.
 *
 * M6: it first starts devmgr (bin/devmgr, if bootfs has it) in a job of
 * its own with a RES_PCI resource sliced from the root, and waits until
 * devmgr has bound its drivers. Then it runs the programs listed in
 * init.cfg one after another, each as a real child process in a job of
 * its own (a child of init's job) with a client end of devmgr's channel
 * (SR_DEVMGR), waits for each to finish and reports how it ended: the
 * lines go into the kernel's RESULTS box. At the end it closes its end of
 * devmgr's channel, which stops devmgr and its drivers, and waits for
 * that. init exits 0 if every program (and devmgr) exited 0. */
#include <os.h>
#include <devmgr.h>

bool init_xhcitest(void);   /* xhcitest.c */

#define MAX_WORDS     16
#define RUN_TIMEOUT_S 240   /* per program */
#define S             1000000000ull

static handle_t devmgr_ch, devmgr_proc, devmgr_job;   /* 0: no devmgr */

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

void init_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void init_say(const char *fmt, ...)
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
        init_say("init: %s: no job (%s)", argv[0], status_str(st));
        return false;
    }
    struct spawn_handle x = { SR_DEVMGR, HANDLE_INVALID };
    if (devmgr_ch && (st = jam_handle_duplicate(devmgr_ch, RIGHT_SAME, &x.h)) != OK) {
        init_say("init: %s: no devmgr channel for it (%s)", argv[0], status_str(st));
        jam_handle_close(job);
        return false;
    }
    struct spawn_args a = {
        .path = argv[0], .argc = argc, .argv = (const char *const *)argv, .job = job,
        .extra = x.h ? &x : NULL, .nextra = x.h ? 1 : 0,
    };
    uint64_t t0 = (uint64_t)jam_clock_get();
    st = spawn(&a, &proc);
    if (st != OK) {
        init_say("init: %s: could not start (%s)", argv[0], status_str(st));
        jam_handle_close(job);
        return false;
    }
    struct process_info info;
    st = spawn_wait(proc, RUN_TIMEOUT_S * 1000000000ull, &info);
    if (st == ERR_TIMED_OUT) {
        /* Its whole job: whatever it started (even orphans) goes too, and
         * job_kill returns once all of it is dead. */
        init_say("init: %s: still running after %d s, killing its job", argv[0], RUN_TIMEOUT_S);
        if (jam_job_kill(job) != OK)
            jam_process_kill(proc);
        st = spawn_wait(proc, 10000000000ull, &info);
    }
    uint64_t ms = ((uint64_t)jam_clock_get() - t0) / 1000000;
    bool ok = false;
    if (st != OK)
        init_say("init: %s: lost track of it (%s)", argv[0], status_str(st));
    else if (info.killed)
        init_say("init: %s was killed after %lu ms", argv[0], (unsigned long)ms);
    else {
        init_say("init: %s exited with code %ld after %lu ms", argv[0], (long)info.exit_code,
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
        init_say("init: no root resource (SR_RESOURCE) in the startup message");
        return false;
    }
    status_t st = jam_resource_create(root, RES_PCI, 0, 0, &pci);
    if (st != OK) {
        init_say("init: can't slice RES_PCI from the root resource (%s)", status_str(st));
        return false;
    }
    jam_handle_close(pci);
    st = jam_resource_create(root, RES_MMIO, 0x100000000ull, 0, &bad);
    if (st != ERR_INVALID_ARGS) {
        init_say("init: a zero-sized MMIO slice gave %s, want ERR_INVALID_ARGS", status_str(st));
        if (st == OK)
            jam_handle_close(bad);
        return false;
    }
    return true;
}

/* devmgr: started before the programs, stopped after them. */
static bool start_devmgr(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, "bin/devmgr", &data, &size) != OK) {
        printf("init: no bin/devmgr in bootfs: no drivers\n");
        return true;
    }
    handle_t pci = HANDLE_INVALID, a = HANDLE_INVALID, b = HANDLE_INVALID;
    status_t st = jam_resource_create(startup_handle(SR_RESOURCE), RES_PCI, 0, 0, &pci);
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &devmgr_job);
    if (st == OK)
        st = jam_channel_create(&a, &b);
    if (st == OK) {
        const char *argv[] = { "bin/devmgr" };
        struct spawn_handle x[] = { { SR_RESOURCE, pci }, { SR_DEVMGR, b } };
        struct spawn_args sa = {
            .path = "bin/devmgr", .argc = 1, .argv = argv, .job = devmgr_job, .extra = x,
            .nextra = 2,
        };
        st = spawn(&sa, &devmgr_proc);   /* consumes pci and b */
        pci = b = HANDLE_INVALID;
    }
    if (st != OK) {
        init_say("init: can't start devmgr (%s)", status_str(st));
        if (pci)
            jam_handle_close(pci);
        if (a)
            jam_handle_close(a);
        if (b)
            jam_handle_close(b);
        return false;
    }
    devmgr_ch = a;
    /* Wait for its first binding pass. */
    struct devmgr_rep r;
    st = devmgr_call(devmgr_ch, DEVMGR_STATUS, 0, 0, 0, &r, NULL, 0, NULL,
                     (uint64_t)jam_clock_get() + 30 * S);
    if (st != OK) {
        init_say("init: devmgr doesn't answer (%s)", status_str(st));
        return false;
    }
    printf("init: devmgr: %u driver(s) bound, %u failed, %u skipped\n", r.a, r.b, r.c);
    return r.b == 0;
}

static bool stop_devmgr(void)
{
    if (!devmgr_proc)
        return true;
    jam_handle_close(devmgr_ch);   /* its last client: it stops its drivers and exits */
    devmgr_ch = HANDLE_INVALID;
    uint64_t t0 = (uint64_t)jam_clock_get();
    struct process_info info;
    status_t st = spawn_wait(devmgr_proc, 30 * S, &info);
    if (st == ERR_TIMED_OUT) {
        init_say("init: devmgr still running 30 s after its channel closed: killing its job");
        jam_job_kill(devmgr_job);
        st = spawn_wait(devmgr_proc, 10 * S, &info);
    }
    bool ok = st == OK && !info.killed && info.exit_code == 0;
    if (st == OK)
        init_say("init: devmgr %s %ld after %lu ms", info.killed ? "was killed, code" : "exited with code",
            (long)info.exit_code, (unsigned long)(((uint64_t)jam_clock_get() - t0) / 1000000));
    struct job_info ji;
    if (jam_job_get_info(devmgr_job, &ji) == OK)
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            if (ji.used[k]) {
                init_say("init: devmgr's job still has %lu units of kind %u",
                    (unsigned long)ji.used[k], k);
                ok = false;
            }
    jam_handle_close(devmgr_proc);
    jam_handle_close(devmgr_job);
    devmgr_proc = devmgr_job = HANDLE_INVALID;
    return ok;
}

/* init.cfg: one program per line, "<path in bootfs> [args...]"; blank lines
 * and lines starting with '#' are ignored. */
static bool run_config(const char *cfg, uint64_t len)
{
    char *text = malloc(len + 1);
    if (!text) {
        init_say("init: no memory for init.cfg");
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
            init_say("init: init.cfg:%d: more than %d words", lineno, MAX_WORDS);
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
    /* Modes the kernel asks for (argv[1]) instead of init.cfg. */
    if (argc > 1 && !strcmp(argv[1], "xhcitest"))
        return init_xhcitest() ? 0 : 1;

    const struct bootfs_view *fs;
    status_t st = bootfs_default(&fs);
    if (st != OK) {
        init_say("init: can't map bootfs (%s)", status_str(st));
        return 1;
    }
    const void *cfg;
    uint64_t len;
    st = bootfs_lookup(fs, "init.cfg", &cfg, &len);
    if (st != OK) {
        init_say("init: no init.cfg in bootfs (%s)", status_str(st));
        return 1;
    }
    bool ok = start_devmgr();
    ok &= run_config(cfg, len);
    ok &= stop_devmgr();
    return ok ? 0 : 1;
}
