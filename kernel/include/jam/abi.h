/* Types and constants that user code needs to make system calls: handle
 * values, rights, signals, port packets, mapping flags and the argument
 * structs of the calls that take more than six arguments.
 *
 * Shared by the kernel and user code (the user build sees this header
 * through a copy of a few allowed ones, never the kernel include tree), so
 * plain C types only and no kernel includes. The kernel headers that need
 * these (handle.h, object.h, port.h) include this one rather than defining
 * them again, so there is only one definition.
 *
 * User addresses are uint64_t, never C pointers, in the argument structs:
 * the kernel never dereferences them (it copies through usercopy.h). */
#pragma once

#include <stdint.h>

/* handles ------------------------------------------------------------------ */

typedef uint32_t handle_t;
#define HANDLE_INVALID 0u

typedef uint32_t rights_t;
/* The generic rights, bits 0 .. 15 (11 used); the root's powers, bits
 * 16 .. 30; RIGHT_SAME, bit 31. */
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
#define RIGHT_SLICE     (1u << 10)  /* resources: make a smaller resource inside this one */
/* VMOs: change the size or decommit pages (with RIGHT_WRITE). A service
 * that maps a buffer it shares hands the other side a handle without it,
 * so the buffer can't shrink under the mapping. */
#define RIGHT_RESIZE    (1u << 11)
/* Powers over the whole system, on the root resource (RES_ROOT) only: each
 * call that needs one checks for it alone, so a program is given exactly
 * the powers it uses (a slice never has them). */
#define RIGHT_ROOT_KLOG    (1u << 16)  /* klog_open: read the kernel log */
#define RIGHT_ROOT_SERIAL  (1u << 17)  /* serial_open: the serial port's input */
#define RIGHT_ROOT_SYSINFO (1u << 18)  /* sys_info, cpu_stat, proc_list */
#define RIGHT_ROOT_CLOCK   (1u << 19)  /* rtc_read: the real-time clock */
#define RIGHT_ROOT_REBOOT  (1u << 20)  /* reboot, kexec_reboot */
#define RIGHT_ROOT_KEXEC   (1u << 21)  /* kexec_load: replace the stored kernel */
#define RIGHT_ROOT_DEBUG   (1u << 22)  /* debug_command: the kernel's tests and debug commands */
#define RIGHT_ROOT_VMEX    (1u << 23)  /* vmo_make_exec: run code from a VMO a process made */
#define RIGHT_ROOT_SCREEN  (1u << 24)  /* framebuffer_take: the screen */
#define RIGHT_ROOT_SERIAL_OUT (1u << 25)  /* serial_write: the serial port's output */
#define RIGHT_ROOT_PAGER   (1u << 26)  /* pager_create: supply other processes' pages */
#define RIGHTS_ROOT (RIGHT_ROOT_KLOG | RIGHT_ROOT_SERIAL | RIGHT_ROOT_SYSINFO | \
                     RIGHT_ROOT_CLOCK | RIGHT_ROOT_REBOOT | RIGHT_ROOT_KEXEC |   \
                     RIGHT_ROOT_DEBUG | RIGHT_ROOT_VMEX | RIGHT_ROOT_SCREEN |    \
                     RIGHT_ROOT_SERIAL_OUT | RIGHT_ROOT_PAGER)
#define RIGHT_SAME      0x80000000u /* in duplicate: keep the same rights */

#define RIGHTS_BASIC (RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_WAIT | RIGHT_INSPECT)
#define RIGHTS_IO    (RIGHT_READ | RIGHT_WRITE)

/* Job handles. job_create gives JOB_RIGHTS (the creator manages the new
 * job); a program is handed its OWN job (SR_JOB) with JOB_RIGHTS_OWN only,
 * so it can start processes and child jobs in it but can't lift the limits
 * its parent set on it (or kill it). */
#define JOB_RIGHTS_OWN (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
#define JOB_RIGHTS     (JOB_RIGHTS_OWN | RIGHT_MANAGE)

/* Handles a process holds at most (a call that would make one more fails
 * with ERR_NO_RESOURCES). */
#define HANDLE_TABLE_MAX 65536

/* The kinds of kernel object a handle names (handle_info's type). */
enum obj_type {
    OBJ_NONE = 0,           /* none (the kernel's lookups: "any type") */
    OBJ_EVENT = 1,
    OBJ_TIMER = 2,
    OBJ_CHANNEL = 3,        /* one end of a channel */
    OBJ_PORT = 4,
    OBJ_VMO = 5,
    OBJ_DMA_CAP = 6,        /* permission to pin memory for device DMA */
    OBJ_PROCESS = 7,
    OBJ_THREAD = 8,
    OBJ_INTERRUPT = 9,      /* a device interrupt vector */
    OBJ_RESOURCE = 10,      /* authority over a piece of hardware */
    OBJ_VMAR = 11,          /* a handle to an address space */
    OBJ_JOB = 12,           /* resource limits for a group of processes */
    OBJ_KLOG = 13,          /* a reader of the kernel log */
    OBJ_SERIAL = 14,        /* COM1 input */
    OBJ_SCREEN = 15,        /* ownership of the boot framebuffer */
    OBJ_PAGER = 16,         /* supplies the pages of its VMOs (pager_create) */
    OBJ_TYPE_COUNT,
};

/* handle_info: what a handle names, and what its holder could learn by
 * trying calls on it (no right needed). */
struct handle_info {
    uint32_t type;              /* enum obj_type */
    rights_t rights;            /* the handle's own rights */
    uint64_t koid;              /* the object's id (never reused) */
    uint64_t related_koid;      /* a channel end: its peer's (0 once the peer has
                                 * closed); a thread: its process's; a process: its
                                 * job's (0 once torn down); a pager VMO: its
                                 * pager's (how a service that maps client VMOs
                                 * tells one, and refuses it); else 0 */
    /* A channel end only (else 0): */
    uint32_t queued;            /* messages waiting on it, to be read */
    uint32_t peer_queued;       /* messages it wrote that wait, unread, at its peer */
    uint64_t peer_queued_bytes; /* the bytes of those */
    uint64_t reserved;          /* 0 */
};

/* signals ------------------------------------------------------------------ */

typedef uint32_t signals_t;

/* Common signal bits. Types document which ones they use. */
#define SIG_READABLE    (1u << 0)
#define SIG_WRITABLE    (1u << 1)
#define SIG_PEER_CLOSED (1u << 2)
#define SIG_SIGNALED    (1u << 3)   /* events, timers */
#define SIG_TERMINATED  (1u << 4)   /* processes, threads: gone for good */
#define SIG_INTERRUPT   (1u << 5)   /* interrupt objects: fired, not acked yet */
#define SIG_USER_ALL    0xff000000u /* bits 24-31: an event's, free for user space (event_signal) */

/* Deadlines are absolute nanoseconds of uptime. Also defined (identically)
 * in <jam/sched.h>; a kernel file that includes both fails to build if the
 * two ever differ. */
#define DEADLINE_NEVER UINT64_MAX

/* ports -------------------------------------------------------------------- */

enum port_packet_type {
    PORT_PACKET_SIGNAL = 1,
    PORT_PACKET_USER = 2,
    /* Interrupts arrive as PORT_PACKET_SIGNAL packets: bind the
     * interrupt object PERSISTENT for SIG_INTERRUPT; `count` says how many
     * times it fired before the packet was read; interrupt_ack re-arms. */
};

struct port_packet {
    uint64_t key;                /* chosen by whoever bound/queued it */
    uint32_t type;               /* enum port_packet_type */
    int32_t  status;             /* OK (port_queue's too); ERR_CANCELED: a binding's teardown */
    union {                      /* which one: `type` */
        struct {
            signals_t trigger;   /* the mask it was bound with */
            signals_t observed;  /* the object's signals at the last edge */
            uint64_t  count;     /* edges coalesced into this packet (>= 1) */
        } signal;                /* PORT_PACKET_SIGNAL */
        struct {
            uint64_t data[4];    /* whatever the queuer put in */
        } user;                  /* PORT_PACKET_USER */
    };
};

#define PORT_BIND_ONCE       0   /* fire once, then the binding is gone */
#define PORT_BIND_PERSISTENT 1   /* fire on every not-matching -> matching edge */

/* A port holds at most PORT_MAX_USER_PACKETS user packets (port_queue past
 * it: ERR_NO_RESOURCES; signal packets never count, each binding owns
 * one) and PORT_MAX_BINDINGS bindings (port_bind past it: the same). */
#define PORT_MAX_USER_PACKETS 4096
#define PORT_MAX_BINDINGS     4096

/* VMOs ------------------------------------------------------------------------
 * VMO flags. Bits 0 and 1 are dma_vmo_create's, for drivers (a dma_cap
 * lets them ask; <jam/driver.h> also calls them DRV_VMO_*): memory a
 * device reaches by DMA. vmo_create takes only VMO_KEEP_PAGES. */

/* Physically contiguous, committed (zeroed) at creation, at most
 * VMO_CONTIGUOUS_MAX bytes. */
#define VMO_CONTIGUOUS (1u << 0)
/* Every page below 4 GiB. */
#define VMO_DMA32      (1u << 1)

/* Pages that stay. Every page is committed at creation and charged to the
 * creator's job (vmo_create fails with ERR_NO_MEMORY, nothing made, if the
 * job can't pay for all of them), and no holder of any handle can ever
 * take one away: vmo_decommit and a shrinking vmo_set_size are
 * ERR_BAD_STATE, and a growing vmo_set_size commits the new pages before
 * it returns (ERR_NO_MEMORY with the size unchanged if they can't all be
 * had). For memory a process reads that a less trusted one gave it (a
 * Wayland client's pool, read by the compositor through VMAR_KEPT_ONLY).
 * Not together with the drivers' bits (ERR_INVALID_ARGS). A
 * copy-on-write child of it (vmo_create_child) never shares its pages
 * (a write would swap a page under the kept readers): it gets copies. */
#define VMO_KEEP_PAGES (1u << 2)

/* vmo_create_child's kinds: exactly one (abi/syscalls.def 63). Both share
 * the parent's pages until either side writes one, which is copied for
 * the writer alone; the child is charged at the call for every page it
 * may come to own, so no later write ever fails for its job's limit.
 * SNAPSHOT: the child holds the parent's bytes as at the call and neither
 * side sees the other's later writes; refused for a pager VMO and a child
 * of one (their untouched pages follow the file). AT_LEAST_ON_WRITE: the
 * same, except that for a pager VMO (or a child of one) a page the child
 * hasn't touched shows the file as it is when the child first touches it
 * (MAP_PRIVATE's promise). */
#define VMO_CHILD_SNAPSHOT                   (1u << 0)
#define VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE (1u << 1)

/* The limits. A VMO's size: 64 GiB of address space (pages are committed
 * as they are touched; ERR_OUT_OF_RANGE past it). A contiguous VMO's: 4
 * MiB, the largest block the page allocator has. One vmo_pin: 4096 pages
 * (16 MiB; ERR_OUT_OF_RANGE past it): the kernel builds the address list
 * in a buffer of its own. */
#define VMO_MAX_SIZE       (1ull << 36)
#define VMO_CONTIGUOUS_MAX (4ull << 20)
#define VMO_PIN_MAX_PAGES  4096u

/* address spaces ------------------------------------------------------------
 * vmar_map / vmar_protect flags. Same values as ASPACE_* in <jam/aspace.h>
 * (checked at compile time in kernel/abi/abi_check.c). */

#define VMAR_READ  (1u << 0)
#define VMAR_WRITE (1u << 1)
#define VMAR_EXEC  (1u << 2)   /* never together with VMAR_WRITE */
#define VMAR_FIXED (1u << 3)   /* map at *addr exactly (else first fit) */
/* vmar_map only: map a VMO_KEEP_PAGES VMO (ERR_NOT_SUPPORTED for any other
 * VMO) read-only (VMAR_READ, maybe VMAR_FIXED; ERR_INVALID_ARGS for any
 * other permission), with every page-table entry filled before the call
 * returns, so no access through the mapping ever faults, whatever the
 * VMO's other holders do. The page tables are charged to the mapper's job
 * then: a refused charge fails the map (ERR_NO_MEMORY, nothing left
 * mapped), never a later access. The mapping's permissions never change
 * (vmar_protect: ERR_ACCESS_DENIED), and its length is what was mapped: a
 * VMO that grows later is mapped again to reach the new pages. */
#define VMAR_KEPT_ONLY (1u << 4)
/* vmar_map only, with VMAR_FIXED (ERR_INVALID_ARGS without it, or with
 * VMAR_KEPT_ONLY): whatever is mapped in [*addr, *addr + len) is unmapped
 * and the new mapping made in one step, under the address space's lock.
 * Another thread's access to the range finds the old mapping or the new
 * one, never nothing (a fault there waits for the step to end). Mappings
 * that reach past the range keep their parts outside it; holes are
 * fine. On any error nothing has changed (ERR_OUT_OF_RANGE past the
 * VMO's end, ERR_NO_RESOURCES past VMAR_MAX_MAPPINGS, ERR_NO_MEMORY).
 * MAP_FIXED's call. */
#define VMAR_REPLACE   (1u << 5)

/* Mappings an address space holds at most (a vmar_map past it, or an
 * unmap or protect that would split one more off, is ERR_NO_RESOURCES). */
#define VMAR_MAX_MAPPINGS 16384u

/* jobs ---------------------------------------------------------------------
 * Every process belongs to a job. A job has a limit on each resource below
 * and counts what its processes (and, through them, its child jobs) use
 * right now; a charge that would take the job or any of its ancestors past
 * its limit fails (ERR_NO_MEMORY for pages and message bytes,
 * ERR_NO_RESOURCES for handles and threads). So a child job can never use
 * more than its parent has left, whatever its own limit says. */

/* What each kind counts (the kernel's process.h has the details):
 *   PAGES      memory in 4 KiB pages: a VMO's committed pages and its own
 *              table pages (the VMO creator's job); a process's address
 *              space: PML4, page tables, a page per 16 mappings; 17 pages
 *              per running thread for its kernel stack and FPU state;
 *   HANDLES    handle-table slots in use, plus one unit per job (charged
 *              to its parent), process and VMO object;
 *   THREADS    live threads;
 *   MSG_BYTES  queued channel messages (1 KiB more per handle carried),
 *              port packets and port bindings, charged to the sender. */
#define JOB_LIMIT_PAGES     1
#define JOB_LIMIT_HANDLES   2
#define JOB_LIMIT_THREADS   3
#define JOB_LIMIT_MSG_BYTES 4
#define JOB_LIMIT_COUNT     5   /* kinds are 1 .. JOB_LIMIT_COUNT - 1 */
#define JOB_NO_LIMIT        UINT64_MAX
/* Jobs nest at most this deep: a root job is depth 0, and job_create fails
 * with ERR_OUT_OF_RANGE for a job that would be at depth JOB_MAX_DEPTH. A
 * job counts as one JOB_LIMIT_HANDLES unit of its parent while it exists. */
#define JOB_MAX_DEPTH       32

/* job_get_info. Arrays are indexed by JOB_LIMIT_*; index 0 is unused. */
struct job_info {
    uint64_t used[JOB_LIMIT_COUNT];    /* this job and its descendants, now */
    uint64_t limit[JOB_LIMIT_COUNT];   /* this job's own limit (JOB_NO_LIMIT: none) */
    uint64_t koid;                     /* the job's kernel object id */
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
    uint32_t pid;         /* its small id: unique among live processes (below) */
    uint64_t koid;        /* the process's kernel object id */
    uint64_t cpu_ns;      /* CPU time of all its threads so far */
};

/* Small ids, for POSIX's pid and tid: a process's pid and a thread's tid
 * are taken when it is made (process_create, thread_create) and given
 * back when its last handle and reference are gone (a dead process whose
 * handle its parent still holds keeps its pid), each in its own space of
 * 1 .. IDS_MAX - 1. Unique among those not given back; handed out in
 * rising order, wrapping, and one given back is not handed out again
 * until the order has gone once round all of them. Never 0. */
#define IDS_MAX (1u << 22)

/* thread_get_info: one thread. */
#define THREAD_NEW     0   /* made, not started */
#define THREAD_RUNNING 1   /* started (running, ready or waiting) */
#define THREAD_DEAD    2   /* left: thread_exit, its process's end */
struct thread_info {
    uint64_t koid;          /* the thread's kernel object id */
    uint64_t process_koid;  /* its process's */
    uint64_t cpu_ns;        /* its CPU time so far */
    uint32_t tid;           /* its small id (above) */
    uint32_t pid;           /* its process's */
    uint32_t state;         /* THREAD_* */
    int32_t  priority;      /* its base priority (thread_set_priority) */
};

/* sys_public_info: what any program may know of the system (uname,
 * sysconf); no handle needed. */
struct sys_public_info {
    char     version[32];       /* Jam OS's version, "0.1.0", NUL-terminated */
    char     machine[16];       /* "x86_64" */
    uint64_t mem_total_pages;   /* pages the kernel manages */
    uint32_t page_size;         /* 4096 */
    uint32_t cpus_online;       /* CPUs running now */
};

/* Thread priorities: 0 (lowest) .. 31. User threads start at
 * THREAD_PRIO_DEFAULT and may be set up to THREAD_PRIO_USER_MAX; the levels
 * above are the kernel's (and, later, a capability's). */
#define THREAD_PRIO_DEFAULT  16
#define THREAD_PRIO_USER_MAX 24

/* A process's or a thread's name (process_create, thread_create): at most
 * this many bytes. */
#define PROCESS_NAME_MAX 31

/* channels --------------------------------------------------------------------
 * A message carries at most CHANNEL_MAX_BYTES bytes and CHANNEL_MAX_HANDLES
 * handles (more: ERR_OUT_OF_RANGE). At most CHANNEL_MAX_QUEUED messages wait
 * on one end (and on one marked CHANNEL_BYTE_BOUND, about
 * CHANNEL_BOUND_BYTES of bytes); a write beyond that fails with
 * ERR_SHOULD_WAIT. A queued
 * message is charged to its writer's job's JOB_LIMIT_MSG_BYTES: its size,
 * plus JOB_OBJECT_BYTES for each handle it carries (the object it may be
 * all that keeps alive), until it is read or dropped with the reading end,
 * also after the writer has closed its end. */
#define CHANNEL_MAX_BYTES   65536
#define CHANNEL_MAX_HANDLES 64
#define CHANNEL_MAX_QUEUED  1024
#define JOB_OBJECT_BYTES    1024

/* The first 4 bytes of a message are its txid. channel_call stamps its own
 * on the request, with this bit set (the rest a counter of the channel
 * pair's), and takes the reply that echoes it. A program that picks its
 * own txids (requests it sends with channel_write and doesn't wait for)
 * keeps this bit clear, so no reply to them is ever taken for a call's. */
#define CHANNEL_TXID_KERNEL 0x80000000u

/* channel_set_flags (abi/syscalls.def): the end receives no handles; the
 * end queues at most about CHANNEL_BOUND_BYTES of message bytes (a write
 * that finds that much queued is ERR_SHOULD_WAIT, so the queue holds less
 * than CHANNEL_BOUND_BYTES + CHANNEL_MAX_BYTES). A pipe's read end is
 * bounded: 1024 messages of 64 KiB would be 64 MiB, the root job's whole
 * message budget, so a pipe whose reader lags would fail its writer (and
 * every other writer in the system) instead of making it wait. */
#define CHANNEL_NO_HANDLES  (1u << 0)
#define CHANNEL_BYTE_BOUND  (1u << 1)
#define CHANNEL_BOUND_BYTES (1u << 20)

/* argument structs -----------------------------------------------------------
 * Calls with more than six arguments take a pointer to one of these. The
 * syscall dispatcher copies the struct into the kernel before the call, so
 * its fields are read once; the buffers they point at are user memory. */

/* The three channel structs name their fields alike. What the call sends
 * is wr_*: wr_len bytes at wr_bytes, wr_nhandles handles at wr_handles.
 * What it receives is rd_*: room for rd_cap bytes at rd_bytes and rd_hcap
 * handles at rd_handles; rd_actual and rd_hactual are the user addresses
 * of two uint32_t that get the message's sizes (0: not wanted). A caller
 * that keeps those two in a row (rd_hactual = rd_actual + 4) gets both in
 * one copy. */

struct channel_read_args {
    handle_t h;                /* the channel endpoint to read */
    uint32_t rd_cap;           /* size of the bytes buffer */
    uint64_t rd_bytes;         /* user address: rd_cap bytes */
    uint64_t rd_handles;       /* user address: rd_hcap handle_t */
    uint32_t rd_hcap;          /* entries in the handles buffer */
    uint32_t reserved;         /* 0 */
    uint64_t rd_actual;        /* user address of a uint32_t, or 0 */
    uint64_t rd_hactual;       /* user address of a uint32_t, or 0 */
};

struct channel_call_args {
    handle_t h;                /* the channel endpoint to call on */
    uint32_t wr_len;           /* request bytes (>= 4: the txid goes first) */
    uint64_t wr_bytes;         /* user address of the request */
    uint64_t wr_handles;       /* user address: wr_nhandles handle_t to send */
    uint32_t wr_nhandles;      /* handles to send */
    uint32_t rd_cap;           /* reply buffer bytes */
    uint64_t rd_bytes;         /* user address of the reply buffer */
    uint64_t rd_handles;       /* user address: rd_hcap handle_t for the reply */
    uint32_t rd_hcap;          /* entries in the reply handle buffer */
    uint32_t flags;            /* CHANNEL_CALL_*; any other bit: ERR_INVALID_ARGS */
    uint64_t rd_actual;        /* user address of a uint32_t, or 0 */
    uint64_t rd_hactual;       /* user address of a uint32_t, or 0 */
    uint64_t deadline_ns;      /* absolute, uptime clock; UINT64_MAX = forever.
                                * With CHANNEL_CALL_TIMEOUT: ns from now */
};

/* channel_call flags. TIMEOUT: deadline_ns is a timeout, which the kernel
 * turns into a deadline from its own clock when the call starts, so a
 * caller needs no clock_get first. UINT64_MAX (or a sum past it) is still
 * forever; 0 times out at once unless the reply is already there. */
#define CHANNEL_CALL_TIMEOUT (1u << 0)

/* channel_reply_wait: a server's reply to one call and its wait for the
 * next request in one system call (abi/syscalls.def has the rules). The
 * fields marked "channel" are for a channel end in `wait`, "port" for a
 * port there; the other form's must be 0. */
struct channel_reply_wait_args {
    handle_t h;                /* the channel end to reply on; HANDLE_INVALID: no reply */
    uint32_t wr_len;           /* reply bytes (the call's txid first, as every reply) */
    uint64_t wr_bytes;         /* user address of the reply */
    uint64_t wr_handles;       /* user address: wr_nhandles handle_t sent with the reply */
    uint32_t wr_nhandles;      /* handles sent with it */
    handle_t wait;             /* where the next request comes from: a channel end or a port */
    uint64_t mark;             /* user address of a uint64_t set to 1 once the reply went
                                * out, before the wait; 0: none */
    uint64_t reply_status;     /* user address of an int32_t: the reply's status, written
                                * as the call returns when there was a reply to send;
                                * 0: none. At rd_actual - 4, with rd_hactual at
                                * rd_actual + 4, the three go out in one copy */
    uint32_t rd_cap;           /* channel: room for the request's bytes */
    uint32_t rd_hcap;          /* channel: entries in the handles buffer */
    uint64_t rd_bytes;         /* channel: user address: rd_cap bytes */
    uint64_t rd_handles;       /* channel: user address: rd_hcap handle_t */
    uint64_t rd_actual;        /* channel: user address of a uint32_t, or 0 */
    uint64_t rd_hactual;       /* channel: user address of a uint32_t, or 0 */
    uint64_t packet;           /* port: user address of a struct port_packet */
    uint32_t flags;            /* CHANNEL_REPLY_WAIT_*; any other bit: ERR_INVALID_ARGS */
    uint32_t reserved;         /* 0 */
    uint64_t deadline_ns;      /* absolute, uptime clock; UINT64_MAX = forever. With
                                * CHANNEL_REPLY_WAIT_TIMEOUT: ns from now */
};

/* channel_reply_wait flags. TIMEOUT: deadline_ns is a timeout, as
 * CHANNEL_CALL_TIMEOUT's. */
#define CHANNEL_REPLY_WAIT_TIMEOUT (1u << 0)

/* hardware -------------------------------------------------------------------
 * Resources are the authority over hardware: userboot gives init the root,
 * which slices it (resource_create) for devmgr; devmgr turns RES_PCI into
 * one RES_PCI_DEV per function (pci_device_open) and each BAR into a
 * RES_MMIO (pci_bar_resource) for the driver. */

#define RES_ROOT     1   /* everything */
#define RES_MMIO     2   /* physical range [base, base + size); never RAM */
#define RES_PCI      3   /* all PCI functions: pci_enum, pci_device_open */
#define RES_PCI_DEV  4   /* one PCI function (index from pci_enum) */

/* vmo_create_physical cache types. */
#define VMO_CACHE_WB 0
#define VMO_CACHE_UC 1   /* device registers */
#define VMO_CACHE_WC 2   /* framebuffers */

/* pci_enum. BAR flags: PCI_BAR_* ; size 0 = BAR not implemented. */
#define PCI_BAR_MMIO     (1u << 0)
#define PCI_BAR_64       (1u << 1)
#define PCI_BAR_PREFETCH (1u << 2)
#define PCI_BAR_IO       (1u << 3)   /* port I/O: never handed to drivers */
#define PCI_BAR_UNSIZED  (1u << 4)   /* not sized (boot display, bridges): size unknown */

#define PCI_INFO_BRIDGE  (1u << 0)
#define PCI_INFO_DISPLAY (1u << 1)   /* holds the boot framebuffer: never touched */

struct pci_dev_info {
    uint16_t segment;          /* PCI segment group */
    uint8_t  bus, dev, fn;     /* the function's address on the bus */
    uint8_t  header_type;      /* config header type (bit 7: multi-function) */
    uint16_t vendor, device;   /* PCI vendor and device ids */
    /* class code, subclass, programming interface, revision */
    uint8_t  class_code, subclass, prog_if, revision;
    uint16_t reserved;         /* 0 */
    uint32_t flags;            /* PCI_INFO_* */
    uint16_t msi_vectors;      /* 0 = no MSI capability */
    uint16_t msix_vectors;     /* 0 = no MSI-X capability */
    struct {
        uint64_t phys;         /* physical base address */
        uint64_t size;         /* bytes; 0 = no BAR or not sized */
        uint32_t flags;        /* PCI_BAR_* */
        uint32_t reserved;     /* 0 */
    } bar[6];                  /* a 64-bit BAR fills bar[i]; bar[i + 1] is empty */
    /* The function's DMA quarantine (pins a dma_cap still held when it
     * closed; see abi/syscalls.def dma_cap_bus_master), in pages. */
    uint32_t dma_quarantined;  /* held right now */
    uint32_t dma_changed;      /* since boot: released pages found written while held */
};

/* interrupt_create_msi flags */
#define IRQ_MSIX  (1u << 0)   /* use MSI-X vector `index` (else MSI, index 0) */

/* vmo_pin: pin [offset, offset + len) of a VMO for the device of a
 * dma_cap (abi/syscalls.def has the rules). Each pin costs the dma_cap's
 * job (the driver's), not the VMO's holder, one JOB_LIMIT_HANDLES unit
 * until it goes. */
struct vmo_pin_args {
    handle_t vmo;              /* the VMO */
    handle_t dma;              /* a dma_cap bound to a function */
    uint64_t offset;           /* page-aligned */
    uint64_t len;              /* page-aligned: 1 .. VMO_PIN_MAX_PAGES pages */
    uint64_t addrs;            /* user address: len / 4096 uint64_t, one device address a page */
    uint64_t pin_id;           /* user address of a uint64_t: the pin's id, for vmo_unpin */
    uint32_t flags;            /* VMO_PIN_*; any other bit: ERR_INVALID_ARGS */
    uint32_t reserved;         /* 0 */
};

/* The device only reads the pages (a transmit buffer, a sound stream):
 * with the IOMMU translating (sys_info's SYSINFO_IOMMU: the dma_cap has a
 * domain) they are mapped for it read only, so its writes there are
 * refused and faulted, and the pin needs only RIGHT_READ on the VMO.
 * Without the IOMMU nothing stops a device's write, so the pin needs
 * RIGHT_WRITE as any other. A page is pinned through one dma_cap one way
 * at a time: a read-only pin of a page that cap has pinned writable (or
 * the reverse) is ERR_BAD_STATE. With or without the IOMMU a read-only pin
 * commits nothing: every page of its range must be committed already
 * (vmo_commit by the VMO's holder, a writable pin, or a contiguous VMO),
 * else ERR_BAD_STATE and nothing happens. */
#define VMO_PIN_READ_ONLY (1u << 0)

/* pagers (abi/syscalls.def 160-164) -------------------------------------------
 * A pager (OBJ_PAGER) supplies the pages of the VMOs made from it (pager
 * VMOs), each under a key the pager chooses: a fault or a vmo_read on an
 * absent page queues a request in the pager, which the pager reads with
 * pager_next and answers with pager_supply or pager_op. Requests stay in
 * the pager object until they are answered, so a pager outlives its
 * process: a successor takes the delivered ones again (PAGER_NEXT_AGAIN). */

/* pager_create's handle: the pager's holder reads requests (READ), makes
 * VMOs and answers (WRITE), and binds it to a port (WAIT: SIG_READABLE
 * while an undelivered request is queued). */
#define PAGER_RIGHTS     (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
/* pager_create_vmo's handle: read and map only. Never RIGHT_WRITE (the
 * pages are the pager's), RIGHT_RESIZE (the size is the pager's) or
 * RIGHT_EXEC. */
#define PAGER_VMO_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP)

/* pager_create's answer time: how long a request may wait for its answer,
 * 1 ms .. 600 s (ERR_OUT_OF_RANGE outside). */
#define PAGER_ANSWER_MIN_NS 1000000ull
#define PAGER_ANSWER_MAX_NS 600000000000ull

/* pager_next's flag: first put every delivered request that has no answer
 * yet back in the queue (a successor's first call). */
#define PAGER_NEXT_AGAIN (1u << 0)

/* pager_supply's most bytes in one call (1 MiB). */
#define PAGER_SUPPLY_MAX (1u << 20)

/* A read request asks for the page touched and the absent pages after it,
 * up to this many in all (64 KiB), stopping at a present page, a page
 * another request covers, or the VMO's end. */
#define PAGER_REQ_PAGES 16u

/* pager_op's ops. */
#define PAGER_OP_FAIL       1   /* fail the range's requests with arg (ERR_IO or ERR_OUT_OF_RANGE) */
#define PAGER_OP_INVALIDATE 2   /* drop the range's present pages: the next access asks again */
#define PAGER_OP_SET_SIZE   3   /* offset is the new size: grow, or shrink (past the end: bus error) */
#define PAGER_OP_DETACH     4   /* cut the key's VMO off for good, or take its PAGER_REQ_GONE */

/* struct pager_request's kinds. */
#define PAGER_REQ_READ 1   /* supply [offset, offset + len), or fail it */
#define PAGER_REQ_GONE 2   /* the key's VMO has no handle and no mapping left */

struct pager_request {
    uint64_t key;        /* the VMO's key */
    uint64_t offset;     /* bytes, page-aligned */
    uint64_t len;        /* bytes, page-aligned; 0 for PAGER_REQ_GONE */
    uint32_t kind;       /* PAGER_REQ_* */
    uint32_t reserved;   /* 0 */
};

/* vmo_make_exec_verified (abi/syscalls.def 59): the largest pager VMO it
 * takes (a program's limit, spawn's from a file), and its table's entry
 * for each page: the SHA-256 of the page's 4096 bytes (the last page's
 * zero past the file's end), page 0's at byte 0 of the table. */
#define VMO_VERIFIED_MAX        (64ull << 20)
#define VMO_VERIFIED_HASH_BYTES 32u

/* console services (abi/syscalls.def 110-119) --------------------------------- */

/* The longest argument each takes, in bytes: debug_write's and
 * debug_report's text, serial_write's, debug_command's command, the log
 * file's name (klog_name), and kexec_load's command line. */
#define DEBUG_WRITE_MAX    4096
#define SERIAL_WRITE_MAX   4096
#define DEBUG_COMMAND_MAX  63
#define KLOG_NAME_MAX      31
#define KEXEC_CMDLINE_MAX  511

/* klog_lines: who wrote the log line starting at byte `pos` of the log. */
#define KLOG_WRITER_KERNEL  0ull            /* the kernel itself (never in a mark) */
#define KLOG_WRITER_UNKNOWN 0xffffffffffffffffull   /* a copy (the panic screen's) */
struct klog_line {
    uint64_t pos;         /* where the line starts (klog_read's positions) */
    uint64_t writer;      /* the writing process's koid, or KLOG_WRITER_* */
};

/* framebuffer_take: the boot framebuffer's geometry. The VMO covers
 * pitch * height bytes rounded up to a page; pixel (x, y) is the uint32_t
 * at y * pitch + x * 4, colour channels at the given bit shifts (8 bits
 * each). */
struct fb_info {
    uint32_t width, height;  /* pixels */
    uint32_t pitch;          /* bytes per line */
    uint32_t bpp;            /* 32 */
    /* bit positions of the 8-bit colour channels; reserved is 0 */
    uint8_t  red_shift, green_shift, blue_shift, reserved;
    uint32_t reserved2;      /* 0 */
    uint64_t size;           /* bytes of the VMO */
};

/* system information for the shell (abi/syscalls.def 130-133) ----------------
 * Read-only views for uname / free / lscpu / top / ps / date. Each call
 * needs a power on a RES_ROOT handle: RIGHT_ROOT_SYSINFO, or for rtc_read
 * RIGHT_ROOT_CLOCK. */

#define SYSINFO_HYBRID  (1u << 0)   /* the CPU has P-cores and E-cores */
#define SYSINFO_KTESTS  (1u << 1)   /* the kernel was built with its tests */
/* An IOMMU translates DMA (the boot word iommu=on): a driver's device
 * reaches only what the driver pinned, and a dead driver's pins are freed
 * once the IOMMU took them away, not quarantined. */
#define SYSINFO_IOMMU   (1u << 2)

struct sys_info {
    char     version[32];        /* "0.0.20-m7" */
    char     cpu_vendor[16];     /* "GenuineIntel" */
    char     cpu_brand[48];      /* CPUID's brand string, trimmed */
    uint64_t uptime_ns;          /* since boot */
    uint64_t tsc_hz;             /* TSC ticks per second */
    uint64_t mem_total_pages;    /* 4 KiB pages the kernel manages */
    uint64_t mem_free_pages;     /* of those, free now */
    uint64_t stack_cache_pages;  /* free pages parked in the thread stack cache */
    uint32_t cpu_count;          /* CPUs online */
    uint32_t flags;              /* SYSINFO_* */
};

#define CPU_TYPE_UNKNOWN     0
#define CPU_TYPE_PERFORMANCE 1   /* Intel P-core */
#define CPU_TYPE_EFFICIENCY  2   /* Intel E-core */

/* cpu_stat: one CPU. idle_ns is how long its idle thread has run (while
 * idle the CPU halts or spins briefly), so busy = elapsed - idle. */
struct cpu_stat {
    uint32_t index;       /* 0 = the boot CPU */
    uint32_t apic_id;     /* local APIC id */
    uint32_t type;        /* CPU_TYPE_* */
    uint32_t core_id;     /* APIC id without the SMT bits: HT siblings share it */
    uint32_t smt_id;      /* thread within its core */
    uint32_t online;      /* 1 if the CPU is up */
    uint64_t idle_ns;     /* time its idle thread has run */
    uint64_t switches;    /* context switches */
};

/* proc_list: one process of the caller's job tree (from its root job). */
struct proc_stat {
    uint64_t koid;        /* the process's kernel object id */
    uint64_t job_koid;    /* its job's kernel object id */
    uint64_t cpu_ns;      /* CPU time of all its threads, ever */
    uint64_t job_pages;   /* pages charged to its job (and the jobs below it) */
    uint32_t state;       /* PROCESS_* */
    uint32_t threads;     /* live threads */
    uint32_t depth;       /* its job's depth below the root job */
    uint32_t reserved;    /* 0 */
    char     name[32];    /* the process's name, NUL-terminated */
};

/* rtc_read: the CMOS real-time clock as it stands, converted to binary and
 * 24 hours. PCs keep it in UTC or (Windows) in local time: the RTC says
 * nothing about which. */
struct rtc_time {
    uint16_t year;                  /* 2000..2099 */
    uint8_t  month;                 /* 1..12 */
    uint8_t  day;                   /* 1..31 */
    uint8_t  hour, minute, second;  /* 24-hour clock */
    uint8_t  status_b;              /* register B as read (bit 2 binary, bit 1 24-hour) */
    uint64_t uptime_ns;             /* when it was read */
};

/* wallclock_get / wallclock_set: the wall clock. The kernel keeps UTC as an offset
 * from its uptime. At boot it has only the real-time clock's reading,
 * taken as UTC (WALLCLOCK_RTC: the RTC keeps UTC or, on a PC that also runs
 * Windows, local time, and says nothing about which); init then sets it
 * (WALLCLOCK_SET) from the RTC as its settings say, with the time zone every
 * program shows times in: a name libos understands (<wallclock.h>). A
 * setter that took the time from the network (bin/sntp) passes
 * WALLCLOCK_NET, the one flag wallclock_set takes; it is kept until a set
 * without it, so init knows not to put the RTC's reading back. */
#define WALLCLOCK_ZONE_MAX 32
struct wall_clock {
    int64_t  utc_ns;                /* ns since 1970-01-01 00:00 UTC */
    uint64_t uptime_ns;             /* the uptime utc_ns is at */
    uint32_t flags;                 /* WALLCLOCK_* (wallclock_set: 0 or WALLCLOCK_NET) */
    uint32_t reserved;              /* 0 */
    char     zone[WALLCLOCK_ZONE_MAX];  /* the time zone's name, NUL-terminated ("": none given) */
};
#define WALLCLOCK_RTC 1u   /* the RTC's reading at boot as if UTC: nobody has set the clock */
#define WALLCLOCK_SET 2u   /* set by wallclock_set: UTC */
#define WALLCLOCK_NET 4u   /* ... from the network's time (SNTP), not the RTC */

/* clock_page: the time page, one page every process may map (read only:
 * the handle has TIME_PAGE_RIGHTS), from which clock_get's uptime and
 * wallclock_get's UTC are read without a system call. The TSC fields are
 * set once at boot; the wall clock's two change at every wallclock_set,
 * under the sequence word: odd while the kernel writes them, bumped
 * again (to even) when it is done. A reader takes time_page_begin's
 * word, reads them (each with one 64- or 32-bit load), and reads again
 * if time_page_retry says the word moved meanwhile.
 *
 * The uptime at TSC value t is time_page_uptime(p, t): exactly what
 * clock_get answers for the same t, floor((t - tsc_boot) * 1e9 /
 * tsc_hz), worked out from mult and shift (an estimate at most 2 ns
 * under) and put right with tsc_hz. Read the TSC with `lfence; rdtsc`,
 * so the read is not made before the instructions ahead of it. UTC is
 * the uptime plus wall_offset when wall_flags isn't 0. */
struct time_page {
    uint32_t seq;           /* odd: wall_* being written; read with time_page_begin */
    uint32_t flags;         /* TIME_PAGE_* (fixed at boot) */
    uint64_t tsc_hz;        /* TSC ticks a second (the kernel's tsc_hz) */
    uint64_t tsc_boot;      /* the TSC at uptime 0 */
    uint64_t mult;          /* uptime ns ~ ((t - tsc_boot) * mult) >> shift, rounded down */
    uint32_t shift;         /* 1 .. 64 */
    uint32_t wall_flags;    /* the wall clock's WALLCLOCK_* (0: no clock); seq */
    int64_t  wall_offset;   /* UTC ns = uptime ns + this; seq */
};
/* The kernel's clock is this conversion of the TSC: a reader may use the
 * page instead of the calls. Without it, use clock_get and wallclock_get. */
#define TIME_PAGE_TSC       1u
/* CPUID says the TSC is invariant (it ticks at one rate in every power
 * state). For information: the kernel's own clock is the TSC either way,
 * so the page agrees with clock_get whether it is set or not. */
#define TIME_PAGE_INVARIANT 2u
/* clock_page's handle: read and map. Never RIGHT_WRITE or RIGHT_RESIZE:
 * the page is the kernel's. */
#define TIME_PAGE_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP)

/* The sequence word to read the wall_* fields under: it waits out a write
 * in progress (a few stores with interrupts off). */
static inline uint32_t time_page_begin(const struct time_page *p)
{
    uint32_t s;
    while ((s = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE)) & 1) {
#ifdef __x86_64__   /* (host tools on other machines include this header too) */
        __asm__ volatile("pause");
#endif
    }
    return s;
}

/* Non-zero if the wall_* fields read since time_page_begin's word s may be
 * torn: read them again. */
static inline int time_page_retry(const struct time_page *p, uint32_t s)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the reads above before the word's */
    return __atomic_load_n(&p->seq, __ATOMIC_RELAXED) != s;
}

/* The uptime in ns at TSC value tsc, as clock_get computes it, for any
 * uptime below 2^64 ns (584 years). The estimate from mult is at most
 * (tsc - tsc_boot) >> shift, plus 1, ns under (2 with shift 64, which
 * every TSC from 1 GHz up has): the loop adds those back one at a time.
 * Only for a page with TIME_PAGE_TSC (tsc_hz isn't 0). */
static inline uint64_t time_page_uptime(const struct time_page *p, uint64_t tsc)
{
    uint64_t d = tsc - p->tsc_boot;
    uint64_t ns = (uint64_t)(((unsigned __int128)d * p->mult) >> p->shift);
    unsigned __int128 rem = (unsigned __int128)d * 1000000000u - (unsigned __int128)ns * p->tsc_hz;
    while (rem >= p->tsc_hz) {
        ns++;
        rem -= p->tsc_hz;
    }
    return ns;
}

/* random_get: the most bytes one call gives (libos's os_random asks in
 * pieces this size). The kernel makes them on its stack. */
#define RANDOM_GET_MAX 256
