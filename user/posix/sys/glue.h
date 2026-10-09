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
 * CLONE_THREAD), so musl's posix_spawn and fork fail cleanly. thread.c. */
int __jam_clone(int (*fn)(void *), void *stack, int flags, void *arg, int *ptid, void *tls,
                int *ctid);

/* __unmapself's first half, on the stack it is about to unmap: this
 * thread's own handle closed and forgotten. Returns the address-space
 * handle __unmapself unmaps through. thread.c. */
unsigned __jam_unmapself_prepare(void);

/* ttyname_r's override: the name of the terminal at descriptor fd into
 * name (size bytes): 0, or an errno (EBADF, ENOTTY, ERANGE). tty.c. */
int __jam_ttyname(int fd, char *name, unsigned long size);

/* crt1's start: read the startup message on the channel ch (a handle_t),
 * build Linux's start block (argc, argv, envp, auxv) and run main through
 * musl's __libc_start_main with init and fini, as musl's own crt1 does
 * (main's type is musl's: called with argc, argv and envp). Never
 * returns. user/posix/crt/start.c. */
_Noreturn void __jam_start(unsigned ch, int (*main)(), void (*init)(void), void (*fini)(void));
