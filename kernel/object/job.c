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
 * behind one handle, and a charge walks at most JOB_MAX_DEPTH levels.
 *
 * The tree (review R8, for job_kill). A job lists its child jobs and its
 * live processes, under its object lock (class "job", interrupts off; only
 * list edits and the `killed` flag happen under it, and no other job's lock
 * is ever taken inside it). A child job is listed from job_create until it
 * is destroyed (it holds a reference on its parent, so the parent outlives
 * the listing); a process from process_create until it is torn down or
 * destroyed (job_attach_process / job_detach_process), before it drops its
 * job reference. Once `killed`, a job takes no new processes or child jobs
 * (ERR_BAD_STATE).
 *
 * job_kill never holds a job lock while it kills or waits: it picks one
 * listed object at a time and takes a reference with kobject_tryref (an
 * object whose last reference is already gone is being destroyed and
 * unlists itself). Processes are marked `kill_seen` (under the job lock)
 * so each is killed once; the kill pass over the whole subtree comes first
 * and the wait pass second, so a caller inside the job (killing its own
 * job) still kills everything before its own wait is cancelled. A child
 * job it holds a reference on stays listed, so the walk continues from its
 * list node. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/string.h>
#include <jam/time.h>

struct job {
    struct kobject    base;
    struct job       *parent;
    uint32_t          depth;      /* a root job is 0 */
    bool              killed;     /* (L) no new processes or child jobs */
    struct list_node  children;   /* (L) struct job, by child_node */
    struct list_node  child_node; /* on parent->children (the parent's lock) */
    struct list_node  procs;      /* (L) struct job_link of each live process */
    volatile uint64_t used[JOB_LIMIT_COUNT];
    volatile uint64_t limit[JOB_LIMIT_COUNT];
};

static uint64_t jlock(struct job *j)
{
    return spin_lock_irqsave(&j->base.lock);
}

static void junlock(struct job *j, uint64_t f)
{
    spin_unlock_irqrestore(&j->base.lock, f);
}

static void job_destroy(struct kobject *obj)
{
    struct job *j = (struct job *)obj;
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (j->used[k])
            panic("job koid %lu destroyed with %lu units of kind %u still charged", obj->koid,
                  j->used[k], k);
    /* Children and processes hold references, so both lists are empty. */
    if (!list_empty(&j->children) || !list_empty(&j->procs))
        panic("job koid %lu destroyed with children or processes listed", obj->koid);
    if (j->parent) {
        uint64_t f = jlock(j->parent);
        list_del(&j->child_node);
        junlock(j->parent, f);
    }
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
    list_init(&j->children);
    list_init(&j->procs);
    j->depth = parent ? parent->depth + 1 : 0;
    if (parent) {
        uint64_t f = jlock(parent);
        bool killed = parent->killed;
        if (!killed)
            list_add_tail(&parent->children, &j->child_node);
        junlock(parent, f);
        if (killed) {
            kfree(j);
            job_uncharge(parent, JOB_LIMIT_HANDLES, 1);
            return ERR_BAD_STATE;
        }
        job_ref(parent);
    }
    j->parent = parent;
    *out = j;
    return OK;
}

status_t job_attach_process(struct job *j, struct job_link *l)
{
    l->kill_seen = false;
    uint64_t f = jlock(j);
    bool killed = j->killed;
    if (!killed)
        list_add_tail(&j->procs, &l->node);
    junlock(j, f);
    return killed ? ERR_BAD_STATE : OK;
}

void job_detach_process(struct job *j, struct job_link *l)
{
    uint64_t f = jlock(j);
    list_del(&l->node);
    junlock(j, f);
}

/* ---- job_kill ------------------------------------------------------------- */

/* A referenced process of j: the first one not yet killed (kill pass) or
 * the first one at all (wait pass: it is still listed, so still alive).
 * NULL if there is none. */
static struct process *pick_process(struct job *j, bool to_kill)
{
    struct process *p = NULL;
    uint64_t f = jlock(j);
    for (struct list_node *n = j->procs.next; n != &j->procs; n = n->next) {
        struct job_link *l = container_of(n, struct job_link, node);
        if (to_kill && l->kill_seen)
            continue;
        struct process *q = process_from_job_link(l);
        if (kobject_tryref(process_kobject(q))) {
            if (to_kill)
                l->kill_seen = true;
            p = q;
            break;
        }
    }
    junlock(j, f);
    return p;
}

/* A referenced child of j listed after `after` (NULL: the first), or NULL.
 * `after` must be referenced (so it is still listed). */
static struct job *next_child(struct job *j, struct job *after)
{
    struct job *c = NULL;
    uint64_t f = jlock(j);
    for (struct list_node *n = after ? after->child_node.next : j->children.next;
         n != &j->children; n = n->next) {
        struct job *cand = container_of(n, struct job, child_node);
        if (kobject_tryref(&cand->base)) {
            c = cand;
            break;
        }
    }
    junlock(j, f);
    return c;
}

/* Mark the subtree killed and kill every process in it (no waiting). The
 * recursion is at most JOB_MAX_DEPTH deep. */
static unsigned kill_tree(struct job *j)
{
    uint64_t f = jlock(j);
    j->killed = true;
    junlock(j, f);
    unsigned killed = 0;
    struct process *p;
    while ((p = pick_process(j, true))) {
        process_kill(p, PROCESS_KILLED_CODE, true);
        kobject_unref(process_kobject(p));
        killed++;
    }
    for (struct job *c = next_child(j, NULL), *next; c; c = next) {
        killed += kill_tree(c);
        next = next_child(j, c);
        job_unref(c);   /* outside j's lock: c's destroy takes it */
    }
    return killed;
}

/* Wait until no process is left in the subtree. */
static status_t wait_tree(struct job *j)
{
    struct process *p;
    while ((p = pick_process(j, false))) {
        status_t st = object_wait_one(process_kobject(p), SIG_TERMINATED, DEADLINE_NEVER, NULL);
        kobject_unref(process_kobject(p));
        if (st != OK)
            return st;   /* ERR_CANCELED: the caller itself was killed */
    }
    for (struct job *c = next_child(j, NULL), *next; c; c = next) {
        status_t st = wait_tree(c);
        next = st == OK ? next_child(j, c) : NULL;
        job_unref(c);
        if (st != OK)
            return st;
    }
    return OK;
}

status_t job_kill(struct job *j, unsigned *killed)
{
    unsigned n = kill_tree(j);
    if (killed)
        *killed = n;
    return wait_tree(j);
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

/* ---- listing (M7: the shell's `ps`, through debug_command) ----------------- */

#define PRINT_MAX 32   /* processes / child jobs shown per job */

void job_print_tree(struct job *j, unsigned depth)
{
    char pad[2 * JOB_MAX_DEPTH + 1];
    unsigned w = depth < JOB_MAX_DEPTH ? depth * 2 : 2 * JOB_MAX_DEPTH;
    memset(pad, ' ', w);
    pad[w] = '\0';
    kprintf("%sjob %lu: %lu pages, %lu handles, %lu threads\n", pad, j->base.koid,
            job_used(j, JOB_LIMIT_PAGES), job_used(j, JOB_LIMIT_HANDLES),
            job_used(j, JOB_LIMIT_THREADS));

    /* References taken under the lock, used outside it (as job_kill does). */
    struct kobject *procs[PRINT_MAX];
    struct job *kids[PRINT_MAX];
    unsigned np = 0, nk = 0, more = 0;
    uint64_t f = jlock(j);
    for (struct list_node *n = j->procs.next; n != &j->procs; n = n->next) {
        struct process *p = process_from_job_link(container_of(n, struct job_link, node));
        if (np < PRINT_MAX && kobject_tryref(process_kobject(p)))
            procs[np++] = process_kobject(p);
        else
            more++;
    }
    for (struct list_node *n = j->children.next; n != &j->children; n = n->next) {
        struct job *c = container_of(n, struct job, child_node);
        if (nk < PRINT_MAX && kobject_tryref(&c->base))
            kids[nk++] = c;
        else
            more++;
    }
    junlock(j, f);

    static const char *const states[] = { "new", "running", "dying", "dead" };
    for (unsigned i = 0; i < np; i++) {
        struct process *p = process_from_kobject(procs[i]);
        struct process_info info;
        process_get_info(p, &info);
        kprintf("%s  process %lu %-16s %-8s %u thread%s\n", pad, info.koid, process_name(p),
                info.state < 4 ? states[info.state] : "?", info.threads,
                info.threads == 1 ? "" : "s");
        kobject_unref(procs[i]);
    }
    for (unsigned i = 0; i < nk; i++) {
        job_print_tree(kids[i], depth + 1);
        kobject_unref(&kids[i]->base);
    }
    if (more)
        kprintf("%s  (%u more not shown)\n", pad, more);
}

void job_list_processes(struct job *j, uint32_t depth, struct proc_stat *out, uint32_t cap,
                        uint32_t *n)
{
    struct kobject *procs[PRINT_MAX];
    struct job *kids[PRINT_MAX];
    unsigned np = 0, nk = 0;
    uint64_t f = jlock(j);
    for (struct list_node *e = j->procs.next; e != &j->procs && np < PRINT_MAX; e = e->next) {
        struct process *p = process_from_job_link(container_of(e, struct job_link, node));
        if (kobject_tryref(process_kobject(p)))
            procs[np++] = process_kobject(p);
    }
    for (struct list_node *e = j->children.next; e != &j->children && nk < PRINT_MAX;
         e = e->next) {
        struct job *c = container_of(e, struct job, child_node);
        if (kobject_tryref(&c->base))
            kids[nk++] = c;
    }
    junlock(j, f);
    uint64_t pages = job_used(j, JOB_LIMIT_PAGES);
    for (unsigned i = 0; i < np; i++) {
        struct process *p = process_from_kobject(procs[i]);
        if (*n < cap) {
            struct proc_stat *s = &out[(*n)++];
            struct process_info info;
            process_get_info(p, &info);
            memset(s, 0, sizeof(*s));
            s->koid = info.koid;
            s->job_koid = j->base.koid;
            s->cpu_ns = tsc_to_ns(process_cpu_tsc(p));
            s->job_pages = pages;
            s->state = info.state;
            s->threads = info.threads;
            s->depth = depth;
            const char *name = process_name(p);
            size_t l = strlen(name);
            memcpy(s->name, name, l < sizeof(s->name) - 1 ? l : sizeof(s->name) - 1);
        }
        kobject_unref(procs[i]);
    }
    for (unsigned i = 0; i < nk; i++) {
        if (*n < cap)
            job_list_processes(kids[i], depth + 1, out, cap, n);
        kobject_unref(&kids[i]->base);
    }
}

struct job *job_root_of(struct job *j)
{
    while (j && j->parent)
        j = j->parent;
    job_ref(j);
    return j;
}

/* Is a a strict ancestor of j? */
static bool job_above(struct job *a, struct job *j)
{
    for (j = j ? j->parent : NULL; j; j = j->parent)
        if (j == a)
            return true;
    return false;
}

struct process *job_find_process(struct job *j, const char *name, struct job *spare)
{
    struct kobject *procs[PRINT_MAX];
    struct job *kids[PRINT_MAX];
    unsigned np = 0, nk = 0;
    /* The processes of the caller's ancestor jobs (init, its supervisor)
     * are never found: killing them leaves the caller unsupervised. */
    bool skip_procs = spare && job_above(j, spare);
    uint64_t f = jlock(j);
    for (struct list_node *n = j->procs.next; n != &j->procs && np < PRINT_MAX && !skip_procs;
         n = n->next) {
        struct process *p = process_from_job_link(container_of(n, struct job_link, node));
        if (kobject_tryref(process_kobject(p)))
            procs[np++] = process_kobject(p);
    }
    for (struct list_node *n = j->children.next; n != &j->children && nk < PRINT_MAX;
         n = n->next) {
        struct job *c = container_of(n, struct job, child_node);
        if (kobject_tryref(&c->base))
            kids[nk++] = c;
    }
    junlock(j, f);
    struct process *found = NULL;
    for (unsigned i = 0; i < np; i++) {
        struct process *p = process_from_kobject(procs[i]);
        if (!found && !strcmp(process_name(p), name))
            found = p;   /* keeps the reference */
        else
            kobject_unref(procs[i]);
    }
    for (unsigned i = 0; i < nk; i++) {
        if (!found)
            found = job_find_process(kids[i], name, spare);
        kobject_unref(&kids[i]->base);
    }
    return found;
}
