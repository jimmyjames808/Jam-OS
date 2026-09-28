/* Handle-level layer for events, timers, ports and object_wait_one. */
#include <jam/event.h>
#include <jam/port.h>
#include <jam/sys.h>
#include <jam/timer.h>

#define EVENT_RIGHTS (RIGHTS_BASIC | RIGHT_SIGNAL)
#define TIMER_RIGHTS (RIGHTS_BASIC | RIGHTS_IO)
#define PORT_RIGHTS  (RIGHTS_BASIC | RIGHTS_IO)

/* Hand a freshly created object (refs = 1) to the table. */
static status_t publish(struct handle_table *t, struct kobject *obj, rights_t rights,
                        handle_t *out)
{
    struct khandle kh = khandle_from_new(obj, rights);
    status_t st = handle_insert(t, &kh, out);
    if (st != OK)
        khandle_release(&kh);   /* destroys the object */
    return st;
}

status_t sys_event_create(struct handle_table *t, handle_t *out)
{
    struct event *e;
    status_t st = event_create(&e);
    return st == OK ? publish(t, &e->base, EVENT_RIGHTS, out) : st;
}

status_t sys_event_signal(struct handle_table *t, handle_t h, signals_t clear, signals_t set)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_EVENT, RIGHT_SIGNAL, &obj, NULL);
    if (st != OK)
        return st;
    st = event_signal(container_of(obj, struct event, base), clear, set);
    kobject_unref(obj);
    return st;
}

status_t sys_timer_create(struct handle_table *t, handle_t *out)
{
    struct ktimer *tm;
    status_t st = timer_create(&tm);
    return st == OK ? publish(t, &tm->base, TIMER_RIGHTS, out) : st;
}

status_t sys_timer_set(struct handle_table *t, handle_t h, uint64_t deadline_ns)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_TIMER, RIGHT_WRITE, &obj, NULL);
    if (st != OK)
        return st;
    st = timer_set(container_of(obj, struct ktimer, base), deadline_ns);
    kobject_unref(obj);
    return st;
}

status_t sys_timer_cancel(struct handle_table *t, handle_t h)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_TIMER, RIGHT_WRITE, &obj, NULL);
    if (st != OK)
        return st;
    st = timer_cancel(container_of(obj, struct ktimer, base));
    kobject_unref(obj);
    return st;
}

status_t sys_port_create(struct handle_table *t, handle_t *out)
{
    struct port *p;
    status_t st = port_create(&p);
    return st == OK ? publish(t, &p->base, PORT_RIGHTS, out) : st;
}

status_t sys_port_bind(struct handle_table *t, handle_t port, handle_t obj, uint64_t key,
                       signals_t mask, uint32_t flags)
{
    struct kobject *po, *o;
    status_t st = handle_get(t, port, OBJ_PORT, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    st = handle_get(t, obj, OBJ_NONE, RIGHT_WAIT, &o, NULL);
    if (st == OK) {
        st = port_bind(container_of(po, struct port, base), o, key, mask, flags);
        kobject_unref(o);
    }
    kobject_unref(po);
    return st;
}

status_t sys_port_unbind(struct handle_table *t, handle_t port, handle_t obj, uint64_t key)
{
    struct kobject *po, *o;
    status_t st = handle_get(t, port, OBJ_PORT, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    st = handle_get(t, obj, OBJ_NONE, 0, &o, NULL);
    if (st == OK) {
        st = port_unbind(container_of(po, struct port, base), o, key);
        kobject_unref(o);
    }
    kobject_unref(po);
    return st;
}

status_t sys_port_queue(struct handle_table *t, handle_t port, const struct port_packet *pkt)
{
    struct kobject *po;
    status_t st = handle_get(t, port, OBJ_PORT, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    st = port_queue_user(container_of(po, struct port, base), pkt);
    kobject_unref(po);
    return st;
}

status_t sys_port_wait(struct handle_table *t, handle_t port, uint64_t deadline_ns,
                       struct port_packet *out)
{
    struct kobject *po;
    status_t st = handle_get(t, port, OBJ_PORT, RIGHT_READ, &po, NULL);
    if (st != OK)
        return st;
    st = port_wait(container_of(po, struct port, base), deadline_ns, out);
    kobject_unref(po);   /* may be the last reference if the handle closed meanwhile */
    return st;
}

status_t sys_object_wait_one(struct handle_table *t, handle_t h, signals_t mask,
                             uint64_t deadline_ns, signals_t *observed)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_NONE, RIGHT_WAIT, &obj, NULL);
    if (st != OK)
        return st;
    st = object_wait_one(obj, mask, deadline_ns, observed);
    kobject_unref(obj);
    return st;
}
