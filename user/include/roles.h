/* The startup roles programs give each other, and the promotion message.
 *
 * <jam/startup.h> has the startup message and the roles the kernel itself
 * gives (SR_SELF_*, SR_JOB, SR_BOOTFS, SR_RESOURCE, SR_CRASHLOG); these
 * fill the numbers between them, and are user space's alone: the kernel
 * never looks at them. <os.h> includes this.
 *
 * Two kinds. The roles up to SR_SIGNALS mean the same in every process
 * that has them (a service's end of its keep channel, a namespace). The
 * numbers from SR_USER on mean something to the one program they are
 * given to and nothing to another: SR_USER + 0 is the music player's
 * channel, the compositor's /svc/wayland and the network stack's control
 * channel. The roles of the programs init and the shell start are named
 * below, `<PROGRAM>_SR_<WHAT>`, so that the starter (init, the shell, a
 * test) and the program say the same name and no bare `SR_USER + n`
 * stands for a role in either. Some services keep theirs in a header of
 * their own: fat's in <fatsvc.h>, volumes' in <volumes.h>, devmgr's in
 * <devmgr.h>, the mixer's in <mixer.h>, the console's last two in
 * <logwriters.h> and user/services/console/console.h (WAYLAND_ROLE, its
 * clients), the splash's in <splash.h>. A test program's own children
 * (utest's modes, ramfs) number theirs as they like. */
#pragma once

#include <stdint.h>
#include <jam/startup.h>

enum {
    SR_STDOUT = 5,          /* channel: libos prints there when given, else through debug_write */
    SR_DEVMGR = 8,          /* devmgr's query channel, its server end (clients
                             * open /svc/devmgr): the `devmgr` protocol
                             * (abi/idl/devmgr.idl), get_service of a device
                             * with a device channel refused */
    SR_CONSOLE = 9,         /* a client end of the console's channel (the shell,
                             * and what the shell runs: a restricted one) */
    SR_DEVMGR_CTL = 10,     /* devmgr's control channel, its server end (`devmgr`
                             * and `devmgrctl`, abi/idl/devmgr.idl; clients
                             * open /svc/devmgr-ctl) */
    SR_NS = 11,             /* the namespace: the mount points and their `fs`
                             * channels and the services under /svc, as libos's
                             * ns.c defines the encoding (<os.h> "files") */
    SR_AUDIO = 12,          /* the mixer's `audio` channel, its server end
                             * (abi/idl/audio.idl; clients open /svc/audio) */
    SR_AUDIO_CTL = 13,      /* the mixer's `audioctl` channel, its server end
                             * (every stream's volume; clients open /svc/audioctl) */
    /* 14: SR_CRASHLOG, <jam/startup.h> */
    SR_DEVMGR_DEVICE = 15,  /* a devmgr channel scoped to one device (a client
                             * end, from init only: the mixer's, for the HD
                             * Audio controller; devmgrctl.device_channel) */
    SR_STATE = 16,          /* a service's state VMO, made and kept by its
                             * supervisor, handed to each instance with read,
                             * write and map only (<svcstate.h>) */
    SR_KEEP = 17,           /* a service's end of its keep channel (<keep.h>):
                             * duplicates of what clients hold go to the keeper */
    SR_STANDBY = 18,        /* a warm spare's promotion channel: libos waits on
                             * it before main for one struct standby_msg
                             * (below), which brings the handles and arguments
                             * the program then starts with */
    SR_SIGNALS = 19,        /* a POSIX program's signal channel, its server end:
                             * the starter keeps the client end and sends the
                             * `signal` protocol's events on it (docs/M13-PLAN.md
                             * section 6) */
    SR_SELF_IMAGE = 20,     /* the program's own executable VMO, the one it runs
                             * from (read, map, execute, duplicate and transfer;
                             * never write): libjam's spawn gives it to every
                             * program started from a VMO (a /data program, a
                             * self-image child), so libc's /proc/self/exe starts
                             * the same pages again (docs/CR-PLAN.md section 6).
                             * A boot-image program has none: its file is
                             * SR_BOOTFS's */
    SR_ARGS = 21,           /* argv and the environment when the startup message
                             * can't hold them (struct startup_args, below):
                             * a VMO, read only, which libos copies into the
                             * program's own memory and closes before main */
};

/* SR_ARGS's VMO: this header, then strings_len bytes of strings (argc
 * argv strings, then envc "KEY=value" strings, each NUL-terminated, as in
 * the startup message), then exe_len bytes of the program's path. libjam's
 * spawn makes one when argv and the environment don't fit the startup
 * message (2 KiB, 128 strings), and to name the path of a program it gives
 * SR_SELF_IMAGE and no POSIX hand-over (the hand-over names it otherwise:
 * <posixfds.h>). The message then has no strings of its own. The strings
 * and a pointer for each, as Linux counts them, are at most
 * STARTUP_ARGS_MAX bytes (Linux's usual ARG_MAX): more is ERR_OUT_OF_RANGE
 * for the spawn (E2BIG). The child checks it as any input from its
 * starter: a malformed one leaves it with argc 0, as a malformed message
 * does. */
#define STARTUP_ARGS_MAGIC    0x53475241u   /* "ARGS" */
#define STARTUP_ARGS_VERSION  1
#define STARTUP_ARGS_MAX      (2u << 20)    /* strings + 8 bytes a string and 2 NULLs */
#define STARTUP_ARGS_EXE_MAX  4096          /* the path, its NUL included */

struct startup_args {
    uint32_t magic;          /* STARTUP_ARGS_MAGIC */
    uint32_t version;        /* STARTUP_ARGS_VERSION */
    uint32_t argc;           /* argv strings */
    uint32_t envc;           /* environment strings after them */
    uint32_t strings_len;    /* bytes of strings after this header */
    uint32_t exe_len;        /* bytes of the path after them, its NUL included (an
                              * absolute path, 2 .. STARTUP_ARGS_EXE_MAX), or 0 */
};

/* The promotion message: the one message on SR_STANDBY, from the
 * supervisor that started the spare. Its handles join the startup
 * message's (by role, as if they had come in it; SR_STANDBY itself is
 * closed and goes), and its strings replace the spare's argv and
 * environment, so a promoted program can't tell it was a spare but by
 * standby_kill_ns() (<svcstate.h>). A malformed one ends the spare (exit
 * code STANDBY_BAD_PROMOTION); its peer closed without one, a dismissal,
 * ends it with code 0. Neither runs main. */
#define STANDBY_MAGIC         0x4d4f5250u   /* "PROM" */
#define STANDBY_VERSION       1
#define STANDBY_MAX_HANDLES   64            /* a channel message's most */
#define STANDBY_MAX_BYTES     8192          /* the whole message, strings included */
#define STANDBY_MAX_STRINGS   128           /* argv and environment strings together */
#define STANDBY_BAD_PROMOTION 3             /* the exit code of a spare given a bad one */

struct standby_msg {
    uint32_t txid;                          /* channel convention: first 4 bytes; 0 here */
    uint32_t magic;                         /* STANDBY_MAGIC */
    uint32_t version;                       /* STANDBY_VERSION */
    uint32_t argc;                          /* argv strings after the struct */
    uint32_t envc;                          /* environment strings after them */
    uint32_t nhandles;                      /* handles carried, in order */
    uint32_t roles[STANDBY_MAX_HANDLES];    /* the startup role of handle i */
    uint64_t kill_ns;                       /* when the instance this one replaces was
                                             * killed or found dead (uptime ns); 0: none */
    uint32_t strings_len;                   /* bytes of strings after the struct */
    uint32_t reserved;                      /* 0 */
    /* then strings_len bytes, as in struct startup_msg */
};

/* ---- the roles of the programs init and the shell start (SR_USER + n) ------------
 *
 * Handles are named by what they are, with the program that gets them. A
 * "server end" is what the program serves; init keeps the other end (and,
 * for a service that restarts, both: the restarted one serves the same
 * channel) and publishes the client end as /svc/<name>. */
enum {
    /* bin/bootfs */
    BOOTFS_SR_FS = SR_USER + 0,         /* the server end of its `fs` channel */
    /* bin/serialin */
    SERIALIN_SR_INPUT = SR_USER + 0,    /* the `input` channel to the compositor */
    /* bin/music, bin/serve */
    MUSIC_SR_CTL = SR_USER + 0,         /* the server end of the `music` channel */
    SERVE_SR_CTL = SR_USER + 0,         /* the server end of the `serve` channel */
    /* bin/netstack (and the network card's device channels, SR_DEVMGR_DEVICE) */
    NETSTACK_SR_CTL = SR_USER + 0,      /* the server end of its control channel */
    NETSTACK_SR_NET = SR_USER + 1,      /* /svc/net's shared channel, server end */
    NETSTACK_SR_NET_LISTEN = SR_USER + 2,   /* /svc/net-listen's */
    NETSTACK_SR_NET_SYS = SR_USER + 3,  /* /svc/net-sys's: the network's own services' */
    NETSTACK_SR_NET_LOW = SR_USER + 4,  /* /svc/net-low's: ports below 1024 too */
    NETSTACK_SR_NETINFO = SR_USER + 5,  /* the read-only control channel, server end */
    NETSTACK_SR_NOTIFY = SR_USER + 6,   /* a duplicate of /svc/notify's shared channel */
    /* bin/dhcp, bin/dns */
    DHCP_SR_NETCTL = SR_USER + 0,       /* a client end of netstack's control channel */
    DNS_SR_DNS = SR_USER + 0,           /* /svc/dns's shared channel, server end */
    DNS_SR_DNS_SYS = SR_USER + 1,       /* /svc/dns-sys's */
    /* bin/netlog */
    NETLOG_SR_STOP = SR_USER + 0,       /* a channel init may close to ask it to flush and exit
                                         * (the regression run; none in shell mode) */
    /* bin/compositor */
    COMP_SR_WAYLAND = SR_USER + 0,      /* /svc/wayland's shared channel, server end */
    COMP_SR_IMAGE = SR_USER + 1,        /* a headless image's VMO (the tests) */
    COMP_SR_ADMIN = SR_USER + 2,        /* the server end of a compctl channel (ADMIN) */
    COMP_SR_INITCTL = SR_USER + 3,      /* init's control channel for it (reboot, terminal,
                                         * launch) */
    COMP_SR_REPORT = SR_USER + 4,       /* the paint report channel (its test scene) */
    COMP_SR_NOTIFY = SR_USER + 5,       /* /svc/notify's shared channel, server end */
    COMP_SR_MIXER = SR_USER + 6,        /* the mixer's desktop channel, client end */
    COMP_SR_NETINFO = SR_USER + 7,      /* netstack's read-only channel, client end */
    /* bin/console (the terminals; its own header has the rest of them) */
    CONSOLE_SR_CHANNEL = SR_USER + 0,   /* the server end of the console's channel */
    CONSOLE_SR_INITCTL = SR_USER + 8,   /* init's control channel for it (reboot, terminal) */
    CONSOLE_SR_WAYLAND = SR_USER + 10,  /* /svc/wayland's client end (window mode) */
    /* bin/logd */
    LOGD_SR_FS = SR_USER + 0,           /* an `fs` channel to use as /data (tests) */
    LOGD_SR_LOG = SR_USER + 1,          /* a channel whose messages are the log (tests) */
    LOGD_SR_CTL = SR_USER + 2,          /* the server end of its `logctl` channel */
    LOGD_SR_CRASH_RESULT = SR_USER + 3, /* the channel it sends the panicked boot's result
                                         * on (crashres, <crashlog.h>) */
    /* bin/shell */
    SHELL_SR_PCI = SR_USER + 1,         /* RES_PCI with RIGHTS_BASIC (pci_enum for `devices`) */
    /* a program the shell runs for a command (play, fetch, speed, update) */
    PROGRAM_SR_FILE = SR_USER + 0,      /* the open file's `file` channel (file_give) */
    PROGRAM_SR_BUF = SR_USER + 1,       /* its transfer buffer */
    PROGRAM_SR_STOP = SR_USER + 2,      /* the shell's stop channel: a message (or its close)
                                         * asks the program to end (Ctrl+C) */
    PROGRAM_SR_BODY = SR_USER + 3,      /* fetch's: the channel a body goes down in */
    /* bin/update */
    UPDATE_SR_OFFER = SR_USER + 0,      /* the offer channel (initctl.update_offer) */
};
