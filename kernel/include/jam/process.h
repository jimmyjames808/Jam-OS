/* Processes, user threads and jobs (M5).
 *
 * A process is a handle table, an address space, a job and a set of user
 * threads. A user thread is a scheduler thread (struct thread) plus the
 * OBJ_THREAD object that handles name it (struct uthread). A job holds
 * resource limits and the current usage of every process in it and in its
 * child jobs (<jam/abi.h> has the user-visible rules).
 *
 * Lifecycle of a process: NEW (created; the parent maps its program and
 * makes its first thread) -> RUNNING (process_start) -> DYING (process_kill,
 * process_exit, a fatal fault, or its last thread left) -> DEAD. On DYING
 * every thread is cancelled (thread_cancel): one in a cancellable wait gets
 * ERR_CANCELED and one in user mode is stopped at its next kernel entry
 * (the return-to-user check). Each thread leaves through
 * uthread_exit_current, which drops the thread's own address-space
 * reference first; the LAST one to leave closes the handle table, drops the
 * process's address-space reference and signals SIG_TERMINATED. So when
 * SIG_TERMINATED is seen, no thread of the process is left in the kernel,
 * its handles are closed and its job has been credited for them, and its
 * address space is gone unless someone else still holds a vmar handle to
 * it. A process killed before it ever had a thread is torn down by its
 * killer.
 *
 * Locking: each object's own lock (classes "process", "thread", "job").
 * "process" ranks above "runqueue" (kill cancels threads under it) and is
 * never held across anything that sleeps. The process's `setup` mutex
 * ("process setup") serialises process_start, the only time another
 * process's code inserts into this handle table, with the teardown that
 * destroys the table. Job usage counters are lock-free atomics, so charges
 * can be made under any lock. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/handle.h>
#include <jam/object.h>
#include <jam/sched.h>
#include <jam/status.h>

struct aspace;
struct job;
struct process;
struct uthread;

#define PROCESS_NAME_MAX 32   /* bytes including the NUL */

#define PROCESS_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
#define THREAD_RIGHTS  (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
/* JOB_RIGHTS / JOB_RIGHTS_OWN: <jam/abi.h> */

/* ---- jobs ------------------------------------------------------------------ */

/* What the kernel memory a job pays for is charged as (review R6):
 *   JOB_LIMIT_PAGES      VMO pages and tables, a process's PML4, page tables
 *                        and mapping structs, and UTHREAD_KMEM_PAGES for every
 *                        running user thread (its kernel stack and XSAVE area);
 *   JOB_LIMIT_HANDLES    handle slots, and one unit for each small object that
 *                        can outlive the handles to it: every job (charged to
 *                        its parent), process and VMO. So a unit stands for at
 *                        most JOB_OBJECT_BYTES of kernel memory;
 *   JOB_LIMIT_MSG_BYTES  channel messages and port packets and bindings (the
 *                        sender's / binder's job), plus JOB_OBJECT_BYTES for
 *                        each handle a message carries or object a binding
 *                        watches, since that may be what keeps it alive.
 * userboot_root_job sizes the handle and message limits so that everything
 * fits in memory next to the page limit and the kernel's reserve. */
#define JOB_OBJECT_BYTES   1024
#define UTHREAD_KMEM_PAGES (THREAD_STACK_SIZE / 4096 + 1)

/* A new job under parent (NULL: a root job) with no limits of its own and
 * one reference for the caller. It costs parent one JOB_LIMIT_HANDLES unit
 * until it is destroyed (ERR_NO_RESOURCES over the limit); ERR_OUT_OF_RANGE
 * if it would be JOB_MAX_DEPTH deep. */
status_t job_create(struct job *parent, struct job **out);
static inline struct kobject *job_kobject(struct job *j) { return (struct kobject *)j; }
static inline struct job *job_from_kobject(struct kobject *o)
{
    return o && o->type == OBJ_JOB ? (struct job *)o : NULL;
}
/* kind: JOB_LIMIT_*; value JOB_NO_LIMIT removes the limit. A limit below
 * the current usage just makes new charges fail. */
status_t job_set_limit(struct job *j, uint32_t kind, uint64_t value);
void     job_get_info(struct job *j, struct job_info *out);
uint64_t job_used(struct job *j, uint32_t kind);
/* Charge n units of kind to j and every ancestor, or nothing: OK, or
 * ERR_NO_MEMORY (pages, message bytes) / ERR_NO_RESOURCES (handles,
 * threads) if any of them would go past its limit. A NULL job (kernel
 * objects) is never charged. Lock-free: callable under any lock. */
status_t job_charge(struct job *j, uint32_t kind, uint64_t n);
void     job_uncharge(struct job *j, uint32_t kind, uint64_t n);
/* A process's entry on its job's list of live processes (job.c; under the
 * job's lock). */
struct job_link {
    struct list_node node;
    bool             kill_seen;   /* job_kill has killed it */
};
/* List / unlist a process in j. Attach fails with ERR_BAD_STATE once j has
 * been killed. */
status_t job_attach_process(struct job *j, struct job_link *l);
void     job_detach_process(struct job *j, struct job_link *l);
struct process *process_from_job_link(struct job_link *l);   /* process.c */
/* Kill every process in j and in every job below it, and mark them all
 * killed: from now on they take no new processes or child jobs
 * (ERR_BAD_STATE). Returns once every one of those processes is dead
 * (SIG_TERMINATED); *killed (may be NULL) gets how many it killed. Needs a
 * context that may sleep; ERR_CANCELED if the calling thread is killed
 * meanwhile (e.g. it was in j: everything was still killed). */
status_t job_kill(struct job *j, unsigned *killed);
/* The current thread's process's job (no new reference: valid while the
 * thread runs), NULL for kernel threads. */
struct job *job_current(void);
void job_ref(struct job *j);     /* NULL is a no-op */
void job_unref(struct job *j);   /* NULL is a no-op */
/* M7 (debug_command "ps"): print j's processes and child jobs, recursively,
 * to the kernel log; and the root of j's tree (a new reference; NULL in,
 * NULL out). */
void job_print_tree(struct job *j, unsigned depth);
struct job *job_root_of(struct job *j);
/* The first live process called `name` in j's tree (a reference), or NULL.
 * Processes directly in a strict ancestor job of `spare` (may be NULL) are
 * skipped: debug_command's "kill" never reaches the caller's supervisors. */
struct process *job_find_process(struct job *j, const char *name, struct job *spare);

/* ---- processes ------------------------------------------------------------- */

static inline struct kobject *process_kobject(struct process *p) { return (struct kobject *)p; }
static inline struct process *process_from_kobject(struct kobject *o)
{
    return o && o->type == OBJ_PROCESS ? (struct process *)o : NULL;
}

/* A NEW process in job with an empty address space and handle table; the
 * caller gets the only reference. name is truncated to PROCESS_NAME_MAX-1.
 * It costs job one JOB_LIMIT_HANDLES unit until it is torn down
 * (ERR_NO_RESOURCES) and its address space's PML4 (ERR_NO_MEMORY). */
status_t process_create(struct job *job, const char *name, struct process **out);
/* M6: a *kernel process* (kernel/drivers/driver_kernel.c runs drivers in
 * them): a process like any other (handle table, job, charges, kill,
 * SIG_TERMINATED) but with no address space of its own; its threads run
 * kernel code (process_start_kernel / uthread_start_kernel) on the
 * kernel's page tables. ctx is its owner's (process_kernel_ctx); fini(ctx)
 * runs exactly once, in process teardown after the handle table is closed
 * and before SIG_TERMINATED, in a context that may sleep. A kernel process
 * that is never started must be process_kill'ed (so fini runs). */
status_t process_create_kernel(struct job *job, const char *name, void *ctx,
                               void (*fini)(void *ctx), struct process **out);
void *process_kernel_ctx(struct process *p);   /* NULL for user processes */
struct handle_table *process_handles(struct process *p);
/* The process's address space with a NEW reference (aspace_unref it), or
 * NULL once the process is dead. */
struct aspace *process_aspace(struct process *p);
struct job *process_job(struct process *p);   /* no new reference */
const char *process_name(struct process *p);
void process_get_info(struct process *p, struct process_info *out);
/* The process of the current thread, NULL for kernel threads. */
struct process *process_current(void);
/* debug_write / debug_report: print buf[0..n) (kernel memory) line by line
 * as "[name] line"; with report_it the lines also go into the RESULTS box
 * (without the prefix; a few dozen at most across all processes). Control
 * characters become '?'. Printing happens with no lock held; plain lines
 * are rate-limited per process (100 at once, then 50 a second; the rest
 * are dropped and counted). Returns how many lines were printed. */
size_t process_debug_write(struct process *p, const char *buf, size_t n, bool report_it);

/* Start a NEW process: move *arg0 (if arg0->obj is set) into its handle
 * table and start ut with rdi = that handle's value (0 if none) and rsi =
 * arg1. On failure *arg0 is still the caller's (and the process is NEW
 * again, unless it was killed meanwhile). mask (NULL = any CPU) restricts
 * the thread from the start (kernel benchmarks pin with it). Until it
 * returns, no other thread of p can be started (uthread_start fails with
 * ERR_BAD_STATE), so nothing in p can run before its first thread. */
status_t process_start(struct process *p, struct uthread *ut, uint64_t entry, uint64_t stack,
                       struct khandle *arg0, uint64_t arg1, const cpumask_t *mask);
/* Test hook DBG_PROCESS_START's argument: the window after process_start
 * made p RUNNING and before its first thread exists. `fail` makes that
 * thread's creation fail as if the kernel were out of memory. */
struct dbg_process_start {
    struct process *p;
    bool            fail;
};
/* Kill p: every thread is cancelled and leaves; p goes DEAD (with
 * SIG_TERMINATED) once the last one has. `code` becomes the exit code;
 * `killed` says it was a kill, not an exit. Returns at once (the teardown
 * finishes asynchronously), except for a process with no threads, whose
 * teardown the caller does. Needs a context that may sleep. A no-op on a
 * process that is already dying. */
void process_kill(struct process *p, int64_t code, bool killed);

/* ---- user threads ---------------------------------------------------------- */

static inline struct kobject *uthread_kobject(struct uthread *u) { return (struct kobject *)u; }
static inline struct uthread *uthread_from_kobject(struct kobject *o)
{
    return o && o->type == OBJ_THREAD ? (struct uthread *)o : NULL;
}

/* A new, not yet started thread of p (caller gets the only reference).
 * ERR_BAD_STATE if p is dying. */
status_t uthread_create(struct process *p, const char *name, struct uthread **out);
struct process *uthread_process(struct uthread *u);   /* no new reference */
/* Start ut (its process must be RUNNING) at entry with stack, rdi = arg0,
 * rsi = arg1. ERR_BAD_STATE if ut was started before or the process isn't
 * running, ERR_NO_RESOURCES over the job's thread limit, ERR_NO_MEMORY if
 * the job refuses UTHREAD_KMEM_PAGES or the kernel thread can't be made. */
status_t uthread_start(struct uthread *ut, uint64_t entry, uint64_t stack, uint64_t arg0,
                       uint64_t arg1, const cpumask_t *mask);
/* Kernel processes (M6): start the first thread (p NEW -> RUNNING) / a
 * further one running fn(arg) in the kernel; the thread leaves when fn
 * returns. Same charges and errors as process_start / uthread_start;
 * ERR_INVALID_ARGS for a user process. */
status_t process_start_kernel(struct process *p, struct uthread *ut, void (*fn)(void *), void *arg);
status_t uthread_start_kernel(struct uthread *ut, void (*fn)(void *), void *arg);
/* prio 0..PRIO_MAX (the syscall layer caps user callers). */
status_t uthread_set_priority(struct uthread *ut, int prio);
/* The current user thread leaves for good (thread_exit, process_exit, a
 * kill, a fatal fault). Interrupts on, nothing held. */
_Noreturn void uthread_exit_current(void);
