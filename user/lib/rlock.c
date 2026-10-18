/* rlock: record locks in a file service (<rlock.h>).
 *
 * A change is built in the copy not in use: every lock it doesn't touch
 * copied over, then the touched owner's pieces, then made current. The
 * table is small (RL_LOCKS of 40 bytes) and locks change seldom, so the
 * copy costs less than keeping it sorted would. */
#include <rlock.h>

/* The two spaces: fcntl's (POSIX and OFD locks) and flock's. */
static bool same_space(uint8_t a, uint8_t b)
{
    return (a == FILE_LOCK_FLOCK) == (b == FILE_LOCK_FLOCK);
}

static bool same_owner(const struct rl_lock *l, uint8_t kind, uint64_t owner)
{
    return l->kind == kind && l->owner == owner;
}

static bool overlap(uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2)
{
    return s1 <= e2 && s2 <= e1;
}

/* [s1, e1] and [s2, e2] overlap or touch (e < FILE_LOCK_EOF < UINT64_MAX:
 * e + 1 never wraps). */
static bool touch(uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2)
{
    return s1 <= e2 + 1 && s2 <= e1 + 1;
}

bool rl_valid(const struct rl_req *q, bool unlock)
{
    if (q->kind > FILE_LOCK_FLOCK || q->start > q->end || q->end > FILE_LOCK_EOF)
        return false;
    if (q->type != FILE_LOCK_READ && q->type != FILE_LOCK_WRITE &&
        !(unlock && q->type == FILE_LOCK_UNLOCK))
        return false;
    return q->kind != FILE_LOCK_FLOCK || (q->start == 0 && q->end == FILE_LOCK_EOF);
}

const char *rl_check(const struct rl_table *t)
{
    if (t->cur > 1)
        return "the locks' table: no copy in use";
    const struct rl_set *s = rl_cur(t);
    if (s->n > RL_LOCKS)
        return "the locks' table: a count out of range";
    for (uint32_t i = 0; i < s->n; i++) {
        const struct rl_lock *l = &s->l[i];
        const struct rl_req q = { .file = l->file, .start = l->start, .end = l->end,
                                  .kind = l->kind, .type = l->type };
        if (!rl_valid(&q, false))
            return "the locks' table: a lock out of range";
    }
    return NULL;
}

const struct rl_lock *rl_conflict(const struct rl_table *t, const struct rl_req *q)
{
    const struct rl_set *s = rl_cur(t);
    for (uint32_t i = 0; i < s->n; i++) {
        const struct rl_lock *l = &s->l[i];
        if (l->file == q->file && same_space(l->kind, q->kind) &&
            !same_owner(l, q->kind, q->owner) && overlap(l->start, l->end, q->start, q->end) &&
            (l->type == FILE_LOCK_WRITE || q->type == FILE_LOCK_WRITE))
            return l;
    }
    return NULL;
}

/* The copy not in use, emptied. */
static struct rl_set *spare(struct rl_table *t)
{
    struct rl_set *s = &t->set[(t->cur & 1) ^ 1];
    s->n = 0;
    return s;
}

static bool put(struct rl_set *s, const struct rl_lock *l)
{
    if (s->n == RL_LOCKS)
        return false;
    s->l[s->n++] = *l;
    return true;
}

/* The spare copy in use, by one store. */
static void flip(struct rl_table *t)
{
    __atomic_store_n(&t->cur, (t->cur & 1) ^ 1, __ATOMIC_RELEASE);
}

status_t rl_set(struct rl_table *t, const struct rl_req *q, bool *freed)
{
    const struct rl_set *s = rl_cur(t);
    struct rl_set *d = spare(t);
    bool lock = q->type != FILE_LOCK_UNLOCK, cut = false;
    /* The new lock's bytes, with the owner's locks of its type that touch
     * it joined in (an owner's locks of one type never touch each other,
     * so one pass finds them all). */
    uint64_t start = q->start, end = q->end;
    for (uint32_t i = 0; i < s->n && lock; i++) {
        const struct rl_lock *l = &s->l[i];
        if (l->file == q->file && same_owner(l, q->kind, q->owner) && l->type == q->type &&
            touch(l->start, l->end, q->start, q->end)) {
            start = l->start < start ? l->start : start;
            end = l->end > end ? l->end : end;
        }
    }
    for (uint32_t i = 0; i < s->n; i++) {
        const struct rl_lock *l = &s->l[i];
        bool mine = l->file == q->file && same_owner(l, q->kind, q->owner);
        if (!mine || !overlap(l->start, l->end, start, end)) {
            if (!put(d, l))
                return ERR_NO_RESOURCES;
            continue;
        }
        /* One of the owner's locks under the new bytes: what is left of it
         * on each side (none of a lock joined in: it lies inside). */
        struct rl_lock left = *l, right = *l;
        if (l->start < start) {
            left.end = start - 1;
            if (!put(d, &left))
                return ERR_NO_RESOURCES;
        }
        if (l->end > end) {
            right.start = end + 1;
            if (!put(d, &right))
                return ERR_NO_RESOURCES;
        }
        if (!lock || l->type != q->type)
            cut = true;   /* unlocked, or of another type: bytes freed or changed */
    }
    bool any = cut;
    if (lock) {
        const struct rl_lock n = { .file = q->file, .start = start, .end = end,
                                   .owner = q->owner, .via = q->via, .kind = q->kind,
                                   .type = q->type };
        if (!put(d, &n))
            return ERR_NO_RESOURCES;
        /* Fewer or more locks than before: joined, or new. As many, none
         * cut: the same lock, unless it wasn't there exactly (another via). */
        any = any || d->n != s->n;
        bool held = false;
        for (uint32_t i = 0; i < s->n && !any && !held; i++)
            held = !memcmp(&s->l[i], &n, sizeof(n));
        any = any || !held;
    }
    *freed = cut;
    if (any)
        flip(t);
    return OK;
}

bool rl_drop(struct rl_table *t, bool (*gone)(const struct rl_lock *l, void *ctx), void *ctx)
{
    const struct rl_set *s = rl_cur(t);
    struct rl_set *d = spare(t);
    for (uint32_t i = 0; i < s->n; i++)
        if (!gone(&s->l[i], ctx))
            d->l[d->n++] = s->l[i];
    if (d->n == s->n)
        return false;
    flip(t);
    return true;
}

struct via_ctx {
    uint64_t file;
    uint64_t pid;
};

static bool pid_on_file(const struct rl_lock *l, void *ctx)
{
    const struct via_ctx *v = ctx;
    return l->kind == FILE_LOCK_POSIX && l->file == v->file && l->owner == v->pid;
}

bool rl_drop_via(struct rl_table *t, uint64_t file, uint32_t via)
{
    bool any = false;
    for (;;) {
        const struct rl_set *s = rl_cur(t);
        const struct rl_lock *l = NULL;
        for (uint32_t i = 0; i < s->n && !l; i++)
            if (s->l[i].kind == FILE_LOCK_POSIX && s->l[i].file == file && s->l[i].via == via)
                l = &s->l[i];
        if (!l)
            return any;
        struct via_ctx v = { .file = file, .pid = l->owner };
        any |= rl_drop(t, pid_on_file, &v);
    }
}

bool rl_deadlock(const struct rl_wait *waits, unsigned n, uint64_t pid,
                 const struct rl_lock *in_way)
{
    if (in_way->kind != FILE_LOCK_POSIX)
        return false;
    uint64_t owner = in_way->owner;
    for (unsigned step = 0; step < RL_DEADLOCK_STEPS; step++) {
        const struct rl_wait *w = NULL;
        for (unsigned i = 0; i < n && !w; i++)
            if (waits[i].used && waits[i].kind == FILE_LOCK_POSIX && waits[i].owner == owner)
                w = &waits[i];
        if (!w || w->blocker_kind != FILE_LOCK_POSIX)
            return false;
        if (w->blocker == pid)
            return true;
        owner = w->blocker;
    }
    return false;
}
