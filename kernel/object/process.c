/* Processes and user threads (see process.h for the model and lifecycle).
 *
 * References. A uthread holds a reference on its process; a started
 * uthread also holds one on itself until its thread has left
 * (uthread_exit_current drops it), and the scheduler thread's join
 * reference until the uthread is destroyed. A process's handle table
 * usually holds a handle to the process itself (SR_SELF_PROCESS), so a
 * running process can't lose its last handle; the cycle is broken when the
 * table is destroyed at teardown.
 *
 * Process fields marked (L) are guarded by the process's object lock
 * ("process"). Under it: kill cancels threads (thread_cancel takes run
 * queue locks) and job charges are taken (atomics). Nothing under it
 * sleeps or allocates.
 *
 * Races that matter:
 *   - Start vs kill: uthread_start counts the thread (nthreads++) and marks
 *     it STARTING under the lock, only if the process isn't dying; it makes
 *     the scheduler thread with the lock dropped, then publishes ut->t
 *     under the lock and cancels it itself if a kill came in between. A
 *     kill cancels every thread whose ut->t is published. So each started
 *     thread is cancelled by exactly one of the two. A thread that is
 *     cancelled before it reaches ring 3 leaves from arch_enter_user's
 *     return-to-user check.
 *   - Teardown runs once: when nthreads drops to 0 on a dying process (the
 *     last thread leaving), or in process_kill if there were no threads.
 *     nthreads can't rise again once the process is dying.
 *   - process_start inserts into the child's handle table, which teardown
 *     destroys: both hold the `setup` mutex, and start re-checks the state
 *     after its insert, so a table is never written after it is destroyed.
 *   - A NEW process whose last handle closes can never be started: it is
 *     marked dying under the lock (on_zero_handles can't sleep, so it can't
 *     tear down); what it owns is freed when its last reference goes. */
#include <jam/aspace.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/uentry.h>

enum ut_state { UT_NEW, UT_STARTING, UT_RUNNING, UT_DEAD };

#define OUT_LINE        200   /* debug_write lines longer than this are split */
#define USER_REPORT_MAX 24    /* debug_report lines kept, all processes together */

struct process {
    struct kobject      base;        /* OBJ_PROCESS */
    struct handle_table handles;
    struct mutex        setup;       /* process_start's insert vs. teardown */
    struct job         *job;         /* a reference */
    struct aspace      *as;          /* (L) a reference; NULL once torn down */
    int                 state;       /* (L) PROCESS_* */
    bool                killed;      /* (L) */
    bool                finished;    /* (L) teardown done */
    int64_t             exit_code;   /* (L) */
    uint32_t            nthreads;    /* (L) started threads that haven't left */
    struct list_node    threads;     /* (L) struct uthread, every one not destroyed */
    char                name[PROCESS_NAME_MAX];
    /* debug_write: the current, unfinished output line ("process output") */
    spinlock_t          out_lock;
    uint32_t            out_len;
    char                out[OUT_LINE];
};

struct uthread {
    struct kobject    base;          /* OBJ_THREAD */
    struct process   *proc;          /* a reference */
    struct list_node  node;          /* on proc->threads (process lock) */
    struct thread    *t;             /* (process lock) once started; our join reference */
    int               state;         /* (process lock) enum ut_state */
    int               prio;
    uint64_t          entry, stack, arg0, arg1;
    char              name[24];
};

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- jobs of the current thread ------------------------------------------- */

struct process *process_current(void)
{
    return current_thread()->process;
}

struct job *job_current(void)
{
    struct process *p = current_thread()->process;
    return p ? p->job : NULL;
}

/* ---- processes ---------------------------------------------------------------- */

static uint64_t plock(struct process *p)
{
    return spin_lock_irqsave(&p->base.lock);
}

static void punlock(struct process *p, uint64_t f)
{
    spin_unlock_irqrestore(&p->base.lock, f);
}

static void process_destroy(struct kobject *obj)
{
    struct process *p = (struct process *)obj;
    /* Every uthread holds a reference, so none can be left. A process that
     * never got a teardown (never started, all handles closed) still owns
     * its table and address space; nothing here sleeps (object teardown
     * runs with preemption off). */
    if (!list_empty(&p->threads))
        panic("process \"%s\" destroyed with threads", p->name);
    handle_table_destroy(&p->handles);
    if (p->as)
        aspace_unref(p->as);
    job_unref(p->job);
    kfree(p);
}

/* The last handle is gone. A process that is running keeps running (its
 * threads don't need handles to it); one that was never started never can
 * be now, so it counts as killed. */
static void process_on_zero_handles(struct kobject *obj)
{
    struct process *p = (struct process *)obj;
    uint64_t f = plock(p);
    if (p->state == PROCESS_NEW) {
        p->state = PROCESS_DYING;
        p->killed = true;
        p->exit_code = PROCESS_KILLED_CODE;
    }
    punlock(p, f);
}

static const struct kobject_ops process_ops = {
    .name = "process",
    .destroy = process_destroy,
    .on_zero_handles = process_on_zero_handles,
};

status_t process_create(struct job *job, const char *name, struct process **out)
{
    struct process *p = kzalloc(sizeof(*p));
    if (!p)
        return ERR_NO_MEMORY;
    status_t st = aspace_create(&p->as);
    if (st != OK) {
        kfree(p);
        return st;
    }
    kobject_init(&p->base, OBJ_PROCESS, &process_ops, "process", 0);
    handle_table_init(&p->handles);
    p->handles.job = job;
    mutex_init(&p->setup, "process setup");
    spin_init(&p->out_lock, "process output");
    job_ref(job);
    p->job = job;
    p->state = PROCESS_NEW;
    list_init(&p->threads);
    copy_name(p->name, sizeof(p->name), name);
    *out = p;
    return OK;
}

struct handle_table *process_handles(struct process *p)
{
    return &p->handles;
}

struct aspace *process_aspace(struct process *p)
{
    uint64_t f = plock(p);
    struct aspace *as = p->as;
    if (as)
        aspace_ref(as);
    punlock(p, f);
    return as;
}

struct job *process_job(struct process *p)
{
    return p->job;
}

const char *process_name(struct process *p)
{
    return p->name;
}

void process_get_info(struct process *p, struct process_info *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t f = plock(p);
    out->state = (uint32_t)p->state;
    out->exit_code = p->exit_code;
    out->killed = p->killed;
    out->threads = p->nthreads;
    punlock(p, f);
    out->koid = p->base.koid;
}

/* ---- debug output ---------------------------------------------------------- */

/* With out_lock held: print the buffered line with the process name. */
static void out_flush_locked(struct process *p, bool report_it)
{
    p->out[p->out_len] = '\0';
    if (report_it)
        report("%s", p->out);   /* prints it too */
    else
        kprintf("[%s] %s\n", p->name, p->out);
    p->out_len = 0;
}

void process_debug_write(struct process *p, const char *buf, size_t n, bool report_it)
{
    static volatile uint32_t reports;
    if (report_it && __atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) >= USER_REPORT_MAX)
        report_it = false;   /* the RESULTS box is for a few lines */
    uint64_t f = spin_lock_irqsave(&p->out_lock);
    if (report_it && p->out_len)
        out_flush_locked(p, false);   /* someone's unfinished line first */
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            out_flush_locked(p, report_it);
            continue;
        }
        if ((c < 0x20 && c != '\t') || c >= 0x7f)
            c = '?';   /* no escape sequences on the console */
        p->out[p->out_len++] = c;
        if (p->out_len == OUT_LINE - 1)
            out_flush_locked(p, report_it);
    }
    if (report_it && p->out_len)
        out_flush_locked(p, true);   /* a report is always a whole line */
    spin_unlock_irqrestore(&p->out_lock, f);
}

/* Close the handle table and drop the address space: the process is dead.
 * Runs once (see the file header), in a context that may sleep, with a
 * reference on p held by the caller. */
static void process_finish(struct process *p)
{
    mutex_lock(&p->setup);
    handle_table_destroy(&p->handles);   /* credits the job for every slot */
    mutex_unlock(&p->setup);
    uint64_t of = spin_lock_irqsave(&p->out_lock);
    if (p->out_len)
        out_flush_locked(p, false);   /* its last words, even without a newline */
    spin_unlock_irqrestore(&p->out_lock, of);

    uint64_t f = plock(p);
    struct aspace *as = p->as;
    p->as = NULL;
    p->state = PROCESS_DEAD;
    p->finished = true;
    punlock(p, f);
    /* Every thread dropped its own reference before it counted itself out,
     * so this is the last one unless another process holds a vmar handle:
     * the address space, its mappings and their pages go now. */
    if (as)
        aspace_unref(as);
    kobject_signal(&p->base, 0, SIG_TERMINATED);
}

void process_kill(struct process *p, int64_t code, bool killed)
{
    uint64_t f = plock(p);
    if (p->state >= PROCESS_DYING) {
        punlock(p, f);
        return;
    }
    p->state = PROCESS_DYING;
    p->exit_code = code;
    p->killed = killed;
    for (struct list_node *n = p->threads.next; n != &p->threads; n = n->next) {
        struct uthread *u = container_of(n, struct uthread, node);
        if (u->t && u->state != UT_DEAD)
            thread_cancel(u->t);
    }
    bool finish = p->nthreads == 0;
    punlock(p, f);
    if (finish)
        process_finish(p);
}

/* ---- user threads -------------------------------------------------------------- */

static void uthread_destroy(struct kobject *obj)
{
    struct uthread *u = (struct uthread *)obj;
    struct process *p = u->proc;
    uint64_t f = plock(p);
    list_del(&u->node);
    punlock(p, f);
    if (u->t)
        thread_detach(u->t);
    kobject_unref(&p->base);
    kfree(u);
}

static const struct kobject_ops uthread_ops = {
    .name = "thread",
    .destroy = uthread_destroy,
};

status_t uthread_create(struct process *p, const char *name, struct uthread **out)
{
    struct uthread *u = kzalloc(sizeof(*u));
    if (!u)
        return ERR_NO_MEMORY;
    kobject_init(&u->base, OBJ_THREAD, &uthread_ops, "thread", 0);
    copy_name(u->name, sizeof(u->name), name);
    u->prio = PRIO_DEFAULT;
    u->state = UT_NEW;
    uint64_t f = plock(p);
    bool dying = p->state >= PROCESS_DYING;
    if (!dying) {
        kobject_ref(&p->base);
        u->proc = p;
        list_add_tail(&p->threads, &u->node);
    }
    punlock(p, f);
    if (dying) {
        kfree(u);
        return ERR_BAD_STATE;
    }
    *out = u;
    return OK;
}

struct process *uthread_process(struct uthread *u)
{
    return u->proc;
}

status_t uthread_set_priority(struct uthread *u, int prio)
{
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return ERR_INVALID_ARGS;
    uint64_t f = plock(u->proc);
    u->prio = prio;
    struct thread *t = u->state != UT_DEAD ? u->t : NULL;
    if (t)
        thread_set_priority(t, prio);   /* takes effect at its next switch */
    punlock(u->proc, f);
    return OK;
}

/* A started thread has left (or never got going): count it out. Returns
 * true if the caller must run the teardown. */
static bool thread_left(struct process *p)
{
    uint64_t f = plock(p);
    if (p->nthreads == 0)
        panic("process \"%s\": thread count underflow", p->name);
    /* Credit the job BEFORE counting ourselves out, under the lock: the
     * thread that brings nthreads to 0 signals SIG_TERMINATED, and whoever
     * sees that must see every thread's credit. Crediting after the unlock
     * (as first written) let the last thread finish while another was
     * still between its count and its credit; the PC stress test caught
     * it at 14 s ("a dead process left something charged to its job"). */
    job_uncharge(p->job, JOB_LIMIT_THREADS, 1);
    bool finish = false;
    if (--p->nthreads == 0) {
        if (p->state == PROCESS_RUNNING) {   /* the last thread left: exit 0 */
            p->state = PROCESS_DYING;
            p->exit_code = 0;
        }
        finish = p->state == PROCESS_DYING;
    }
    punlock(p, f);
    return finish;
}

_Noreturn void uthread_exit_current(void)
{
    struct thread *t = current_thread();
    struct uthread *u = t->uthread;
    struct process *p = u->proc;
    if (!irqs_enabled())
        panic("uthread_exit_current with interrupts off");

    /* Leave the address space first, so once we count ourselves out below
     * no CPU has it loaded on our behalf and our reference is gone. */
    irq_disable();
    struct aspace *as = t->aspace;
    t->aspace = NULL;
    aspace_switch(as, NULL);
    irq_enable();
    if (as)
        aspace_unref(as);

    uint64_t f = plock(p);
    u->state = UT_DEAD;
    punlock(p, f);
    if (thread_left(p))
        process_finish(p);   /* our uthread's reference keeps p alive */
    /* Only now: whoever sees SIG_TERMINATED must also see the job credited
     * for this thread (and, for the last one, the process torn down). */
    kobject_signal(&u->base, 0, SIG_TERMINATED);
    t->process = NULL;
    t->uthread = NULL;
    kobject_unref(&u->base);   /* the running thread's own reference */
    thread_exit();
}

/* The scheduler thread of a user thread starts here, in the kernel. */
static void uthread_main(void *arg)
{
    struct uthread *u = arg;
    struct thread *t = current_thread();
    struct process *p = u->proc;
    t->uthread = u;
    t->process = p;

    uint64_t f = plock(p);
    struct aspace *as = p->state < PROCESS_DYING ? p->as : NULL;
    if (as)
        aspace_ref(as);
    u->state = UT_RUNNING;
    punlock(p, f);
    if (!as || fpu_ustate_alloc(t) != OK) {
        if (as)
            aspace_unref(as);
        uthread_exit_current();
    }
    /* This CPU has the kernel's tables loaded (we are a thread without an
     * address space until now): switch to ours before anyone can see
     * t->aspace and switch us out of it. */
    irq_disable();
    t->aspace = as;
    aspace_switch(NULL, as);
    irq_enable();
    /* A kill from here on is caught by arch_enter_user's return-to-user
     * check, since the cancel flag is set before the thread is woken. */
    arch_enter_user(u->entry, u->stack, u->arg0, u->arg1);
}

/* Start u. With `from_new`, p must be NEW and becomes RUNNING (process_start);
 * otherwise it must already be RUNNING. On failure, *finish says whether
 * the caller must run the teardown (outside the setup mutex). */
static status_t start_thread(struct uthread *u, uint64_t entry, uint64_t stack, uint64_t arg0,
                             uint64_t arg1, const cpumask_t *mask, bool from_new, bool *finish)
{
    struct process *p = u->proc;
    *finish = false;
    uint64_t f = plock(p);
    status_t st = OK;
    if (u->state != UT_NEW)
        st = ERR_BAD_STATE;
    else if (p->state != (from_new ? PROCESS_NEW : PROCESS_RUNNING))
        st = ERR_BAD_STATE;
    else
        st = job_charge(p->job, JOB_LIMIT_THREADS, 1);
    if (st == OK) {
        u->state = UT_STARTING;
        u->entry = entry;
        u->stack = stack;
        u->arg0 = arg0;
        u->arg1 = arg1;
        p->nthreads++;
        if (from_new)
            p->state = PROCESS_RUNNING;
        kobject_ref(&u->base);   /* the running thread's own; dropped as it leaves */
    }
    int prio = u->prio;
    punlock(p, f);
    if (st != OK)
        return st;

    struct thread *t = thread_try_create_capped(u->name, uthread_main, u, prio, mask,
                                                PRIO_USER_MAX);
    if (!t) {
        f = plock(p);
        u->state = UT_NEW;
        if (from_new && p->state == PROCESS_RUNNING && p->nthreads == 1)
            p->state = PROCESS_NEW;   /* as if the start never happened */
        punlock(p, f);
        *finish = thread_left(p);
        kobject_unref(&u->base);
        return ERR_NO_MEMORY;
    }
    f = plock(p);
    u->t = t;
    bool dying = p->state >= PROCESS_DYING;
    punlock(p, f);
    if (dying)
        thread_cancel(t);   /* a kill ran before u->t was published */
    return OK;
}

status_t uthread_start(struct uthread *u, uint64_t entry, uint64_t stack, uint64_t arg0,
                       uint64_t arg1, const cpumask_t *mask)
{
    bool finish;
    status_t st = start_thread(u, entry, stack, arg0, arg1, mask, false, &finish);
    if (finish)
        process_finish(u->proc);
    return st;
}

status_t process_start(struct process *p, struct uthread *u, uint64_t entry, uint64_t stack,
                       struct khandle *arg0, uint64_t arg1, const cpumask_t *mask)
{
    if (u->proc != p)
        return ERR_INVALID_ARGS;
    bool finish = false;
    mutex_lock(&p->setup);
    uint64_t f = plock(p);
    status_t st = p->state == PROCESS_NEW ? OK : ERR_BAD_STATE;
    punlock(p, f);
    handle_t hv = HANDLE_INVALID;
    if (st == OK && arg0 && arg0->obj)
        st = handle_insert(&p->handles, arg0, &hv);
    if (st == OK)
        st = start_thread(u, entry, stack, hv, arg1, mask, true, &finish);
    if (st != OK && hv != HANDLE_INVALID) {
        /* Give arg0 back: nothing in the child can have seen it. */
        if (handle_remove(&p->handles, hv, arg0) != OK)
            panic("process_start: lost the startup handle");
    }
    mutex_unlock(&p->setup);
    if (finish)
        process_finish(p);
    return st;
}
