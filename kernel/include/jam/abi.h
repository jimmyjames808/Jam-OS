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
 * the kernel never dereferences them (it copies through usercopy.h). */
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
#define RIGHT_SLICE     (1u << 10)  /* resources: make a smaller resource inside this one */
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
#define SIG_TERMINATED  (1u << 4)   /* processes, threads: gone for good */
#define SIG_INTERRUPT   (1u << 5)   /* interrupt objects: fired, not acked yet */
#define SIG_USER_ALL    0xff000000u /* bits 24-31: free for userspace (sys_object_signal) */

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
    uint16_t segment;
    uint8_t  bus, dev, fn;
    uint8_t  header_type;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if, revision;
    uint32_t flags;           /* PCI_INFO_* */
    uint16_t msi_vectors;     /* 0 = no MSI capability */
    uint16_t msix_vectors;    /* 0 = no MSI-X capability */
    struct {
        uint64_t phys;
        uint64_t size;
        uint32_t flags;       /* PCI_BAR_* */
        uint32_t reserved;
    } bar[6];                 /* a 64-bit BAR fills bar[i]; bar[i + 1] is empty */
    /* The function's DMA quarantine (pins a dma_cap still held when it
     * closed; see abi/syscalls.def dma_cap_bus_master), in pages. */
    uint32_t dma_quarantined; /* held right now */
    uint32_t dma_changed;     /* since boot: released pages found written while held */
};

/* interrupt_create_msi flags */
#define IRQ_MSIX  (1u << 0)   /* use MSI-X vector `index` (else MSI, index 0) */

/* input (abi/idl/input.idl, console.idl) ------------------------------------- */

#define INPUT_KEY_UP     0
#define INPUT_KEY_DOWN   1
#define INPUT_KEY_REPEAT 2

/* The HID boot report's modifier byte. */
#define INPUT_MOD_LCTRL  (1u << 0)
#define INPUT_MOD_LSHIFT (1u << 1)
#define INPUT_MOD_LALT   (1u << 2)
#define INPUT_MOD_LGUI   (1u << 3)
#define INPUT_MOD_RCTRL  (1u << 4)
#define INPUT_MOD_RSHIFT (1u << 5)
#define INPUT_MOD_RALT   (1u << 6)
#define INPUT_MOD_RGUI   (1u << 7)
#define INPUT_MOD_CTRL   (INPUT_MOD_LCTRL | INPUT_MOD_RCTRL)
#define INPUT_MOD_SHIFT  (INPUT_MOD_LSHIFT | INPUT_MOD_RSHIFT)
#define INPUT_MOD_ALT    (INPUT_MOD_LALT | INPUT_MOD_RALT)

/* One message on a console.open_keys channel. */
struct input_key_event {
    uint16_t usage;       /* HID keyboard page usage id, 0 for text from a terminal */
    uint8_t  state;       /* INPUT_KEY_* */
    uint8_t  mods;        /* INPUT_MOD_* */
    uint32_t codepoint;   /* the character typed, or 0 */
};

/* console services (abi/syscalls.def 110-117) --------------------------------- */

/* framebuffer_take: the boot framebuffer's geometry. The VMO covers
 * pitch * height bytes rounded up to a page; pixel (x, y) is the uint32_t
 * at y * pitch + x * 4, colour channels at the given bit shifts (8 bits
 * each). */
struct fb_info {
    uint32_t width, height;
    uint32_t pitch;         /* bytes per line */
    uint32_t bpp;           /* 32 */
    uint8_t  red_shift, green_shift, blue_shift, reserved;
    uint32_t reserved2;
    uint64_t size;          /* bytes of the VMO */
};

/* system information for the shell (abi/syscalls.def 130-133) ----------------
 * Read-only views for uname / free / lscpu / top / ps / date. Each call
 * needs RIGHT_READ on a RES_ROOT handle (the shell's root has it). */

#define SYSINFO_HYBRID  (1u << 0)   /* the CPU has P-cores and E-cores */
#define SYSINFO_KTESTS  (1u << 1)   /* the kernel was built with its tests */

struct sys_info {
    char     version[32];       /* "0.0.20-m7" */
    char     cpu_vendor[16];    /* "GenuineIntel" */
    char     cpu_brand[48];     /* CPUID's brand string, trimmed */
    uint64_t uptime_ns;
    uint64_t tsc_hz;
    uint64_t mem_total_pages;   /* 4 KiB pages the kernel manages */
    uint64_t mem_free_pages;
    uint64_t stack_cache_pages; /* free pages parked in the thread stack cache */
    uint32_t cpu_count;         /* CPUs online */
    uint32_t flags;             /* SYSINFO_* */
};

#define CPU_TYPE_UNKNOWN     0
#define CPU_TYPE_PERFORMANCE 1   /* Intel P-core */
#define CPU_TYPE_EFFICIENCY  2   /* Intel E-core */

/* cpu_stat: one CPU. idle_ns is how long its idle thread has run (while
 * idle the CPU halts or spins briefly), so busy = elapsed - idle. */
struct cpu_stat {
    uint32_t index;       /* 0 = the boot CPU */
    uint32_t apic_id;
    uint32_t type;        /* CPU_TYPE_* */
    uint32_t core_id;     /* APIC id without the SMT bits: HT siblings share it */
    uint32_t smt_id;
    uint32_t online;
    uint64_t idle_ns;
    uint64_t switches;    /* context switches */
};

/* proc_list: one process of the caller's job tree (from its root job). */
struct proc_stat {
    uint64_t koid;
    uint64_t job_koid;
    uint64_t cpu_ns;      /* CPU time of all its threads, ever */
    uint64_t job_pages;   /* pages charged to its job (and the jobs below it) */
    uint32_t state;       /* PROCESS_* */
    uint32_t threads;     /* live threads */
    uint32_t depth;       /* its job's depth below the root job */
    uint32_t reserved;
    char     name[32];
};

/* rtc_read: the CMOS real-time clock as it stands, converted to binary and
 * 24 hours. PCs keep it in UTC or (Windows) in local time: the RTC says
 * nothing about which. */
struct rtc_time {
    uint16_t year;        /* 2000..2099 */
    uint8_t  month;       /* 1..12 */
    uint8_t  day;         /* 1..31 */
    uint8_t  hour, minute, second;
    uint8_t  status_b;    /* register B as read (bit 2 binary, bit 1 24-hour) */
    uint64_t uptime_ns;   /* when it was read */
};
