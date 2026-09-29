/* Timer objects: a timer asserts SIG_SIGNALED once uptime reaches its
 * deadline.
 *
 * One kernel thread, "timer service", serves every timer: armed timers sit
 * on one list sorted by deadline, and the service sleeps until the earliest.
 * Arming an earlier timer wakes it to re-plan. service_lock guards the list
 * and each timer's `armed` and `deadline_ns`; it is taken before the timer's
 * own object lock (kobject_signal), never after. The service thread starts
 * with the first timer_create, so it costs nothing until a timer exists. */
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/timer.h>

#define SERVICE_PRIO 28   /* above ordinary threads, below real-time */

static spinlock_t service_lock = SPINLOCK_INIT("timer service");
static struct list_node armed = LIST_INIT(armed);   /* by deadline, earliest first */
static struct thread *service;                      /* NULL until it is running */
static bool service_started;

/* With service_lock held. Ties keep arming order. */
static void insert_sorted(struct ktimer *t)
{
    struct list_node *pos = armed.prev;
    while (pos != &armed && container_of(pos, struct ktimer, node)->deadline_ns > t->deadline_ns)
        pos = pos->prev;
    list_add(pos, &t->node);
    t->armed = true;
}

static void disarm_locked(struct ktimer *t)
{
    if (t->armed) {
        list_del(&t->node);
        t->armed = false;
    }
}

/* With service_lock held: signal every timer whose deadline has passed. */
static void expire_locked(uint64_t now)
{
    while (!list_empty(&armed)) {
        struct ktimer *t = list_first(&armed, struct ktimer, node);
        if (t->deadline_ns > now)
            break;
        disarm_locked(t);
        kobject_signal(&t->base, 0, SIG_SIGNALED);
    }
}

static void service_main(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&service_lock);
    service = current_thread();
    for (;;) {
        expire_locked(uptime_ns());
        uint64_t next = list_empty(&armed) ? DEADLINE_NEVER
                                           : list_first(&armed, struct ktimer, node)->deadline_ns;
        /* Woken at the deadline, or early by timer_set arming an earlier
         * timer; either way the loop re-reads the list. */
        thread_block(&service_lock, &f, next);
    }
}

/* User code can make the first timer, so running out of memory for
 * the service thread is an error (and the next timer_create tries again),
 * not a panic. */
static status_t service_start(void)
{
    if (__atomic_load_n(&service_started, __ATOMIC_ACQUIRE) ||
        __atomic_exchange_n(&service_started, true, __ATOMIC_ACQ_REL))
        return OK;
    /* Timers armed before the thread reaches its loop are found there. */
    struct thread *t = thread_try_create_on("timer service", service_main, NULL, SERVICE_PRIO,
                                            NULL);
    if (!t) {
        __atomic_store_n(&service_started, false, __ATOMIC_RELEASE);
        return ERR_NO_MEMORY;
    }
    thread_detach(t);
    return OK;
}

static void timer_destroy(struct kobject *obj)
{
    struct ktimer *t = container_of(obj, struct ktimer, base);
    uint64_t f = spin_lock_irqsave(&service_lock);
    disarm_locked(t);
    spin_unlock_irqrestore(&service_lock, f);
    kfree(t);
}

static const struct kobject_ops timer_ops = {
    .name = "timer",
    .destroy = timer_destroy,
};

status_t timer_create(struct ktimer **out)
{
    struct ktimer *t = kzalloc(sizeof(*t));
    if (!t)
        return ERR_NO_MEMORY;
    status_t st = service_start();
    if (st != OK) {
        kfree(t);
        return st;
    }
    kobject_init(&t->base, OBJ_TIMER, &timer_ops, "timer", 0);
    *out = t;
    return OK;
}

status_t timer_set(struct ktimer *t, uint64_t deadline_ns)
{
    uint64_t f = spin_lock_irqsave(&service_lock);
    disarm_locked(t);
    kobject_signal(&t->base, SIG_SIGNALED, 0);
    if (deadline_ns <= uptime_ns()) {
        kobject_signal(&t->base, 0, SIG_SIGNALED);
    } else {
        t->deadline_ns = deadline_ns;
        insert_sorted(t);
        /* The service sleeps until the old earliest deadline: if this one
         * is sooner, wake it to re-plan. */
        if (armed.next == &t->node && service)
            thread_wake(service);
    }
    spin_unlock_irqrestore(&service_lock, f);
    return OK;
}

status_t timer_cancel(struct ktimer *t)
{
    uint64_t f = spin_lock_irqsave(&service_lock);
    disarm_locked(t);
    kobject_signal(&t->base, SIG_SIGNALED, 0);
    spin_unlock_irqrestore(&service_lock, f);
    return OK;
}
