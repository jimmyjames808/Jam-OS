/* init's shell mode: a plain boot ("Jam OS", or "shell" on
 * the command line) ends at a shell prompt on the screen.
 *
 * init starts and then supervises fifteen services, each in a job of its
 * own under init's: the bootfs server, the console, the boot splash,
 * serialin, devmgr, the mixer, the music player, netstack, the DHCP
 * client, the resolver, logd, netlog, sntp, the file server and the shell;
 * the compositor first, unless the boot word `nocomp` says no (comp.c),
 * and then each terminal opened later, a console and a shell (terms.c). How each
 * one is started and what it is given is in services.c (the network's in
 * net.c, the terminals' in terms.c); this file is the order, the
 * namespace they follow and the restarts.
 *
 * init's own namespace holds its mounts (/boot, and what devmgr mounts:
 * mounts.c) and the services it publishes under /svc (services.c). The
 * services that have a namespace get the part of it their grants name
 * (followers[], <os.h> "grants"): the shell all of it as it is, the music
 * player every mount read-only and the mixer, logd /data (its top-level
 * etc left alone), netlog /svc/net-sys, sntp /svc/net-sys and /svc/dns-sys, the
 * splash the mixer and /svc/wayland, the file server /svc/net and /svc/net-low. init
 * keeps its end of each one's
 * SR_NS channel and sends it every later change, with ns_update: each
 * change takes back the one it hasn't read yet (logd never looks up a
 * path again after it opens its file), so however often the namespace
 * changes, each holds at most one message from init.
 *
 * A service that ends is started again (in the order above; the console's
 * clients wait for the console, logd, netlog and sntp for /data):
 *   - a new bootfs server is mounted at /boot again, and the shell is sent
 *     the new mount (until then /boot answers ERR_PEER_CLOSED).
 *   - a new console, a new devmgr: services.c (services_closed) says what
 *     their ends change for the others.
 * Restarts back off from 100 ms to 5 s; one that ends more than 10 times
 * in a minute (from its first end) is given up on (a line in the log and
 * the RESULTS box), but for the first terminal's console and shell, and
 * the compositor they show in: without them nobody can use the machine
 * until a reset, so they are started again for good, every 5 s at worst
 * (said once a minute). Another terminal given up on closes (terms.c),
 * as one does whose window is closed or whose shell ends with `exit`. The console's clients (the
 * shell; serialin, under `nocomp`) end when it does, often before init
 * has seen the console's own end: an end of theirs while the console is
 * gone doesn't count, and they start again at once with the new console;
 * serialin goes with the compositor the same way.
 * The mixer outlives its process (spare.c: init keeps its state VMO, what
 * it hands its keeper, and a warm spare to promote), and has a rule of its
 * own: a deliberate kill (initctl.kill) neither counts nor waits, and the
 * first crash in a minute is restarted at once; later crashes count and
 * back off as above. init itself never returns in this mode. */
#include <os.h>
#include "init.h"

#define GIVE_UP_COUNT  10
#define GIVE_UP_WINDOW (60 * NS_PER_S)

struct svc svcs[NSVC] = {
    [BOOTFS] = { BOOTFS_PATH }, [COMPOSITOR] = { "bin/compositor" },
    [CONSOLE] = { "bin/console" }, [SPLASH] = { "bin/splash" },
    [SERIALIN] = { "bin/serialin" },
    [DEVMGR] = { "bin/devmgr" }, [MIXER] = { "bin/mixer" }, [MUSIC] = { "bin/music" },
    [NETSTACK] = { "bin/netstack" }, [DHCP] = { "bin/dhcp" }, [DNS] = { "bin/dns" },
    [LOGD] = { "bin/logd" }, [NETLOG] = { "bin/netlog" }, [SNTP] = { "bin/sntp" },
    [SERVE] = { "bin/serve" }, [SHELL] = { "bin/shell" },
};

/* A service that has a namespace, kept in step with init's. */
struct follower {
    const char *const *only;    /* its grants (<os.h> "grants"); NULL: no namespace */
    handle_t           ns;      /* init's end of its SR_NS channel (0: not running) */
    handle_t           back;    /* a duplicate of its end, for ns_update (0: none) */
};

/* The splash: the mixer, and its full-screen window (/svc/wayland, which
 * only a boot with a compositor has). */
static const char *const splash_grants[] = { "/svc/" SVC_AUDIO, "/svc/" SVC_WAYLAND, NULL };
static const char *const music_grants[] = { "*:r", "/svc/" SVC_AUDIO, NULL };
static const char *const logd_grants[] = { DATA_MOUNT ":w", NULL };
/* The network's own services reach netstack through /svc/net-sys and the
 * resolver through /svc/dns-sys, the reserves no program can use up
 * (libos's net_svc and dns_svc prefer them). */
static const char *const netlog_grants[] = { "/svc/" SVC_NET_SYS, NULL };
static const char *const dns_grants[] = { "/svc/" SVC_NET_SYS, NULL };
static const char *const sntp_grants[] = { "/svc/" SVC_NET_SYS, "/svc/" SVC_DNS_SYS, NULL };
/* The file server: what its list says (`svc net listen low`: it may serve
 * on port 80; it is given its files by the shell, no mount). */
static const char *const serve_grants[] = { "/svc/" SVC_NET, "/svc/" SVC_NET_LISTEN_LOW, NULL };
static struct follower followers[NSVC] = {
    [SPLASH] = { .only = splash_grants }, [MUSIC] = { .only = music_grants },
    [LOGD] = { .only = logd_grants }, [NETLOG] = { .only = netlog_grants },
    [SHELL] = { .only = NS_ALL }, [DNS] = { .only = dns_grants },
    [SNTP] = { .only = sntp_grants }, [SERVE] = { .only = serve_grants },
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

/* Svc i runs as proc in job (both ours now): its end comes to the loop. */
static status_t watch(unsigned i, handle_t proc, handle_t job)
{
    struct svc *s = &svcs[i];
    status_t st = jam_port_bind(port, proc, i, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st != OK) {
        jam_job_kill(job);
        jam_handle_close(proc);
        jam_handle_close(job);
        return st;
    }
    s->proc = proc;
    s->job = job;
    s->running = true;
    s->started = now();
    s->kill_at = 0;
    writers_started(i, proc);
    return OK;
}

status_t svc_start_args(unsigned i, const struct svc_args *sa)
{
    struct svc *s = &svcs[i];
    struct follower *f = &followers[i];
    handle_t ns = HANDLE_INVALID, back = HANDLE_INVALID, job, proc = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        for (unsigned k = 0; k < sa->nx; k++)
            if (sa->x[k].h)
                jam_handle_close(sa->x[k].h);
        return st;
    }
    struct spawn_args a = {
        .path = s->path, .argc = sa->argc, .argv = sa->argv, .job = job, .extra = sa->x,
        .nextra = sa->nx, .extra_rights = sa->rights, .ns = f->only,
        .ns_out = f->only ? &ns : NULL, .ns_back_out = f->only ? &back : NULL,
    };
    st = spawn(&a, &proc);
    if (st != OK) {
        jam_handle_close(job);
        return st;
    }
    st = watch(i, proc, job);
    if (st != OK) {
        if (ns)
            jam_handle_close(ns);
        if (back)
            jam_handle_close(back);
        return st;
    }
    f->ns = ns;
    f->back = back;
    return OK;
}

status_t svc_start(unsigned i, int argc, const char *const *argv, struct spawn_handle *x,
                   unsigned nx)
{
    struct svc_args a = { .argc = argc, .argv = argv, .x = x, .rights = NULL, .nx = nx };
    return svc_start_args(i, &a);
}

status_t svc_start1(unsigned i, struct spawn_handle *x, unsigned nx)
{
    const char *argv[] = { svcs[i].path };
    return svc_start(i, 1, argv, x, nx);
}

status_t svc_adopt(unsigned i, handle_t proc, handle_t job)
{
    if (followers[i].only) {   /* a namespace comes only with a spawn */
        jam_job_kill(job);
        jam_handle_close(proc);
        jam_handle_close(job);
        return ERR_NOT_SUPPORTED;
    }
    return watch(i, proc, job);
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

/* Service i is called name: its program's ("console": the first that
 * runs), or an extra terminal's ("console-2", terms_named). */
static bool named(unsigned i, const char *name)
{
    unsigned t;
    if (terms_named(name, &t))
        return t == i;
    if (term_of(i) > 0)
        return false;   /* "console" is the first terminal's alone */
    const char *last = svcs[i].path;
    for (const char *p = svcs[i].path; *p; p++)
        if (*p == '/')
            last = p + 1;
    return !strcmp(last, name);
}

status_t shell_kill_service(const char *name, uint64_t *koid)
{
    for (unsigned i = 0; i < NSVC; i++) {
        struct svc *s = &svcs[i];
        if (!named(i, name) || !s->running)
            continue;
        struct process_info info;
        status_t st = jam_process_get_info(s->proc, &info);
        s->kill_at = now();   /* a deliberate end (ended() reads it), from this moment */
        if (st == OK)
            st = jam_process_kill(s->proc);   /* the loop sees it end: its job, the restart */
        if (st == OK)
            *koid = info.koid;
        else
            s->kill_at = 0;
        return st;
    }
    return ERR_NOT_FOUND;
}

/* The services init never gives up on, however often they end: the first
 * terminal and, when there is one, the compositor it shows in. */
static bool never_given_up(unsigned i)
{
    return i == CONSOLE || i == SHELL || i == COMPOSITOR;
}

/* Svc i is one of a console's clients (a shell, its terminal's) or of
 * the input's hub (serialin: the compositor's, or under `nocomp` the
 * first console's) and that has ended (its end may still be on its way
 * to us): i went with it, no fault of its own. */
static bool went_with_console(unsigned i)
{
    int k = term_of(i);
    if (i != SERIALIN && (k < 0 || (unsigned)TERM_SHELL(k) != i))
        return false;
    unsigned hub = comp_on() ? COMPOSITOR : CONSOLE;
    struct svc *c = &svcs[i == SERIALIN ? hub : (unsigned)TERM_CONSOLE(k)];
    signals_t seen;
    return !c->running || jam_object_wait_one(c->proc, SIG_TERMINATED, 0, &seen) == OK;
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

/* The restart rule of a service that outlives its process (spare_kept),
 * ended at t: a deliberate kill neither counts nor waits; a crash counts
 * as any service's (ended() has counted it), and the first in its minute
 * starts again at once (from the spare), later ones back off as any
 * service's. true: it is scheduled; false: back off (ended() goes on). */
static bool kept_restart(unsigned i, uint64_t t)
{
    struct svc *s = &svcs[i];
    if (!spare_kept(i))
        return false;
    if (s->kill_at)
        printf("init: %s: killed on purpose: not counted, started again at once\n", s->path);
    else if (s->ends > 1)
        return false;
    else
        s->backoff = 0;   /* the next crash in this minute waits the first step */
    s->next_try = t;
    return true;
}

/* Svc i ended: say how, clean up, schedule the restart. */
static void ended(unsigned i)
{
    struct svc *s = &svcs[i];
    struct process_info info;
    bool got = jam_process_get_info(s->proc, &info) == OK;
    char term[16] = "";   /* another terminal's: which */
    if (term_of(i) > 0)
        snprintf(term, sizeof(term), " (terminal %d)", term_of(i) + 1);
    if (got)
        printf("init: %s%s %s %ld\n", s->path, term,
               info.killed ? "was killed, code" : "exited with code", (long)info.exit_code);
    jam_job_kill(s->job);   /* anything it started (devmgr: every driver) */
    jam_handle_close(s->proc);
    jam_handle_close(s->job);
    s->proc = s->job = HANDLE_INVALID;
    s->running = false;
    uint64_t t = now();
    s->ended_at = t;
    services_closed(i);
    net_ended(i);
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
    if (terms_ended(i, got && info.killed, got ? info.exit_code : -1))
        return;   /* an extra terminal closes */
    bool took = went_with_console(i);
    bool counted = !took && !(spare_kept(i) && s->kill_at);
    if (counted && !count_end(i, t))
        return;
    if (kept_restart(i, t))
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
        comp_settings();   /* the saved window layout */
        terms_settings();  /* the terminals' font */
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
        if (i != CONSOLE && i != BOOTFS && i != COMPOSITOR && i != LOGD && !services_console_up())
            continue;   /* waits for the console */
        if (term_of(i) > 0 && (unsigned)TERM_SHELL(term_of(i)) == i && !terms_console_up(i))
            continue;   /* an extra terminal's shell waits for its console */
        if (i == SERIALIN && comp_on() && !comp_up())
            continue;   /* its source is the compositor's: it waits for one */
        if ((i == LOGD || i == NETLOG || i == SNTP) && !mounted(DATA_MOUNT))
            continue;   /* waits for /data (netlog, sntp: their settings): a mount's packet
                         * wakes the loop */
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
        if ((i == DHCP || i == DNS || i == SNTP || i == SERVE) && !svcs[NETSTACK].running)
            continue;   /* after netstack (started just before them) */
        uint64_t dhcp_at = i == DHCP ? net_dhcp_wait(t, mounted(DATA_MOUNT)) : 0;
        if (dhcp_at) {   /* /data's settings may say the address is static */
            deadline = dhcp_at < deadline ? dhcp_at : deadline;
            continue;
        }
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
    for (unsigned k = 1; k < TERM_MAX; k++)
        followers[TERM_SHELL(k)].only = NS_ALL;   /* every terminal's shell: all of it */
    spare_init(port, !init_nospare);
    settings_clock();   /* the defaults until /data's settings are read */
    lastboot_init(port, KEY_LASTBOOT);
    printf("init: shell mode%s: starting the bootfs server,%s the console,%s the serial input, "
           "devmgr, the mixer, the music player, netstack, dhcp, dns, logd, netlog, sntp, the file "
           "server and the shell\n",
           no_usb ? " (safe mode: nousb)" : "", comp_on() ? " the compositor," : "",
           splash ? " the boot splash," : "");
    for (;;) {
        uint64_t t = now(), deadline = start_due(t), net = net_due(t);
        uint64_t spare = spare_due(t);   /* after the starts: a promoted spare runs first */
        if (net < deadline)
            deadline = net;
        if (spare < deadline)
            deadline = spare;
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
            reboot_note_esp();   /* the first /esp: its files as the stored kernel's */
        } else if (pkt.key == KEY_LASTBOOT) {
            lastboot_event();
        } else if (pkt.key >= KEY_CTL && pkt.key < KEY_CTL + CTL_COUNT) {
            ctl_serve((unsigned)(pkt.key - KEY_CTL));
        } else if (pkt.key == KEY_SPLASH) {
            splash_event();
        } else if (pkt.key == KEY_UPDATE) {
            update_event();
        } else if (pkt.key == KEY_NETCTL) {
            net_netctl_event();
        } else if (pkt.key == KEY_SPARE) {
            spare_event();
        } else if (pkt.key == KEY_KEEP) {
            kept_event();
        } else if (pkt.key == KEY_COMP) {
            comp_event();
        }
    }
}
