/* userboot: the kernel's own tiny program loader (M5). It starts init from
 * bootfs; after that, programs are loaded by libos in user space. Kernel
 * tests and benchmarks use it too, to start user programs directly. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/handle.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/status.h>

/* Where userboot puts a program's stack (and the guard page below it). */
#define USERBOOT_STACK_TOP   0x00007ff000000000ull
#define USERBOOT_STACK_PAGES 32   /* 128 KiB */

/* An extra handle for the startup message: role (enum startup_role) and
 * the khandle, which the spawn consumes (moves or releases) either way. */
struct userboot_handle {
    uint32_t       role;
    struct khandle kh;
};

/* Load bootfs file `path` (a static ELF) into a new process in `job` and
 * start it with argv[0..argc) and a startup message carrying
 * SELF_PROCESS, SELF_VMAR, SELF_THREAD, JOB, BOOTFS (read-only: never
 * RIGHT_WRITE, since bootfs pages are not write-protected) and the extra
 * handles. Text and read-only data map the bootfs pages directly; writable
 * data is copied into a fresh VMO charged to the job, bss zero-filled.
 * mask (NULL = any CPU) pins the first thread. *out gets a reference to
 * the process. Needs a context that may sleep. */
status_t userboot_spawn(const char *path, const char *const *argv, unsigned argc,
                        struct job *job, struct userboot_handle *extra, unsigned nextra,
                        const cpumask_t *mask, struct process **out);

/* A new root job with the limits init gets (most of free memory, plenty
 * of handles and threads). The caller gets the only reference. */
status_t userboot_root_job(struct job **out);

/* Boot: run bin/init under a new root job, wait up to timeout_s seconds
 * for it to exit, and report how it went. Returns true if it exited 0 and
 * left its job with nothing charged. arg (may be NULL) becomes init's
 * argv[1]: a mode (e.g. "xhcitest") that init runs instead of init.cfg. */
bool userboot_run_init(uint64_t timeout_s, const char *arg);
