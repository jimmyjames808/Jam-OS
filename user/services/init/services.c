/* init's shell mode, the services: how each one is started, with which
 * handles, and what init keeps of it (shell.c supervises them all: the
 * order, the restarts, the giving up).
 *
 * Each service runs in a job of its own under init's (and, first, the
 * boot splash: splash.c):
 *   bootfs    bin/bootfs: the boot image as a mount, with the server end of
 *             its `fs` channel (SR_USER + 0); init mounts the client end at
 *             /boot in its own namespace, the one the shell is given
 *   console   bin/console: root with CONSOLE_ROOT (klog, the screen,
 *             serial output; reboot on Ctrl+Alt+Del), the server end of a
 *             console channel (SR_USER + 0) and a control channel of init's
 *             that answers only `reboot` (SR_USER + 8, ctl.c: Ctrl+Alt+Del
 *             goes through init, which syncs /data first), and the log
 *             writers' table read-only (CONSOLE_WRITERS_ROLE, writers.c);
 *             init keeps the client end. With the splash (the argument
 *             "quiet") it draws nothing until the splash has borrowed the
 *             screen and given it back; on a boot with the splash every
 *             console also gets "nolog": the kernel log off the screen
 *             but for its notices
 *   splash    bin/splash, once, on a plain boot (argv "splash" from the
 *             kernel): a PROGRAM-level console channel (SR_CONSOLE), a
 *             channel of init's (SR_USER + 0, <splash.h>) and a namespace
 *             with /svc/audio (shell.c's grants). It plays the boot
 *             animation while the rest start; the shell is started only
 *             once it has played (or ended), and it gives the screen back
 *             when the shell says it is ready (initctl.shell_ready)
 *   serialin  bin/serialin: root with RIGHT_ROOT_SERIAL (serial_open) and an `input`
 *             channel from console.connect_input (SR_USER + 0)
 *   devmgr    bin/devmgr: RES_PCI sliced from the root (SR_RESOURCE), the
 *             server ends of its control and query channels
 *             (SR_DEVMGR_CTL, SR_DEVMGR; init keeps a client end of each
 *             and publishes them as /svc/devmgr-ctl and /svc/devmgr) and a
 *             copy of init's (ADMIN) console client end (SR_CONSOLE), so its
 *             HID drivers type into the console. init waits for its first
 *             binding pass (up to 30 s), then asks it for each HD Audio
 *             controller's device channel (<devmgr.h> DEVMGR_DEVICE_CHANNEL:
 *             only its holder, and the control channel, can reach that hda
 *             driver; init keeps them and gives the mixer duplicates), and
 *             only then publishes the two names, so no program
 *             can ask the query channel for hda before it is the mixer's.
 *             "nousb" (the safe mode boot entry) is passed on: no USB
 *             controller driver. Its mounts (/data, /esp) are followed from
 *             then on (mounts.c). The NIC's device channel goes to netstack
 *             the same way: each network card's goes to netstack (net.c),
 *             whose /svc/net (a channel per opener) init publishes here
 *   mixer     bin/mixer, once devmgr runs: a duplicate of each HD Audio
 *             controller's device channel (SR_DEVMGR_DEVICE, one handle
 *             each; none without one: the mixer then has no output), and the
 *             server ends of the `audio` and `audioctl` channels (SR_AUDIO,
 *             SR_AUDIO_CTL; abi/idl/audio.idl, audioctl.idl). init makes
 *             those two channels once, publishes their client ends as
 *             /svc/audio and /svc/audioctl and keeps their server ends, so a
 *             restarted mixer serves the same channels (calls made while
 *             it is down wait for it) and nobody needs new client ends;
 *             init closes them only if it gives up on the mixer. The mixer
 *             ends when devmgr does and is started again with the new one
 *   music     bin/music, the background music player, after the mixer: the
 *             server end of the `music` channel (SR_USER + 0;
 *             abi/idl/music.idl) and a namespace of every mount
 *             read-only and /svc/audio (SR_NS, followed like the
 *             shell's). init makes the `music` channel once, keeps both
 *             ends, as the mixer's, so a restarted player serves the same
 *             channel, and publishes it as /svc/music (a channel per
 *             opener)
 *   logd      bin/logd, once /data is mounted: root with RIGHT_ROOT_KLOG
 *             (the kernel log), a namespace holding only /data (SR_NS) and the server
 *             end of a `logctl` channel (SR_USER + 2; init keeps the client
 *             end, publishes it as /svc/logd and asks for a flush before
 *             a reboot). It saves each
 *             boot's log as /data/logs/boot-NNNN.txt. On the boot after a
 *             panic it also gets the panicked boot's log and a channel for
 *             its answer (lastboot.c), and saves that log first
 *   shell     bin/shell: a SHELL-level console channel (SR_CONSOLE:
 *             console.new_client; no connect_input), root with SHELL_ROOT,
 *             RES_PCI with RIGHTS_BASIC (SR_USER + 1), a channel from
 *             init (SR_USER + 2) and init's whole namespace as it is
 *             (SR_NS, followed: every mount, /data's etc included, and
 *             every service: devmgr's channels, init's control channel
 *             /svc/init, made anew for each shell (ctl.c: kill, sync,
 *             reboot, mount), the mixer's, the music player's, logd's,
 *             netstack's /svc/net).
 *             On the boot after a panic the first shell waits for logd's
 *             answer (lastboot.c) and finds its one line queued on the
 *             SR_USER + 2 channel (INIT_SHELL_NOTE) when it starts
 * None of the console, serialin and the shell gets RIGHT_MAP or
 * RIGHT_SLICE on the root: they can't reach hardware beyond the calls made
 * for them.
 *
 * What a service's end changes here (services_closed): a new console gets
 * a new channel, so serialin and the shell (whose channel then closes)
 * exit and come back connected to it, and devmgr gets the new channel
 * (DEVMGR_SET_CONSOLE): its HID drivers, which end when their console
 * goes, come back connected to it. devmgr dying (killed, or a crash)
 * takes its whole job with it: every driver it started (usb-bus, each
 * hid). A new devmgr binds them again from scratch (the kernel's safe
 * rebind: a new dma_cap with Bus Master Enable off until usb-bus has
 * reset the controller; the dead one's DMA pages stay quarantined until
 * then), connected to the console. /svc/devmgr and /svc/devmgr-ctl name
 * the new devmgr's channels, and the followers are told.
 * The mounts that came from the dead devmgr leave the namespace (the
 * shell's and logd's too) until the new one serves them again; logd then
 * carries on in the same file. */
#include <devmgr.h>
#include <idl/console.h>
#include <idl/logctl.h>
#include <logwriters.h>
#include <os.h>
#include <splash.h>
#include "init.h"

/* The root's powers each service gets (<jam/abi.h> RIGHT_ROOT_*; none can
 * map or slice): the console reads the log, draws on the screen, mirrors
 * it to the serial port's output and reboots on Ctrl+Alt+Del if init doesn't
 * answer; serialin reads the serial port; logd the log; the shell the
 * log, the system's figures, the clock, the kernel's debug commands, a
 * reboot when init doesn't answer, and programs from /data (VMEX). Only
 * init keeps RIGHT_ROOT_KEXEC. */
#define CONSOLE_ROOT (RIGHT_ROOT_KLOG | RIGHT_ROOT_SCREEN | RIGHT_ROOT_SERIAL_OUT | \
                      RIGHT_ROOT_REBOOT)
#define SHELL_ROOT   (RIGHT_ROOT_KLOG | RIGHT_ROOT_SYSINFO | RIGHT_ROOT_CLOCK | RIGHT_ROOT_DEBUG | \
                      RIGHT_ROOT_REBOOT | RIGHT_ROOT_VMEX)

static handle_t root, port;
static handle_t cons;       /* the console client end (0: none) */
static handle_t devmgr;     /* devmgr's control channel, client end (0: none running) */
static handle_t devmgr_q;   /* its query channel, client end */
/* Its device channels for the HD Audio controllers, the mixer's, and for
 * the network cards, held for netstack (0: none). */
static handle_t devmgr_hda[INIT_MAX_CLAIMED], devmgr_net[INIT_MAX_CLAIMED];
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
static bool nolog_console;   /* every console keeps the log off the screen (a splash boot) */
/* An argument for the first shell started ("soak=3": run the soak test), or NULL. */
static const char *first_arg;

handle_t shell_root(void)
{
    return root;
}

handle_t shell_devmgr(void)
{
    return devmgr;
}

handle_t shell_console(void)
{
    return cons;
}

void services_settings(unsigned i)
{
    if (i == MIXER && svcs[MIXER].running)
        settings_master(audio_cli[1]);
    if (i == MUSIC && svcs[MUSIC].running)
        settings_music(music_cli);
    if (i == NETSTACK)
        net_settings();
}

bool services_console_up(void)
{
    return cons != HANDLE_INVALID;
}

bool services_devmgr_up(void)
{
    return devmgr != HANDLE_INVALID;
}

static handle_t root_with(rights_t rights)
{
    handle_t h = HANDLE_INVALID;
    if (jam_handle_duplicate(root, rights, &h) != OK)
        return HANDLE_INVALID;
    return h;
}

static handle_t dup_of(handle_t h)
{
    handle_t d = HANDLE_INVALID;
    if (!h || jam_handle_duplicate(h, RIGHT_SAME, &d) != OK)
        return HANDLE_INVALID;
    return d;
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

/* Publish h (a duplicate is taken; h stays ours) as /svc/<name> in our
 * namespace, the one the shell and the other followers get; HANDLE_INVALID
 * takes the name away. Followers hear of it with the next tell_mounts. */
static void publish(const char *name, handle_t h, bool connect)
{
    handle_t d = dup_of(h);
    status_t st = d ? ns_svc_set(name, d, connect) : ns_svc_remove(name);
    if (st != OK && (d || st != ERR_NOT_FOUND))
        printf("init: /svc/%s: %s\n", name, status_str(st));
}

static status_t start_bootfs(void)
{
    handle_t a, b;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_USER + 0, b } };
    st = svc_start1(BOOTFS, x, 1);
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
    struct spawn_handle x[4] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | CONSOLE_ROOT) },
        { SR_USER + 0, b },
    };
    unsigned nx = 2;
    handle_t writers = writers_for_console();   /* without it no process makes notices */
    if (writers)
        x[nx++] = (struct spawn_handle){ CONSOLE_WRITERS_ROLE, writers };
    if (ctl)
        x[nx++] = (struct spawn_handle){ SR_USER + 8, ctl };
    const char *argv[3] = { svcs[CONSOLE].path };
    int argc = 1;
    if (nolog_console)
        argv[argc++] = "nolog";
    if (quiet_console)
        argv[argc++] = "quiet";
    st = svc_start(CONSOLE, argc, argv, x, nx);
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
    struct spawn_handle x[] = { { SR_CONSOLE, c }, { SR_USER + SPLASH_INIT_ROLE, theirs } };
    const char *argv[] = { svcs[SPLASH].path, "--hang", NULL };
    return svc_start(SPLASH, init_splashhang ? 2 : 1, argv, x, 2);   /* no /svc/audio: silent */
}

static status_t start_serialin(void)
{
    handle_t src;
    status_t st = console_connect_input_until(cons, now() + 5 * NS_PER_S, &src);
    if (st != OK)
        return st;
    struct spawn_handle x[] = {
        { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_ROOT_SERIAL) }, { SR_USER + 0, src },
    };
    return svc_start1(SERIALIN, x, 2);
}

unsigned services_claim_class(handle_t devmgr_ctl, uint32_t cls, handle_t *out, unsigned max)
{
    unsigned n = 0;
    while (n < max) {
        status_t st = devmgr_device_channel(devmgr_ctl, DEVMGR_PCI_CLASS, (uint16_t)n, cls,
                                            now() + 5 * NS_PER_S, &out[n]);
        if (st == ERR_NOT_FOUND)
            break;   /* no more of them */
        if (st != OK) {
            printf("init: no device channel for class %06x number %u (%s)\n", cls, n,
                   status_str(st));
            break;
        }
        n++;
    }
    return n;
}

unsigned services_net_devices(handle_t *out, unsigned max)
{
    unsigned n = 0;
    for (unsigned k = 0; k < INIT_MAX_CLAIMED && n < max; k++)
        if (devmgr_net[k] && jam_handle_duplicate(devmgr_net[k], RIGHT_SAME, &out[n]) == OK)
            n++;
    return n;
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
    const char *argv[6] = { "bin/devmgr" };
    int argc = 1;
    if (nousb)
        argv[argc++] = "nousb";
    if (init_hidboot)
        argv[argc++] = "hidboot";
    if (init_netprobe)
        argv[argc++] = "netprobe";
    else if (init_netsend)
        argv[argc++] = "netsend";
    else if (init_net)
        argv[argc++] = "net";
    if (init_vlan)
        argv[argc++] = init_vlan;
    if (init_bootdisk)
        argv[argc++] = init_bootdisk;
    struct spawn_handle x[] = { { SR_RESOURCE, pci }, { SR_DEVMGR_CTL, b }, { SR_DEVMGR, qb },
                                { SR_CONSOLE, c } };
    st = svc_start(DEVMGR, argc, argv, x, 4);   /* consumes pci, b, qb and c */
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
    unsigned cards = services_claim_class(devmgr, DEVMGR_CLASS_HDA, devmgr_hda, INIT_MAX_CLAIMED);
    if (cards > 1)
        printf("init: %u HD Audio controllers: all of them the mixer's\n", cards);
    unsigned nics = services_claim_class(devmgr, DEVMGR_CLASS_NET, devmgr_net, INIT_MAX_CLAIMED);
    if (nics)
        printf("init: %u network card(s): held for netstack\n", nics);
    publish(SVC_DEVMGR, devmgr_q, true);   /* a channel per opener (svc.connect) */
    publish(SVC_DEVMGR_CTL, devmgr, false);
    tell_mounts();   /* a restart: the shell's /svc/devmgr is the dead one's */
    handle_t watch;
    st = jam_handle_duplicate(devmgr, RIGHT_SAME, &watch);
    if (st == OK)
        st = mounts_watch(watch, port, KEY_MOUNTS);
    if (st != OK)
        printf("init: not following devmgr's mounts (%s)\n", status_str(st));
    return OK;
}

/* logd: the kernel log into /data/logs. shell.c starts it only once
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
    struct spawn_handle x[4] = { { SR_RESOURCE, root_with(RIGHTS_BASIC | RIGHT_ROOT_KLOG) },
                                 { SR_USER + 2, theirs } };
    unsigned nx = 2 + lastboot_logd_handles(&x[2]);   /* after a panic: its log first */
    st = svc_start1(LOGD, x, nx);
    if (st != OK) {
        jam_handle_close(mine);
        return st;
    }
    logd_ctl = mine;
    publish(SVC_LOGD, logd_ctl, true);   /* a channel per opener (svc.connect) */
    return OK;
}

status_t shell_stop_devmgr(uint64_t deadline)
{
    /* The sound's clients first: the mixer holds a channel to the hda
     * driver, which ends only once every client has gone, so devmgr would
     * wait its whole STOP_WAIT for it (the PC's 30 s reboot). They are
     * stopped for good: the machine is about to restart. Not the console:
     * when it ends the kernel takes the screen back and redraws its log,
     * which flashed text over the blank screen. */
    static const int clients[] = { MUSIC, SPLASH, MIXER };
    for (unsigned i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
        struct svc *c = &svcs[clients[i]];
        c->given_up = true;
        if (!c->running)
            continue;
        signals_t seen;
        jam_job_kill(c->job);
        (void)jam_object_wait_one(c->proc, SIG_TERMINATED, deadline, &seen);
    }
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

/* The mixer: its server ends again (the same channels as any mixer
 * before it), and the sound cards' device channels to reach their drivers. */
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
    struct spawn_handle x[2 + INIT_MAX_CLAIMED] = { { SR_AUDIO, dup_of(audio_srv[0]) },
                                                    { SR_AUDIO_CTL, dup_of(audio_srv[1]) } };
    unsigned n = 2;
    for (unsigned k = 0; k < INIT_MAX_CLAIMED; k++)
        if (devmgr_hda[k] && (x[n].h = dup_of(devmgr_hda[k])) != HANDLE_INVALID)
            x[n++].role = SR_DEVMGR_DEVICE;   /* none: it answers "no output" */
    if (!x[0].h || !x[1].h) {
        for (unsigned k = 0; k < n; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return ERR_NO_RESOURCES;
    }
    return svc_start1(MIXER, x, n);
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
    struct spawn_handle x[] = { { SR_USER + 0, dup_of(music_srv) } };
    if (!x[0].h)
        return ERR_NO_RESOURCES;
    return svc_start1(MUSIC, x, 1);   /* no /svc/audio: it answers "no output" */
}

/* The line the boot's first shell prints after a panic (lastboot.c), queued
 * on its init channel before it starts. */
static void queue_banner(handle_t to)
{
    const char *b = lastboot_banner();
    size_t n = strlen(b);
    if (!to || !n)
        return;
    struct { uint32_t kind; char text[INIT_SHELL_NOTE_MAX]; } m = { INIT_SHELL_NOTE, { 0 } };
    if (n > sizeof(m.text))
        n = sizeof(m.text);
    memcpy(m.text, b, n);
    if (jam_channel_write(to, &m, (uint32_t)(sizeof(m.kind) + n), NULL, 0) != OK)
        printf("init: the shell can't be given the last boot's line\n");
}

static status_t start_shell(void)
{
    handle_t c = HANDLE_INVALID, pci = HANDLE_INVALID, p2 = HANDLE_INVALID;
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID, ctl = HANDLE_INVALID;
    /* A SHELL-level console channel: no input sources of its own. */
    status_t st = console_new_client_until(cons, now() + 5 * NS_PER_S, 1, &c);
    if (st != OK)
        return st;
    if (jam_resource_create(root, RES_PCI, 0, 0, &pci) == OK &&
        jam_handle_replace(pci, RIGHTS_BASIC, &p2) != OK)
        p2 = HANDLE_INVALID;
    if (jam_channel_create(&mine, &theirs) != OK)
        mine = theirs = HANDLE_INVALID;
    queue_banner(mine);
    /* Its control channel of init's: /svc/init, published before the
     * shell is given our namespace (a new one each time: the old one's
     * holders see ERR_PEER_CLOSED). */
    if (ctl_new(CTL_SHELL, port, KEY_CTL + CTL_SHELL, &ctl) == OK) {
        publish(SVC_INIT, ctl, false);
        jam_handle_close(ctl);
    }
    struct spawn_handle x[] = {
        { SR_CONSOLE, c },
        { SR_RESOURCE, root_with(RIGHTS_BASIC | SHELL_ROOT) },
        { SR_USER + 1, p2 },
        { SR_USER + 2, theirs },
    };
    /* Leave out the ones we don't have. */
    struct spawn_handle y[4];
    unsigned n = 0;
    for (unsigned k = 0; k < 4; k++)
        if (x[k].h)
            y[n++] = x[k];
    /* The boot's first shell gets the boot word's command (shell_first_arg);
     * one init restarts later is an ordinary shell. */
    const char *argv[] = { svcs[SHELL].path, first_arg };
    st = svc_start(SHELL, first_arg ? 2 : 1, argv, y, n);
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

status_t services_start(unsigned i)
{
    return i == BOOTFS     ? start_bootfs()
           : i == CONSOLE  ? start_console()
           : i == SPLASH   ? start_splash()
           : i == SERIALIN ? start_serialin()
           : i == DEVMGR   ? start_devmgr()
           : i == MIXER    ? start_mixer()
           : i == MUSIC    ? start_music()
           : i == NETSTACK ? net_start()
           : i == DHCP     ? net_dhcp_start()
           : i == DNS      ? net_dns_start()
           : i == LOGD     ? start_logd()
                           : start_shell();
}

void services_closed(unsigned i)
{
    if (i == CONSOLE && cons) {
        jam_handle_close(cons);   /* the shell and serialin see PEER_CLOSED */
        cons = HANDLE_INVALID;
    }
    if (i == DEVMGR && devmgr) {
        jam_handle_close(devmgr);   /* the shell's copies see PEER_CLOSED */
        jam_handle_close(devmgr_q);
        for (unsigned k = 0; k < INIT_MAX_CLAIMED; k++) {
            if (devmgr_hda[k])
                jam_handle_close(devmgr_hda[k]);
            if (devmgr_net[k])
                jam_handle_close(devmgr_net[k]);
        }
        memset(devmgr_hda, 0, sizeof(devmgr_hda));
        memset(devmgr_net, 0, sizeof(devmgr_net));
        devmgr = devmgr_q = HANDLE_INVALID;
        publish(SVC_DEVMGR, HANDLE_INVALID, false);
        publish(SVC_DEVMGR_CTL, HANDLE_INVALID, false);
        mounts_unwatch();       /* its fat services went with its job */
        net_devmgr_gone();
        tell_mounts();
        printf("init: devmgr and its drivers are gone: starting them again\n");
    }
    if (i == LOGD && logd_ctl) {
        jam_handle_close(logd_ctl);
        logd_ctl = HANDLE_INVALID;
        publish(SVC_LOGD, HANDLE_INVALID, false);
    }
    if (i == SHELL && to_shell) {
        jam_handle_close(to_shell);
        to_shell = HANDLE_INVALID;
    }
}

void services_given_up(unsigned i)
{
    net_service_given_up(i);   /* the DHCP client's and the resolver's */
    if (i == NETSTACK) {
        net_given_up();
        publish(SVC_NET, HANDLE_INVALID, false);   /* nobody new gets it */
        tell_mounts();
    }
    for (unsigned k = 0; i == MIXER && k < 2; k++) {
        jam_handle_close(audio_srv[k]);   /* calls waiting for a mixer fail now */
        audio_srv[k] = HANDLE_INVALID;
    }
    if (i == MUSIC && music_srv) {
        jam_handle_close(music_srv);   /* the shell's `music` fails now */
        music_srv = HANDLE_INVALID;
    }
    if (i == MIXER || i == MUSIC) {   /* and nobody new gets them */
        publish(i == MIXER ? SVC_AUDIO : SVC_MUSIC, HANDLE_INVALID, false);
        if (i == MIXER)
            publish(SVC_AUDIOCTL, HANDLE_INVALID, false);
        tell_mounts();
    }
}

void services_init(handle_t loop_port, bool no_usb, bool splash, const char *shell_arg)
{
    root = startup_handle(SR_RESOURCE);
    port = loop_port;
    nousb = no_usb;
    first_arg = shell_arg;
    quiet_console = nolog_console = splash;
    make_audio_channels();
    net_init();
    if (jam_channel_create(&music_cli, &music_srv) != OK)
        music_cli = music_srv = HANDLE_INVALID;
    publish(SVC_AUDIO, audio_cli[0], true);      /* each a channel per opener */
    publish(SVC_AUDIOCTL, audio_cli[1], true);
    publish(SVC_MUSIC, music_cli, true);   /* a channel per opener (svc.connect) */
    publish(SVC_NET, net_svc_channel(), true);
}
