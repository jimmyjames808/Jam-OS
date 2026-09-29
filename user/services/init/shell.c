/* init's shell mode: a plain boot ("Jam OS", or "shell" on
 * the command line) ends at a shell prompt on the screen.
 *
 * init starts and then supervises four services, each in a job of its own
 * under init's:
 *   console   bin/console: root with READ | WRITE | MANAGE (klog, the screen,
 *             serial output; reboot on Ctrl+Alt+Del), the server end of a
 *             console channel (SR_USER + 0);
 *             init keeps the client end
 *   serialin  bin/serialin: root with READ (serial_open) and an `input`
 *             channel from console.connect_input (SR_USER + 0)
 *   devmgr    bin/devmgr: RES_PCI sliced from the root (SR_RESOURCE), the
 *             server ends of its control and query channels (SR_DEVMGR_CTL,
 *             SR_DEVMGR; init keeps a client end of each) and a copy of
 *             init's (ADMIN) console client end (SR_CONSOLE), so its HID

 *             drivers type into the console. init waits
 *             for its first binding pass (up to 30 s). "nousb" (the safe
 *             mode boot entry) is passed on: no USB controller driver
 *   shell     bin/shell: a SHELL-level console channel (SR_CONSOLE:
 *             console.new_client; no connect_input), root with READ |
 *             MANAGE, RES_PCI with RIGHTS_BASIC (SR_USER + 1), devmgr's
 *             query and control client ends (SR_DEVMGR, SR_DEVMGR_CTL: it
 *             passes control only to its utest/usbtest commands) and a
 *             channel from init (SR_USER + 2) on which init sends it each
 *             new devmgr's pair (INIT_SHELL_DEVMGR, <devmgr.h>)
 * None of the console, serialin and the shell gets RIGHT_MAP or
 * RIGHT_SLICE on the root: they can't reach hardware beyond the calls made
 * for them.
 *
 * A service that ends is started again (in the order above, each waiting
 * for the console):
 *   - a new console gets a new channel, so serialin and the shell (whose
 *     channel then closes) exit and come back connected to it, and devmgr
 *     gets the new channel (DEVMGR_SET_CONSOLE): its HID drivers, which
 *     end when their console goes, come back connected to it.
 *   - devmgr dying (killed, or a crash) takes its whole job
 *     with it: every driver it started (usb-bus, each hid). A new devmgr
 *     binds them again from scratch (the kernel's safe rebind: a new
 *     dma_cap with Bus Master Enable off until usb-bus has reset the
 *     controller; the dead one's DMA pages stay quarantined until then),
 *     connected to the console. The shell gets the new devmgr channel.
 * Restarts back off from 100 ms to 5 s; one that ends more than 10 times
 * in a minute is given up on (a line in the log and the RESULTS box). init
 * itself never returns in this mode. */
#include <devmgr.h>
#include <idl/console.h>
#include <os.h>
#include "init.h"

#define GIVE_UP_COUNT  10
#define GIVE_UP_WINDOW (60 * NS_PER_S)

enum { CONSOLE, SERIALIN, DEVMGR, SHELL, NSVC };

struct svc {
    const char *path;          /* in bootfs */
    handle_t    proc, job;     /* while it runs */
    bool        running;       /* started, its end not seen yet */
    bool        given_up;      /* ended too often: not started again */
    uint64_t    next_try;      /* uptime ns */
    uint64_t    backoff;       /* the last delay before a restart, ns */
    uint64_t    started;       /* uptime ns */
    uint64_t    window_start;  /* the minute its ends are counted in (uptime ns) */
    unsigned    ends;          /* in the current window */
};

static struct svc svcs[NSVC] = {
    [CONSOLE] = { "bin/console" }, [SERIALIN] = { "bin/serialin" },
    [DEVMGR] = { "bin/devmgr" }, [SHELL] = { "bin/shell" },
};
static handle_t root, port;
static handle_t cons;       /* the console client end (0: none) */
static handle_t devmgr;     /* devmgr's control channel, client end (0: none running) */
static handle_t devmgr_q;   /* its query channel, client end */
static handle_t to_shell;   /* init's end of the shell's SR_USER + 2 channel */
static bool nousb;

static handle_t root_with(rights_t rights)
{
    handle_t h = HANDLE_INVALID;
    if (jam_handle_duplicate(root, rights, &h) != OK)
        return HANDLE_INVALID;
    return h;
}

/* Start svc i with these arguments and extra handles (consumed). */
static status_t start(unsigned i, int argc, const char *const *argv, struct spawn_handle *x,
                      unsigned nx)
{
    struct svc *s = &svcs[i];
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &s->job);
    if (st != OK) {
        for (unsigned k = 0; k < nx; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return st;
    }
    struct spawn_args a = {
        .path = s->path, .argc = argc, .argv = argv, .job = s->job, .extra = x, .nextra = nx,
    };
    st = spawn(&a, &s->proc);
    if (st == OK)
        st = jam_port_bind(port, s->proc, i, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st != OK) {
        if (s->proc) {
            jam_job_kill(s->job);
            jam_handle_close(s->proc);
            s->proc = HANDLE_INVALID;
        }
        jam_handle_close(s->job);
        s->job = HANDLE_INVALID;
        return st;
    }
    s->running = true;
    s->started = now();
    return OK;
}

static status_t start1(unsigned i, struct spawn_handle *x, unsigned nx)
{
    const char *argv[] = { svcs[i].path };
    return start(i, 1, argv, x, nx);
}

/* devmgr takes a new console (after the console restarted): its HID
 * drivers come back connected to it. */
static void tell_devmgr(void)
{
    handle_t c = HANDLE_INVALID;
    if (!devmgr || jam_handle_duplicate(cons, RIGHT_SAME, &c) != OK)
        return;
    struct devmgr_req q = { 0, DEVMGR_SET_CONSOLE, 0, 0, 0 };
    struct devmgr_rep r;
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = devmgr, .wn = sizeof(q), .wbytes = (uint64_t)(uintptr_t)&q,
        .wh = (uint64_t)(uintptr_t)&c, .whn = 1, .rcap = sizeof(r),
        .rbytes = (uint64_t)(uintptr_t)&r, .ractual = (uint64_t)(uintptr_t)&n,
        .rhactual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + 5 * NS_PER_S,
    };
    status_t st = jam_channel_call(&a);   /* c goes with the request either way */
    if (st != OK || n < DEVMGR_REP_HDR || r.status != OK)
        printf("init: devmgr didn't take the new console (%s)\n",
               status_str(st != OK ? st : r.status));
}

/* The shell gets each new devmgr client end on its init channel. */
static void tell_shell(void)
{
    handle_t d[2] = { HANDLE_INVALID, HANDLE_INVALID };
    if (!to_shell || !devmgr || jam_handle_duplicate(devmgr_q, RIGHT_SAME, &d[0]) != OK)
        return;
    if (jam_handle_duplicate(devmgr, RIGHT_SAME, &d[1]) != OK) {
        jam_handle_close(d[0]);
        return;
    }
    uint32_t kind = INIT_SHELL_DEVMGR;
    if (jam_channel_write(to_shell, &kind, sizeof(kind), d, 2) != OK) {
        jam_handle_close(d[0]);   /* the shell is gone: it gets them when it restarts */
        jam_handle_close(d[1]);
    }
}

static status_t start_console(void)
{
    handle_t a, b;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    struct spawn_handle x[] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE) },
        { SR_USER + 0, b },
    };
    st = start1(CONSOLE, x, 2);
    if (st != OK) {
        jam_handle_close(a);
        return st;
    }
    if (cons)
        jam_handle_close(cons);
    cons = a;
    tell_devmgr();   /* a restart: devmgr reconnects its HID drivers */
    return OK;
}

static status_t start_serialin(void)
{
    handle_t src;
    status_t st = console_connect_input_until(cons, now() + 5 * NS_PER_S, &src);
    if (st != OK)
        return st;
    struct spawn_handle x[] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ) }, { SR_USER + 0, src },
    };
    return start1(SERIALIN, x, 2);
}

static status_t start_devmgr(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, "bin/devmgr", &data, &size) != OK) {
        printf("init: no bin/devmgr in bootfs: no drivers\n");
        svcs[DEVMGR].given_up = true;
        return OK;
    }
    handle_t pci = HANDLE_INVALID, a = HANDLE_INVALID, b = HANDLE_INVALID, c = HANDLE_INVALID;
    handle_t qa = HANDLE_INVALID, qb = HANDLE_INVALID;
    status_t st = jam_resource_create(root, RES_PCI, 0, 0, &pci);
    if (st == OK)
        st = jam_channel_create(&a, &b);
    if (st == OK)
        st = jam_channel_create(&qa, &qb);
    if (st == OK)
        st = jam_handle_duplicate(cons, RIGHT_SAME, &c);
    if (st != OK) {
        handle_t left[] = { pci, a, b, qa, qb };
        for (unsigned k = 0; k < 5; k++)
            if (left[k])
                jam_handle_close(left[k]);
        return st;
    }
    const char *argv[] = { "bin/devmgr", "nousb" };
    struct spawn_handle x[] = { { SR_RESOURCE, pci }, { SR_DEVMGR_CTL, b }, { SR_DEVMGR, qb },
                                { SR_CONSOLE, c } };
    st = start(DEVMGR, nousb ? 2 : 1, argv, x, 4);   /* consumes pci, b, qb and c */
    if (st != OK) {
        jam_handle_close(a);
        jam_handle_close(qa);
        return st;
    }
    devmgr = a;
    devmgr_q = qa;
    /* Its first binding pass (usb-bus on the PC's controller). */
    struct devmgr_rep r;
    st = devmgr_call(devmgr, DEVMGR_STATUS, 0, 0, 0, &r, NULL, 0, NULL, now() + 30 * NS_PER_S);
    if (st != OK)
        init_say("init: devmgr doesn't answer (%s)", status_str(st));
    else
        printf("init: devmgr: %u driver(s) bound, %u failed, %u skipped%s\n", r.a, r.b, r.c,
               nousb ? " (nousb: no USB drivers)" : "");
    tell_shell();
    return OK;
}

static status_t start_shell(void)
{
    handle_t c = HANDLE_INVALID, d = HANDLE_INVALID, dc = HANDLE_INVALID, pci = HANDLE_INVALID;
    handle_t p2 = HANDLE_INVALID, mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    /* A SHELL-level console channel: no input sources of its own. */
    status_t st = console_new_client_until(cons, now() + 5 * NS_PER_S, 1, &c);
    if (st != OK)
        return st;
    if (devmgr) {
        jam_handle_duplicate(devmgr_q, RIGHT_SAME, &d);
        jam_handle_duplicate(devmgr, RIGHT_SAME, &dc);
    }
    if (jam_resource_create(root, RES_PCI, 0, 0, &pci) == OK &&
        jam_handle_replace(pci, RIGHTS_BASIC, &p2) != OK)
        p2 = HANDLE_INVALID;
    if (jam_channel_create(&mine, &theirs) != OK)
        mine = theirs = HANDLE_INVALID;
    struct spawn_handle x[] = {
        { SR_CONSOLE, c },
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ | RIGHT_MANAGE) },
        { SR_USER + 1, p2 },
        { SR_DEVMGR, d },
        { SR_DEVMGR_CTL, dc },
        { SR_USER + 2, theirs },
    };
    /* Leave out the ones we don't have. */
    struct spawn_handle y[6];
    unsigned n = 0;
    for (unsigned k = 0; k < 6; k++)
        if (x[k].h)
            y[n++] = x[k];
    st = start1(SHELL, y, n);
    if (st != OK) {
        if (mine)
            jam_handle_close(mine);
        return st;
    }
    if (to_shell)
        jam_handle_close(to_shell);
    to_shell = mine;
    return OK;
}

/* Svc i ended: say how, clean up, schedule the restart. */
static void ended(unsigned i)
{
    struct svc *s = &svcs[i];
    struct process_info info;
    if (jam_process_get_info(s->proc, &info) == OK)
        printf("init: %s %s %ld\n", s->path, info.killed ? "was killed, code" : "exited with code",
               (long)info.exit_code);
    jam_job_kill(s->job);   /* anything it started (devmgr: every driver) */
    jam_handle_close(s->proc);
    jam_handle_close(s->job);
    s->proc = s->job = HANDLE_INVALID;
    s->running = false;
    uint64_t t = now();
    if (t - s->window_start > GIVE_UP_WINDOW) {
        s->window_start = t;
        s->ends = 0;
    }
    if (i == CONSOLE && cons) {
        jam_handle_close(cons);   /* the shell and serialin see PEER_CLOSED */
        cons = HANDLE_INVALID;
    }
    if (i == DEVMGR && devmgr) {
        jam_handle_close(devmgr);   /* the shell's copies see PEER_CLOSED */
        jam_handle_close(devmgr_q);
        devmgr = devmgr_q = HANDLE_INVALID;

        printf("init: devmgr and its drivers are gone: starting them again\n");
    }
    if (i == SHELL && to_shell) {
        jam_handle_close(to_shell);
        to_shell = HANDLE_INVALID;
    }
    if (++s->ends > GIVE_UP_COUNT) {
        s->given_up = true;
        init_say("init: %s ended %u times in a minute: not restarting it", s->path, s->ends);
        return;
    }
    /* Ran for a while: start again soon; else back off. */
    s->backoff = t - s->started > 10 * NS_PER_S || !s->backoff ? 100 * NS_PER_MS : s->backoff * 2;
    if (s->backoff > 5 * NS_PER_S)
        s->backoff = 5 * NS_PER_S;
    s->next_try = t + s->backoff;
}

bool init_shell(bool no_usb)
{
    root = startup_handle(SR_RESOURCE);
    nousb = no_usb;
    status_t st = jam_port_create(&port);
    if (st != OK) {
        init_say("init: shell mode: no port (%s)", status_str(st));
        return false;
    }
    printf("init: shell mode%s: starting the console, the serial input, devmgr and the shell\n",
           nousb ? " (safe mode: nousb)" : "");
    for (;;) {
        uint64_t t = now(), deadline = DEADLINE_NEVER;
        for (unsigned i = 0; i < NSVC; i++) {
            struct svc *s = &svcs[i];
            if (s->running || s->given_up)
                continue;
            if (i != CONSOLE && !cons)
                continue;   /* waits for the console */
            if (t < s->next_try) {
                deadline = s->next_try < deadline ? s->next_try : deadline;
                continue;
            }
            st = i == CONSOLE    ? start_console()
                 : i == SERIALIN ? start_serialin()
                 : i == DEVMGR   ? start_devmgr()
                                 : start_shell();
            if (st != OK) {
                printf("init: can't start %s (%s)\n", s->path, status_str(st));
                s->backoff = s->backoff ? s->backoff * 2 : 100 * NS_PER_MS;
                if (s->backoff > 5 * NS_PER_S)
                    s->backoff = 5 * NS_PER_S;
                s->next_try = t + s->backoff;
                deadline = s->next_try < deadline ? s->next_try : deadline;
            }
        }
        struct port_packet pkt;
        st = jam_port_wait(port, deadline, &pkt);
        if (st == OK && pkt.key < NSVC && svcs[pkt.key].running)
            ended((unsigned)pkt.key);
        else if (st != OK && st != ERR_TIMED_OUT)
            return false;
    }
}
