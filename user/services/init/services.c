/* init's shell mode, the services: how each one is started, with which
 * handles, and what init keeps of it (shell.c supervises them all: the
 * order, the restarts, the giving up).
 *
 * Each service runs in a job of its own under init's (and, first, the
 * boot splash: splash.c):
 *   bootfs    bin/bootfs: the boot image as a mount, with the server end of
 *             its `fs` channel (SR_USER + 0); init mounts the client end at
 *             /boot in its own namespace, the one the shell is given
 *   console   bin/console, and the shell: terms.c
 *   splash    bin/splash, once, on a plain boot (argv "splash" from the
 *             kernel): a PROGRAM-level console channel (SR_CONSOLE), a
 *             channel of init's (SR_USER + 0, <splash.h>) and a namespace
 *             with /svc/audio and, with a compositor, /svc/wayland
 *             (shell.c's grants: it plays in a full-screen window that
 *             takes no keys, over the first terminal's). It plays the boot
 *             animation while the rest start; the shell is started only
 *             once it has played (or ended), and it gives the screen back
 *             when the shell says it is ready (initctl.shell_ready)
 *   compositor  bin/compositor, first of all with a compositor: comp.c
 *   serialin  bin/serialin: root with RIGHT_ROOT_SERIAL (serial_open) and an `input`
 *             channel (SR_USER + 0) from compctl.connect_input (the
 *             compositor's: init asks on its own channel), or under
 *             `nocomp` from console.connect_input
 *   devmgr    bin/devmgr: RES_PCI sliced from the root (SR_RESOURCE), the
 *             server ends of its control and query channels
 *             (SR_DEVMGR_CTL, SR_DEVMGR; init keeps a client end of each
 *             and publishes them as /svc/devmgr-ctl and /svc/devmgr), the
 *             server end of the ESP channel (DEVMGR_SR_ESP: the ESP made
 *             writable, for `update -w`; init keeps the client end and
 *             hands it to nobody, update.c's stick write alone uses it) and
 *             what its HID drivers type into (SR_CONSOLE): an INPUT-level
 *             compctl channel of the compositor's, with the argument
 *             "comp" (none while the compositor restarts: it is sent once
 *             one runs, DEVMGR_SET_CONSOLE), or under `nocomp` a copy of
 *             init's (ADMIN) console client end. init waits for its first
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
 *             ends when devmgr does and is started again with the new one.
 *             It also gets its state VMO (SR_STATE) and a keep channel
 *             (SR_KEEP), and is started from a warm spare when one waits
 *             (spare.c)
 *   music     bin/music, the background music player, after the mixer: the
 *             server end of the `music` channel (SR_USER + 0;
 *             abi/idl/music.idl) and a namespace of every mount
 *             read-only and /svc/audio (SR_NS, followed like the
 *             shell's). init makes the `music` channel once, keeps both
 *             ends, as the mixer's, so a restarted player serves the same
 *             channel, and publishes it as /svc/music (a channel per
 *             opener)
 *   serve     bin/serve, the file server, after netstack: the server end
 *             of the `serve` channel (SR_USER + 0; abi/idl/serve.idl), made
 *             once and kept as the music player's, published as /svc/serve
 *             (a channel per opener), and a namespace of /svc/net and
 *             /svc/net-listen only (shell.c's grants: its list's `svc net
 *             listen`). It has no mount: the shell hands it each file
 *   logd      bin/logd, once /data is mounted: root with RIGHT_ROOT_KLOG
 *             (the kernel log), a namespace holding only /data (SR_NS) and the server
 *             end of a `logctl` channel (SR_USER + 2; init keeps the client
 *             end, publishes it as /svc/logd and asks for a flush before
 *             a reboot). It saves each
 *             boot's log as /data/logs/boot-NNNN.txt. On the boot after a
 *             panic it also gets the panicked boot's log and a channel for
 *             its answer (lastboot.c), and saves that log first
 *   netlog    bin/netlog, once /data is mounted, if its settings say so:
 *             net.c says with what
 * Neither serialin nor the console nor the shell gets RIGHT_MAP or
 * RIGHT_SLICE on the root: they can't reach hardware beyond the calls made
 * for them.
 *
 * What a service's end changes here (services_closed): a new console gets
 * a new channel, so the shell (whose channel then closes) exits and comes
 * back connected to it. The input sources (serialin's, the HID drivers')
 * are the compositor's: a new compositor gets a new compctl channel, so
 * serialin (its source closed) exits and comes back with a source of the
 * new one, and devmgr gets an INPUT channel of it (DEVMGR_SET_CONSOLE):
 * its HID drivers, which end when their compositor goes, come back
 * connected to it. Under `nocomp` the console is their hub instead, and
 * the same happens when it restarts. devmgr dying (killed, or a crash)
 * takes its whole job with it: every driver it started (usb-bus, each
 * hid). A new devmgr binds them again from scratch (the kernel's safe
 * rebind: a new dma_cap with Bus Master Enable off until usb-bus has
 * reset the controller; the dead one's DMA pages stay quarantined until
 * then), connected to the compositor (or the console). /svc/devmgr and /svc/devmgr-ctl name
 * the new devmgr's channels, and the followers are told.
 * The mounts that came from the dead devmgr leave the namespace (the
 * shell's and logd's too) until the new one serves them again; logd then
 * carries on in the same file. */
#include <devmgr.h>
#include <idl/console.h>
#include <idl/logctl.h>
#include <logwriters.h>
#include <mixer.h>
#include <os.h>
#include <splash.h>
#include "init.h"

/* The root's powers each service gets (<jam/abi.h> RIGHT_ROOT_*; none can
 * map or slice): serialin reads the serial port; logd the log; the
 * console's and the shell's are terms.c's. Only init keeps
 * RIGHT_ROOT_KEXEC. */

static handle_t root, port;
static handle_t devmgr;     /* devmgr's control channel, client end (0: none running) */
static handle_t devmgr_q;   /* its query channel, client end */
static handle_t devmgr_esp; /* its ESP channel, client end: never handed on (update.c's) */
/* Its device channels for the HD Audio controllers, the mixer's, and for
 * the network cards, held for netstack (0: none). */
static handle_t devmgr_hda[INIT_MAX_CLAIMED], devmgr_net[INIT_MAX_CLAIMED];
static handle_t logd_ctl;   /* logd's control channel, client end (0: no logd) */
/* The mixer's channels, made once: server ends (each mixer gets
 * duplicates) and client ends (the shell gets duplicates). [0] `audio`,
 * [1] `audioctl`; 0: none (no bin/mixer, or given up on). */
static handle_t audio_srv[3], audio_cli[3];   /* [2]: the desktop's (the compositor's) */
/* The music player's channel, made once in the same way (0: none). */
static handle_t music_srv, music_cli;
/* The file server's, the same. */
static handle_t serve_srv, serve_cli;
static bool nousb;

handle_t shell_root(void)
{
    return root;
}

handle_t shell_devmgr(void)
{
    return devmgr;
}

handle_t shell_devmgr_esp(void)
{
    return devmgr_esp;
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

bool services_devmgr_up(void)
{
    return devmgr != HANDLE_INVALID;
}

handle_t services_root_with(rights_t rights)
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

/* Publish h (a duplicate is taken; h stays ours) as /svc/<name> in our
 * namespace, the one the shell and the other followers get; HANDLE_INVALID
 * takes the name away. Followers hear of it with the next tell_mounts. */
void services_publish(const char *name, handle_t h, bool connect)
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

/* The boot splash: a PROGRAM-level console channel (the screen and the
 * keys, as any app), init's channel, and the mixer's `audio` channel. */
static status_t start_splash(void)
{
    handle_t c = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = console_new_client_until(shell_console(), now() + 5 * NS_PER_S, 2, &c);
    if (st == OK && (st = splash_channel(port, KEY_SPLASH, &theirs)) != OK)
        jam_handle_close(c);
    if (st != OK)
        return st;
    struct spawn_handle x[] = { { SR_CONSOLE, c }, { SR_USER + SPLASH_INIT_ROLE, theirs } };
    const char *argv[] = { svcs[SPLASH].path, "--hang", NULL };
    return svc_start(SPLASH, init_splashhang ? 2 : 1, argv, x, 2);   /* no /svc/audio: silent */
}

/* serialin: an input source of the compositor's (comp.c), or of the
 * console's under `nocomp`. It holds the source alone: nothing that makes
 * more of them. */
static status_t start_serialin(void)
{
    handle_t src;
    status_t st = comp_on() ? comp_source(&src)
                            : console_connect_input_until(shell_console(), now() + 5 * NS_PER_S,
                                                          &src);
    if (st != OK)
        return st;
    struct spawn_handle x[] = {
        { SR_RESOURCE, services_root_with(RIGHTS_BASIC | RIGHT_ROOT_SERIAL) }, { SR_USER + 0, src },
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
    handle_t qa = HANDLE_INVALID, qb = HANDLE_INVALID, ea = HANDLE_INVALID, eb = HANDLE_INVALID;
    status_t st = jam_resource_create(root, RES_PCI, 0, 0, &pci);
    if (st == OK)
        st = jam_channel_create(&a, &b);
    if (st == OK)
        st = jam_channel_create(&qa, &qb);
    if (st == OK)
        st = jam_channel_create(&ea, &eb);
    /* What its HID drivers type into: the compositor (an INPUT channel; none
     * while it restarts: comp_restarted sends one), or the console. */
    if (st == OK && comp_on() && comp_input_channel(&c) != OK)
        c = HANDLE_INVALID;
    else if (st == OK && !comp_on())
        st = jam_handle_duplicate(shell_console(), RIGHT_SAME, &c);
    if (st != OK) {
        handle_t left[] = { pci, a, b, qa, qb, ea, eb };
        for (unsigned k = 0; k < 7; k++)
            if (left[k])
                jam_handle_close(left[k]);
        return st;
    }
    const char *argv[10] = { "bin/devmgr" };   /* it and every word below */
    int argc = 1;
    if (nousb)
        argv[argc++] = "nousb";
    if (init_vtdtest)
        argv[argc++] = "vtdtest";   /* drv/hda's deliberate DMA faults */
    if (comp_on())
        argv[argc++] = "comp";      /* SR_CONSOLE is a compctl INPUT channel */
    if (init_nospare)
        argv[argc++] = "nospare";   /* no warm spare fat either */
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
    struct spawn_handle x[6] = { { SR_RESOURCE, pci }, { SR_DEVMGR_CTL, b }, { SR_DEVMGR, qb },
                                 { DEVMGR_SR_ESP, eb } };
    unsigned nx = 4;
    if (c)
        x[nx++] = (struct spawn_handle){ SR_CONSOLE, c };
    /* Without it (`nocomp`) a stick's news is the console's notices alone. */
    x[nx] = (struct spawn_handle){ DEVMGR_SR_NOTIFY, comp_notify_client() };
    if (x[nx].h)
        nx++;
    st = svc_start(DEVMGR, argc, argv, x, nx);   /* consumes them all */
    if (st != OK) {
        jam_handle_close(a);
        jam_handle_close(qa);
        jam_handle_close(ea);
        return st;
    }
    devmgr = a;
    devmgr_q = qa;
    devmgr_esp = ea;
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
    services_publish(SVC_DEVMGR, devmgr_q, true);   /* a channel per opener (svc.connect) */
    services_publish(SVC_DEVMGR_CTL, devmgr, false);
    tell_mounts();   /* a restart: the shell's /svc/devmgr is the dead one's */
    handle_t watch;
    st = jam_handle_duplicate(devmgr, RIGHT_SAME, &watch);
    if (st == OK)
        st = mounts_watch(watch, port, KEY_MOUNTS);
    if (st != OK)
        printf("init: not following devmgr's mounts (%s)\n", status_str(st));
    return OK;
}

void services_devmgr_input(handle_t c)
{
    if (!devmgr) {
        jam_handle_close(c);
        return;
    }
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
        printf("init: devmgr didn't take the new input channel (%s)\n",
               status_str(st != OK ? st : r.status));
}

void comp_restarted(void)
{
    handle_t c;
    if (devmgr && comp_input_channel(&c) == OK)
        services_devmgr_input(c);   /* its HID drivers, which ended with the old one, come back */
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
    struct spawn_handle x[4] = { { SR_RESOURCE, services_root_with(RIGHTS_BASIC | RIGHT_ROOT_KLOG) },
                                 { SR_USER + 2, theirs } };
    unsigned nx = 2 + lastboot_logd_handles(&x[2]);   /* after a panic: its log first */
    st = svc_start1(LOGD, x, nx);
    if (st != OK) {
        jam_handle_close(mine);
        return st;
    }
    logd_ctl = mine;
    services_publish(SVC_LOGD, logd_ctl, true);   /* a channel per opener (svc.connect) */
    return OK;
}

status_t shell_stop_devmgr(uint64_t deadline)
{
    /* The sound's clients first: the mixer holds channels to the hda
     * driver, which ends only once every client has gone, so devmgr would
     * wait its whole STOP_WAIT for it, twice (the PC's 30 s reboot). The
     * keeper holds duplicates of them too (the mixer's driver channel,
     * stream channel and ring, so they outlive a restart): those go with
     * it (kept_given_up), or the driver never sees its client go. They are
     * stopped for good: the machine is about to restart. Not the console:
     * when it ends the kernel takes the screen back and redraws its log,
     * which flashed text over the blank screen. */
    static const int clients[] = { MUSIC, SPLASH, MIXER };
    for (unsigned i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
        struct svc *c = &svcs[clients[i]];
        c->given_up = true;
        if (c->running) {
            signals_t seen;
            jam_job_kill(c->job);
            (void)jam_object_wait_one(c->proc, SIG_TERMINATED, deadline, &seen);
        }
        kept_given_up(clients[i]);   /* the spare and what the keeper held (the mixer's) */
    }
    struct svc *s = &svcs[DEVMGR];
    if (!devmgr || !s->running || s->given_up)
        return OK;   /* none, or stopped already (a kexec that failed after it) */
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

/* The mixer's three channels, once (all, or none). */
static void make_audio_channels(void)
{
    for (unsigned k = 0; k < 3; k++)
        if (jam_channel_create(&audio_cli[k], &audio_srv[k]) != OK)
            audio_cli[k] = audio_srv[k] = HANDLE_INVALID;
    if (audio_cli[0] && audio_cli[1] && audio_cli[2])
        return;
    for (unsigned k = 0; k < 3; k++) {
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
    struct spawn_handle x[3 + INIT_MAX_CLAIMED + KEPT_EXTRA] = {
        { SR_AUDIO, dup_of(audio_srv[0]) }, { SR_AUDIO_CTL, dup_of(audio_srv[1]) },
        { SR_AUDIO_DESK, dup_of(audio_srv[2]) },
    };
    unsigned n = 3;
    for (unsigned k = 0; k < INIT_MAX_CLAIMED; k++)
        if (devmgr_hda[k] && (x[n].h = dup_of(devmgr_hda[k])) != HANDLE_INVALID)
            x[n++].role = SR_DEVMGR_DEVICE;   /* none: it answers "no output" */
    if (!x[0].h || !x[1].h || !x[2].h) {
        for (unsigned k = 0; k < n; k++)
            if (x[k].h)
                jam_handle_close(x[k].h);
        return ERR_NO_RESOURCES;
    }
    return kept_start(MIXER, x, n);   /* + its state and keep channel; a spare if one waits */
}

handle_t services_audioctl(void)
{
    return audio_cli[1];
}

handle_t services_audio_desk(void)
{
    return audio_cli[2] ? dup_of(audio_cli[2]) : HANDLE_INVALID;
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

/* The file server: its server end again (the same channel as any server
 * before it). Its namespace (the network, with the listen permission) is
 * shell.c's grants. */
static status_t start_serve(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (!serve_srv || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, svcs[SERVE].path, &data, &size) != OK) {
        printf("init: no %s (or no channel for it): no file server\n", svcs[SERVE].path);
        svcs[SERVE].given_up = true;
        return OK;
    }
    struct spawn_handle x[] = { { SR_USER + 0, dup_of(serve_srv) } };
    if (!x[0].h)
        return ERR_NO_RESOURCES;
    return svc_start1(SERVE, x, 1);
}

status_t services_start(unsigned i)
{
    if (term_of(i) >= 0)
        return terms_start(i);
    return i == BOOTFS       ? start_bootfs()
           : i == COMPOSITOR ? comp_start()
           : i == SPLASH     ? start_splash()
           : i == SERIALIN   ? start_serialin()
           : i == DEVMGR     ? start_devmgr()
           : i == MIXER      ? start_mixer()
           : i == MUSIC      ? start_music()
           : i == NETSTACK   ? net_start()
           : i == DHCP       ? net_dhcp_start()
           : i == DNS        ? net_dns_start()
           : i == LOGD       ? start_logd()
           : i == NETLOG     ? net_netlog_start()
           : i == SNTP       ? net_sntp_start()
                             : start_serve();
}

void services_closed(unsigned i)
{
    terms_closed(i);
    if (i == COMPOSITOR)
        comp_closed();
    if (i == DEVMGR && devmgr) {
        jam_handle_close(devmgr);   /* the shell's copies see PEER_CLOSED */
        jam_handle_close(devmgr_q);
        jam_handle_close(devmgr_esp);
        for (unsigned k = 0; k < INIT_MAX_CLAIMED; k++) {
            if (devmgr_hda[k])
                jam_handle_close(devmgr_hda[k]);
            if (devmgr_net[k])
                jam_handle_close(devmgr_net[k]);
        }
        memset(devmgr_hda, 0, sizeof(devmgr_hda));
        memset(devmgr_net, 0, sizeof(devmgr_net));
        devmgr = devmgr_q = devmgr_esp = HANDLE_INVALID;
        services_publish(SVC_DEVMGR, HANDLE_INVALID, false);
        services_publish(SVC_DEVMGR_CTL, HANDLE_INVALID, false);
        mounts_unwatch();       /* its fat services went with its job */
        net_devmgr_gone();
        tell_mounts();
        printf("init: devmgr and its drivers are gone: starting them again\n");
    }
    if (i == LOGD && logd_ctl) {
        jam_handle_close(logd_ctl);
        logd_ctl = HANDLE_INVALID;
        services_publish(SVC_LOGD, HANDLE_INVALID, false);
    }
}

void services_given_up(unsigned i)
{
    terms_given_up(i);   /* its terminal closes (with a compositor) */
    net_service_given_up(i);   /* the DHCP client's and the resolver's */
    if (i == NETSTACK) {
        net_given_up();
        services_publish(SVC_NET, HANDLE_INVALID, false);   /* nobody new gets them */
        services_publish(SVC_NET_LISTEN, HANDLE_INVALID, false);
        services_publish(SVC_NET_LISTEN_LOW, HANDLE_INVALID, false);
        services_publish(SVC_NET_SYS, HANDLE_INVALID, false);
        tell_mounts();
    }
    for (unsigned k = 0; i == MIXER && k < 3; k++) {
        jam_handle_close(audio_srv[k]);   /* calls waiting for a mixer fail now */
        audio_srv[k] = HANDLE_INVALID;
    }
    kept_given_up(i);   /* the mixer's spare, what it kept, its state */
    if (i == MUSIC && music_srv) {
        jam_handle_close(music_srv);   /* the shell's `music` fails now */
        music_srv = HANDLE_INVALID;
    }
    if (i == SERVE && serve_srv) {
        jam_handle_close(serve_srv);   /* the shell's `serve` fails now */
        serve_srv = HANDLE_INVALID;
        services_publish(SVC_SERVE, HANDLE_INVALID, false);
        tell_mounts();
    }
    if (i == MIXER || i == MUSIC) {   /* and nobody new gets them */
        services_publish(i == MIXER ? SVC_AUDIO : SVC_MUSIC, HANDLE_INVALID, false);
        if (i == MIXER)
            services_publish(SVC_AUDIOCTL, HANDLE_INVALID, false);
        tell_mounts();
    }
}

void services_init(handle_t loop_port, bool no_usb, bool splash, const char *shell_arg)
{
    root = startup_handle(SR_RESOURCE);
    port = loop_port;
    nousb = no_usb;
    terms_init(port, splash, shell_arg);
    apps_init(port);
    comp_init(port, !init_nocomp);
    make_audio_channels();
    net_init(port);
    if (jam_channel_create(&music_cli, &music_srv) != OK)
        music_cli = music_srv = HANDLE_INVALID;
    services_publish(SVC_AUDIO, audio_cli[0], true);      /* each a channel per opener */
    services_publish(SVC_AUDIOCTL, audio_cli[1], true);
    services_publish(SVC_MUSIC, music_cli, true);   /* a channel per opener (svc.connect) */
    if (jam_channel_create(&serve_cli, &serve_srv) != OK)
        serve_cli = serve_srv = HANDLE_INVALID;
    services_publish(SVC_SERVE, serve_cli, true);
    services_publish(SVC_NET, net_svc_channel(), true);
    services_publish(SVC_NET_LISTEN, net_listen_channel(), true);   /* the shell's to give */
    services_publish(SVC_NET_LISTEN_LOW, net_listen_low_channel(), true);   /* ... to bin/serve */
    services_publish(SVC_NET_SYS, net_sys_channel(), true);   /* the network's services' */
}
