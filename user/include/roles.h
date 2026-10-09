/* The startup roles programs give each other, and the promotion message.
 *
 * <jam/startup.h> has the startup message and the roles the kernel itself
 * gives (SR_SELF_*, SR_JOB, SR_BOOTFS, SR_RESOURCE, SR_CRASHLOG); these
 * fill the numbers between them, and are user space's alone: the kernel
 * never looks at them. <os.h> includes this. */
#pragma once

#include <stdint.h>
#include <jam/startup.h>

enum {
    SR_STDOUT = 5,          /* channel; libos prints through debug_write instead */
    SR_DEVMGR = 8,          /* devmgr's query channel, its server end (clients
                             * open /svc/devmgr, <devmgr.h>): STATUS,
                             * GET_SERVICE (not of a device with a device
                             * channel), GET_DRIVER, SUPERVISION */
    SR_CONSOLE = 9,         /* a client end of the console's channel (the shell,
                             * and what the shell runs: a restricted one) */
    SR_DEVMGR_CTL = 10,     /* devmgr's control channel, its server end (every
                             * call; <devmgr.h>; clients open /svc/devmgr-ctl) */
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
