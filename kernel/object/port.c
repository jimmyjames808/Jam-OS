/* Ports. See port.h for the lock order.
 *
 * Each binding is an observer on the watched object and owns one queue
 * entry, so signal packets never allocate and a PERSISTENT binding's edges
 * coalesce into its one entry (count++) while it is queued. A binding is
 * reference counted, because three parties can outlive each other:
 *   - the port's binding list (one reference, which also stands for the
 *     binding's reference on the watched object); whoever takes the
 *     binding off the list (unbind, port destroy, or dequeuing a spent
 *     ONCE packet) first detaches it from the object, then drops both;
 *   - the queue (one reference while its entry is queued), dropped by
 *     whoever dequeues or drains it.
 * The observer registration needs no reference of its own: it is always
 * removed, under the object's lock, before the list's reference drops.
 *
 * Job charges: a user packet and a binding are kernel memory
 * a process makes the port hold (up to PORT_MAX_* each), so they are
 * charged as JOB_LIMIT_MSG_BYTES to the job of whoever queued / bound them
 * (job_current(); kernel callers are never charged), like a channel
 * message, from creation until they are freed. A binding also pays
 * JOB_OBJECT_BYTES for the object it watches: it holds a reference, so it
 * may be what keeps that object alive. Each keeps a job reference. */
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/port.h>
#include <jam/process.h>
#include <jam/time.h>

struct port_binding;

struct port_qentry {
    struct list_node     node;      /* in port->queue */
    struct port_binding *binding;   /* NULL: a user packet, freed when dequeued */
    struct job          *job;       /* user packet: charged USER_PACKET_CHARGE (a reference) */
    struct port_packet   pkt;
};

#define USER_PACKET_CHARGE sizeof(struct port_qentry)
#define BINDING_CHARGE     (sizeof(struct port_binding) + JOB_OBJECT_BYTES)

struct port_binding {
    struct observer    obs;         /* on obj; guarded by obj->lock */
    struct port       *port;
    struct kobject    *obj;
    uint64_t           key;
    uint32_t           flags;
    volatile uint32_t  refs;
    bool               matched;     /* PERSISTENT: last seen state; obj->lock */
    bool               queued;      /* entry is in port->queue; port lock */
    struct list_node   port_node;   /* in port->bindings; bindings_lock */
    struct list_node   reap_node;   /* private list of whoever removed it */
    struct job        *job;         /* charged BINDING_CHARGE (a reference) */
    struct port_qentry entry;
};

static volatile uint64_t live_ports, live_bindings, live_user_packets;

static void stat_add(volatile uint64_t *c, int64_t d)
{
    __atomic_add_fetch(c, (uint64_t)d, __ATOMIC_RELAXED);
}

void port_get_stats(struct port_stats *s)
{
    s->ports = __atomic_load_n(&live_ports, __ATOMIC_RELAXED);
    s->bindings = __atomic_load_n(&live_bindings, __ATOMIC_RELAXED);
    s->user_packets = __atomic_load_n(&live_user_packets, __ATOMIC_RELAXED);
}

/* Credit and release a charge taken by charge_current. */
static void uncharge_job(struct job *job, uint64_t n)
{
    job_uncharge(job, JOB_LIMIT_MSG_BYTES, n);
    job_unref(job);
}

/* Charge n message bytes to the current thread's job; *out gets it (with
 * a reference) for uncharge_job, NULL for kernel threads. */
static status_t charge_current(uint64_t n, struct job **out)
{
    struct job *job = job_current();
    status_t st = job_charge(job, JOB_LIMIT_MSG_BYTES, n);
    if (st != OK)
        return st;
    job_ref(job);
    *out = job;
    return OK;
}

static void binding_put(struct port_binding *b)
{
    if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        uncharge_job(b->job, BINDING_CHARGE);
        kfree(b);
        stat_add(&live_bindings, -1);
    }
}

static void user_packet_free(struct port_qentry *e)
{
    uncharge_job(e->job, USER_PACKET_CHARGE);
    kfree(e);
    stat_add(&live_user_packets, -1);
}

/* With the port lock held: append e and tell waiters. */
static void enqueue_locked(struct port *p, struct port_qentry *e)
{
    list_add_tail(&p->queue, &e->node);
    kobject_signal_locked(&p->base, 0, SIG_READABLE);
    waitqueue_wake_one(&p->waiters);
}

/* ---- observer side (under the watched object's lock, IRQs off) ---------- */

static void queue_signal(struct port_binding *b, signals_t current)
{
    struct port *p = b->port;
    spin_lock(&p->base.lock);
    if (b->queued) {
        /* Still waiting to be read: fold this edge into it. */
        b->entry.pkt.signal.count++;
        b->entry.pkt.signal.observed = current;
    } else {
        b->queued = true;
        __atomic_add_fetch(&b->refs, 1, __ATOMIC_RELAXED);   /* the queue's */
        b->entry.pkt = (struct port_packet){
            .key = b->key,
            .type = PORT_PACKET_SIGNAL,
            .status = OK,
            .signal = { .trigger = b->obs.mask, .observed = current, .count = 1 },
        };
        enqueue_locked(p, &b->entry);
    }
    spin_unlock(&p->base.lock);
}

static void binding_fire(struct observer *o, signals_t current)
{
    struct port_binding *b = container_of(o, struct port_binding, obs);
    bool match = (current & o->mask) != 0;
    if (b->flags & PORT_BIND_PERSISTENT) {
        bool edge = match && !b->matched;
        b->matched = match;
        if (!edge)
            return;
    } else {
        if (!match)
            return;
        list_del(&o->node);   /* spent: no more callbacks */
    }
    queue_signal(b, current);
}

/* ---- binding list side ------------------------------------------------- */

/* With p->bindings_lock held: take b off the list and off its object.
 * Returns whether it was still armed (a fired ONCE binding is not). The
 * caller then owes kobject_unref(b->obj) and binding_put(b), outside all
 * locks. */
static bool detach_locked(struct port_binding *b)
{
    struct kobject *obj = b->obj;
    spin_lock(&obj->lock);   /* IRQs are already off */
    bool armed = b->obs.node.next != NULL;
    if (armed)
        list_del(&b->obs.node);
    spin_unlock(&obj->lock);
    list_del(&b->port_node);
    b->port->nbindings--;
    return armed;
}

static void reap(struct list_node *list)
{
    while (!list_empty(list)) {
        struct port_binding *b = list_first(list, struct port_binding, reap_node);
        list_del(&b->reap_node);
        kobject_unref(b->obj);
        binding_put(b);
    }
}

/* A binding's packet was just dequeued (its queue reference is ours now). */
static void binding_dequeued(struct port *p, struct port_binding *b)
{
    if (!(b->flags & PORT_BIND_PERSISTENT)) {
        /* A ONCE binding is spent: retire it unless unbind beat us to it. */
        uint64_t f = spin_lock_irqsave(&p->bindings_lock);
        bool listed = b->port_node.next != NULL;
        if (listed) {
            list_del(&b->port_node);   /* its observer already removed itself */
            p->nbindings--;
        }
        spin_unlock_irqrestore(&p->bindings_lock, f);
        if (listed) {
            kobject_unref(b->obj);
            binding_put(b);
        }
    }
    binding_put(b);
}

/* ---- the object -------------------------------------------------------- */

static void port_destroy(struct kobject *obj)
{
    struct port *p = container_of(obj, struct port, base);

    /* No references are left, so nobody can bind, unbind or wait. Detach
     * every binding from its object first; after that no observer callback
     * can reach this port, and the queue is ours alone. */
    struct list_node doomed;
    list_init(&doomed);
    uint64_t f = spin_lock_irqsave(&p->bindings_lock);
    while (!list_empty(&p->bindings)) {
        struct port_binding *b = list_first(&p->bindings, struct port_binding, port_node);
        detach_locked(b);
        list_add_tail(&doomed, &b->reap_node);
    }
    spin_unlock_irqrestore(&p->bindings_lock, f);
    reap(&doomed);

    while (!list_empty(&p->queue)) {
        struct port_qentry *e = list_first(&p->queue, struct port_qentry, node);
        list_del(&e->node);
        if (e->binding) {
            e->binding->queued = false;
            binding_put(e->binding);
        } else {
            user_packet_free(e);
        }
    }
    kfree(p);
    stat_add(&live_ports, -1);
}

static const struct kobject_ops port_ops = {
    .name = "port",
    .destroy = port_destroy,
};

status_t port_create(struct port **out)
{
    struct port *p = kzalloc(sizeof(*p));
    if (!p)
        return ERR_NO_MEMORY;
    kobject_init(&p->base, OBJ_PORT, &port_ops, "port", 0);
    list_init(&p->queue);
    spin_init(&p->bindings_lock, "port bindings");
    list_init(&p->bindings);
    waitqueue_init(&p->waiters, "port waiters");
    stat_add(&live_ports, 1);
    *out = p;
    return OK;
}

status_t port_bind(struct port *p, struct kobject *obj, uint64_t key, signals_t mask,
                   uint32_t flags)
{
    if (obj->type == OBJ_PORT)
        return ERR_NOT_SUPPORTED;   /* "port" would nest inside "port" */
    if (!mask || (flags != PORT_BIND_ONCE && flags != PORT_BIND_PERSISTENT))
        return ERR_INVALID_ARGS;
    /* Read-only quick check; the authoritative test is under bindings_lock. */
    if (__atomic_load_n(&p->nbindings, __ATOMIC_RELAXED) >= PORT_MAX_BINDINGS)
        return ERR_NO_RESOURCES;
    struct job *job;
    status_t st = charge_current(BINDING_CHARGE, &job);
    if (st != OK)
        return st;
    struct port_binding *b = kzalloc(sizeof(*b));
    if (!b) {
        uncharge_job(job, BINDING_CHARGE);
        return ERR_NO_MEMORY;
    }
    b->job = job;
    b->obs.mask = mask;
    b->obs.fire = binding_fire;
    b->port = p;
    b->obj = obj;
    b->key = key;
    b->flags = flags;
    b->refs = 1;   /* the list's */
    b->entry.binding = b;
    kobject_ref(obj);
    stat_add(&live_bindings, 1);

    /* Listed before it can fire, so a spent ONCE packet dequeued right away
     * always finds it on the list to retire. */
    uint64_t f = spin_lock_irqsave(&p->bindings_lock);
    if (p->nbindings >= PORT_MAX_BINDINGS) {   /* authoritative check (O3a) */
        spin_unlock_irqrestore(&p->bindings_lock, f);
        kobject_unref(obj);          /* undo the ref taken above */
        binding_put(b);              /* the only reference: frees and credits it */
        return ERR_NO_RESOURCES;
    }
    p->nbindings++;
    list_add_tail(&p->bindings, &b->port_node);
    kobject_observe(obj, &b->obs);   /* fires now if already matching */
    spin_unlock_irqrestore(&p->bindings_lock, f);
    return OK;
}

status_t port_unbind(struct port *p, struct kobject *obj, uint64_t key)
{
    struct list_node doomed;
    list_init(&doomed);
    bool found = false;
    uint64_t f = spin_lock_irqsave(&p->bindings_lock);
    for (struct list_node *n = p->bindings.next; n != &p->bindings;) {
        struct port_binding *b = container_of(n, struct port_binding, port_node);
        n = n->next;
        if (b->obj != obj || b->key != key)
            continue;
        found |= detach_locked(b);
        list_add_tail(&doomed, &b->reap_node);
    }
    spin_unlock_irqrestore(&p->bindings_lock, f);
    reap(&doomed);   /* a still-queued entry keeps its binding alive */
    return found ? OK : ERR_NOT_FOUND;
}

status_t port_queue_user(struct port *p, const struct port_packet *pkt)
{
    struct job *job;
    status_t st = charge_current(USER_PACKET_CHARGE, &job);
    if (st != OK)
        return st;
    struct port_qentry *e = kmalloc(sizeof(*e));
    if (!e) {
        uncharge_job(job, USER_PACKET_CHARGE);
        return ERR_NO_MEMORY;
    }
    e->binding = NULL;
    e->job = job;
    e->pkt = *pkt;
    e->pkt.type = PORT_PACKET_USER;
    stat_add(&live_user_packets, 1);
    uint64_t f = spin_lock_irqsave(&p->base.lock);
    if (p->user_queued >= PORT_MAX_USER_PACKETS) {
        spin_unlock_irqrestore(&p->base.lock, f);
        user_packet_free(e);
        return ERR_NO_RESOURCES;
    }
    p->user_queued++;
    enqueue_locked(p, e);
    spin_unlock_irqrestore(&p->base.lock, f);
    return OK;
}

status_t port_wait(struct port *p, uint64_t deadline_ns, struct port_packet *out)
{
    uint64_t f = spin_lock_irqsave(&p->base.lock);
    /* The queue is checked before the deadline: a waiter woken for a packet
     * just as it timed out still takes it, so no wakeup is wasted. */
    while (list_empty(&p->queue)) {
        if (uptime_ns() >= deadline_ns) {
            spin_unlock_irqrestore(&p->base.lock, f);
            return ERR_TIMED_OUT;
        }
        if (waitqueue_wait_cancellable(&p->waiters, &p->base.lock, &f, deadline_ns) != OK &&
            list_empty(&p->queue)) {
            spin_unlock_irqrestore(&p->base.lock, f);
            return ERR_CANCELED;
        }
    }
    struct port_qentry *e = list_first(&p->queue, struct port_qentry, node);
    list_del(&e->node);
    *out = e->pkt;
    struct port_binding *b = e->binding;
    if (b)
        b->queued = false;
    else
        p->user_queued--;
    if (list_empty(&p->queue))
        kobject_signal_locked(&p->base, SIG_READABLE, 0);
    else
        waitqueue_wake_one(&p->waiters);   /* more to take: pass it on */
    spin_unlock_irqrestore(&p->base.lock, f);

    if (b)
        binding_dequeued(p, b);
    else
        user_packet_free(e);
    return OK;
}
