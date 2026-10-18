/* extfs: record locks (file.idl's lock, getlock and lockowner; D9), on
 * the library a file service can share (<rlock.h>: the table, the
 * conflict, replace and deadlock rules, Linux's).
 *
 * A file is named in the table by its inode's number and generation, so
 * every open of it, through any path or view, by any process, meets the
 * same locks. The owners:
 *   - a POSIX lock's is the process that wrote the request (op_pid: the
 *     kernel stamps it, so no process can speak for another), and the lock
 *     remembers the open it was set through (`via`: the file slot and its
 *     generation). When that open closes (its client closed it, or its
 *     process ended and the kernel closed it), every POSIX lock of each
 *     process that set one through it on the file goes (locks_closed):
 *     a process's death ends all of its locks, and libc unlocks the whole
 *     file at each close(), Linux's rule;
 *   - an OFD or flock lock's is a lockowner: a number (random, so it
 *     can't be guessed) and a token, a channel whose peer libc keeps in
 *     the description (a forked child gets a duplicate). When the last
 *     handle of the peer closes, in whichever process, the owner's locks
 *     go (locks_event: the token's SIG_PEER_CLOSED, bound to the port).
 * A lock found in the way whose holder is gone already (its open's
 * channel or its owner's token closed, the packet not yet served) goes
 * first, so an answer never waits on a dead holder.
 *
 * **Waits.** lock with FILE_LOCK_WAIT that finds a lock in the way records
 * the wait (waiter, blocker) and gives the caller the peer of a wake
 * channel; the service closes its end when the file's locks change (or
 * dies), and the caller asks again. The record is what the deadlock check
 * follows; a waiter that gave up (a signal) closed its end, and its
 * record is swept before the next check. The waits are this instance's
 * memory only: a successor's waiters wake (its ends died with it) and ask
 * again, which counts them again.
 *
 * **Restarts.** The table and the owners' numbers are in the state
 * (kept->locks), each token with the keeper (XF_KEEP_LOCK) from its
 * lockowner on; a change to the table is one store (<rlock.h>), so a
 * successor finds the table whole, takes the tokens back and drops the
 * owners whose token didn't come back or whose last handle closed while
 * no instance ran; a POSIX lock goes with its open as before (files_adopt
 * closes an open whose client is gone). Locks are never lost silently: a
 * table that fails its checks is a fresh start, which closes every open
 * file (its client sees ERR_PEER_CLOSED). */
#include <rlock.h>
#include "extfs.h"

#define OWNER_TRIES 8u   /* random numbers tried for a new owner's */

static handle_t token[XF_LOCK_OWNERS];   /* each owner's end of its token */
static struct rl_wait waits[RL_WAITS];
static handle_t wake[RL_WAITS];          /* each wait's end of its wake channel */

static struct rl_table *table(void)
{
    return &kept->locks.table;
}

static uint64_t file_key(const struct xf_file *f)
{
    return (uint64_t)f->ino << 32 | f->igen;
}

static bool closed(handle_t h)
{
    return h != HANDLE_INVALID && jam_object_wait_one(h, SIG_PEER_CLOSED, 0, NULL) == OK;
}

/* ---- waits ------------------------------------------------------------------------ */

static void wake_one(unsigned i)
{
    if (wake[i] != HANDLE_INVALID)
        jam_handle_close(wake[i]);
    wake[i] = HANDLE_INVALID;
    waits[i].used = false;
}

/* The waiters on `file` (0: on every file) asked again: their locks may
 * be free now. */
static void wake_all(uint64_t file)
{
    for (unsigned i = 0; i < RL_WAITS; i++)
        if (waits[i].used && (!file || waits[i].file == file))
            wake_one(i);
}

/* Waiters that gave up (their end closed) forgotten. */
static void sweep_waits(void)
{
    for (unsigned i = 0; i < RL_WAITS; i++)
        if (waits[i].used && closed(wake[i]))
            wake_one(i);
}

/* q waits for in_way's owner: the caller's end of its wake channel into
 * *out, or not (no room: the caller waits a moment and asks again). */
static bool wait_add(const struct rl_req *q, const struct rl_lock *in_way, handle_t *out)
{
    unsigned i = 0;
    while (i < RL_WAITS && waits[i].used)
        i++;
    handle_t peer;
    if (i == RL_WAITS || jam_channel_create(&wake[i], &peer) != OK)
        return false;
    waits[i] = (struct rl_wait){ .used = true, .kind = q->kind, .blocker_kind = in_way->kind,
                                 .file = q->file, .owner = q->owner, .blocker = in_way->owner };
    *out = peer;
    return true;
}

/* ---- owners ------------------------------------------------------------------------ */

static int owner_slot(uint64_t id)
{
    for (unsigned i = 0; id && i < XF_LOCK_OWNERS; i++)
        if (kept->locks.owners[i] == id)
            return (int)i;
    return -1;
}

static bool owners_lock(const struct rl_lock *l, void *ctx)
{
    return l->kind != FILE_LOCK_POSIX && l->owner == *(const uint64_t *)ctx;
}

/* Owner i goes: its locks, then the state's record, then its token. */
static void owner_drop(unsigned i)
{
    uint64_t id = kept->locks.owners[i];
    if (id && rl_drop(table(), owners_lock, &id))
        wake_all(0);
    __atomic_store_n(&kept->locks.owners[i], 0, __ATOMIC_RELEASE);
    if (token[i] != HANDLE_INVALID) {
        (void)jam_port_unbind(vol.port, token[i], XF_KEY_LOCK(i));
        jam_handle_close(token[i]);
        token[i] = HANDLE_INVALID;
        kept_drop(XF_KEEP_LOCK(i));
    }
}

static status_t owner_arm(unsigned i)
{
    return jam_port_bind(vol.port, token[i], XF_KEY_LOCK(i), SIG_PEER_CLOSED, PORT_BIND_ONCE);
}

void locks_event(uint64_t key)
{
    unsigned i = (unsigned)(key & ~XF_KEY_LOCK_BIT);
    if (i < XF_LOCK_OWNERS && kept->locks.owners[i] && closed(token[i]))
        owner_drop(i);
}

/* ---- the gone holders ---------------------------------------------------------------- */

/* Is in_way's holder gone already? Then its locks go now. */
static bool holder_gone(const struct rl_lock *in_way)
{
    if (in_way->kind == FILE_LOCK_POSIX) {
        if (!files_via_gone(in_way->via))
            return false;
        if (rl_drop_via(table(), in_way->file, in_way->via))
            wake_all(in_way->file);
        return true;
    }
    int i = owner_slot(in_way->owner);
    if (i >= 0 && !closed(token[i]))
        return false;
    if (i >= 0) {
        owner_drop((unsigned)i);
    } else {
        uint64_t id = in_way->owner;   /* an owner the state doesn't know: never */
        (void)rl_drop(table(), owners_lock, &id);
    }
    return true;
}

static const struct rl_lock *in_the_way(const struct rl_req *q)
{
    for (;;) {
        const struct rl_lock *l = rl_conflict(table(), q);
        if (!l || !holder_gone(l))
            return l;
    }
}

/* ---- the requests -------------------------------------------------------------------- */

/* The request's lock, checked: kind, type and range, its owner (the
 * writer's pid, or a known owner), the open's rights. */
static status_t request(const struct xf_file *f, uint8_t type, uint8_t flags, uint64_t start,
                        uint64_t end, uint64_t owner, bool unlock, struct rl_req *q)
{
    *q = (struct rl_req){ .file = file_key(f), .start = start, .end = end,
                          .kind = flags & FILE_LOCK_KINDS, .type = type,
                          .via = files_via(f) };
    if (!rl_valid(q, unlock))
        return ERR_INVALID_ARGS;
    if (q->kind == FILE_LOCK_POSIX) {
        q->owner = op_pid();
        if (!q->owner)
            return ERR_INVALID_ARGS;   /* the kernel's own message: never */
    } else {
        q->owner = owner;
        if (owner_slot(owner) < 0)
            return ERR_NOT_FOUND;
    }
    if (q->kind == FILE_LOCK_FLOCK)
        return OK;   /* flock asks no rights (Linux's) */
    if ((type == FILE_LOCK_READ && !(f->flags & FS_READ)) ||
        (type == FILE_LOCK_WRITE && !(f->flags & FS_WRITE)))
        return ERR_ACCESS_DENIED;
    return OK;
}

status_t locks_op_lock(void *ctx, uint8_t type, uint8_t flags, uint64_t start, uint64_t end,
                       uint64_t owner, uint8_t *out_result, handle_t *out_wake,
                       uint32_t *out_wake_len)
{
    const struct xf_file *f = ctx;
    *out_result = FILE_LOCK_GRANTED;
    *out_wake_len = 0;
    if (flags & ~(uint32_t)(FILE_LOCK_KINDS | FILE_LOCK_WAIT))
        return ERR_INVALID_ARGS;
    struct rl_req q;
    status_t st = request(f, type, flags, start, end, owner, true, &q);
    if (st != OK)
        return st;
    const struct rl_lock *l = type != FILE_LOCK_UNLOCK ? in_the_way(&q) : NULL;
    if (l && !(flags & FILE_LOCK_WAIT)) {
        *out_result = FILE_LOCK_CONFLICT;
        return OK;
    }
    if (l) {
        sweep_waits();
        if (q.kind == FILE_LOCK_POSIX && rl_deadlock(waits, RL_WAITS, q.owner, l)) {
            *out_result = FILE_LOCK_DEADLOCK;
            return OK;
        }
        *out_result = FILE_LOCK_CONFLICT;
        if (wait_add(&q, l, out_wake))
            *out_wake_len = 1;
        return OK;
    }
    bool freed = false;
    st = rl_set(table(), &q, &freed);
    if (st == OK && freed)
        wake_all(q.file);
    return st;
}

status_t locks_op_getlock(void *ctx, uint8_t type, uint8_t flags, uint64_t start, uint64_t end,
                          uint64_t owner, uint8_t *out_held, uint64_t *out_held_start,
                          uint64_t *out_held_end, uint32_t *out_pid)
{
    const struct xf_file *f = ctx;
    *out_held = FILE_LOCK_UNLOCK;
    *out_held_start = *out_held_end = 0;
    *out_pid = 0;
    if (flags & ~(uint32_t)FILE_LOCK_KINDS)
        return ERR_INVALID_ARGS;
    struct rl_req q;
    status_t st = request(f, type, flags, start, end, owner, false, &q);
    if (st != OK)
        return st;
    const struct rl_lock *l = in_the_way(&q);
    if (l) {
        *out_held = l->type;
        *out_held_start = l->start;
        *out_held_end = l->end;
        *out_pid = l->kind == FILE_LOCK_POSIX ? (uint32_t)l->owner : 0;
    }
    return OK;
}

status_t locks_op_lockowner(void *ctx, uint64_t *out_owner, handle_t *out_token,
                            uint32_t *out_token_len)
{
    (void)ctx;
    *out_owner = 0;
    *out_token_len = 0;
    int i = -1;
    for (unsigned k = 0; k < XF_LOCK_OWNERS && i < 0; k++)
        if (!kept->locks.owners[k])
            i = (int)k;
    for (unsigned k = 0; k < XF_LOCK_OWNERS && i < 0; k++)
        if (closed(token[k])) {   /* one whose packet isn't served yet */
            owner_drop(k);
            i = (int)k;
        }
    if (i < 0)
        return ERR_NO_RESOURCES;
    uint64_t id = 0;
    for (unsigned k = 0; k < OWNER_TRIES && (!id || owner_slot(id) >= 0); k++)
        id = (uint64_t)os_random_u32() << 32 | os_random_u32();
    if (!id || owner_slot(id) >= 0)
        return ERR_NO_RESOURCES;
    handle_t peer;
    status_t st = jam_channel_create(&token[i], &peer);
    if (st != OK) {
        token[i] = HANDLE_INVALID;
        return st;
    }
    /* With the keeper before the state knows it (keeper.c's rule). */
    st = kept_put(XF_KEEP_LOCK(i), &token[i], 1);
    if (st == OK)
        st = owner_arm((unsigned)i);
    if (st != OK) {
        jam_handle_close(peer);
        jam_handle_close(token[i]);
        token[i] = HANDLE_INVALID;
        kept_drop(XF_KEEP_LOCK(i));
        return st;
    }
    __atomic_store_n(&kept->locks.owners[i], id, __ATOMIC_RELEASE);
    *out_owner = id;
    out_token[0] = peer;
    *out_token_len = 1;
    return OK;
}

/* ---- an open's close ------------------------------------------------------------------ */

void locks_closed(const struct xf_file *f)
{
    uint64_t file = file_key(f);
    if (rl_drop_via(table(), file, files_via(f)))
        wake_all(file);
}

/* ---- a start, a successor's ---------------------------------------------------------- */

void locks_reset(void)
{
    for (unsigned i = 0; i < XF_LOCK_OWNERS; i++) {
        if (token[i] != HANDLE_INVALID) {
            (void)jam_port_unbind(vol.port, token[i], XF_KEY_LOCK(i));
            jam_handle_close(token[i]);
            kept_drop(XF_KEEP_LOCK(i));
        }
        token[i] = HANDLE_INVALID;
    }
    for (unsigned i = 0; i < RL_WAITS; i++)
        if (waits[i].used)
            wake_one(i);
}

const char *locks_check(void)
{
    return rl_check(table());
}

bool locks_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= XF_LOCK_OWNERS || !kept->locks.owners[i] || n != 1 || token[i])
        return false;
    token[i] = hs[0];
    return true;
}

/* A lock whose holder the state doesn't know: an open not in use, or an
 * owner not there (never, but a successor checks). */
static bool orphan(const struct rl_lock *l, void *ctx)
{
    (void)ctx;
    if (l->kind == FILE_LOCK_POSIX)
        return files_via_gone(l->via);
    return owner_slot(l->owner) < 0;
}

unsigned locks_adopt(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < XF_LOCK_OWNERS; i++) {
        if (!kept->locks.owners[i])
            continue;
        if (token[i] == HANDLE_INVALID || closed(token[i]) || owner_arm(i) != OK) {
            owner_drop(i);   /* its last handle closed while no instance ran */
            continue;
        }
        n++;
    }
    (void)rl_drop(table(), orphan, NULL);
    return n;
}
