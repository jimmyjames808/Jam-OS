/* init's shell mode: a plain boot ("Jam OS", or "shell" on
 * the command line) ends at a shell prompt on the screen.
 *
 * init starts and then supervises ten services, each in a job of its own
 * under init's: the bootfs server, the console, the boot splash, serialin,
 * devmgr, the mixer, the music player, netstack, logd and the shell. How each one is
 * started and what it is given is in services.c; this file is the order,
 * the namespace they follow and the restarts.
 *
 * init's own namespace holds its mounts (/boot, and what devmgr mounts:
 * mounts.c) and the services it publishes under /svc (services.c). The
 * services that have a namespace get the part of it their grants name
 * (followers[], <os.h> "grants"): the shell all of it as it is, the music
 * player every mount read-only and the mixer, logd /data (its top-level
 * etc left alone), the splash the mixer. init keeps its end of each one's
 * SR_NS channel and sends it every later change, with ns_update: each
 * change takes back the one it hasn't read yet (logd never looks up a
 * path again after it opens its file), so however often the namespace
 * changes, each holds at most one message from init.
 *
 * A service that ends is started again (in the order above; the console's
 * clients wait for the console, logd for /data):
 *   - a new bootfs server is mounted at /boot again, and the shell is sent
 *     the new mount (until then /boot answers ERR_PEER_CLOSED).
 *   - a new console, a new devmgr: services.c (services_closed) says what
 *     their ends change for the others.
 * Restarts back off from 100 ms to 5 s; one that ends more than 10 times
 * in a minute (from its first end) is given up on (a line in the log and
 * the RESULTS box), but for the console and the shell: without them nobody
 * can use the machine until a reset, so they are started again for good,
 * every 5 s at worst (said once a minute). The console's clients (serialin,
 * the shell) end when it does, often before init has seen the console's
 * own end: an end of theirs while the console is gone doesn't count, and
 * they start again at once with the new console. init itself never
 * returns in this mode. */
#include <os.h>
#include "init.h"

#define GIVE_UP_COUNT  10
#define GIVE_UP_WINDOW (60 * NS_PER_S)

struct svc svcs[NSVC] = {
    [BOOTFS] = { BOOTFS_PATH }, [CONSOLE] = { "bin/console" }, [SPLASH] = { "bin/splash" },
    [SERIALIN] = { "bin/serialin" },
    [DEVMGR] = { "bin/devmgr" }, [MIXER] = { "bin/mixer" }, [MUSIC] = { "bin/music" },
    [NETSTACK] = { "bin/netstack" },
    [LOGD] = { "bin/logd" }, [SHELL] = { "bin/shell" },
};

/* A service that has a namespace, kept in step with init's. */
struct follower {
    const char *const *only;    /* its grants (<os.h> "grants"); NULL: no namespace */
    handle_t           ns;      /* init's end of its SR_NS channel (0: not running) */
    handle_t           back;    /* a duplicate of its end, for ns_update (0: none) */
};

static const char *const splash_grants[] = { "/svc/" SVC_AUDIO, NULL };
static const char *const music_grants[] = { "*:r", "/svc/" SVC_AUDIO, NULL };
static const char *const logd_grants[] = { DATA_MOUNT ":w", NULL };
static struct follower followers[NSVC] = {
    [SPLASH] = { .only = splash_grants }, [MUSIC] = { .only = music_grants },
    [LOGD] = { .only = logd_grants }, [SHELL] = { .only = NS_ALL },
};
static handle_t port;

/* init has a mount at path now. */
static bool mounted(const char *path)
{
    char point[NS_NAME_MAX];
    for (unsigned i = 0; ns_mount_at(i, point); i++)
        if (!strcmp(point, path))
            return true;
    return false;
}

/* Start svc i with these arguments and extra handles (consumed). A
 * follower gets its part of init's namespace. */
status_t svc_start(unsigned i, int argc, const char *const *argv, struct spawn_handle *x,
                   unsigned nx)
{
    struct svc *s = &svcs[i];
    struct follower *f = &followers[i];
    handle_t ns = HANDLE_INVALID, back = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &s->job);
    if (st != OK) {
        for (unsigned k = 0; k < nx; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return st;
    }
    struct spawn_args a = {
        .path = s->path, .argc = argc, .argv = argv, .job = s->job, .extra = x, .nextra = nx,
        .ns = f->only, .ns_out = f->only ? &ns : NULL, .ns_back_out = f->only ? &back : NULL,
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
        if (ns)
            jam_handle_close(ns);
        if (back)
            jam_handle_close(back);
        return st;
    }
    s->running = true;
    s->started = now();
    writers_started(i, s->proc);
    f->ns = ns;
    f->back = back;
    return OK;
}

status_t svc_start1(unsigned i, struct spawn_handle *x, unsigned nx)
{
    const char *argv[] = { svcs[i].path };
    return svc_start(i, 1, argv, x, nx);
}

/* Svc i's namespace follows init's: the whole of it that its grants
 * name (a mount whose service restarted has a new channel, a view a new
 * view), replacing any earlier one it hasn't read. */
static void tell_follower(unsigned i)
{
    struct follower *f = &followers[i];
    if (!f->ns)
        return;
    status_t st = ns_update(f->ns, f->back, f->only);
    /* It is gone, or out of memory: the next change sends the whole
     * namespace again, so nothing is lost for good. */
    if (st != OK && st != ERR_PEER_CLOSED)
        printf("init: %s didn't get the new mounts (%s)\n", svcs[i].path, status_str(st));
}

void tell_mounts(void)
{
    /* The shell first: it is what someone types at, and it takes no views
     * (each of the others' views is a call to that mount's service). */
    tell_follower(SHELL);
    for (unsigned i = 0; i < NSVC; i++)
        if (i != SHELL)
            tell_follower(i);
}

status_t shell_kill_service(const char *name, uint64_t *koid)
{
    for (unsigned i = 0; i < NSVC; i++) {
        struct svc *s = &svcs[i];
        const char *last = s->path;
        for (const char *p = s->path; *p; p++)
            if (*p == '/')
                last = p + 1;
        if (strcmp(last, name) || !s->running)
            continue;
        struct process_info info;
        status_t st = jam_process_get_info(s->proc, &info);
        if (st == OK)
            st = jam_process_kill(s->proc);   /* the loop sees it end: its job, the restart */
        if (st == OK)
            *koid = info.koid;
        return st;
    }
    return ERR_NOT_FOUND;
}

/* The services init never gives up on, however often they end. */
static bool never_given_up(unsigned i)
{
    return i == CONSOLE || i == SHELL;
}

/* Svc i is one of the console's clients and the console has ended (its
 * end may still be on its way to us): i went with it, no fault of its own. */
static bool went_with_console(unsigned i)
{
    if (i != SERIALIN && i != SHELL)
        return false;
    signals_t seen;
    return !svcs[CONSOLE].running ||
           jam_object_wait_one(svcs[CONSOLE].proc, SIG_TERMINATED, 0, &seen) == OK;
}

/* Count svc i's end at t in its minute: true if it may start again. */
static bool count_end(unsigned i, uint64_t t)
{
    struct svc *s = &svcs[i];
    if (!s->ends || t - s->window_start > GIVE_UP_WINDOW) {
        s->window_start = t;
        s->ends = 0;
    }
    if (++s->ends > GIVE_UP_COUNT && !never_given_up(i)) {
        s->given_up = true;
        services_given_up(i);
        init_say("init: %s ended %u times in a minute: not restarting it", s->path, s->ends);
        return false;
    }
    if (s->ends == GIVE_UP_COUNT + 1)
        init_say("init: %s ended %u times in a minute: restarting it anyway (never given up)",
                 s->path, s->ends);
    return true;
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
    services_closed(i);
    if (followers[i].ns) {
        jam_handle_close(followers[i].ns);
        followers[i].ns = HANDLE_INVALID;
    }
    if (followers[i].back) {
        jam_handle_close(followers[i].back);   /* its end, and what waits on it, go now */
        followers[i].back = HANDLE_INVALID;
    }
    if (i == SPLASH) {   /* it plays once: the shell may start now */
        splash_ended();
        s->given_up = true;
        return;
    }
    bool took = went_with_console(i);
    if (!took && !count_end(i, t))
        return;
    /* Ran for a while (or went with its console): start again soon; else
     * back off. */
    s->backoff = took || t - s->started > 10 * NS_PER_S || !s->backoff ? 100 * NS_PER_MS
                                                                       : s->backoff * 2;
    if (s->backoff > 5 * NS_PER_S)
        s->backoff = 5 * NS_PER_S;
    s->next_try = t + s->backoff;
}

/* /data has come (at boot, or back): the settings again, before logd
 * starts, so its file is dated by the settings' clock. */
static void data_came(void)
{
    static bool had;
    bool has = mounted(DATA_MOUNT);
    if (has && !had) {
        settings_clock();
        settings_first_file();
        services_settings(MIXER);
        services_settings(MUSIC);
        services_settings(NETSTACK);
    }
    had = has;
}

/* Start every service that is due, in order (all but the console wait
 * for it); one that fails backs off. The next time one is due, or
 * DEADLINE_NEVER. */
static uint64_t start_due(uint64_t t)
{
    uint64_t deadline = DEADLINE_NEVER;
    for (unsigned i = 0; i < NSVC; i++) {
        struct svc *s = &svcs[i];
        if (s->running || s->given_up)
            continue;
        if (i != CONSOLE && i != BOOTFS && i != LOGD && !services_console_up())
            continue;   /* waits for the console */
        if (i == LOGD && !mounted(DATA_MOUNT))
            continue;   /* waits for /data: a mount's packet wakes the loop */
        if ((i == MIXER || i == NETSTACK) && !services_devmgr_up() && !svcs[DEVMGR].given_up)
            continue;   /* waits for devmgr (started just before it) */
        if (i == SHELL && !splash_played() && t < splash_deadline()) {
            /* waits for the splash (its packet wakes the loop), at most until then */
            deadline = splash_deadline() < deadline ? splash_deadline() : deadline;
            continue;
        }
        if (i == SHELL && !splash_played()) {
            splash_overdue();
            if (svcs[SPLASH].running)
                jam_job_kill(svcs[SPLASH].job);   /* its lease ends: the console draws */
        }
        if (i == SHELL && lastboot_wait_until(t) != DEADLINE_NEVER) {
            /* waits for logd's answer (its packet wakes the loop), at most until then */
            uint64_t until = lastboot_wait_until(t);
            deadline = until < deadline ? until : deadline;
            continue;
        }
        if (i == MUSIC && !svcs[MIXER].running && !svcs[MIXER].given_up)
            continue;   /* after the mixer (it opens its stream only on `music start`) */
        if (t < s->next_try) {
            deadline = s->next_try < deadline ? s->next_try : deadline;
            continue;
        }
        status_t st = services_start(i);
        if (st == OK && mounted(DATA_MOUNT))
            services_settings(i);   /* a (re)started mixer or player gets its volume */
        if (st != OK) {
            printf("init: can't start %s (%s)\n", s->path, status_str(st));
            s->backoff = s->backoff ? s->backoff * 2 : 100 * NS_PER_MS;
            if (s->backoff > 5 * NS_PER_S)
                s->backoff = 5 * NS_PER_S;
            s->next_try = t + s->backoff;
            deadline = s->next_try < deadline ? s->next_try : deadline;
        }
    }
    return deadline;
}

bool init_shell(bool no_usb, bool splash, const char *shell_arg)
{
    if (splash)
        splash_expect();
    else
        svcs[SPLASH].given_up = true;   /* `verbose`, `nosplash`, safe mode, tests */
    status_t st = jam_port_create(&port);
    if (st != OK) {
        init_say("init: shell mode: no port (%s)", status_str(st));
        return false;
    }
    writers_init();
    services_init(port, no_usb, splash, shell_arg);
    settings_clock();   /* the defaults until /data's settings are read */
    lastboot_init(port, KEY_LASTBOOT);
    printf("init: shell mode%s: starting the bootfs server, the console,%s the serial input, "
           "devmgr, the mixer, the music player, netstack, logd and the shell\n",
           no_usb ? " (safe mode: nousb)" : "", splash ? " the boot splash," : "");
    for (;;) {
        uint64_t deadline = start_due(now());
        struct port_packet pkt;
        st = jam_port_wait(port, deadline, &pkt);
        if (st != OK && st != ERR_TIMED_OUT)
            return false;
        if (st != OK)
            continue;
        if (pkt.key < NSVC && pkt.type == PORT_PACKET_SIGNAL && svcs[pkt.key].running) {
            ended((unsigned)pkt.key);
        } else if (pkt.key == KEY_MOUNTS) {
            tell_mounts();
            data_came();
            reboot_note_esp();   /* the first /esp: what the stored kernel came from */
        } else if (pkt.key == KEY_LASTBOOT) {
            lastboot_event();
        } else if (pkt.key >= KEY_CTL && pkt.key < KEY_CTL + CTL_COUNT) {
            ctl_serve((unsigned)(pkt.key - KEY_CTL));
        } else if (pkt.key == KEY_SPLASH) {
            splash_event();
        } else if (pkt.key == KEY_UPDATE) {
            update_event();
        }
    }
}
