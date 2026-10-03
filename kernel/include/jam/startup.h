/* The startup message: the first (and only) message on the channel handle
 * a new process receives in rdi at entry. Shared by the kernel (userboot)
 * and user code (libos), so plain C types only. */
#pragma once

#include <stdint.h>

#define STARTUP_MAGIC        0x534d414au   /* "JAMS" */
#define STARTUP_VERSION      1
#define STARTUP_MAX_HANDLES  16

/* What each handle in the message is for. */
enum startup_role {
    SR_NONE = 0,
    SR_SELF_PROCESS,
    SR_SELF_VMAR,
    SR_SELF_THREAD,
    SR_JOB,
    SR_STDOUT,      /* channel; libos prints through debug_write instead */
    SR_BOOTFS,      /* a VMO of the whole bootfs image, read-only */
    SR_RESOURCE,    /* a resource (init: the root; devmgr: RES_PCI) */
    SR_DEVMGR,      /* devmgr's query channel, its server end (clients
                     * open /svc/devmgr, <devmgr.h>): STATUS, GET_SERVICE
                     * (not of a device with a device channel), GET_DRIVER,
                     * SUPERVISION */
    SR_CONSOLE,     /* a client end of the console's channel (the shell,
                     * and what the shell runs: a restricted one) */
    SR_DEVMGR_CTL,  /* devmgr's control channel, its server end (every
                     * call; <devmgr.h>; clients open /svc/devmgr-ctl) */
    SR_NS,          /* the namespace: the mount points and their `fs`
                     * channels and the services under /svc, as libos's
                     * ns.c defines the encoding (<os.h> "files") */
    SR_AUDIO,       /* the mixer's `audio` channel, its server end
                     * (abi/idl/audio.idl; clients open /svc/audio) */
    SR_AUDIO_CTL,   /* the mixer's `audioctl` channel, its server end
                     * (every stream's volume; clients open /svc/audioctl) */
    SR_CRASHLOG,    /* a boot after a panic only: a read-only VMO holding
                     * the panicked kernel's log (struct crashlog_header,
                     * then the text): init, and logd, which saves it */
    SR_DEVMGR_DEVICE,/* a devmgr channel scoped to one device (a client
                     * end, from init only: the mixer's, for the HD Audio
                     * controller; <devmgr.h> DEVMGR_DEVICE_CHANNEL) */
    SR_STATE,       /* a service's state VMO, made and kept by its
                     * supervisor, handed to each instance with read, write
                     * and map only (<svcstate.h>) */
    SR_KEEP,        /* a service's end of its keep channel (<keep.h>):
                     * duplicates of what clients hold go to the keeper */
    SR_STANDBY,     /* a warm spare's promotion channel: libos waits on it
                     * before main for one struct standby_msg (below),
                     * which brings the handles and arguments the program
                     * then starts with */

    SR_USER = 64,   /* SR_USER + n: program-specific */
};

struct startup_msg {
    uint32_t txid;                        /* channel convention: first 4 bytes; 0 here */
    uint32_t magic;                       /* STARTUP_MAGIC */
    uint32_t version;                     /* STARTUP_VERSION */
    uint32_t argc;                        /* argv strings after the struct */
    uint32_t envc;                        /* environment strings after them */
    uint32_t nhandles;                    /* handles carried by the message, in order */
    uint32_t roles[STARTUP_MAX_HANDLES];  /* enum startup_role of handle i */
    uint32_t strings_len;                 /* bytes of strings after the struct */
    /* then strings_len bytes: argc argv strings, then envc "KEY=value"
     * strings, each NUL-terminated */
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
    uint32_t roles[STANDBY_MAX_HANDLES];    /* enum startup_role of handle i */
    uint64_t kill_ns;                       /* when the instance this one replaces was
                                             * killed or found dead (uptime ns); 0: none */
    uint32_t strings_len;                   /* bytes of strings after the struct */
    uint32_t reserved;                      /* 0 */
    /* then strings_len bytes, as in struct startup_msg */
};

/* SR_CRASHLOG's VMO: this header, then text_len bytes of log text, the
 * panicked kernel's log ring from its oldest byte to its newest. The
 * kernel checked what it copied (kernel/kexec/crashlog.c); a reader checks
 * the header again (it crosses a process boundary). */
#define CRASHLOG_MAGIC   0x474f4c48534152ull   /* "RASHLOG" */
#define CRASHLOG_VERSION 2
#define CRASHLOG_NAME    32
#define CRASHLOG_MESSAGE 128

struct crashlog_header {
    uint64_t magic;                       /* CRASHLOG_MAGIC */
    uint32_t version;                     /* CRASHLOG_VERSION */
    uint32_t panics;                      /* panics in a row, that one included */
    uint64_t text_len;                    /* bytes of text after the header */
    uint64_t panic_at;                    /* where in the text the panic's own lines start */
    uint64_t lost;                        /* bytes it logged before the text (out of the ring) */
    uint64_t uptime_ns;                   /* how long that kernel ran */
    char     name[CRASHLOG_NAME];         /* its log file ("boot-0042"), "" if it had none */
    char     message[CRASHLOG_MESSAGE];   /* the panic's message (printable ASCII) */
};
