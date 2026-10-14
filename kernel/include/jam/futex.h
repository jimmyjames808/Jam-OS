/* Futexes: a wait on a 32-bit word of the caller's own memory
 * (kernel/sched/futex.c). Private only: a futex is named by the calling
 * thread's address space and the word's user address, so only threads of
 * one process meet on it. `flags` is reserved (0) for a shared kind later
 * (named by VMO and offset, for process-shared mutexes). These are the
 * system calls' bodies: they work on current_thread()'s address space. */
#pragma once

#include <stdint.h>
#include <jam/status.h>

struct aspace;

/* If the word at uaddr (4-aligned, mapped readable) holds `expected`, sleep
 * until a futex_wake (or futex_requeue) on it picks this thread, or
 * uptime_ns() reaches deadline_ns (DEADLINE_NEVER: no limit). OK when
 * woken (spurious wakes are allowed: callers check their word again);
 * ERR_BAD_STATE at once if the word held something else; ERR_TIMED_OUT;
 * ERR_INVALID_ARGS for flags, an unaligned address, or one outside user
 * space or not readable; ERR_IO if the word is on a pager VMO's page that
 * its pager failed (or that didn't match a verified VMO's hash);
 * ERR_CANCELED once the thread is being killed (in the wait, or while
 * the read of the word waited for its pager);
 * ERR_INTERRUPTED when thread_interrupt ends the wait (or ended it before
 * it began) and no wake picked this thread (a wake wins: OK, and the
 * interrupt stays for the next interruptible wait).
 * The word is read after the thread is queued, so a wake that follows a
 * store to it is never lost: either the read sees the store, or the wake
 * sees the waiter. */
status_t futex_wait(uint64_t uaddr, uint32_t expected, uint64_t deadline_ns, uint32_t flags);
/* Wake up to `count` threads waiting on uaddr, oldest first; how many. */
int64_t futex_wake(uint64_t uaddr, uint32_t count, uint32_t flags);
/* Wake up to `wake` threads waiting on uaddr, then move up to `requeue` of
 * the rest to wait on uaddr2 instead (they wake from a futex_wake there);
 * how many it woke and moved together. */
int64_t futex_requeue(uint64_t uaddr, uint32_t wake, uint64_t uaddr2, uint32_t requeue,
                      uint32_t flags);
/* TEST ONLY: threads queued on uaddr of address space `as` now. */
uint32_t futex_queued(const struct aspace *as, uint64_t uaddr);
