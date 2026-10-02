/* init: what its files share. main.c starts init (the init.cfg programs,
 * keytest); shell.c is the shell mode, where init starts and supervises
 * the bootfs server, the console, serialin, devmgr, the mixer, the music
 * player, logd and the shell, each started as services.c says; splash.c
 * the boot splash that plays first in shell mode; lastboot.c the boot
 * before this one, if it panicked (its log saved by logd, one line for the
 * shell); reboot.c a reboot by kexec; mounts.c keeps init's namespace in
 * step with devmgr's mounts; ctl.c serves init's control channels
 * (abi/idl/initctl.idl); settings.c the clock and the volumes from
 * /data/etc/settings; update.c checks a fetched build and makes it the
 * stored kernel (<update.h>).
 *
 * The namespace: init's own (libos's, <os.h> "files") is the one every
 * program it starts is given. /boot is the bootfs server's channel, which
 * main.c or shell.c mounts; the rest come from devmgr (mounts.c). */
#pragma once

#include <os.h>

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
/* The option word "bootdisk=0x<id>" (main.c; NULL: none): the MBR disk id
 * the machine booted from, passed on to devmgr as it is. */
extern const char *init_bootdisk;
/* The option word "vlan=<id>" (main.c; NULL: none, the network stays
 * off): the network's VLAN, passed on to devmgr as it is, which passes it
 * to every network driver. Nobody else is told it: netstack hears it from
 * the driver (netdev.info). */
extern const char *init_vlan;
/* The option word "splashhang" (main.c, a test's): bin/splash is started
 * with --hang, so it never finishes (the shell's deadline is tested). */
extern bool init_splashhang;


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

/* Who holds a control channel: the shell may ask everything, the console
 * only reboot. */
enum { CTL_SHELL, CTL_CONSOLE, CTL_COUNT };

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

/* The services, in the order they are started. */
enum { BOOTFS, CONSOLE, SPLASH, SERIALIN, DEVMGR, MIXER, MUSIC, LOGD, SHELL, NSVC };

/* Port keys of shell mode's loop: a service's index (its process ended),
 * or one of these. */
#define KEY_MOUNTS   0x100u   /* the mounts watcher changed the namespace */
#define KEY_CTL      0x200u   /* + CTL_*: requests on a control channel */
#define KEY_SPLASH   0x300u   /* the splash's channel (splash.c) */
#define KEY_LASTBOOT 0x400u   /* logd's answer about the last boot's log (lastboot.c) */
#define KEY_UPDATE   0x500u   /* an update's offer channel (update.c) */

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

/* Every service's state (shell.c's; services.c reads the paths and marks
 * one given up when its program isn't in bootfs). */
extern struct svc svcs[NSVC];

/* shell.c: start svc i with these arguments and extra handles (consumed);
 * a service that follows init's namespace gets its part of it. */
status_t svc_start(unsigned i, int argc, const char *const *argv, struct spawn_handle *x,
                   unsigned nx);
/* The same with argv = { its path }. */
status_t svc_start1(unsigned i, struct spawn_handle *x, unsigned nx);
/* writers.c: the table of log writers the console trusts (<logwriters.h>):
 * made once, before the first console; a started devmgr's or logd's koid
 * goes in (any other i is ignored); a read-only handle to it for a new
 * console (HANDLE_INVALID if there is no table). */
void     writers_init(void);
void     writers_started(unsigned i, handle_t proc);
handle_t writers_for_console(void);
/* shell.c: every service that follows init's namespace gets it as it is now. */
void     tell_mounts(void);

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

/* ---- reboot.c -------------------------------------------------------------------- */

/* /esp is mounted (now or again): the first time, note the size and
 * modification time of its kernel and boot image, which the stored kernel
 * was loaded from. */
void     reboot_note_esp(void);
/* Reboot by kexec: into the stored kernel, or, if /esp's kernel or boot
 * image changed since reboot_note_esp, into the files on /esp. Returns
 * only if that failed (said in the log); the caller resets through the
 * firmware. */
status_t init_reboot_kexec(void);
/* initctl.kernel_load: /esp's kernel and boot image read and made the
 * stored copy now (and noted as such); their sizes and the read's time.
 * ERR_NOT_FOUND without them; kexec_load's errors (the old copy stays). */
status_t init_kernel_load(uint64_t *kernel_bytes, uint64_t *bootfs_bytes, uint32_t *read_ms);
/* The stored copy came from somewhere else (an update): /esp's kernel and
 * boot image as they are now are noted as its, so the next reboot keeps
 * it and reads nothing unless the stick changes. Without /esp nothing is
 * noted now (its first mount notes it, reboot_note_esp). */
void     reboot_keep_stored(void);

/* ---- update.c: a fetched build checked and made the stored kernel (<update.h>) ----- */

/* initctl.update_offer: a new offer channel (replacing an older one, whose
 * sender sees ERR_PEER_CLOSED); ours bound on port with key (call
 * update_event on its packets), *client to hand over. */
status_t update_offer_new(handle_t port, uint64_t key, handle_t *client);
/* A packet on the offer channel: the offer (checked, loaded if it passes,
 * answered), or its sender gone. Either way the channel is closed. */
void     update_event(void);

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
