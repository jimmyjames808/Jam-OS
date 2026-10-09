/* The startup message: the first (and only) message on the channel handle
 * a new process receives in rdi at entry. Shared by the kernel (userboot)
 * and user code (libos), so plain C types only. */
#pragma once

#include <stdint.h>

#define STARTUP_MAGIC        0x534d414au   /* "JAMS" */
#define STARTUP_VERSION      1
#define STARTUP_MAX_HANDLES  16

/* What each handle in the message is for: the roles the kernel gives
 * (userboot, to init). The numbers between them are user space's, the
 * roles programs give each other: <roles.h> (user/include/roles.h) names
 * them. SR_USER + n is program-specific. */
enum startup_role {
    SR_NONE = 0,
    SR_SELF_PROCESS = 1,
    SR_SELF_VMAR = 2,
    SR_SELF_THREAD = 3,
    SR_JOB = 4,
    /* 5: <roles.h> */
    SR_BOOTFS = 6,      /* a VMO of the whole bootfs image, read-only */
    SR_RESOURCE = 7,    /* a resource (init: the root; devmgr: RES_PCI) */
    /* 8 .. 13: <roles.h> */
    SR_CRASHLOG = 14,   /* a boot after a panic only: a read-only VMO holding
                         * the panicked kernel's log (struct crashlog_header,
                         * then the text): init, and logd, which saves it */
    /* 15 .. 63: <roles.h> */
    SR_USER = 64,       /* SR_USER + n: program-specific */
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
