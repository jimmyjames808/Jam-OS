/* Where a process's threads wait: debug_command "threads <pid|name>"
 * (kernel/debug/threads.c), the shell's ps -t. For each thread: its state,
 * what it waits for and on which object (its wait note, <jam/sched.h>), its
 * user RIP, RSP and RBP at its last kernel entry, its kernel frames while
 * it is off a CPU, and a best-effort user backtrace by frame pointers.
 *
 * Read only, and only from the debug command's kernel thread (the caller
 * needs RIGHT_ROOT_DEBUG): another process's memory is read a word at a
 * time through its page tables (aspace_peek), never with a fault, a wait
 * or a lock held across more than one word; a page that isn't there ends
 * the walk. Nothing a program does can make the listing read outside its
 * own stack words or its page tables' plain RAM. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

struct aspace;
struct job;
struct thread;

#define LOOK_KFRAMES 16   /* kernel return addresses kept per thread */
#define LOOK_UFRAMES 48   /* user frames walked at most */
#define LOOK_SCAN    256  /* stack words a backtrace looks through for a record */
#define LOOK_WORDS   64   /* stack words the listing's scan looks at by default... */
#define LOOK_WORDS_MAX 4096   /* ...and at most (debug_command's "threads <which> <words>") */
#define LOOK_HITS    64   /* the scan's words printed at most */

/* One thread, as thread_look saw it. */
struct thread_look {
    uint64_t koid;          /* its thread object's (process_look_threads) */
    uint32_t tid;
    char     name[32];
    int      state;         /* T_* */
    uint32_t cpu;           /* the CPU it runs or is queued on */
    bool     on_cpu;        /* its kernel stack is in use: no kernel frames */
    uint8_t  wait_on;       /* WAITON_* */
    uint64_t wait_koid, wait_arg;
    uint64_t deadline_ns;   /* its last wait's (DEADLINE_NEVER: none) */
    uint64_t off_ns;        /* how long ago it last left a CPU (0: never ran) */
    uint64_t cpu_ns;        /* its CPU time */
    uint64_t mutex_since;   /* waiting for a kernel mutex since (uptime ns), 0: not */
    bool     cancelled, interrupted;
    bool     exc_window;    /* in its exception entry's window */
    uint64_t exc_rip, exc_addr;
    /* Its user registers at its last kernel entry (a system call's frame,
     * or a trap's from ring 3): stale while it runs in ring 3. */
    bool     in_syscall;
    uint64_t sys_nr, sys_args[3];
    bool     have_user;     /* a user thread that has entered the kernel */
    uint64_t urip, ursp, urbp;
    uint32_t nk;            /* kernel return addresses, innermost first */
    uint64_t kframes[LOOK_KFRAMES];
};

/* Look at t. Its kernel stack must stay in place meanwhile (the caller
 * holds what keeps it: process_look_threads holds t's process's lock, and
 * an exiting thread marks itself gone under that lock first). Reads only
 * words of t's own stack; the frame walk is bounded to it. Spinlocks only
 * (thread_cpu_tsc's run queue lock). */
void thread_look(struct thread *t, struct thread_look *out);

/* A user backtrace of as from (rip, rbp): rip, then each frame's return
 * address, into out[0..cap); how many. A frame record is a saved rbp (0,
 * or higher up the stack) and a return address into code. Records are
 * followed only upward and within 16 MiB of rsp, at most LOOK_UFRAMES.
 * Where rbp (at the start) or a record's saved rbp isn't a record (code
 * built without frame pointers, musl's, used the register for something
 * else), the walk goes on from the first record in the LOOK_SCAN words
 * above, and scanned[i] (scanned may be NULL) says out[i] came from one
 * found that way. */
uint32_t user_backtrace(struct aspace *as, uint64_t rip, uint64_t rsp, uint64_t rbp,
                        uint64_t *out, bool *scanned, uint32_t cap);

/* The stack words in [rsp, rsp + words * 8) that point into pages of as
 * that may run (return addresses, most likely, of frames without frame
 * pointers, and stale ones): their offsets from rsp into off[] and values
 * into val[]; how many (at most cap). Stops at a page that isn't there. */
uint32_t user_stack_scan(struct aspace *as, uint64_t rsp, uint32_t words, uint32_t *off,
                         uint64_t *val, uint32_t cap);

/* debug_command "threads <which> [words]": print the threads of the
 * process in scope's tree whose koid (ps's PID) is `which`, or else the
 * first called `which`, to the kernel log; each thread's stack scan looks
 * at `words` words (0: LOOK_WORDS; at most LOOK_WORDS_MAX). Returns how
 * many threads it printed, or ERR_NOT_FOUND. */
int64_t threads_report(struct job *scope, const char *which, uint32_t words);
