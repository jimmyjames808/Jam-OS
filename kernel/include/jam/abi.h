/* Types and constants that user code needs to make system calls: handle
 * values, rights, signals, port packets, mapping flags and the argument
 * structs of the calls that take more than six arguments.
 *
 * Shared by the kernel and user code (the user build sees this header
 * through a copy of a few allowed ones, never the kernel include tree), so
 * plain C types only and no kernel includes. The kernel headers that used
 * to define these (handle.h, object.h, port.h) include this one instead,
 * so there is only one definition.
 *
 * User addresses are uint64_t, never C pointers, in the argument structs:
 * the kernel never dereferences them (M5-PLAN.md, "Fixed decisions"). */
#pragma once

#include <stdint.h>

/* handles ------------------------------------------------------------------ */

typedef uint32_t handle_t;
#define HANDLE_INVALID 0u

typedef uint32_t rights_t;
#define RIGHT_READ      (1u << 0)
#define RIGHT_WRITE     (1u << 1)
#define RIGHT_EXEC      (1u << 2)
#define RIGHT_MAP       (1u << 3)
#define RIGHT_DUPLICATE (1u << 4)
#define RIGHT_TRANSFER  (1u << 5)
#define RIGHT_SIGNAL    (1u << 6)   /* may set/clear user signals */
#define RIGHT_WAIT      (1u << 7)   /* may wait on it / bind it to a port */
#define RIGHT_INSPECT   (1u << 8)
#define RIGHT_MANAGE    (1u << 9)   /* jobs: change limits, kill everything in it */
#define RIGHT_SAME      0x80000000u /* in duplicate: keep the same rights */

#define RIGHTS_BASIC (RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_WAIT | RIGHT_INSPECT)
#define RIGHTS_IO    (RIGHT_READ | RIGHT_WRITE)

/* Job handles. job_create gives JOB_RIGHTS (the creator manages the new
 * job); a program is handed its OWN job (SR_JOB) with JOB_RIGHTS_OWN only,
 * so it can start processes and child jobs in it but can't lift the limits
 * its parent set on it (or kill it). */
#define JOB_RIGHTS_OWN (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
#define JOB_RIGHTS     (JOB_RIGHTS_OWN | RIGHT_MANAGE)

/* signals ------------------------------------------------------------------ */

typedef uint32_t signals_t;

/* Common signal bits. Types document which ones they use. */
#define SIG_READABLE    (1u << 0)
#define SIG_WRITABLE    (1u << 1)
#define SIG_PEER_CLOSED (1u << 2)
#define SIG_SIGNALED    (1u << 3)   /* events, timers */
#define SIG_TERMINATED  (1u << 4)   /* processes, threads: gone for good (M5) */
#define SIG_USER_ALL    0xff000000u /* bits 24-31: free for userspace (sys_object_signal) */

/* Deadlines are absolute nanoseconds of uptime. Also defined (identically)
 * in <jam/sched.h>; a kernel file that includes both fails to build if the
 * two ever differ. */
#define DEADLINE_NEVER UINT64_MAX

/* ports -------------------------------------------------------------------- */

enum port_packet_type {
    PORT_PACKET_SIGNAL = 1,
    PORT_PACKET_USER = 2,
    /* M6: PORT_PACKET_INTERRUPT */
};

struct port_packet {
    uint64_t key;       /* chosen by whoever bound/queued it */
    uint32_t type;      /* enum port_packet_type */
    int32_t  status;    /* OK; ERR_CANCELED is reserved for binding teardown */
    union {
        struct {
            signals_t trigger;    /* the mask it was bound with */
            signals_t observed;   /* the object's signals at the last edge */
            uint64_t  count;      /* edges coalesced into this packet (>= 1) */
        } signal;
        struct {
            uint64_t data[4];
        } user;
    };
};

#define PORT_BIND_ONCE       0   /* fire once, then the binding is gone */
#define PORT_BIND_PERSISTENT 1   /* fire on every not-matching -> matching edge */

/* address spaces ------------------------------------------------------------
 * vmar_map / vmar_protect flags. Same values as ASPACE_* in <jam/aspace.h>
 * (checked at compile time in kernel/abi/abi_check.c). */

#define VMAR_READ  (1u << 0)
#define VMAR_WRITE (1u << 1)
#define VMAR_EXEC  (1u << 2)   /* never together with VMAR_WRITE */
#define VMAR_FIXED (1u << 3)   /* map at *addr exactly (else first fit) */

/* jobs ---------------------------------------------------------------------
 * Every process belongs to a job. A job has a limit on each resource below
 * and counts what its processes (and, through them, its child jobs) use
 * right now; a charge that would take the job or any of its ancestors past
 * its limit fails (ERR_NO_MEMORY for pages and message bytes,
 * ERR_NO_RESOURCES for handles and threads). So a child job can never use
 * more than its parent has left, whatever its own limit says. */

#define JOB_LIMIT_PAGES     1   /* committed VMO pages (charged to the VMO creator's job) */
#define JOB_LIMIT_HANDLES   2   /* handle-table slots in use */
#define JOB_LIMIT_THREADS   3   /* live threads */
#define JOB_LIMIT_MSG_BYTES 4   /* bytes of queued channel messages, charged to the sender */
#define JOB_LIMIT_COUNT     5   /* kinds are 1 .. JOB_LIMIT_COUNT - 1 */
#define JOB_NO_LIMIT        UINT64_MAX

/* job_get_info. Arrays are indexed by JOB_LIMIT_*; index 0 is unused. */
struct job_info {
    uint64_t used[JOB_LIMIT_COUNT];    /* this job and its descendants, now */
    uint64_t limit[JOB_LIMIT_COUNT];   /* this job's own limit (JOB_NO_LIMIT: none) */
    uint64_t koid;
};

/* processes and threads ------------------------------------------------------ */

#define PROCESS_NEW     0   /* created, not started */
#define PROCESS_RUNNING 1
#define PROCESS_DYING   2   /* killed or exiting: its threads are leaving */
#define PROCESS_DEAD    3   /* every thread gone, handles closed (SIG_TERMINATED) */

/* The exit code of a process that was killed (process_kill, a fatal fault,
 * or its last handle closed before it started) rather than exiting. */
#define PROCESS_KILLED_CODE (-1)

struct process_info {
    int64_t  exit_code;   /* once DEAD: process_exit's code, or PROCESS_KILLED_CODE */
    uint32_t state;       /* PROCESS_* */
    uint32_t killed;      /* 1 if it was killed rather than exiting */
    uint32_t threads;     /* live threads */
    uint32_t reserved;
    uint64_t koid;
};

/* Thread priorities: 0 (lowest) .. 31. User threads start at
 * THREAD_PRIO_DEFAULT and may be set up to THREAD_PRIO_USER_MAX; the levels
 * above are the kernel's (and, later, a capability's). */
#define THREAD_PRIO_DEFAULT  16
#define THREAD_PRIO_USER_MAX 24

/* argument structs -----------------------------------------------------------
 * Calls with more than six arguments take a pointer to one of these. The
 * syscall dispatcher copies the struct into the kernel before the call, so
 * its fields are read once; the buffers they point at are user memory. */

struct channel_read_args {
    handle_t h;
    uint32_t bytes_cap;
    uint64_t bytes;            /* user address: bytes_cap bytes */
    uint64_t actual_bytes;     /* user address of a uint32_t, or 0 */
    uint64_t handles;          /* user address: handles_cap handle_t */
    uint32_t handles_cap;
    uint32_t reserved;         /* 0 */
    uint64_t actual_handles;   /* user address of a uint32_t, or 0 */
};

struct channel_call_args {
    handle_t h;
    uint32_t wn;               /* request bytes (>= 4: the txid goes first) */
    uint64_t wbytes;           /* user address of the request */
    uint64_t wh;               /* user address: whn handle_t to send */
    uint32_t whn;
    uint32_t rcap;             /* reply buffer bytes */
    uint64_t rbytes;           /* user address of the reply buffer */
    uint64_t ractual;          /* user address of a uint32_t, or 0 */
    uint64_t rh;               /* user address: rhcap handle_t for the reply */
    uint32_t rhcap;
    uint32_t reserved;         /* 0 */
    uint64_t rhactual;         /* user address of a uint32_t, or 0 */
    uint64_t deadline_ns;      /* absolute, uptime clock; UINT64_MAX = forever */
};
