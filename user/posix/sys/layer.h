/* The Linux call layer's internal header (user/posix/sys/): what its area
 * files share. The layer turns musl's Linux system calls into Jam OS's
 * (docs/M13-PLAN.md section 1): calls.c dispatches by Linux's number to
 * one handler per call (calls.h), each in its area's file (mem.c,
 * thread.c, time.c, misc.c, ...), written over libjam (<jamos/libjam.h>).
 *
 * Handlers return Linux's way: a value, or -errno; lx_err gives a
 * status's errno. They run in whatever thread made the call, so shared
 * state has a lock_t of its own (each file says which). mem.c and
 * maptable.c never call malloc: musl's malloc calls mmap and brk, which
 * are theirs. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jamos/libjam.h>
#include "calls.h"
#include "glue.h"

/* ERR_INTERRUPTED (-26): a wait ended by thread_interrupt. Defined by the
 * kernel's <jam/status.h> once it has thread_interrupt; the number is the
 * plan's either way. */
#ifdef ERR_INTERRUPTED
#define LX_ERR_INTERRUPTED ERR_INTERRUPTED
#else
#define LX_ERR_INTERRUPTED (-26)
#endif

/* -errno for a status (errno.c): the default meaning of each ERR_*. */
long lx_err(status_t st);

/* The process's own handles from the startup message (start.c reads
 * them before main and before any handler runs). */
handle_t lx_self_process(void);
handle_t lx_self_vmar(void);
/* The calling thread's handle (thread.c). */
handle_t lx_self_thread(void);
/* The POSIX hand-over's state and current directory (<posixfds.h>; read
 * by start.c before main), or NULL if the starter gave none. */
struct posixfds_state;
const struct posixfds_state *lx_handover(const char **cwd);

/* ---- the kernel's calls for POSIX (kposix.c) ------------------------------
 * Small thread ids, a thread's tid word, CPU time and the figures any
 * process may read are new kernel calls (docs/M13-PLAN.md, "The new and
 * changed interfaces"). kposix.c is the one file that calls them; built
 * against a kernel without them, it says so (kp_threads false,
 * ERR_NOT_SUPPORTED) and the layer makes no thread but the first. */

/* The kernel clears a thread's tid word at its end: threads can be made. */
bool     kp_threads(void);
/* thread_set_tid_word: at this thread's end, after its last user access,
 * the kernel stores 0 at *word and wakes it (NULL: nothing). This
 * thread's id (> 0), or an ERR_*. */
int64_t  kp_set_tid_word(uint32_t *word);
/* The id of the thread behind handle t. */
status_t kp_thread_tid(handle_t t, uint32_t *tid);
/* CPU time used so far (ns) by the process behind proc, or the thread
 * behind thread (the other HANDLE_INVALID). */
status_t kp_cpu_ns(handle_t proc, handle_t thread, uint64_t *ns);

/* What any process may know about the machine (sys_public_info). */
struct kp_public {
    char     version[32];   /* the kernel's version, NUL-terminated */
    uint32_t cpus;          /* CPUs online */
    uint64_t pages;         /* memory, in pages of page_size */
    uint32_t page_size;     /* bytes */
};
status_t kp_public_info(struct kp_public *out);
