#include <jam/kprintf.h>
#include <jam/object.h>
#include <jam/panic.h>
#include <jam/sched.h>
#include <jam/time.h>

static volatile uint64_t next_koid = 1024;   /* small numbers reserved */

void kobject_init(struct kobject *obj, enum obj_type type, const struct kobject_ops *ops,
                  const char *lock_name, signals_t initial)
{
    ASSERT(ops && ops->destroy);
    obj->ops = ops;
    obj->type = type;
    obj->refs = 1;
    obj->handles = 0;
    obj->signals = initial;
    spin_init(&obj->lock, lock_name);
    list_init(&obj->observers);
    obj->koid = __atomic_fetch_add(&next_koid, 1, __ATOMIC_RELAXED);
}

void kobject_ref(struct kobject *obj)
{
    uint32_t old = __atomic_fetch_add(&obj->refs, 1, __ATOMIC_RELAXED);
    if (old == 0)
        panic("kobject: ref of a dead %s (koid %lu)", obj->ops->name, obj->koid);
}

void kobject_unref(struct kobject *obj)
{
    uint32_t left = __atomic_sub_fetch(&obj->refs, 1, __ATOMIC_ACQ_REL);
    if (left == UINT32_MAX)
        panic("kobject: %s (koid %lu) unreferenced too many times", obj->ops->name, obj->koid);
    if (left == 0) {
        if (!list_empty(&obj->observers))
            panic("kobject: %s (koid %lu) destroyed with observers attached",
                  obj->ops->name, obj->koid);
        obj->ops->destroy(obj);
    }
}

void kobject_handle_gain(struct kobject *obj)
{
    __atomic_add_fetch(&obj->handles, 1, __ATOMIC_RELAXED);
}

void kobject_handle_drop(struct kobject *obj)
{
    uint32_t left = __atomic_sub_fetch(&obj->handles, 1, __ATOMIC_ACQ_REL);
    if (left == UINT32_MAX)
        panic("kobject: %s (koid %lu) handle count underflow", obj->ops->name, obj->koid);
    if (left == 0 && obj->ops->on_zero_handles)
        obj->ops->on_zero_handles(obj);
}

void kobject_signal_locked(struct kobject *obj, signals_t clear, signals_t set)
{
    signals_t old = obj->signals;
    signals_t now = (old & ~clear) | set;
    if (now == old)
        return;
    obj->signals = now;
    /* An observer may remove itself from the list while firing. */
    for (struct list_node *n = obj->observers.next; n != &obj->observers;) {
        struct observer *o = container_of(n, struct observer, node);
        n = n->next;
        o->fire(o, now);
    }
}

void kobject_signal(struct kobject *obj, signals_t clear, signals_t set)
{
    uint64_t f = spin_lock_irqsave(&obj->lock);
    kobject_signal_locked(obj, clear, set);
    spin_unlock_irqrestore(&obj->lock, f);
}

signals_t kobject_signals(struct kobject *obj)
{
    return __atomic_load_n(&obj->signals, __ATOMIC_ACQUIRE);
}

void kobject_observe(struct kobject *obj, struct observer *o)
{
    uint64_t f = spin_lock_irqsave(&obj->lock);
    list_add_tail(&obj->observers, &o->node);
    if (obj->signals & o->mask)
        o->fire(o, obj->signals);
    spin_unlock_irqrestore(&obj->lock, f);
}

void kobject_unobserve(struct kobject *obj, struct observer *o)
{
    uint64_t f = spin_lock_irqsave(&obj->lock);
    if (o->node.next)
        list_del(&o->node);
    spin_unlock_irqrestore(&obj->lock, f);
}

/* ---- object_wait_one ------------------------------------------------------ */

struct one_waiter {
    struct observer obs;
    struct thread  *thread;
    bool            hit;
};

static void one_waiter_fire(struct observer *o, signals_t current)
{
    struct one_waiter *w = container_of(o, struct one_waiter, obs);
    if (current & o->mask) {
        w->hit = true;
        thread_wake(w->thread);
    }
}

status_t object_wait_one(struct kobject *obj, signals_t mask, uint64_t deadline_ns,
                         signals_t *observed)
{
    struct one_waiter w = {
        .obs = { .mask = mask, .fire = one_waiter_fire },
        .thread = current_thread(),
        .hit = false,
    };
    uint64_t f = spin_lock_irqsave(&obj->lock);
    status_t st = OK;
    if (!(obj->signals & mask)) {
        list_add_tail(&obj->observers, &w.obs.node);
        while (!w.hit) {
            if (uptime_ns() >= deadline_ns) {
                st = ERR_TIMED_OUT;
                break;
            }
            thread_block(&obj->lock, &f, deadline_ns);
        }
        list_del(&w.obs.node);
    }
    if (observed)
        *observed = obj->signals;
    spin_unlock_irqrestore(&obj->lock, f);
    return st;
}

/* ---- names ------------------------------------------------------------ */

const char *obj_type_name(enum obj_type t)
{
    static const char *const names[OBJ_TYPE_COUNT] = {
        [OBJ_NONE] = "none", [OBJ_EVENT] = "event", [OBJ_TIMER] = "timer",
        [OBJ_CHANNEL] = "channel", [OBJ_PORT] = "port", [OBJ_VMO] = "vmo",
        [OBJ_DMA_CAP] = "dma_cap", [OBJ_PROCESS] = "process", [OBJ_THREAD] = "thread",
        [OBJ_INTERRUPT] = "interrupt", [OBJ_RESOURCE] = "resource",
    };
    return (unsigned)t < OBJ_TYPE_COUNT && names[t] ? names[t] : "?";
}

const char *status_str(status_t s)
{
    switch (s) {
    case OK:                   return "OK";
    case ERR_INTERNAL:         return "ERR_INTERNAL";
    case ERR_NOT_SUPPORTED:    return "ERR_NOT_SUPPORTED";
    case ERR_NO_MEMORY:        return "ERR_NO_MEMORY";
    case ERR_INVALID_ARGS:     return "ERR_INVALID_ARGS";
    case ERR_BAD_HANDLE:       return "ERR_BAD_HANDLE";
    case ERR_WRONG_TYPE:       return "ERR_WRONG_TYPE";
    case ERR_ACCESS_DENIED:    return "ERR_ACCESS_DENIED";
    case ERR_BAD_STATE:        return "ERR_BAD_STATE";
    case ERR_OUT_OF_RANGE:     return "ERR_OUT_OF_RANGE";
    case ERR_BUFFER_TOO_SMALL: return "ERR_BUFFER_TOO_SMALL";
    case ERR_SHOULD_WAIT:      return "ERR_SHOULD_WAIT";
    case ERR_TIMED_OUT:        return "ERR_TIMED_OUT";
    case ERR_PEER_CLOSED:      return "ERR_PEER_CLOSED";
    case ERR_CANCELED:         return "ERR_CANCELED";
    case ERR_ALREADY_BOUND:    return "ERR_ALREADY_BOUND";
    case ERR_NOT_FOUND:        return "ERR_NOT_FOUND";
    case ERR_NO_RESOURCES:     return "ERR_NO_RESOURCES";
    default:                   return "ERR_?";
    }
}
