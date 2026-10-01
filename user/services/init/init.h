/* init: what its files share. main.c starts init (the init.cfg programs,
 * keytest); shell.c is the shell mode, where init starts and supervises
 * the bootfs server, the console, serialin, devmgr and the shell; splash.c
 * the boot splash that plays first in shell mode; lastboot.c the boot
 * before this one, if it panicked (its log saved by logd, one line for the
 * shell); reboot.c a reboot by kexec; mounts.c keeps init's namespace in
 * step with devmgr's mounts; ctl.c serves init's control channels
 * (abi/idl/initctl.idl).
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
/* The splash's channel: ours bound on port with key (call splash_event on
 * its packets), *theirs to hand over as its SR_USER + SPLASH_INIT_ROLE. */
status_t splash_channel(handle_t port, uint64_t key, handle_t *theirs);
/* A packet on the channel: SPLASH_PLAYED, or the splash closed it. */
void     splash_event(void);
/* The splash process ended (it is never started again). */
void     splash_ended(void);
/* initctl.shell_ready: SPLASH_GO to a splash still holding the screen. */
void     splash_shell_ready(void);

/* ---- shell.c, for ctl.c ---------------------------------------------------------- */

/* The root resource (reboot, proc_list). */
handle_t shell_root(void);
/* devmgr's control channel, or 0 while none runs. */
handle_t shell_devmgr(void);
/* logd writes out and syncs the log up to now (logctl.flush), waited for
 * until deadline at most. Nothing to do without a logd. */
void     shell_flush_log(uint64_t deadline);
/* For a kexec reboot (reboot.c): devmgr stops every driver in order and
 * exits (DEVMGR_SHUTDOWN), waited for until deadline; if it doesn't, its
 * job is killed. It is not started again. OK, or what went wrong. */
status_t shell_stop_devmgr(uint64_t deadline);

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
