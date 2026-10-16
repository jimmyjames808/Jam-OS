/* Compile-time checks that the user-visible ABI (<jam/abi.h>,
 * <jam/startup.h>, <jam/bootfs.h>) matches what the kernel uses. User code
 * is built from copies of these headers, so a layout change here must be a
 * deliberate ABI change, not an accident. No code. */
#include <stddef.h>
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/kexec_handoff.h>
#include <jam/percpu.h>
#include <jam/sched.h>   /* its DEADLINE_NEVER must match abi.h's */
#include <jam/startup.h>

_Static_assert(VMAR_READ == ASPACE_READ && VMAR_WRITE == ASPACE_WRITE &&
               VMAR_EXEC == ASPACE_EXEC && VMAR_FIXED == ASPACE_FIXED &&
               VMAR_KEPT_ONLY == ASPACE_KEPT_ONLY && VMAR_REPLACE == ASPACE_REPLACE,
               "vmar_map flags are passed straight to aspace_map");

_Static_assert(sizeof(struct time_page) == 48 && offsetof(struct time_page, tsc_hz) == 8 &&
                   offsetof(struct time_page, shift) == 32 &&
                   offsetof(struct time_page, wall_offset) == 40,
               "time_page layout");

_Static_assert(sizeof(struct port_packet) == 48, "port_packet layout");
_Static_assert(offsetof(struct port_packet, signal.count) == 24, "port_packet layout");

_Static_assert(sizeof(struct channel_read_args) == 48, "channel_read_args layout");
_Static_assert(offsetof(struct channel_read_args, rd_hcap) == 24 &&
                   offsetof(struct channel_read_args, rd_hactual) == 40,
               "channel_read_args layout");
_Static_assert(sizeof(struct channel_call_args) == 80, "channel_call_args layout");
_Static_assert(offsetof(struct channel_call_args, rd_cap) == 28 &&
                   offsetof(struct channel_call_args, flags) == 52 &&
                   offsetof(struct channel_call_args, rd_hactual) == 64 &&
                   offsetof(struct channel_call_args, deadline_ns) == 72,
               "channel_call_args layout");

_Static_assert(sizeof(struct channel_reply_wait_args) == 112, "channel_reply_wait_args layout");
_Static_assert(offsetof(struct channel_reply_wait_args, wait) == 28 &&
                   offsetof(struct channel_reply_wait_args, rd_cap) == 48 &&
                   offsetof(struct channel_reply_wait_args, packet) == 88 &&
                   offsetof(struct channel_reply_wait_args, deadline_ns) == 104,
               "channel_reply_wait_args layout");

_Static_assert(sizeof(struct startup_msg) == 92, "startup_msg layout");
_Static_assert(sizeof(struct bootfs_header) == 24, "bootfs header layout");
_Static_assert(sizeof(struct bootfs_entry) == 72, "bootfs entry layout");

_Static_assert(sizeof(struct pci_dev_info) == 176 && offsetof(struct pci_dev_info, flags) == 16 &&
                   offsetof(struct pci_dev_info, bar) == 24,
               "pci_dev_info layout");
_Static_assert(sizeof(struct job_info) == 88, "job_info layout");
_Static_assert(sizeof(struct process_info) == 88 && offsetof(struct process_info, pid) == 20 &&
                   offsetof(struct process_info, cpu_ns) == 32 &&
                   offsetof(struct process_info, mappings) == 40 &&
                   offsetof(struct process_info, start_ns) == 48 &&
                   offsetof(struct process_info, name) == 56 &&
                   sizeof(((struct process_info *)0)->name) == PROCESS_NAME_MAX + 1,
               "process_info layout");
_Static_assert(sizeof(struct map_info) == 48 && offsetof(struct map_info, resident) == 32 &&
                   offsetof(struct map_info, perms) == 40,
               "map_info layout");
_Static_assert(sizeof(struct thread_info) == 40 && offsetof(struct thread_info, tid) == 24,
               "thread_info layout");
_Static_assert(sizeof(struct exc_frame) == EXC_FRAME_XSAVE &&
                   offsetof(struct exc_frame, xsave) == EXC_FRAME_XSAVE &&
                   EXC_FRAME_XSAVE % 64 == 0 && offsetof(struct exc_frame, error) == 16 &&
                   offsetof(struct exc_frame, cookie) == 32 &&
                   offsetof(struct exc_frame, r8) == 48 &&
                   offsetof(struct exc_frame, rip) == 48 + 16 * 8 &&
                   offsetof(struct exc_frame, rflags) == 48 + 17 * 8,
               "exc_frame layout: 18 registers in gregs order, the XSAVE area 64-byte aligned");
_Static_assert(sizeof(struct sys_public_info) == 72 &&
                   offsetof(struct sys_public_info, mem_total_pages) == 48 &&
                   offsetof(struct sys_public_info, mem_free_pages) == 64,
               "sys_public_info layout");
_Static_assert(sizeof(struct fb_info) == 32 && offsetof(struct fb_info, size) == 24,
               "fb_info layout");
_Static_assert(sizeof(struct klog_line) == 16 && offsetof(struct klog_line, writer) == 8,
               "klog_line layout");
_Static_assert(sizeof(struct sys_info) == 144, "sys_info layout");
_Static_assert(sizeof(struct cpu_stat) == 40, "cpu_stat layout");
_Static_assert(sizeof(struct proc_stat) == 80, "proc_stat layout");
_Static_assert(sizeof(struct rtc_time) == 16, "rtc_time layout");
_Static_assert(sizeof(struct wall_clock) == 56 && offsetof(struct wall_clock, zone) == 24,
               "wall_clock layout");
_Static_assert(CPU_TYPE_PERFORMANCE == CORE_PERFORMANCE && CPU_TYPE_EFFICIENCY == CORE_EFFICIENCY,
               "cpu_stat.type is enum core_type");

_Static_assert(VMO_CONTIGUOUS == 1u && VMO_DMA32 == 2u && VMO_KEEP_PAGES == 4u &&
                   VMO_MAX_SIZE == 1ull << 47 && VMO_SMALL_MAX_SIZE == 1ull << 36 &&
                   VMO_CONTIGUOUS_MAX == 4ull << 20 &&
                   VMO_PIN_MAX_PAGES == 4096u && VMAR_MAX_MAPPINGS == 65536u &&
                   VMAR_MAX_OVERLAP == 64u,
               "the VMO flags and limits user code sees");
_Static_assert(sizeof(struct vmo_pin_args) == 48 && offsetof(struct vmo_pin_args, flags) == 40,
               "vmo_pin_args layout");

/* The limits user code sees, against the kernel's own sizes. */
_Static_assert(KEXEC_CMDLINE_MAX + 1 == KEXEC_CMDLINE && KLOG_NAME_MAX + 1 <= KEXEC_NAME,
               "kexec_load's and klog_name's limits are what kexec keeps");
_Static_assert(PROCESS_NAME_MAX + 1 == sizeof(((struct proc_stat *)0)->name),
               "proc_stat.name holds a whole name and its NUL");

/* The rights' word: generic rights in bits 0-15, powers in 16-30. */
#define RIGHTS_GENERIC_CHECK (RIGHT_READ | RIGHT_WRITE | RIGHT_EXEC | RIGHT_MAP | RIGHT_DUPLICATE | \
                              RIGHT_TRANSFER | RIGHT_SIGNAL | RIGHT_WAIT | RIGHT_INSPECT |         \
                              RIGHT_MANAGE | RIGHT_SLICE | RIGHT_RESIZE)
_Static_assert(RIGHTS_GENERIC_CHECK == 0xfffu && (RIGHTS_ROOT & 0x8000ffffu) == 0 &&
                   RIGHTS_ROOT == 0xfff0000u,
               "rights: the generic ones in bits 0-11, the powers from bit 16 in order");

_Static_assert(sizeof(struct handle_info) == 48 && offsetof(struct handle_info, queued) == 24 &&
                   offsetof(struct handle_info, peer_queued_bytes) == 32,
               "handle_info layout");
_Static_assert(OBJ_PAGER == 16 && OBJ_TYPE_COUNT == 17, "the object types user code sees");
_Static_assert(sizeof(struct pager_request) == 32 && offsetof(struct pager_request, kind) == 24,
               "pager_request layout");
