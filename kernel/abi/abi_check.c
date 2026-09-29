/* Compile-time checks that the user-visible ABI (<jam/abi.h>,
 * <jam/startup.h>, <jam/bootfs.h>) matches what the kernel uses. User code
 * is built from copies of these headers, so a layout change here must be a
 * deliberate ABI change, not an accident. No code. */
#include <stddef.h>
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/percpu.h>
#include <jam/sched.h>   /* its DEADLINE_NEVER must match abi.h's */
#include <jam/startup.h>

_Static_assert(VMAR_READ == ASPACE_READ && VMAR_WRITE == ASPACE_WRITE &&
               VMAR_EXEC == ASPACE_EXEC && VMAR_FIXED == ASPACE_FIXED,
               "vmar_map flags are passed straight to aspace_map");

_Static_assert(sizeof(struct port_packet) == 48, "port_packet layout");
_Static_assert(offsetof(struct port_packet, signal.count) == 24, "port_packet layout");

_Static_assert(sizeof(struct channel_read_args) == 48, "channel_read_args layout");
_Static_assert(offsetof(struct channel_read_args, actual_handles) == 40,
               "channel_read_args layout");
_Static_assert(sizeof(struct channel_call_args) == 80, "channel_call_args layout");
_Static_assert(offsetof(struct channel_call_args, deadline_ns) == 72, "channel_call_args layout");

_Static_assert(sizeof(struct startup_msg) == 92, "startup_msg layout");
_Static_assert(sizeof(struct bootfs_header) == 24, "bootfs header layout");
_Static_assert(sizeof(struct bootfs_entry) == 72, "bootfs entry layout");

_Static_assert(sizeof(struct sys_info) == 144, "sys_info layout");
_Static_assert(sizeof(struct cpu_stat) == 40, "cpu_stat layout");
_Static_assert(sizeof(struct proc_stat) == 80, "proc_stat layout");
_Static_assert(sizeof(struct rtc_time) == 16, "rtc_time layout");
_Static_assert(CPU_TYPE_PERFORMANCE == CORE_PERFORMANCE && CPU_TYPE_EFFICIENCY == CORE_EFFICIENCY,
               "cpu_stat.type is enum core_type");
