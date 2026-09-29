/* Drivers built into the kernel (M6, Track D; kernel/drivers/driver_kernel.c).
 *
 * Every driver in drivers/<name>/ is compiled twice: into a user ELF in
 * bootfs (drv/<name>, run as a process) and into jamos.elf, where it runs
 * as a *kernel process*: a struct process with its own handle table and
 * job but no address space (process_create_kernel), whose threads run the
 * driver in ring 0. Its <jam/driver.h> calls are the handle-level kernel
 * functions on that process's table, so handle rights and job charges work
 * exactly as for the process build:
 *   - handles, channels, ports, waits, VMOs: the sys_* layer (<jam/sys.h>)
 *     and the handle layer, with kernel pointers;
 *   - drv_vmo_map / drv_mmio_map map into the kernel's vmap area (there is
 *     no user half); the mappings are the driver's and go when it dies;
 *   - drv_malloc: a per-driver heap in a VMO charged to its job, mapped in
 *     64 KiB chunks as it grows (never kmalloc);
 *   - drv_log / drv_report: "[name] ..." lines through the process's
 *     debug output (rate limit and RESULTS cap as for processes).
 * A kill (process_kill, job_kill) cancels the driver's threads: a wait
 * returns at once and the thread leaves at its next drv_* call (the kernel
 * build's "return to user" check). A kernel driver that never calls drv_*
 * can't be killed: the kernel build trusts its drivers that far.
 *
 * The driver's code sees none of this: the build compiles it with only
 * driver.h on the include path, renames its driver_main to
 * driver_main__<name>, hides every other symbol it defines, and
 * tools/checkdriver.py rejects anything it uses that driver.h doesn't
 * provide. */
#pragma once

#include <stdint.h>
#include <jam/handle.h>
#include <jam/process.h>
#include <jam/status.h>

struct driver_start;
struct job;

typedef int (*driver_main_fn)(const struct driver_start *s);

/* The kernel builds of drivers/<name>/ (generated table, build/kdrivers.c). */
struct kdriver {
    const char    *name;
    driver_main_fn main;
};
extern const struct kdriver kdrivers[];   /* ends with { NULL, NULL } */
driver_main_fn driver_kernel_find(const char *name);   /* NULL if not built in */

/* A handle for the driver, by role (DR_* in <jam/driver.h>). */
struct driver_kernel_handle {
    uint32_t       role;
    struct khandle kh;    /* consumed by driver_kernel_start either way */
};

/* Start fn as driver `name` in a new kernel process in `job` (charged like
 * any process: the process, its handle slots, its threads' kernel stacks,
 * its heap and VMOs). The handles go into the process's table and
 * driver_start lists them by role. *out (may be NULL) gets a reference to
 * the process: wait for SIG_TERMINATED, read its exit code with
 * process_get_info (driver_main's return value or drv_exit's code), kill it
 * with process_kill. Needs a context that may sleep. */
status_t driver_kernel_start(const char *name, driver_main_fn fn, struct driver_kernel_handle *hs,
                             unsigned n, struct job *job, struct process **out);

/* Needed from Track C (<jam/resource.h>) for drv_mmio_map: the physical
 * range a RES_MMIO resource covers. A weak ERR_NOT_SUPPORTED default lives
 * in driver_kernel.c until the real one exists. */
struct kobject;
status_t resource_mmio_range(struct kobject *res, uint64_t *base, uint64_t *size);
