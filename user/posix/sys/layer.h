/* The Linux call layer's internal header (user/posix/sys/): what its area
 * files share. The layer turns musl's Linux system calls into Jam OS's
 * (docs/M13-PLAN.md section 1): calls.c dispatches by Linux's number to
 * one handler per call (calls.h), each in its area's file (mem.c,
 * thread.c, time.c, misc.c, ...), written over libjam (<jamos/libjam.h>).
 *
 * Handlers return Linux's way: a value, or -errno; lx_err gives a
 * status's errno. They run in whatever thread made the call, so shared
 * state has a lock_t of its own (each file says which). The layer never
 * calls the program's malloc (nor musl's: tools/checklayerheap.py): it
 * allocates from its own heap (heap.c), jam_malloc and the rest. mem.c
 * and maptable.c allocate nothing: musl's malloc calls mmap and brk,
 * which are theirs. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jamos/libjam.h>
#include "calls.h"
#include "glue.h"
#include "ucopy.h"   /* a caller's pointer is read and written only through these */

/* -errno for a status (errno.c): the default meaning of each ERR_*. */
long lx_err(status_t st);

/* The layer's heap (heap.c, which gives its rules): jam_malloc, jam_calloc
 * and jam_free (<os.h>), realloc's (a block moved keeps its first n
 * bytes; NULL leaves p as it was) and strdup's. */
void *jam_realloc(void *p, size_t n);
char *jam_strdup(const char *s);
/* The thread pointer of the thread holding the heap's lock, 1 for a holder
 * that has none to read, 0 while it is free (sig_fault_begin's). */
uintptr_t heap_holder(void);

/* The process's own handles from the startup message (start.c reads
 * them before main and before any handler runs). */
handle_t lx_self_process(void);
handle_t lx_self_vmar(void);
/* [addr, addr + len) is being unmapped outside mem.c (a detached
 * thread's own stack, by __unmapself): libc's table of mappings forgets
 * it, so a later mapping there isn't taken for this one (mem.c). */
void mem_forget(uint64_t addr, uint64_t len);
/* What libc's table of its own mappings (mem.c) says of the mapping that
 * holds addr, for /proc: MEM_UNKNOWN (not one it made: the program's image,
 * a stack, libjam's), else MEM_* bits. */
#define MEM_UNKNOWN 0xffffffffu
#define MEM_SHARED  (1u << 0)   /* MAP_SHARED: the VMO's pages are other holders' too */
#define MEM_FILE    (1u << 1)   /* a read-only private mapping on a file's own pages */
#define MEM_BRK     (1u << 2)   /* the break's reservation */
uint32_t mem_kind_at(uint64_t addr);
/* The JIT power (SR_JIT, docs/CR-PLAN.md section 12): whether we hold it,
 * and giving it up for good (prctl's PR_SET_MDWE): closed under mem.c's
 * lock, so no mmap of ours is using it meanwhile. */
bool mem_jit_power(void);
void mem_jit_give_up(void);
/* The TSC's rate (Hz) from the time page, or 0 without one (time.c). */
uint64_t time_tsc_hz(void);
/* The soft limit of a resource (rlimit.c): RLIMIT_NOFILE and RLIMIT_DATA are
 * the ones acted on (fd_alloc, brk); UINT64_MAX is unlimited. */
uint64_t rlimit_cur(int resource);
/* The umask now (meta.c): what a child is handed. */
uint32_t lx_umask_now(void);
/* The calling thread's handle (thread.c). */
handle_t lx_self_thread(void);
/* The POSIX hand-over's state and current directory (<posixfds.h>; read
 * by start.c before main), or NULL if the starter gave none. */
struct posixfds_state;
const struct posixfds_state *lx_handover(const char **cwd);
/* The program's path the hand-over named (absolute), else the one the
 * startup's SR_ARGS named (startup_exe: a program started from its image
 * with no hand-over), or NULL. */
const char *lx_handover_exe(void);
/* This image started as AT_SECURE (the hand-over's POSIXFDS_SECURE: a
 * raised program, init's /svc/exec's word), as a forked copy of it did. */
bool        lx_handover_secure(void);
/* The hand-over's current directory as a directory channel
 * (POSIXFDS_CWD), taken once: the channel (the caller's from now on) and
 * its data (len bytes, checked by the reader), or HANDLE_INVALID: none. */
handle_t    lx_handover_dir(const void **data, uint32_t *len);

/* ---- fork (fork.c) ----------------------------------------------------------------
 * fork's step in each module whose state a forked child goes on using
 * (docs/FORK-PLAN.md section 1.5; fork.c calls them in its fixed order and
 * says why that order). LX_FORK_TAKE, in the parent just before the clone:
 * the module's lock taken, so its state is whole in the child's copy;
 * LX_FORK_GIVE, in the parent after the clone: the lock given back;
 * LX_FORK_CHILD, in the child before fork returns 0 there: the lock made
 * fresh, and what the child can't use dropped without closing anything
 * (those handles were the parent's). None of them allocates or frees
 * memory (fork holds the layer heap's lock across the call: heap_fork,
 * taken last). proc.h and sig.h have proc.c's and signal.c's. */
#define LX_FORK_TAKE  0
#define LX_FORK_GIVE  1
#define LX_FORK_CHILD 2
void path_fork(int step);     /* path.c: the current directory (and the read-only cache) */
void umask_fork(int step);    /* meta.c: the umask */
void fd_fork(int step);       /* fd.c: the descriptor table (its descriptors are FL2's) */
void mem_fork(int step);      /* mem.c: libc's table of mappings and the break; the child's
                               * JIT entries removed (its JIT memory isn't in it) */
void rlimit_fork(int step);   /* rlimit.c: the limits */
void heap_fork(int step);     /* heap.c: the layer's heap (after sig_fork's, for every way in) */
/* In the child only, first: start.c's own process and address space
 * handles become the child's (startup_handle's, which libjam has made
 * the child's), and thread.c's table holds this thread alone, whose id
 * is the child's pid (Linux's: the first thread's) once proc.c's step
 * has it. */
void lx_start_forked(void);
void thread_forked(void);

/* The handles a fork moves to the child (fork.c; <posixfork.h>, FL2): in
 * the parent, after the clone with every lock above held, each module's
 * give puts its handles, each under a key (POSIXFORK_KEY), the keys
 * growing; in the child, after the steps above, the same walk takes them
 * by the same keys. gen is this fork's number, the same on both sides and
 * greater than every earlier fork's: a module marks with it what its walk
 * has met (an object several descriptions share is passed once). */
struct lx_fork_io;
bool     lx_fork_child(const struct lx_fork_io *io);
uint32_t lx_fork_gen(const struct lx_fork_io *io);
/* Parent: h (consumed; HANDLE_INVALID puts nothing) under key. */
void     lx_fork_put(struct lx_fork_io *io, uint32_t key, handle_t h);
/* Parent: a duplicate of h under key, if h may be passed (RIGHT_DUPLICATE
 * and RIGHT_TRANSFER: docs/FORK-PLAN.md section 1.6); else nothing. */
void     lx_fork_dup(struct lx_fork_io *io, uint32_t key, handle_t h);
/* Child: the handle put under key, now the child's, or HANDLE_INVALID if
 * none came. */
handle_t lx_fork_take(struct lx_fork_io *io, uint32_t key);
/* The walks: fd.c's (every description, the terminal, /dev/shm's names,
 * libc's files), mem.c's (the table of mappings and the break). */
void     fd_fork_walk(struct lx_fork_io *io);
void     mem_fork_walk(struct lx_fork_io *io);
/* The table of mappings' VMOs mapped as copies, which vmar_clone gives
 * children of (mem.c; fork.c's prepare asks, mem_lock held), the room
 * their children go in, and how many: false if the room couldn't be made
 * (the fork fails then, ENOMEM). */
bool     mem_fork_clones(const handle_t **vmos, handle_t **children, uint32_t *n);
/* Child-only steps of modules whose state holds the parent's handles but
 * whose locks fork doesn't take (each made fresh, the state dropped
 * unclosed): socket.c's streams and poll's idle event, epoll.c's sets,
 * poll.c's and sockwait.c's per-thread ports, pty.c's table, procfs.c's
 * and enosys.c's locks. fork.c calls the ones linked. */
void     sock_forked(void);
void     epoll_forked(void);
void     poll_forked(void);
void     sockwait_forked(void);
void     pty_forked(void);
void     procfs_forked(void);
void     enosys_forked(void);

/* ---- the kernel's calls for POSIX (kposix.c) ------------------------------
 * Small thread ids, a thread's tid word, CPU time and the figures any
 * process may read (docs/M13-PLAN.md, "The new and changed interfaces").
 * kposix.c is the one file that calls them. */

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
    uint64_t free_pages;    /* of those, free now */
    uint32_t page_size;     /* bytes */
};
status_t kp_public_info(struct kp_public *out);
/* The mappings of the process behind proc (RIGHT_INSPECT), from the first
 * that ends after `from`: up to cap (at most MAP_INFO_MAX) into out. Their
 * number, or -errno. */
long     kp_process_maps(handle_t proc, uint64_t from, struct map_info *out, uint32_t cap);
