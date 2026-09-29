/* Jobs: resource limits for a tree of processes (see process.h, abi.h).
 *
 * Each job counts, per resource kind, what its processes and all its
 * descendants use right now (`used`), and may have a limit of its own. A
 * charge walks from the job to the root adding n at every level with a
 * compare-and-swap that refuses to go past that level's limit; if one
 * level refuses, the levels already charged are credited back. So a child
 * job can never use more than any ancestor has left, and there is no lock:
 * charges happen under VMO, handle-table and channel locks.
 *
 * A job whose used counter would be visible mid-charge (a failed charge
 * adds then subtracts) can momentarily look fuller than it is: a charge
 * racing it may fail when it would have fit. That is the price of being
 * lock-free, and only matters right at a limit.
 *
 * Lifetime: a job holds a reference to its parent. Charged VMOs, queued
 * channel messages and address spaces hold references to the job they
 * charged, so a charge can always be credited back; a process holds one
 * until it is torn down (process_finish), so once SIG_TERMINATED is seen a
 * dead process no longer keeps its job alive.
 *
 * What a job costs (review R3): a job is charged to its PARENT as one
 * JOB_LIMIT_HANDLES unit (a small kernel object, like the ones handle slots
 * name) from creation until it is destroyed, and jobs nest at most
 * JOB_MAX_DEPTH deep. So a process can't hold an unbounded chain of jobs
 * behind one handle, and a charge walks at most JOB_MAX_DEPTH levels. */
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/string.h>

struct job {
    struct kobject    base;
    struct job       *parent;
    uint32_t          depth;   /* a root job is 0 */
    volatile uint64_t used[JOB_LIMIT_COUNT];
    volatile uint64_t limit[JOB_LIMIT_COUNT];
};

static void job_destroy(struct kobject *obj)
{
    struct job *j = (struct job *)obj;
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (j->used[k])
            panic("job koid %lu destroyed with %lu units of kind %u still charged", obj->koid,
                  j->used[k], k);
    job_uncharge(j->parent, JOB_LIMIT_HANDLES, 1);   /* this job itself */
    job_unref(j->parent);
    kfree(j);
}

static const struct kobject_ops job_ops = {
    .name = "job",
    .destroy = job_destroy,
};

status_t job_create(struct job *parent, struct job **out)
{
    if (parent && parent->depth + 1 >= JOB_MAX_DEPTH)
        return ERR_OUT_OF_RANGE;
    status_t st = job_charge(parent, JOB_LIMIT_HANDLES, 1);
    if (st != OK)
        return st;
    struct job *j = kzalloc(sizeof(*j));
    if (!j) {
        job_uncharge(parent, JOB_LIMIT_HANDLES, 1);
        return ERR_NO_MEMORY;
    }
    kobject_init(&j->base, OBJ_JOB, &job_ops, "job", 0);
    for (unsigned k = 0; k < JOB_LIMIT_COUNT; k++)
        j->limit[k] = JOB_NO_LIMIT;
    job_ref(parent);
    j->parent = parent;
    j->depth = parent ? parent->depth + 1 : 0;
    *out = j;
    return OK;
}

void job_ref(struct job *j)
{
    if (j)
        kobject_ref(&j->base);
}

void job_unref(struct job *j)
{
    if (j)
        kobject_unref(&j->base);
}

static bool kind_ok(uint32_t kind)
{
    return kind >= 1 && kind < JOB_LIMIT_COUNT;
}

status_t job_set_limit(struct job *j, uint32_t kind, uint64_t value)
{
    if (!kind_ok(kind))
        return ERR_INVALID_ARGS;
    __atomic_store_n(&j->limit[kind], value, __ATOMIC_RELAXED);
    return OK;
}

uint64_t job_used(struct job *j, uint32_t kind)
{
    return kind_ok(kind) ? __atomic_load_n(&j->used[kind], __ATOMIC_RELAXED) : 0;
}

void job_get_info(struct job *j, struct job_info *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++) {
        out->used[k] = __atomic_load_n(&j->used[k], __ATOMIC_RELAXED);
        out->limit[k] = __atomic_load_n(&j->limit[k], __ATOMIC_RELAXED);
    }
    out->koid = j->base.koid;
}

/* Undo a charge of n at every level from j up to (not including) stop. */
static void credit(struct job *j, struct job *stop, uint32_t kind, uint64_t n)
{
    for (; j != stop; j = j->parent) {
        uint64_t old = __atomic_fetch_sub(&j->used[kind], n, __ATOMIC_RELAXED);
        if (old < n)
            panic("job koid %lu: kind %u credited %lu with only %lu charged", j->base.koid,
                  kind, n, old);
    }
}

status_t job_charge(struct job *j, uint32_t kind, uint64_t n)
{
    if (!j || !n)
        return OK;
    ASSERT(kind_ok(kind));
    for (struct job *l = j; l; l = l->parent) {
        uint64_t limit = __atomic_load_n(&l->limit[kind], __ATOMIC_RELAXED);
        uint64_t old = __atomic_load_n(&l->used[kind], __ATOMIC_RELAXED);
        do {
            if (old + n < old || old + n > limit) {
                credit(j, l, kind, n);
                return kind == JOB_LIMIT_PAGES || kind == JOB_LIMIT_MSG_BYTES ? ERR_NO_MEMORY
                                                                               : ERR_NO_RESOURCES;
            }
        } while (!__atomic_compare_exchange_n(&l->used[kind], &old, old + n, true,
                                              __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    }
    return OK;
}

void job_uncharge(struct job *j, uint32_t kind, uint64_t n)
{
    if (!j || !n)
        return;
    ASSERT(kind_ok(kind));
    credit(j, NULL, kind, n);
}
