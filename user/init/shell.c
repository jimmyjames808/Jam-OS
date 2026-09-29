/* init's shell mode (M7 Track C): a plain boot ("Jam OS", or "shell" on
 * the command line) ends at a shell prompt on the screen.
 *
 * After devmgr (main.c), init starts and then supervises three services,
 * each in a job of its own under init's:
 *   console   bin/console: root with READ | WRITE | MANAGE (klog, the screen,
 *             serial output; reboot on Ctrl+Alt+Del), the server end of a
 *             console channel (SR_USER + 0);
 *             init keeps the client end
 *   serialin  bin/serialin: root with READ (serial_open) and an `input`
 *             channel from console.connect_input (SR_USER + 0)
 *   shell     bin/shell: a copy of the console client end (SR_CONSOLE),
 *             root with READ | MANAGE, RES_PCI with RIGHTS_BASIC
 *             (SR_USER + 1), a devmgr client end (SR_DEVMGR)
 * None of them gets RIGHT_MAP or RIGHT_SLICE on the root: they can't reach
 * hardware beyond the calls made for them.
 *
 * A service that ends is started again, console first: a new console gets
 * a new channel, so serialin and the shell (whose channel then closes)
 * exit and come back connected to it. Restarts back off from 100 ms to
 * 5 s; one that ends more than 10 times in a minute is given up on (a line
 * in the log and the RESULTS box). init itself never returns in this mode. */
#include <os.h>
#include <devmgr.h>
#include <idl/console.h>

#define MS 1000000ull
#define S  1000000000ull
#define GIVE_UP_COUNT  10
#define GIVE_UP_WINDOW (60 * S)

void init_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

enum { CONSOLE, SERIALIN, SHELL, NSVC };

struct svc {
    const char *path;
    handle_t    proc, job;
    bool        running, given_up;
    uint64_t    next_try;      /* uptime ns */
    uint64_t    backoff;
    uint64_t    started;       /* uptime ns */
    uint64_t    window_start;
    unsigned    ends;          /* in the current window */
};

static struct svc svcs[NSVC] = {
    [CONSOLE] = { "bin/console" }, [SERIALIN] = { "bin/serialin" }, [SHELL] = { "bin/shell" },
};
static handle_t root, devmgr, cons;   /* cons: the console client end (0: none) */
static handle_t port;

static uint64_t now(void)
{
    return (uint64_t)jam_clock_get();
}

static handle_t root_with(rights_t rights)
{
    handle_t h = HANDLE_INVALID;
    if (jam_handle_duplicate(root, rights, &h) != OK)
        return HANDLE_INVALID;
    return h;
}

/* Start svc i with these extra handles (consumed). */
static status_t start(unsigned i, struct spawn_handle *x, unsigned nx)
{
    struct svc *s = &svcs[i];
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &s->job);
    if (st != OK) {
        for (unsigned k = 0; k < nx; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return st;
    }
    const char *argv[] = { s->path };
    struct spawn_args a = {
        .path = s->path, .argc = 1, .argv = argv, .job = s->job, .extra = x, .nextra = nx,
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
    st = start(CONSOLE, x, 2);
    if (st != OK) {
        jam_handle_close(a);
        return st;
    }
    if (cons)
        jam_handle_close(cons);
    cons = a;
    return OK;
}

static status_t start_serialin(void)
{
    handle_t src;
    status_t st = console_connect_input_until(cons, now() + 5 * S, &src);
    if (st != OK)
        return st;
    struct spawn_handle x[] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ) }, { SR_USER + 0, src },
    };
    return start(SERIALIN, x, 2);
}

static status_t start_shell(void)
{
    handle_t c = HANDLE_INVALID, d = HANDLE_INVALID, pci = HANDLE_INVALID, p2 = HANDLE_INVALID;
    status_t st = jam_handle_duplicate(cons, RIGHT_SAME, &c);
    if (st != OK)
        return st;
    if (devmgr)
        jam_handle_duplicate(devmgr, RIGHT_SAME, &d);
    if (jam_resource_create(root, RES_PCI, 0, 0, &pci) == OK &&
        jam_handle_replace(pci, RIGHTS_BASIC, &p2) != OK)
        p2 = HANDLE_INVALID;
    struct spawn_handle x[] = {
        { SR_CONSOLE, c },
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ | RIGHT_MANAGE) },
        { SR_USER + 1, p2 },
        { SR_DEVMGR, d },
    };
    /* Leave out the ones we don't have. */
    struct spawn_handle y[4];
    unsigned n = 0;
    for (unsigned k = 0; k < 4; k++)
        if (x[k].h)
            y[n++] = x[k];
    return start(SHELL, y, n);
}

/* Svc i ended: say how, clean up, schedule the restart. */
static void ended(unsigned i)
{
    struct svc *s = &svcs[i];
    struct process_info info;
    if (jam_process_get_info(s->proc, &info) == OK)
        printf("init: %s %s %ld\n", s->path, info.killed ? "was killed, code" : "exited with code",
               (long)info.exit_code);
    jam_job_kill(s->job);   /* anything it started */
    jam_handle_close(s->proc);
    jam_handle_close(s->job);
    s->proc = s->job = HANDLE_INVALID;
    s->running = false;
    uint64_t t = now();
    if (t - s->window_start > GIVE_UP_WINDOW) {
        s->window_start = t;
        s->ends = 0;
    }
    if (++s->ends > GIVE_UP_COUNT) {
        s->given_up = true;
        init_say("init: %s ended %u times in a minute: not restarting it", s->path, s->ends);
        return;
    }
    /* Ran for a while: start again soon; else back off. */
    s->backoff = t - s->started > 10 * S || !s->backoff ? 100 * MS : s->backoff * 2;
    if (s->backoff > 5 * S)
        s->backoff = 5 * S;
    s->next_try = t + s->backoff;
    if (i == CONSOLE && cons) {
        jam_handle_close(cons);   /* the shell and serialin see PEER_CLOSED */
        cons = HANDLE_INVALID;
    }
}

bool init_shell(handle_t devmgr_ch)
{
    root = startup_handle(SR_RESOURCE);
    devmgr = devmgr_ch;
    status_t st = jam_port_create(&port);
    if (st != OK) {
        init_say("init: shell mode: no port (%s)", status_str(st));
        return false;
    }
    printf("init: shell mode: starting the console, the serial input and the shell\n");
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
            st = i == CONSOLE ? start_console() : i == SERIALIN ? start_serialin() : start_shell();
            if (st != OK) {
                printf("init: can't start %s (%s)\n", s->path, status_str(st));
                s->backoff = s->backoff ? s->backoff * 2 : 100 * MS;
                if (s->backoff > 5 * S)
                    s->backoff = 5 * S;
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
