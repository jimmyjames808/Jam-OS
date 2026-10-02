/* init: the first user process, started by the kernel's userboot with the
 * root job, the root resource and the bootfs image.
 *
 * With "shell" (a plain boot), "shell-nousb" (the safe mode entry:
 * devmgr leaves USB controllers alone) or "soak=<minutes>" (a plain boot
 * whose shell starts the soak test) it starts and supervises the bootfs
 * server, the console, serial input, devmgr and the shell (shell.c) and
 * never exits; an option word "splash" after it plays the boot splash
 * first (splash.c). The option word "hidboot" (with any mode) is passed
 * on to devmgr, which passes it to every hid: mice stay in the boot
 * protocol; so is "bootdisk=0x<id>" (the disk the machine booted from:
 * devmgr's boot disk). Otherwise it starts the bootfs server (bin/bootfs: the boot
 * image as the mount /boot) and devmgr (bin/devmgr, if bootfs has it) in a
 * job of its own with a RES_PCI resource sliced from the root, waits until
 * devmgr has bound its drivers, and runs the programs listed in init.cfg
 * one after another, each as a real child process in a job of its own (a
 * child of init's job) with the root resource to read with (SR_RESOURCE:
 * TEST_ROOT's powers) and init's namespace as it is then (SR_NS: /boot, what
 * devmgr has mounted (mounts.c), and devmgr's query and control channels
 * as /svc/devmgr and /svc/devmgr-ctl); it waits for each to finish and reports how it
 * ended: the lines go into the kernel's RESULTS box. At the end it closes
 * its end of devmgr's channel, which stops devmgr and its drivers, waits
 * for that, and stops the bootfs server the same way. init exits 0 if
 * every program (and devmgr, and the bootfs server) exited 0. */
#include <devmgr.h>
#include <os.h>
#include "init.h"

#define MAX_WORDS     16
/* The root's powers an init.cfg program gets (<jam/abi.h> RIGHT_ROOT_*). */
#define TEST_ROOT (RIGHT_ROOT_KLOG | RIGHT_ROOT_SYSINFO | RIGHT_ROOT_CLOCK | RIGHT_ROOT_VMEX)
#define RUN_TIMEOUT_S 240   /* per program */

/* devmgr_ch: its control channel, devmgr_q: its query channel (<devmgr.h>
 * "Trust"); the programs init runs are the test suites: they get both.
 * devmgr_hda: the sound cards' device channels, made as in shell mode and
 * kept (no mixer runs here), so the query channel never hands hda out. */
static handle_t devmgr_ch, devmgr_q, devmgr_proc, devmgr_job;   /* 0: no devmgr */
static handle_t devmgr_hda[INIT_MAX_CLAIMED];
static handle_t bootfs_proc, bootfs_job;                        /* 0: no bootfs server */

bool init_hidboot;
const char *init_bootdisk;
bool init_splashhang;

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
    /* The root resource to read with (the kernel log, the system's
     * figures, the clock) and to make a VMO executable (utest's tests of
     * that); devmgr's channels are in our namespace (/svc), which it gets
     * whole. */
    struct spawn_handle x[1] = { { SR_RESOURCE, HANDLE_INVALID } };
    st = jam_handle_duplicate(startup_handle(SR_RESOURCE), RIGHTS_BASIC | TEST_ROOT, &x[0].h);
    if (st != OK) {
        init_say("init: %s: no handles for it (%s)", argv[0], status_str(st));
        jam_handle_close(job);
        return false;
    }
    struct spawn_args a = {
        .path = argv[0], .argc = argc, .argv = (const char *const *)argv, .job = job,
        .extra = x, .nextra = 1, .ns = NS_ALL,
    };
    uint64_t t0 = now();
    st = spawn(&a, &proc);
    if (st != OK) {
        init_say("init: %s: could not start (%s)", argv[0], status_str(st));
        jam_handle_close(job);
        return false;
    }
    struct process_info info;
    st = spawn_wait(proc, RUN_TIMEOUT_S * NS_PER_S, &info);
    if (st == ERR_TIMED_OUT) {
        /* Its whole job: whatever it started (even orphans) goes too, and
         * job_kill returns once all of it is dead. */
        init_say("init: %s: still running after %d s, killing its job", argv[0], RUN_TIMEOUT_S);
        if (jam_job_kill(job) != OK)
            jam_process_kill(proc);
        st = spawn_wait(proc, 10 * NS_PER_S, &info);
    }
    uint64_t ms = (now() - t0) / NS_PER_MS;
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

/* userboot gives init the root resource (SR_RESOURCE). Check it is one:
 * slicing RES_PCI out of it works (devmgr gets such a slice), a
 * zero-sized MMIO slice doesn't. */
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

/* devmgr: started before the programs, stopped after them. With a
 * console client end (consumed) for its class drivers' input. */
static bool start_devmgr(handle_t console)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, "bin/devmgr", &data, &size) != OK) {
        printf("init: no bin/devmgr in bootfs: no drivers\n");
        if (console)
            jam_handle_close(console);
        return true;
    }
    handle_t pci = HANDLE_INVALID, a = HANDLE_INVALID, b = HANDLE_INVALID;
    handle_t qa = HANDLE_INVALID, qb = HANDLE_INVALID;
    status_t st = jam_resource_create(startup_handle(SR_RESOURCE), RES_PCI, 0, 0, &pci);
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &devmgr_job);
    if (st == OK)
        st = jam_channel_create(&a, &b);
    if (st == OK)
        st = jam_channel_create(&qa, &qb);
    if (st == OK) {
        const char *argv[3] = { "bin/devmgr" };
        int argc = 1;
        if (init_hidboot)
            argv[argc++] = "hidboot";
        if (init_bootdisk)
            argv[argc++] = init_bootdisk;
        struct spawn_handle x[] = { { SR_RESOURCE, pci }, { SR_DEVMGR_CTL, b },
                                    { SR_DEVMGR, qb }, { SR_CONSOLE, console } };
        struct spawn_args sa = {
            .path = "bin/devmgr", .argc = argc, .argv = argv, .job = devmgr_job,
            .extra = x,
            .nextra = console ? 4 : 3,
        };
        st = spawn(&sa, &devmgr_proc);   /* consumes pci, b, qb and console */
        pci = b = qb = console = HANDLE_INVALID;
    }
    if (console)
        jam_handle_close(console);
    if (st != OK) {
        init_say("init: can't start devmgr (%s)", status_str(st));
        handle_t left[] = { pci, a, b, qa, qb };
        for (unsigned k = 0; k < 5; k++)
            if (left[k])
                jam_handle_close(left[k]);
        return false;
    }
    devmgr_ch = a;
    devmgr_q = qa;
    /* The programs we run reach it through our namespace. */
    handle_t dq = HANDLE_INVALID, dc = HANDLE_INVALID;
    if (jam_handle_duplicate(devmgr_q, RIGHT_SAME, &dq) == OK)
        (void)ns_svc_set(SVC_DEVMGR, dq, true);   /* per opener; without it the tests skip */
    if (jam_handle_duplicate(devmgr_ch, RIGHT_SAME, &dc) == OK)
        (void)ns_svc_set(SVC_DEVMGR_CTL, dc, false);
    /* Wait for its first binding pass. */
    struct devmgr_rep r;
    st = devmgr_call(devmgr_ch, DEVMGR_STATUS, 0, 0, 0, &r, NULL, 0, NULL,
                     now() + 30 * NS_PER_S);
    if (st != OK) {
        init_say("init: devmgr doesn't answer (%s)", status_str(st));
        return false;
    }
    printf("init: devmgr: %u driver(s) bound, %u failed, %u skipped\n", r.a, r.b, r.c);
    (void)services_claim_class(devmgr_ch, DEVMGR_CLASS_HDA, devmgr_hda, INIT_MAX_CLAIMED);
    /* Its mounts, as they come: a program gets those there when it starts. */
    handle_t watch;
    st = jam_handle_duplicate(devmgr_ch, RIGHT_SAME, &watch);
    if (st == OK)
        st = mounts_watch(watch, HANDLE_INVALID, 0);
    if (st != OK)
        printf("init: not following devmgr's mounts (%s)\n", status_str(st));
    return r.b == 0;
}

static bool stop_devmgr(void)
{
    if (!devmgr_proc)
        return true;
    (void)ns_svc_remove(SVC_DEVMGR);       /* our copies too: none may keep it */
    (void)ns_svc_remove(SVC_DEVMGR_CTL);
    jam_handle_close(devmgr_q);
    jam_handle_close(devmgr_ch);
    for (unsigned k = 0; k < INIT_MAX_CLAIMED; k++)
        if (devmgr_hda[k])
            jam_handle_close(devmgr_hda[k]);
    memset(devmgr_hda, 0, sizeof(devmgr_hda));
    devmgr_ch = devmgr_q = HANDLE_INVALID;
    mounts_unwatch();   /* its last control client gone: it stops its drivers, exits */

    uint64_t t0 = now();
    struct process_info info;
    status_t st = spawn_wait(devmgr_proc, 30 * NS_PER_S, &info);
    if (st == ERR_TIMED_OUT) {
        init_say("init: devmgr still running 30 s after its channel closed: killing its job");
        jam_job_kill(devmgr_job);
        st = spawn_wait(devmgr_proc, 10 * NS_PER_S, &info);
    }
    bool ok = st == OK && !info.killed && info.exit_code == 0;
    if (st == OK)
        init_say("init: devmgr %s %ld after %lu ms",
                 info.killed ? "was killed, code" : "exited with code", (long)info.exit_code,
                 (unsigned long)((now() - t0) / NS_PER_MS));
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

/* The bootfs server: /boot in our namespace, which every program we run is
 * given. */
static bool start_bootfs(void)
{
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &bootfs_job);
    if (st == OK)
        st = jam_channel_create(&mine, &theirs);
    if (st == OK) {
        const char *argv[] = { BOOTFS_PATH };
        struct spawn_handle x = { SR_USER + 0, theirs };
        struct spawn_args a = {
            .path = BOOTFS_PATH, .argc = 1, .argv = argv, .job = bootfs_job, .extra = &x,
            .nextra = 1,
        };
        st = spawn(&a, &bootfs_proc);   /* consumes theirs */
    }
    if (st == OK)
        st = ns_mount(BOOT_MOUNT, mine);   /* consumes mine */
    else if (mine)
        jam_handle_close(mine);
    if (st != OK) {
        init_say("init: no " BOOT_MOUNT ": the bootfs server didn't start (%s)", status_str(st));
        if (bootfs_proc)
            jam_handle_close(bootfs_proc);   /* without a client it exits by itself */
        if (bootfs_job)
            jam_handle_close(bootfs_job);
        bootfs_proc = bootfs_job = HANDLE_INVALID;
    }
    return st == OK;
}

static bool stop_bootfs(void)
{
    if (!bootfs_proc)
        return true;
    (void)ns_unmount(BOOT_MOUNT);   /* its last client: it exits by itself */
    struct process_info info;
    status_t st = spawn_wait(bootfs_proc, 5 * NS_PER_S, &info);
    if (st == ERR_TIMED_OUT) {
        init_say("init: the bootfs server still runs 5 s after its last client left: killing it");
        jam_job_kill(bootfs_job);
        st = spawn_wait(bootfs_proc, 5 * NS_PER_S, &info);
    }
    bool ok = st == OK && !info.killed && info.exit_code == 0;
    if (st == OK && !ok)
        init_say("init: the bootfs server %s %ld", info.killed ? "was killed, code"
                                                                : "exited with code",
                 (long)info.exit_code);
    jam_handle_close(bootfs_proc);
    jam_handle_close(bootfs_job);
    bootfs_proc = bootfs_job = HANDLE_INVALID;
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

/* The hidden `keytest` boot word: devmgr (usb-bus, a hid per HID
 * interface, no console: each hid logs every key DOWN), KEYTEST_S seconds
 * to type on the PC, then everything stops; each hid puts its count of
 * keys into the RESULTS box. */
#define KEYTEST_S 30
static bool run_keytest(void)
{
    bool ok = start_devmgr(HANDLE_INVALID);
    init_say("keytest: type on the USB keyboard now: %d s; each key DOWN is logged by its hid "
             "(hid-<port>:<interface>)", KEYTEST_S);
    for (int left = KEYTEST_S; left > 0; left -= 10) {
        jam_nanosleep(now() + (uint64_t)(left < 10 ? left : 10) * NS_PER_S);
        if (left > 10)
            printf("init: keytest: %d s left\n", left - 10);
    }
    printf("init: keytest: time is up; stopping the drivers\n");
    ok &= stop_devmgr();
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
    /* The option words after the mode (argv[2] on): "splash" (the kernel's
     * choice: a plain boot without `verbose` or `nosplash`: the boot splash
     * plays first), "hidboot", "bootdisk=0x<id>", "splashhang". */
    bool splash = false;
    for (int i = 2; i < argc; i++) {
        splash |= !strcmp(argv[i], "splash");
        init_hidboot |= !strcmp(argv[i], "hidboot");
        if (!strncmp(argv[i], "bootdisk=", 9))
            init_bootdisk = argv[i];
        init_splashhang |= !strcmp(argv[i], "splashhang");
    }
    /* The modes the kernel asks for (argv[1]) instead of init.cfg. A plain
     * boot: the console, devmgr (connected to it), serial input and the
     * shell; the safe mode entry: the same without USB. */
    if (argc > 1 && (!strcmp(argv[1], "shell") || !strcmp(argv[1], "shell-nousb"))) {
        init_shell(!strcmp(argv[1], "shell-nousb"), splash, NULL);
        return 1;
    }
    /* The boot word soak[=minutes]: a plain boot whose first shell runs the
     * soak test by itself (the shell's main.c reads the argument). */
    if (argc > 1 && !strncmp(argv[1], "soak=", 5)) {
        init_shell(false, splash, argv[1]);
        return 1;
    }

    if (argc > 1 && !strcmp(argv[1], "keytest"))
        return run_keytest() ? 0 : 1;

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
    bool ok = start_bootfs();
    ok &= start_devmgr(HANDLE_INVALID);
    ok &= run_config(cfg, len);
    ok &= stop_devmgr();
    ok &= stop_bootfs();
    return ok ? 0 : 1;
}
