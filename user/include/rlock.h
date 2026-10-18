/* rlock: record locks in a file service, the part a service can share
 * (abi/idl/file.idl's lock, getlock and lockowner; user/lib/rlock.c).
 *
 * A service that keeps locks (extfs: user/services/extfs/locks.c) keeps
 * one struct rl_table per volume, in memory it chooses (its state VMO,
 * <svcstate.h>), and names each file by a number of its own (extfs: the
 * inode's number and generation). This library keeps the rules, Linux's
 * (fs/locks.c) and POSIX's:
 *   - three kinds of owner (FILE_LOCK_POSIX: a process, by its pid;
 *     FILE_LOCK_OFD and FILE_LOCK_FLOCK: an open file description, by the
 *     number lockowner gave it) in two spaces: fcntl's (POSIX and OFD
 *     locks meet) and flock's (only flock locks meet);
 *   - a lock conflicts with another owner's in its space that covers any
 *     of its bytes when either is a write lock (rl_conflict);
 *   - an owner's new lock replaces its own on the bytes it covers, cutting
 *     them (a lock in the middle of a longer one leaves two), and joins its
 *     neighbours of its own type that touch it (rl_set); an unlock cuts;
 *   - a POSIX request that would wait for an owner that waits, through
 *     others, for the requester is a deadlock (rl_deadlock: Linux's
 *     posix_locks_deadlock, the same ten steps; OFD and flock owners are
 *     never checked, as on Linux).
 *
 * **One store.** The table has two copies; a change is made in the one
 * not in use and then made the current one by one store (rl_set and
 * rl_drop), so a service killed in the middle of a change leaves its
 * successor the table as it was before or as it is after, never half of
 * it. A request run again (the slots' rule) finds its own change made and
 * makes it again: setting a lock its owner holds already, or dropping one
 * that is gone, changes nothing.
 *
 * The waits (struct rl_wait) are the service's memory, not the table's: a
 * waiter asks again after each wake, and a successor's first answers
 * count it again.
 *
 * None of this is thread-safe: one thread owns a volume's table. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <fs_idl.h>

#define RL_LOCKS 256u    /* locks on a volume at once (every owner, every file) */
#define RL_WAITS 64u     /* waits counted at once */
#define RL_DEADLOCK_STEPS 10u   /* Linux's MAX_DEADLK_ITERATIONS */

/* One lock: bytes start to end of file `file` (both included), of one
 * owner. `via` is the service's name for the open a POSIX lock was last
 * set through (its locks go when that open closes: rl_drop_via). */
struct rl_lock {
    uint64_t file;
    uint64_t start, end;
    uint64_t owner;     /* FILE_LOCK_POSIX: the pid; else lockowner's number */
    uint32_t via;
    uint8_t  kind;      /* FILE_LOCK_POSIX, FILE_LOCK_OFD, FILE_LOCK_FLOCK */
    uint8_t  type;      /* FILE_LOCK_READ, FILE_LOCK_WRITE */
    uint16_t pad;
};

struct rl_set {
    uint32_t       n;
    uint32_t       pad;
    struct rl_lock l[RL_LOCKS];
};

/* The table: set[cur] is the one in use. All zeros is an empty table. */
struct rl_table {
    uint32_t      cur;
    uint32_t      pad;
    struct rl_set set[2];
};

/* A request: as a lock, its type FILE_LOCK_READ, FILE_LOCK_WRITE or (rl_set
 * only) FILE_LOCK_UNLOCK. */
struct rl_req {
    uint64_t file;
    uint64_t start, end;
    uint64_t owner;
    uint32_t via;
    uint8_t  kind;
    uint8_t  type;
};

/* A waiter: an owner whose lock `blocker`'s lock was in the way of. */
struct rl_wait {
    bool     used;
    uint8_t  kind;      /* the waiter's */
    uint8_t  blocker_kind;
    uint64_t file;
    uint64_t owner;
    uint64_t blocker;
};

/* The locks in use (a successor reads them as they are). */
static inline const struct rl_set *rl_cur(const struct rl_table *t)
{
    return &t->set[t->cur & 1];
}

/* Is the request well formed (type, kind, start <= end <= FILE_LOCK_EOF; a
 * flock lock the whole file)? `unlock`: FILE_LOCK_UNLOCK allowed. */
bool rl_valid(const struct rl_req *q, bool unlock);
/* Does the table pass its checks (cur, n, each lock's fields)? A
 * successor asks before it uses one: NULL, or what is wrong. */
const char *rl_check(const struct rl_table *t);
/* The first lock in the way of q (a READ or WRITE), or NULL. */
const struct rl_lock *rl_conflict(const struct rl_table *t, const struct rl_req *q);
/* q made the owner's (READ or WRITE: replacing its own on those bytes; UNLOCK:
 * cutting them), conflicts left to the caller. *freed: some of the owner's
 * locks were cut or changed type, so another owner may go on now (a lock
 * only added or joined frees nothing). ERR_NO_RESOURCES: no room for the
 * pieces (RL_LOCKS), nothing changed. */
status_t rl_set(struct rl_table *t, const struct rl_req *q, bool *freed);
/* Every lock for which gone(lock, ctx) is true dropped, in one store.
 * Whether any was. */
bool rl_drop(struct rl_table *t, bool (*gone)(const struct rl_lock *l, void *ctx), void *ctx);
/* A POSIX owner's locks on `file` go with any open they were set through:
 * every lock of each process that set one on `file` through `via`. Whether
 * any went. */
bool rl_drop_via(struct rl_table *t, uint64_t file, uint32_t via);
/* Would a POSIX request of owner `pid` that `in_way` is in the way of
 * close a circle of waits (waits: n of them, the unused skipped)? */
bool rl_deadlock(const struct rl_wait *waits, unsigned n, uint64_t pid,
                 const struct rl_lock *in_way);
