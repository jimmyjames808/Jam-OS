/* init: what its files share. main.c starts init (the init.cfg programs,
 * keytest); shell.c is the shell mode, where init starts and supervises
 * the bootfs server, the console, serialin, devmgr and the shell; mounts.c
 * keeps init's namespace in step with devmgr's mounts; ctl.c serves
 * init's control channels (abi/idl/initctl.idl).
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
 * controllers alone (the safe mode boot entry). */
bool init_shell(bool nousb);

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

/* ---- shell.c, for ctl.c ---------------------------------------------------------- */

/* The root resource (reboot, proc_list). */
handle_t shell_root(void);
/* devmgr's control channel, or 0 while none runs. */
handle_t shell_devmgr(void);
/* Kill the service init runs under this name ("console", ...): its whole
 * job; init's loop then starts it again. *koid: its process's id.
 * ERR_NOT_FOUND: not a service of init's, or not running. */
status_t shell_kill_service(const char *name, uint64_t *koid);
