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
    SR_DEVMGR,      /* a channel to devmgr (devmgr: its server end; the
                     * programs init starts: a client end, <devmgr.h>),
                     * queries only (STATUS, GET_SERVICE, GET_DRIVER,
                     * SUPERVISION) */
    SR_CONSOLE,     /* a client end of the console's channel (the shell,
                     * and what the shell runs: a restricted one) */
    SR_DEVMGR_CTL,  /* devmgr's control channel (every call; <devmgr.h>):
                     * init, and the test programs init or the shell runs */
    SR_NS,          /* the file namespace (M8): the mount points and their
                     * `fs` channels, as libos's fs.c defines the encoding */
    SR_AUDIO,       /* the mixer's `audio` channel (abi/idl/audio.idl; the
                     * mixer: its server end): open a sound stream */
    SR_AUDIO_CTL,   /* the mixer's `audioctl` channel (every stream's
                     * volume): the shell and its test programs */

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
