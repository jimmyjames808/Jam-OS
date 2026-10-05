/* init: what its files share. main.c starts init (the init.cfg programs,
 * keytest); shell.c is the shell mode, where init starts and supervises
 * the bootfs server, the console, serialin, devmgr, the mixer, the music
 * player, netstack, logd, netlog, sntp and the shell, each started as services.c says; splash.c
 * the boot splash that plays first in shell mode; lastboot.c the boot
 * before this one, if it panicked (its log saved by logd, one line for the
 * shell); reboot.c a reboot by kexec; mounts.c keeps init's namespace in
 * step with devmgr's mounts; ctl.c serves init's control channels
 * (abi/idl/initctl.idl); settings.c the clock and the volumes from
 * /data/etc/settings; net.c the network services (netstack, dhcp, dns, netlog, sntp); update.c
 * checks a fetched build and makes it the stored kernel (<update.h>).
 *
 * The namespace: init's own (libos's, <os.h> "files") is the one every
 * program it starts is given. /boot is the bootfs server's channel, which
 * main.c or shell.c mounts; the rest come from devmgr (mounts.c). */
#pragma once

#include <os.h>
#include <update.h>

#define BOOT_MOUNT "/boot"
#define DATA_MOUNT "/data"
#define USB_MOUNT  "/usb"    /* another stick's mounts: /usb0, /usb1, ... */
#define USB_MOUNTS 8         /* ... the N that are tried */
#define BOOTFS_PATH "bin/bootfs"   /* the bootfs server, in bootfs */

/* One line into the kernel's RESULTS box (and the log). */
void init_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Shell mode (shell.c). Runs for as long as the system does: returns
 * (false) only when init's own port fails. nousb: devmgr leaves the USB
 * controllers alone (the safe mode boot entry); splash: the boot splash
 * plays first (splash.c). */
bool init_shell(bool nousb, bool splash, const char *shell_arg);
/* The option word "hidboot" (main.c): devmgr is started with it, so every
 * hid keeps its mouse in the boot protocol. */
extern bool init_hidboot;
/* The option word "netprobe" (main.c): shell mode's devmgr is started
 * with it, so it binds the RTL8125's listen-only probe (drv/rtl8125). */
extern bool init_netprobe;
/* The option word "netsend" (main.c): the same for the RTL8125's ARP send
 * test (devmgr gets it only without "netprobe"). */
extern bool init_netsend;
/* The option word "net" (main.c): the same for the RTL8125's netdev
 * service (devmgr gets it only without "netprobe" and "netsend"). */
extern bool init_net;
/* The option word "bootdisk=0x<id>" (main.c; NULL: none): the MBR disk id
 * the machine booted from, passed on to devmgr as it is. */
extern const char *init_bootdisk;
/* The option word "vlan=<id>" or "vlan=none" (main.c; NULL: none, the
 * network stays off): the network's mode, passed on to devmgr as it is,
 * which passes it to every network driver. Nobody else is told it: netstack hears it from
 * the driver (netdev.info). */
extern const char *init_vlan;
/* The option word "splashhang" (main.c, a test's): bin/splash is started
 * with --hang, so it never finishes (the shell's deadline is tested). */
extern bool init_splashhang;
/* The option word "vtdtest" (main.c; with "iommu=on", the IOMMU checks
 * test entry): devmgr is started with it, so it passes it to drv/hda,
 * which runs its deliberate DMA faults before serving (M11 stage 5). */
extern bool init_vtdtest;


/* ---- mounts.c -------------------------------------------------------------------- */

/* Follow devmgr's mounts (DEVMGR_MOUNTS, <devmgr.h>) on a thread of its
 * own: each answer's mounts go into init's namespace, those no longer
 * listed leave it. devmgr_ctl: a duplicate of devmgr's control channel,
 * consumed. After every change a PORT_PACKET_USER with `key` is queued on
 * port (0: nobody is told). One devmgr at a time: mounts_unwatch first. */
status_t mounts_watch(handle_t devmgr_ctl, handle_t port, uint64_t key);
/* Stop following (devmgr is gone, or about to be told to stop: close our
 * other ends of its control channel first) and unmount what came from it.
 * Waits for the thread, at most 2 s. */
void     mounts_unwatch(void);
/* Everything written to /data is on the stick: fs.sync, given up after
 * 2 s (said in the log). Nothing to do without a /data. */
void     mounts_sync(void);
/* /data once more, without a word, for at most 1 s: after logd's last
 * write, so the volume is left marked clean. */
void     mounts_settle(void);

/* ---- ctl.c ----------------------------------------------------------------------- */

/* Who holds a control channel: the shell may ask everything, each
 * terminal's console (CTL_CONSOLE + its index) only reboot and terminal. */
#define TERM_MAX 8   /* terminals at most, the first included (terms.c) */
enum { CTL_SHELL, CTL_CONSOLE, CTL_COUNT = CTL_CONSOLE + TERM_MAX };

/* A new control channel for holder `who`, replacing its old one (whose
 * client ends see ERR_PEER_CLOSED). *client: the end to hand over. Its
 * requests arrive as packets with `key` on port: call ctl_serve then. */
status_t ctl_new(unsigned who, handle_t port, uint64_t key, handle_t *client);
/* Answer what is queued on who's channel. */
void     ctl_serve(unsigned who);

/* ---- splash.c: the boot splash (<splash.h>) --------------------------------------- */

/* This boot plays the splash: the shell waits for it (splash_played). */
void     splash_expect(void);
/* The shell may start: no splash on this boot, or it has played, or ended. */
bool     splash_played(void);
/* When the shell stops waiting for it (uptime ns; DEADLINE_NEVER once it
 * has played). */
uint64_t splash_deadline(void);
/* That deadline has passed: say so in the log, and the shell may start
 * (the caller kills a splash still running, so the console gets the
 * screen back). */
void     splash_overdue(void);
/* The splash's channel: ours bound on port with key (call splash_event on
 * its packets), *theirs to hand over as its SR_USER + SPLASH_INIT_ROLE. */
status_t splash_channel(handle_t port, uint64_t key, handle_t *theirs);
/* A packet on the channel: SPLASH_PLAYED, or the splash closed it. */
void     splash_event(void);
/* The splash process ended (it is never started again). */
void     splash_ended(void);
/* initctl.shell_ready: SPLASH_GO to a splash still holding the screen. */
void     splash_shell_ready(void);

/* ---- shell.c and services.c: shell mode's services --------------------------------- */

/* The services, in the order they are started; then the other terminals'
 * consoles and shells (terms.c), a pair each, started only when one is
 * opened. */
enum { BOOTFS, COMPOSITOR, CONSOLE, SPLASH, SERIALIN, DEVMGR, MIXER, MUSIC, NETSTACK, DHCP, DNS,
       LOGD, NETLOG, SNTP, SERVE, SHELL, TERMS, NSVC = TERMS + 2 * (TERM_MAX - 1) };
/* Terminal k's (0: the first, the system's) console and shell. */
#define TERM_CONSOLE(k) ((k) ? TERMS + 2 * ((k) - 1) : CONSOLE)
#define TERM_SHELL(k)   ((k) ? TERMS + 2 * ((k) - 1) + 1 : SHELL)

/* Port keys of shell mode's loop: a service's index (its process ended),
 * or one of these. */
#define KEY_MOUNTS   0x100u   /* the mounts watcher changed the namespace */
#define KEY_CTL      0x200u   /* + CTL_*: requests on a control channel */
#define KEY_SPLASH   0x300u   /* the splash's channel (splash.c) */
#define KEY_LASTBOOT 0x400u   /* logd's answer about the last boot's log (lastboot.c) */
#define KEY_UPDATE   0x500u   /* an update's offer channel (update.c) */
#define KEY_NETCTL   0x600u   /* netstack's answers to init's netctl calls (net.c) */
#define KEY_SPARE    0x700u   /* the warm spare's process ended (spare.c) */
#define KEY_KEEP     0x800u   /* the kept service wrote to its keeper (spare.c) */

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
    uint64_t    kill_at;       /* a deliberate kill (initctl.kill), uptime ns, until the next
                                * start; 0: none */
    uint64_t    ended_at;      /* when its last end was seen (uptime ns; 0: never) */
};

/* Every service's state (shell.c's; services.c reads the paths and marks
 * one given up when its program isn't in bootfs). */
extern struct svc svcs[NSVC];

/* How svc_start_args starts a service. */
struct svc_args {
    int                        argc;     /* entries in argv */
    const char *const         *argv;     /* argv[0]: its path */
    struct spawn_handle       *x;        /* extra handles, consumed */
    const rights_t            *rights;   /* NULL, or nx entries: what each arrives with */
    unsigned                   nx;       /* entries in x */
};

/* shell.c: start svc i as a new process with a's arguments and extra
 * handles (consumed, whatever happens); a service that follows init's
 * namespace gets its part of it. */
status_t svc_start_args(unsigned i, const struct svc_args *a);
/* The same with these arguments and handles, each as it is. */
status_t svc_start(unsigned i, int argc, const char *const *argv, struct spawn_handle *x,
                   unsigned nx);
/* The same with argv = { its path }. */
status_t svc_start1(unsigned i, struct spawn_handle *x, unsigned nx);
/* Svc i runs as proc, in job (a promoted spare: spare.c), both consumed:
 * supervised from now on like one svc_start_args started.
 * ERR_NOT_SUPPORTED (and proc killed): svc i follows init's namespace,
 * which only a spawn hands over. */
status_t svc_adopt(unsigned i, handle_t proc, handle_t job);
/* writers.c: the table of log writers the console trusts (<logwriters.h>):
 * made once, before the first console; a started devmgr's or logd's koid
 * goes in (any other i is ignored); a read-only handle to it for a new
 * console (HANDLE_INVALID if there is no table). */
void     writers_init(void);
void     writers_started(unsigned i, handle_t proc);
handle_t writers_for_console(void);
/* shell.c: every service that follows init's namespace gets it as it is now. */
void     tell_mounts(void);
/* services.c: a duplicate of the root resource with these rights only
 * (HANDLE_INVALID if that fails). */
handle_t services_root_with(rights_t rights);
/* services.c: publish h (a duplicate is taken; h stays the caller's) as
 * /svc/<name> in init's namespace, the one the shell and the other
 * followers get, a channel per opener if connect; HANDLE_INVALID takes
 * the name away. Followers hear of it with the next tell_mounts. */
void     services_publish(const char *name, handle_t h, bool connect);

/* ---- terms.c: the terminals: a console and a shell each --------------------------- */

/* Set up once before the loop: the loop's port, whether the splash plays
 * first (the first console starts quiet, every console with "nolog"),
 * the first shell's argument; the other terminals' services, closed. */
void     terms_init(handle_t port, bool splash, const char *shell_arg);
/* Which terminal service i belongs to (0: the first), -1 for none. */
int      term_of(unsigned i);
/* Start service i, a terminal's console or shell (services_start's). */
status_t terms_start(unsigned i);
/* The console of service i's terminal runs (its shell may start). */
bool     terms_console_up(unsigned i);
/* A terminal other than the first is open (or closing). */
bool     terms_extra_open(void);
/* Service i ended: the console's or the shell's ends that init kept go. */
void     terms_closed(unsigned i);
/* Service i ended (after terms_closed), killed or with code: true if its
 * terminal closes (an extra one's window closed, or its shell's `exit`)
 * or is closing: it is not started again. */
bool     terms_ended(unsigned i, bool killed, int64_t code);
/* Service i is given up on: an extra terminal closes. */
void     terms_given_up(unsigned i);
/* initctl.terminal: open another terminal; *number: its number (2 and
 * up). ERR_NOT_SUPPORTED: no compositor; ERR_NO_RESOURCES: TERM_MAX are
 * open. Its console and shell start at the loop's next turn. */
status_t terms_open(uint8_t *number);
/* "console-<n>", "shell-<n>" (n 2 to TERM_MAX): terminal n's service i. */
bool     terms_named(const char *name, unsigned *i);

/* ---- comp.c: the compositor (the boot word `comp`) ---------------------------------- */

/* Set up once before the loop: on (the word given): /svc/wayland's
 * channel made; off: the compositor is not started this boot. */
void     comp_init(bool on);
/* A compositor draws the screen this boot. */
bool     comp_on(void);
/* Start it (services_start's, for COMPOSITOR). */
status_t comp_start(void);
/* A duplicate of /svc/wayland's client end for a console, HANDLE_INVALID
 * without a compositor. */
handle_t comp_wayland(void);
/* The boot word `comp` (main.c). */
extern bool init_comp;

/* services.c: what shell mode's services share, set up once before the
 * loop: the loop's port, the safe mode word, whether the splash plays
 * first (the console starts quiet), the first shell's argument. */
void     services_init(handle_t port, bool nousb, bool splash, const char *shell_arg);
/* Start service i (one of the enum above); OK, or why not (backed off). */
status_t services_start(unsigned i);
/* Service i ended: drop what init kept of it (its client ends). */
void     services_closed(unsigned i);
/* Service i is given up on: calls waiting for it fail now. */
void     services_given_up(unsigned i);
/* A console runs (its clients may start); a devmgr runs. */
bool     services_console_up(void);
/* What /data/etc/settings says for svc i (settings.c), if it runs: the
 * mixer's master volume, the music player's volume. */
void     services_settings(unsigned i);
bool     services_devmgr_up(void);
/* The mixer's shared audioctl channel, init's client end (0: none): the
 * one /svc/audioctl's openers connect through. */
handle_t services_audioctl(void);
/* devmgr's device channels (<devmgr.h> DEVMGR_DEVICE_CHANNEL, asked on its
 * control channel devmgr_ctl) for every PCI function of class `cls` that
 * has a driver, at most `max`, into out[]: init gives them to the class's
 * user (the sound cards to the mixer), and while they exist the query
 * channel hands none of them out. Returns how many. Used by both modes
 * (main.c too). */
#define INIT_MAX_CLAIMED 4u
unsigned services_claim_class(handle_t devmgr_ctl, uint32_t cls, handle_t *out, unsigned max);
/* Duplicates of the network cards' device channels (class 02 00 00,
 * DEVMGR_CLASS_NET), which init claims before it publishes /svc/devmgr
 * and holds for netstack, the only program that gets them; at most max,
 * into out[] (the caller's). Returns how many (0: no devmgr running, or no
 * network card with a driver). */
unsigned services_net_devices(handle_t *out, unsigned max);

/* ---- shell.c and services.c, for ctl.c and reboot.c -------------------------------- */

/* The root resource (reboot, proc_list). */
handle_t shell_root(void);
/* devmgr's control channel, or 0 while none runs. */
handle_t shell_devmgr(void);
/* devmgr's ESP channel (DEVMGR_ESP_WRITE), init's alone: or 0 while no
 * devmgr runs (or none started in shell mode). Only update.c uses it. */
handle_t shell_devmgr_esp(void);
/* init's (ADMIN) console channel, or 0 while no console runs. */
handle_t shell_console(void);
/* logd writes out and syncs the log up to now (logctl.flush), waited for
 * until deadline at most. Nothing to do without a logd. */
void     shell_flush_log(uint64_t deadline);
/* For a kexec reboot (reboot.c): devmgr stops every driver in order and
 * exits (DEVMGR_SHUTDOWN), waited for until deadline; if it doesn't, its
 * job is killed. It is not started again. OK, or what went wrong. */
status_t shell_stop_devmgr(uint64_t deadline);

/* ---- settings.c: /data/etc/settings (<settings.h>) ------------------------------- */

/* Set the kernel's clock from the real-time clock, as the settings say it
 * keeps time (the defaults without /data), with their time zone. */
void     settings_clock(void);
/* The settings' volumes to the mixer's master (audioctl) and to the music
 * player (music), if there are any; nothing waits long. */
void     settings_master(handle_t audioctl);
void     settings_music(handle_t music);
/* /data has no settings file: write one with the defaults, commented. */
void     settings_first_file(void);

/* ---- spare.c: a service that outlives its process (the mixer) ------------------- */

/* Handles kept_start adds to a start's: the state VMO and the keep channel. */
#define KEPT_EXTRA 2u

/* Set up once before the loop: the loop's port, and whether a warm spare
 * is kept ready (false: the boot word `nospare`). */
void     spare_init(handle_t port, bool spares);
/* Svc i outlives its process (a keeper, a state VMO, a spare: the mixer):
 * a deliberate kill of it neither counts nor waits, and its first crash in
 * a minute is restarted at once (shell.c). */
bool     spare_kept(unsigned i);
/* Start kept svc i with handles x[0..nx) (consumed; x has room for
 * KEPT_EXTRA more): its state VMO (SR_STATE) and a new keep channel
 * (SR_KEEP) are added, the spare is promoted if one waits (else a process
 * is started), then the keeper hands over what it kept. Says in the log
 * how long a restart took. Errors as svc_start_args's. */
status_t kept_start(unsigned i, struct spawn_handle *x, unsigned nx);
/* Start the spare if one is due: the next time one is, or DEADLINE_NEVER. */
uint64_t spare_due(uint64_t t);
/* KEY_SPARE: the spare's process ended (or an old packet of one). */
void     spare_event(void);
/* KEY_KEEP: what the kept service wrote to its keeper. */
void     kept_event(void);
/* Svc i is given up on: its spare is dismissed, what its keeper held is
 * closed (its clients see ERR_PEER_CLOSED) and its state VMO dropped. */
void     kept_given_up(unsigned i);
/* The boot word `nospare` (main.c): no warm spares. */
extern bool init_nospare;

/* ---- net.c: the network services ------------------------------------------------ */

/* netstack's control channel and /svc's network channels, made once
 * before the loop; port: the loop's (netctl's answers come to it). */
void     net_init(handle_t port);
/* Start netstack (shell.c's NETSTACK) with the control channel's server
 * end and the network cards' device channels. */
status_t net_start(void);
/* /data/etc/settings' net.address to a running netstack: sent without
 * waiting (a running DHCP client is stopped first; the address goes once
 * its end has come, net_ended). */
void     net_settings(void);
/* KEY_NETCTL: netstack answered (the next call goes, or a failure is said). */
void     net_netctl_event(void);
/* At t: a netctl answer overdue is said and given up on. The next time
 * one is due, or DEADLINE_NEVER. */
uint64_t net_due(uint64_t t);
/* Service i's end came to the loop (after services_closed's own work). */
void     net_ended(unsigned i);
/* devmgr ended: netstack, whose device channels were its, starts again. */
void     net_devmgr_gone(void);
/* netstack is given up on: calls waiting for it fail now. */
void     net_given_up(void);
/* /svc/net's shared channel, client end (init's: services.c publishes a
 * duplicate), or 0. */
handle_t net_svc_channel(void);
/* /svc/net-listen's, the same: its openers may listen (take a fixed port
 * below NET_PORT_EPHEMERAL). */
handle_t net_listen_channel(void);
/* /svc/net-low's, the same: its openers may listen on ports below
 * 1024 too (the shell gives it only to a program whose list says `svc net
 * listen low`). */
handle_t net_listen_low_channel(void);
/* /svc/net-sys's client end (published: init's own network services and
 * bin/update reach netstack's reserve through it), or 0. */
handle_t net_sys_channel(void);
/* Start netlog (shell.c's NETLOG, once /data is mounted) if the settings
 * name a Mac (`net.host`) and don't say `netlog = off`; otherwise it is
 * marked given up for this boot, said once. */
status_t net_netlog_start(void);
/* Start the DHCP client (shell.c's DHCP) with a client end of netctl,
 * unless the settings have a static net.address (then it is given up on,
 * said in the log). */
status_t net_dhcp_start(void);
/* When the DHCP client may start, asked at t: 0 (now), or the time it may
 * (/data's settings may still come and say the address is static). data:
 * /data is mounted. */
uint64_t net_dhcp_wait(uint64_t t, bool data);
/* Start the resolver (shell.c's DNS) with /svc/dns's server end. */
status_t net_dns_start(void);
/* Start bin/sntp (shell.c's SNTP, once /data is mounted) with the root's
 * RIGHT_ROOT_CLOCK and `ntp.server`, unless the settings say `ntp = off`
 * (then it is given up on for this boot, said once). */
status_t net_sntp_start(void);
/* Service i (DHCP or DNS; any other is ignored) is given up on. */
void     net_service_given_up(unsigned i);

/* ---- reboot.c -------------------------------------------------------------------- */

/* /esp is mounted (now or again): the first time, note the size and
 * modification time of its kernel and boot image, as the files the stored
 * kernel stands for (usually it was loaded from them; after an update it
 * is the fetched build, which this boot can't tell apart). */
void     reboot_note_esp(void);
/* Reboot by kexec: into the stored kernel, or, if /esp's kernel or boot
 * image changed since reboot_note_esp, into the files on /esp. Returns
 * only if that failed (said in the log); the caller resets through the
 * firmware. */
status_t init_reboot_kexec(void);
/* Reset through the firmware (`reboot -f`, and when kexec can't be done):
 * the same steps as init_reboot_kexec's before its jump (/data synced,
 * the log flushed, devmgr's drivers stopped in order), then the reboot
 * system call. Returns only if that failed (said in the log). */
status_t init_reboot_firmware(void);
/* initctl.kernel_load: /esp's kernel and boot image read and made the
 * stored copy now (and noted as such); their sizes and the read's time.
 * ERR_NOT_FOUND without them; kexec_load's errors (the old copy stays). */
status_t init_kernel_load(uint64_t *kernel_bytes, uint64_t *bootfs_bytes, uint32_t *read_ms);
/* The stored copy came from somewhere else (an update): /esp's kernel and
 * boot image as they are now are noted as its, so the next reboot keeps
 * it and reads nothing unless the stick changes. Without /esp nothing is
 * noted now (its first mount notes it, reboot_note_esp). */
void     reboot_keep_stored(void);
/* The same, with the stick's files as `update -w` saw them last (sizes and
 * modification times; espwrite.c: /esp itself is no mount at that
 * moment). */
void     reboot_keep_written(const uint64_t size[UPDATE_FILES], const uint64_t mtime[UPDATE_FILES]);

/* ---- update.c: a fetched build checked and made the stored kernel (<update.h>) ----- */

/* initctl.update_offer: a new offer channel (replacing an older one, whose
 * sender sees ERR_PEER_CLOSED); ours bound on port with key (call
 * update_event on its packets), *client to hand over. ERR_BAD_STATE while
 * the last offer is still being checked. */
status_t update_offer_new(handle_t port, uint64_t key, handle_t *client);
/* A packet with the offer channel's key: the offer (its copy and hash
 * handed to a worker thread), the worker done (then loaded if it passed,
 * and answered), or the sender gone. After the answer the channel is
 * closed. */
void     update_event(void);

/* ---- espwrite.c: `update -w`'s stick write, on update.c's worker thread ----------- */

/* One stick write: what to write, and how far it got. */
struct esp_write {
    /* Given. */
    handle_t       esp;                        /* devmgr's ESP channel (the caller's) */
    handle_t       vmo[UPDATE_FILES];          /* init's checked copies of the new build */
    uint64_t       size[UPDATE_FILES];         /* their sizes in bytes */
    const uint8_t *sha256[UPDATE_FILES];       /* the signed manifest's SHA-256s */
    uint32_t       fail_at;                    /* a test's: fail at this step, once (NONE: no) */
    uint32_t       stop_at;                    /* a test's: stop after this swap change (0: no) */
    /* Answered. */
    uint32_t       step;                       /* enum update_write_step: DONE, or the failed one */
    status_t       st;                         /* OK, or why it failed */
    uint32_t       stick;                      /* enum update_stick: what the stick boots now */
    uint32_t       write_ms;                   /* how long it took */
    bool           noted;                      /* the stick's files, as they are now: */
    uint64_t       file_size[UPDATE_FILES];    /* ... their sizes */
    uint64_t       mtime[UPDATE_FILES];        /* ... and modification times */
};
/* Write the build in j to the stick's ESP (the file's header has the
 * steps and what each failure leaves); blocks for as long as that takes
 * (seconds: never call it from init's loop). */
void esp_write_build(struct esp_write *j);

/* ---- lastboot.c: the boot before this one, if it panicked -------------------------- */

/* At the start of shell mode: take its log (SR_CRASHLOG) if the kernel
 * gave one; results arrive on port with key (call lastboot_event then). */
void     lastboot_init(handle_t port, uint64_t key);
/* For logd's start: the extra handles it gets (the log, and the channel
 * for its answer), into x; how many (0: nothing to save, or done). */
unsigned lastboot_logd_handles(struct spawn_handle *x);
/* logd's answer arrived (or its end closed). */
void     lastboot_event(void);
/* The shell waits for the result until this time (uptime ns), or
 * DEADLINE_NEVER: the shell may start (nothing to wait for). Past
 * the deadline it gives up waiting and says the log was not saved. */
uint64_t lastboot_wait_until(uint64_t t);
/* The banner line for the boot's first shell ("" if this boot did not
 * follow a panic); after the first call, always "". */
const char *lastboot_banner(void);

/* Kill the service init runs under this name ("console", ...): its whole
 * job; init's loop then starts it again. *koid: its process's id.
 * ERR_NOT_FOUND: not a service of init's, or not running. */
status_t shell_kill_service(const char *name, uint64_t *koid);
