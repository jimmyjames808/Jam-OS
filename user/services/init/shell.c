/* init's shell mode: a plain boot ("Jam OS", or "shell" on
 * the command line) ends at a shell prompt on the screen.
 *
 * init starts and then supervises eight services, each in a job of its own
 * under init's (and, first, the boot splash: splash.c):
 *   bootfs    bin/bootfs: the boot image as a mount, with the server end of
 *             its `fs` channel (SR_USER + 0); init mounts the client end at
 *             /boot in its own namespace, the one the shell is given
 *   console   bin/console: root with READ | WRITE | MANAGE (klog, the screen,
 *             serial output; reboot on Ctrl+Alt+Del), the server end of a
 *             console channel (SR_USER + 0) and a control channel of init's
 *             that answers only `reboot` (SR_USER + 8, ctl.c: Ctrl+Alt+Del
 *             goes through init, which syncs /data first);
 *             init keeps the client end. With the splash (the second
 *             argument "quiet") it draws nothing until the splash has
 *             borrowed the screen and given it back
 *   splash    bin/splash, once, on a plain boot (argv "splash" from the
 *             kernel): a PROGRAM-level console channel (SR_CONSOLE), a
 *             channel of init's (SR_USER + 0, <splash.h>) and a client end
 *             of the mixer's `audio` channel (SR_AUDIO). It plays the boot
 *             animation while the rest start; the shell is started only
 *             once it has played (or ended), and it gives the screen back
 *             when the shell says it is ready (initctl.shell_ready)
 *   serialin  bin/serialin: root with READ (serial_open) and an `input`
 *             channel from console.connect_input (SR_USER + 0)
 *   devmgr    bin/devmgr: RES_PCI sliced from the root (SR_RESOURCE), the
 *             server ends of its control and query channels (SR_DEVMGR_CTL,
 *             SR_DEVMGR; init keeps a client end of each) and a copy of
 *             init's (ADMIN) console client end (SR_CONSOLE), so its HID
 *             drivers type into the console. init waits
 *             for its first binding pass (up to 30 s). "nousb" (the safe
 *             mode boot entry) is passed on: no USB controller driver.
 *             Its mounts (/data, /esp) are followed from then on (mounts.c)
 *   mixer     bin/mixer, once devmgr runs: a duplicate of devmgr's query
 *             client end (SR_DEVMGR: it finds the hda driver), and the
 *             server ends of the `audio` and `audioctl` channels (SR_AUDIO,
 *             SR_AUDIO_CTL; abi/idl/audio.idl, audioctl.idl). init makes
 *             those two channels once and keeps their server ends, so a
 *             restarted mixer serves the same channels (calls made while
 *             it is down wait for it) and nobody needs new client ends;
 *             init closes them only if it gives up on the mixer. The mixer
 *             ends when devmgr does and is started again with the new one
 *   music     bin/music, the background music player, after the mixer: the
 *             server end of the `music` channel (SR_USER + 0;
 *             abi/idl/music.idl), a client end of the mixer's `audio`
 *             channel (SR_AUDIO) and init's namespace (SR_NS, followed
 *             like the shell's). init makes the `music` channel once and
 *             keeps both ends, as the mixer's, so a restarted player
 *             serves the same channel and the shell's end stays good
 *   logd      bin/logd, once /data is mounted: root with READ (the kernel
 *             log), a namespace holding only /data (SR_NS) and the server
 *             end of a `logctl` channel (SR_USER + 2; init keeps the client
 *             end and asks for a flush before a reboot). It saves each
 *             boot's log as /data/logs/boot-NNNN.txt
 *   shell     bin/shell: a SHELL-level console channel (SR_CONSOLE:
 *             console.new_client; no connect_input), root with READ |
 *             MANAGE, RES_PCI with RIGHTS_BASIC (SR_USER + 1), devmgr's
 *             query and control client ends (SR_DEVMGR, SR_DEVMGR_CTL: it
 *             passes control only to its utest/usbtest commands), a
 *             channel from init (SR_USER + 2) on which init sends it each
 *             new devmgr's pair (INIT_SHELL_DEVMGR, <devmgr.h>), init's
 *             control channel (SR_USER + 3, ctl.c: kill, sync, reboot),
 *             init's namespace (SR_NS) and client ends of the mixer's
 *             channels (SR_AUDIO, SR_AUDIO_CTL) and of the music player's (SR_USER + 4)
 * init keeps its end of the shell's, the music player's and logd's SR_NS channels and sends
 * them every later change of its mounts (logd: of /data), with ns_update:
 * each change takes back the one they haven't read yet (logd never looks
 * up a path again after it opens its file), so however often the mounts
 * change, each holds at most one message from init.
 * None of the console, serialin and the shell gets RIGHT_MAP or
 * RIGHT_SLICE on the root: they can't reach hardware beyond the calls made
 * for them.
 *
 * A service that ends is started again (in the order above; the console's
 * clients wait for the console, logd for /data):
 *   - a new bootfs server is mounted at /boot again, and the shell is sent
 *     the new mount (until then /boot answers ERR_PEER_CLOSED).
 *   - a new console gets a new channel, so serialin and the shell (whose
 *     channel then closes) exit and come back connected to it, and devmgr
 *     gets the new channel (DEVMGR_SET_CONSOLE): its HID drivers, which
 *     end when their console goes, come back connected to it.
 *   - devmgr dying (killed, or a crash) takes its whole job
 *     with it: every driver it started (usb-bus, each hid). A new devmgr
 *     binds them again from scratch (the kernel's safe rebind: a new
 *     dma_cap with Bus Master Enable off until usb-bus has reset the
 *     controller; the dead one's DMA pages stay quarantined until then),
 *     connected to the console. The shell gets the new devmgr channel. The
 *     mounts that came from the dead devmgr leave the namespace (the
 *     shell's and logd's too) until the new one serves them again; logd
 *     then carries on in the same file.
 * Restarts back off from 100 ms to 5 s; one that ends more than 10 times
 * in a minute is given up on (a line in the log and the RESULTS box). init
 * itself never returns in this mode. */
#include <devmgr.h>
#include <idl/console.h>
#include <idl/logctl.h>
#include <os.h>
#include <splash.h>
#include "init.h"

#define GIVE_UP_COUNT  10
#define GIVE_UP_WINDOW (60 * NS_PER_S)

enum { BOOTFS, CONSOLE, SPLASH, SERIALIN, DEVMGR, MIXER, MUSIC, LOGD, SHELL, NSVC };

/* Port keys: a service's index (its process ended), or one of these. */
#define KEY_MOUNTS 0x100u   /* the mounts watcher changed the namespace */
#define KEY_CTL    0x200u   /* + CTL_*: requests on a control channel */
#define KEY_SPLASH 0x300u   /* the splash's channel (splash.c) */

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
    [BOOTFS] = { BOOTFS_PATH }, [CONSOLE] = { "bin/console" }, [SPLASH] = { "bin/splash" },
    [SERIALIN] = { "bin/serialin" },
    [DEVMGR] = { "bin/devmgr" }, [MIXER] = { "bin/mixer" }, [MUSIC] = { "bin/music" },
    [LOGD] = { "bin/logd" }, [SHELL] = { "bin/shell" },
};

/* A service that has a namespace, kept in step with init's. */
struct follower {
    const char *const *only;    /* the mounts it may have (NS_ALL: every one); NULL: none */
    handle_t           ns;      /* init's end of its SR_NS channel (0: not running) */
    handle_t           back;    /* a duplicate of its end, for ns_update (0: none) */
};

static const char *const logd_mounts[] = { DATA_MOUNT, NULL };
static struct follower followers[NSVC] = {
    [MUSIC] = { .only = NS_ALL }, [LOGD] = { .only = logd_mounts }, [SHELL] = { .only = NS_ALL },
};
static handle_t root, port;
static handle_t cons;       /* the console client end (0: none) */
static handle_t devmgr;     /* devmgr's control channel, client end (0: none running) */
static handle_t devmgr_q;   /* its query channel, client end */
static handle_t to_shell;   /* init's end of the shell's SR_USER + 2 channel */
static handle_t logd_ctl;   /* logd's control channel, client end (0: no logd) */
/* The mixer's channels, made once: server ends (each mixer gets
 * duplicates) and client ends (the shell gets duplicates). [0] `audio`,
 * [1] `audioctl`; 0: none (no bin/mixer, or given up on). */
static handle_t audio_srv[2], audio_cli[2];
/* The music player's channel, made once in the same way (0: none). */
static handle_t music_srv, music_cli;
static bool nousb;
static bool quiet_console;   /* the next console starts quiet (the splash's first one) */

static handle_t dup_of(handle_t h);

handle_t shell_root(void)
{
    return root;
}

handle_t shell_devmgr(void)
{
    return devmgr;
}

static handle_t root_with(rights_t rights)
{
    handle_t h = HANDLE_INVALID;
    if (jam_handle_duplicate(root, rights, &h) != OK)
        return HANDLE_INVALID;
    return h;
}

/* path is one of list's mount points (NS_ALL lists every one). */
static bool in_list(const char *const *list, const char *path)
{
    if (list[0] && !strcmp(list[0], NS_ALL[0]))
        return true;
    for (unsigned i = 0; list[i]; i++)
        if (!strcmp(list[i], path))
            return true;
    return false;
}

/* init's mount points now that are in `only`, into out; how many. */
static unsigned mount_points(const char *const *only, char out[NS_MAX_MOUNTS][NS_NAME_MAX])
{
    unsigned n = 0;
    char point[NS_NAME_MAX];
    for (unsigned i = 0; n < NS_MAX_MOUNTS && ns_mount_at(i, point); i++)
        if (in_list(only, point))
            memcpy(out[n++], point, NS_NAME_MAX);
    return n;
}

/* init has a mount at path now. */
static bool mounted(const char *path)
{
    char all[NS_MAX_MOUNTS][NS_NAME_MAX];
    unsigned n = mount_points(NS_ALL, all);
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(all[i], path))
            return true;
    return false;
}

/* Start svc i with these arguments and extra handles (consumed). A
 * follower gets its part of init's namespace. */
static status_t start(unsigned i, int argc, const char *const *argv, struct spawn_handle *x,
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
    f->ns = ns;
    f->back = back;
    return OK;
}

static status_t start1(unsigned i, struct spawn_handle *x, unsigned nx)
{
    const char *argv[] = { svcs[i].path };
    return start(i, 1, argv, x, nx);
}

/* Svc i's namespace follows init's: the whole of it that its list
 * allows (a mount whose service restarted has a new channel), replacing
 * any earlier one it hasn't read. */
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

static void tell_mounts(void)
{
    for (unsigned i = 0; i < NSVC; i++)
        tell_follower(i);
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

static status_t start_bootfs(void)
{
    handle_t a, b;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_USER + 0, b } };
    st = start1(BOOTFS, x, 1);
    if (st == OK)
        st = ns_mount(BOOT_MOUNT, a);   /* consumes a */
    else
        jam_handle_close(a);
    if (st == OK)
        tell_mounts();   /* a restart: the shell's /boot is the dead one's */
    return st;
}

static status_t start_console(void)
{
    handle_t a, b, ctl = HANDLE_INVALID;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    /* Without it the console still reboots, without the sync. */
    if (ctl_new(CTL_CONSOLE, port, KEY_CTL + CTL_CONSOLE, &ctl) != OK)
        ctl = HANDLE_INVALID;
    struct spawn_handle x[] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE) },
        { SR_USER + 0, b },
        { SR_USER + 8, ctl },
    };
    const char *argv[] = { svcs[CONSOLE].path, "quiet" };
    st = start(CONSOLE, quiet_console ? 2 : 1, argv, x, ctl ? 3 : 2);
    quiet_console = false;   /* a restarted console draws at once */
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

/* The boot splash: a PROGRAM-level console channel (the screen and the
 * keys, as any app), init's channel, and the mixer's `audio` channel. */
static status_t start_splash(void)
{
    handle_t c = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = console_new_client_until(cons, now() + 5 * NS_PER_S, 2, &c);
    if (st == OK && (st = splash_channel(port, KEY_SPLASH, &theirs)) != OK)
        jam_handle_close(c);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_CONSOLE, c }, { SR_USER + SPLASH_INIT_ROLE, theirs },
                                { SR_AUDIO, dup_of(audio_cli[0]) } };
    return start1(SPLASH, x, x[2].h ? 3 : 2);   /* no mixer channel: it plays silently */
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
    handle_t watch;
    st = jam_handle_duplicate(devmgr, RIGHT_SAME, &watch);
    if (st == OK)
        st = mounts_watch(watch, port, KEY_MOUNTS);
    if (st != OK)
        printf("init: not following devmgr's mounts (%s)\n", status_str(st));
    return OK;
}

/* logd: the kernel log into /data/logs. start_due starts it only once
 * /data is mounted. */
static status_t start_logd(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, svcs[LOGD].path, &data, &size) != OK) {
        printf("init: no %s in bootfs: the boot logs are not saved\n", svcs[LOGD].path);
        svcs[LOGD].given_up = true;
        return OK;
    }
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ) },
                                { SR_USER + 2, theirs } };
    st = start1(LOGD, x, 2);
    if (st != OK) {
        jam_handle_close(mine);
        return st;
    }
    logd_ctl = mine;
    return OK;
}

status_t shell_stop_devmgr(uint64_t deadline)
{
    struct svc *s = &svcs[DEVMGR];
    if (!devmgr || !s->running)
        return OK;
    struct devmgr_rep r;
    status_t st = devmgr_call(devmgr, DEVMGR_SHUTDOWN, 0, 0, 0, &r, NULL, 0, NULL, deadline);
    signals_t seen;
    if (st == OK)
        st = jam_object_wait_one(s->proc, SIG_TERMINATED, deadline, &seen);
    if (st != OK)
        jam_job_kill(s->job);
    s->given_up = true;   /* not again: the machine is about to restart */
    return st;
}

void shell_flush_log(uint64_t deadline)
{
    status_t st = logd_ctl ? logctl_flush_until(logd_ctl, deadline) : ERR_NOT_FOUND;
    /* No logd or no /data: nothing to save. Else said on the screen and
     * the serial port, which is all that is left. */
    if (st != OK && st != ERR_NOT_FOUND && st != ERR_PEER_CLOSED)
        printf("init: the boot log's last lines were not saved (%s)\n", status_str(st));
}

/* The mixer's two channels, once (both, or neither). */
static void make_audio_channels(void)
{
    for (unsigned k = 0; k < 2; k++)
        if (jam_channel_create(&audio_cli[k], &audio_srv[k]) != OK)
            audio_cli[k] = audio_srv[k] = HANDLE_INVALID;
    if (audio_cli[0] && audio_cli[1])
        return;
    for (unsigned k = 0; k < 2; k++) {
        if (audio_cli[k]) {
            jam_handle_close(audio_cli[k]);
            jam_handle_close(audio_srv[k]);
        }
        audio_cli[k] = audio_srv[k] = HANDLE_INVALID;
    }
}

static handle_t dup_of(handle_t h)
{
    handle_t d = HANDLE_INVALID;
    if (!h || jam_handle_duplicate(h, RIGHT_SAME, &d) != OK)
        return HANDLE_INVALID;
    return d;
}

/* The mixer: its server ends again (the same channels as any mixer
 * before it), and devmgr's query channel to find the hda driver. */
static status_t start_mixer(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (!audio_srv[0] || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, svcs[MIXER].path, &data, &size) != OK) {
        printf("init: no %s (or no channels for it): no sound\n", svcs[MIXER].path);
        svcs[MIXER].given_up = true;
        return OK;
    }
    struct spawn_handle x[] = { { SR_AUDIO, dup_of(audio_srv[0]) },
                                { SR_AUDIO_CTL, dup_of(audio_srv[1]) },
                                { SR_DEVMGR, dup_of(devmgr_q) } };
    if (!x[0].h || !x[1].h) {
        for (unsigned k = 0; k < 3; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return ERR_NO_RESOURCES;
    }
    return start1(MIXER, x, x[2].h ? 3 : 2);   /* no devmgr: it answers "no output" */
}

/* The music player: its server end again (the same channel as any player
 * before it) and a client end of the mixer's `audio` channel. */
static status_t start_music(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (!music_srv || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, svcs[MUSIC].path, &data, &size) != OK) {
        printf("init: no %s (or no channel for it): no music player\n", svcs[MUSIC].path);
        svcs[MUSIC].given_up = true;
        return OK;
    }
    struct spawn_handle x[] = { { SR_USER + 0, dup_of(music_srv) },
                                { SR_AUDIO, dup_of(audio_cli[0]) } };
    if (!x[0].h) {
        if (x[1].h)
            jam_handle_close(x[1].h);
        return ERR_NO_RESOURCES;
    }
    return start1(MUSIC, x, x[1].h ? 2 : 1);   /* no mixer: it answers "no output" */
}

/* An argument for the first shell started ("soak=3": run the soak test), or NULL. */
static const char *first_arg;

static status_t start_shell(void)
{
    handle_t c = HANDLE_INVALID, d = HANDLE_INVALID, dc = HANDLE_INVALID, pci = HANDLE_INVALID;
    handle_t p2 = HANDLE_INVALID, mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    handle_t ctl = HANDLE_INVALID;
    handle_t au = dup_of(audio_cli[0]), auc = dup_of(audio_cli[1]), mu = dup_of(music_cli);
    /* A SHELL-level console channel: no input sources of its own. */
    status_t st = console_new_client_until(cons, now() + 5 * NS_PER_S, 1, &c);
    if (st != OK) {
        if (au)
            jam_handle_close(au);
        if (auc)
            jam_handle_close(auc);
        if (mu)
            jam_handle_close(mu);
        return st;
    }
    if (devmgr) {
        jam_handle_duplicate(devmgr_q, RIGHT_SAME, &d);
        jam_handle_duplicate(devmgr, RIGHT_SAME, &dc);
    }
    if (jam_resource_create(root, RES_PCI, 0, 0, &pci) == OK &&
        jam_handle_replace(pci, RIGHTS_BASIC, &p2) != OK)
        p2 = HANDLE_INVALID;
    if (jam_channel_create(&mine, &theirs) != OK)
        mine = theirs = HANDLE_INVALID;
    if (ctl_new(CTL_SHELL, port, KEY_CTL + CTL_SHELL, &ctl) != OK)
        ctl = HANDLE_INVALID;
    struct spawn_handle x[] = {
        { SR_CONSOLE, c },
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_READ | RIGHT_MANAGE) },
        { SR_USER + 1, p2 },
        { SR_DEVMGR, d },
        { SR_DEVMGR_CTL, dc },
        { SR_USER + 2, theirs },
        { SR_USER + 3, ctl },
        { SR_AUDIO, au },
        { SR_AUDIO_CTL, auc },
        { SR_USER + 4, mu },
    };
    /* Leave out the ones we don't have. */
    struct spawn_handle y[10];
    unsigned n = 0;
    for (unsigned k = 0; k < 10; k++)
        if (x[k].h)
            y[n++] = x[k];
    /* The boot's first shell gets the boot word's command (shell_first_arg);
     * one init restarts later is an ordinary shell. */
    const char *argv[] = { svcs[SHELL].path, first_arg };
    st = start(SHELL, first_arg ? 2 : 1, argv, y, n);
    first_arg = NULL;
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
        mounts_unwatch();       /* its fat services went with its job */
        tell_mounts();
        printf("init: devmgr and its drivers are gone: starting them again\n");
    }
    if (i == LOGD && logd_ctl) {
        jam_handle_close(logd_ctl);
        logd_ctl = HANDLE_INVALID;
    }
    if (i == SHELL && to_shell) {
        jam_handle_close(to_shell);
        to_shell = HANDLE_INVALID;
    }
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
    if (++s->ends > GIVE_UP_COUNT) {
        s->given_up = true;
        for (unsigned k = 0; i == MIXER && k < 2; k++) {
            jam_handle_close(audio_srv[k]);   /* calls waiting for a mixer fail now */
            audio_srv[k] = HANDLE_INVALID;
        }
        if (i == MUSIC && music_srv) {
            jam_handle_close(music_srv);   /* the shell's `music` fails now */
            music_srv = HANDLE_INVALID;
        }
        init_say("init: %s ended %u times in a minute: not restarting it", s->path, s->ends);
        return;
    }
    /* Ran for a while: start again soon; else back off. */
    s->backoff = t - s->started > 10 * NS_PER_S || !s->backoff ? 100 * NS_PER_MS : s->backoff * 2;
    if (s->backoff > 5 * NS_PER_S)
        s->backoff = 5 * NS_PER_S;
    s->next_try = t + s->backoff;
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
        if (i != CONSOLE && i != BOOTFS && i != LOGD && !cons)
            continue;   /* waits for the console */
        if (i == LOGD && !mounted(DATA_MOUNT))
            continue;   /* waits for /data: a mount's packet wakes the loop */
        if (i == MIXER && !devmgr && !svcs[DEVMGR].given_up)
            continue;   /* waits for devmgr (started just before it) */
        if (i == SHELL && !splash_played())
            continue;   /* waits for the splash: its packet wakes the loop */
        if (i == MUSIC && !svcs[MIXER].running && !svcs[MIXER].given_up)
            continue;   /* after the mixer (it opens its stream only on `music start`) */
        if (t < s->next_try) {
            deadline = s->next_try < deadline ? s->next_try : deadline;
            continue;
        }
        status_t st = i == BOOTFS     ? start_bootfs()
                      : i == CONSOLE  ? start_console()
                      : i == SPLASH   ? start_splash()
                      : i == SERIALIN ? start_serialin()
                      : i == DEVMGR   ? start_devmgr()
                      : i == MIXER    ? start_mixer()
                      : i == MUSIC    ? start_music()
                      : i == LOGD     ? start_logd()
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
    return deadline;
}

bool init_shell(bool no_usb, bool splash, const char *shell_arg)
{
    root = startup_handle(SR_RESOURCE);
    nousb = no_usb;
    first_arg = shell_arg;
    quiet_console = splash;
    if (splash)
        splash_expect();
    else
        svcs[SPLASH].given_up = true;   /* `verbose`, `nosplash`, safe mode, tests */
    status_t st = jam_port_create(&port);
    if (st != OK) {
        init_say("init: shell mode: no port (%s)", status_str(st));
        return false;
    }
    make_audio_channels();
    if (jam_channel_create(&music_cli, &music_srv) != OK)
        music_cli = music_srv = HANDLE_INVALID;
    printf("init: shell mode%s: starting the bootfs server, the console,%s the serial input, "
           "devmgr, the mixer, the music player, logd and the shell\n",
           nousb ? " (safe mode: nousb)" : "", splash ? " the boot splash," : "");
    for (;;) {
        uint64_t deadline = start_due(now());
        struct port_packet pkt;
        st = jam_port_wait(port, deadline, &pkt);
        if (st != OK && st != ERR_TIMED_OUT)
            return false;
        if (st != OK)
            continue;
        if (pkt.key < NSVC && pkt.type == PORT_PACKET_SIGNAL && svcs[pkt.key].running)
            ended((unsigned)pkt.key);
        else if (pkt.key == KEY_MOUNTS)
            tell_mounts();
        else if (pkt.key >= KEY_CTL && pkt.key < KEY_CTL + CTL_COUNT)
            ctl_serve((unsigned)(pkt.key - KEY_CTL));
        else if (pkt.key == KEY_SPLASH)
            splash_event();
    }
}
