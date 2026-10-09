/* What musl's overrides (user/posix/musl/, user/posix/crt/) call in the
 * Linux call layer (user/posix/sys/). The overrides are compiled with
 * musl's own flags and headers, so this header uses plain C types only
 * and includes nothing of Jam OS's: the layer's files include it too, so
 * a declaration and its definition can't drift apart. */
#pragma once

/* Every system call musl makes (the override of syscall_arch.h, the
 * cancellation point's syscall_cp): Linux's number and six arguments in,
 * Linux's result out (a value, or -errno). calls.c. */
long __jam_syscall(long n, long a, long b, long c, long d, long e, long f);

/* __set_thread_area: this thread's thread pointer (the FS base). 0, or
 * -errno. thread.c. */
int __jam_set_thread_area(void *tp);

/* __clone, threads only: a new thread of this process running fn(arg) on
 * stack, with the thread pointer tls, its id stored at *ptid before it
 * runs (CLONE_PARENT_SETTID) and ctid as its tid word (CLONE_CHILD_CLEARTID:
 * 0 is stored there and woken once it is gone). The new thread's id, or
 * -errno: -ENOSYS for anything but a thread of this process (CLONE_VM and
 * CLONE_THREAD), so fork fails cleanly (posix_spawn is overridden). thread.c. */
int __jam_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls,
                int *ctid);

/* __unmapself's first half, on the stack it is about to unmap: this
 * thread's own handle closed and forgotten. Returns the address-space
 * handle __unmapself unmaps through. thread.c. */
unsigned __jam_unmapself_prepare(void);

/* crt1's start: read the startup message on the channel ch (a handle_t),
 * build Linux's start block (argc, argv, envp, auxv) and run main through
 * musl's __libc_start_main with init and fini, as musl's own crt1 does
 * (main's type is musl's: called with argc, argv and envp). Never
 * returns. user/posix/crt/start.c. */
_Noreturn void __jam_start(unsigned ch, int (*main)(), void (*init)(void), void (*fini)(void));

/* ---- processes: posix_spawn's override (spawn.c) ---------------------------------- */

/* One of posix_spawn's file actions, as the override hands them over (in
 * the order they are applied): close fd; dup2 srcfd onto fd; open path
 * (oflag, mode) as fd; chdir to path; fchdir to fd. */
#define JAM_SPAWN_CLOSE  1
#define JAM_SPAWN_DUP2   2
#define JAM_SPAWN_OPEN   3
#define JAM_SPAWN_CHDIR  4
#define JAM_SPAWN_FCHDIR 5
struct jam_spawn_op {
    int         cmd;      /* JAM_SPAWN_* */
    int         fd;       /* the descriptor it acts on */
    int         srcfd;    /* DUP2: the one copied */
    int         oflag;    /* OPEN: open's flags */
    unsigned    mode;     /* OPEN: the mode a created file gets */
    const char *path;     /* OPEN, CHDIR */
};

/* A posix_spawn or posix_spawnp, in plain C (the override reads musl's
 * posix_spawnattr_t and file actions into this). */
struct jam_spawn_req {
    const char                *path;     /* the program; with search, a name for PATH */
    char *const               *argv;     /* NULL-terminated */
    char *const               *envp;     /* NULL-terminated; NULL: none */
    const struct jam_spawn_op *ops;      /* nops file actions */
    unsigned                   nops;
    int                        flags;    /* POSIX_SPAWN_* */
    int                        pgrp;     /* POSIX_SPAWN_SETPGROUP's group (0: its own) */
    unsigned long long         sigdef;   /* POSIX_SPAWN_SETSIGDEF's signals (bit n - 1) */
    unsigned long long         sigmask;  /* POSIX_SPAWN_SETSIGMASK's mask */
    const char *const         *grants;   /* posix_spawnattr_setgrants_np's list, or NULL */
    const char                *search;   /* posix_spawnp: the PATH to search; else NULL */
};

/* Start a child as r says (docs/M13-PLAN.md section 5). 0 with its pid
 * in *pid (if pid isn't NULL), or an errno value (positive, posix_spawn's
 * own convention). */
int __jam_posix_spawn(int *pid, const struct jam_spawn_req *r);
