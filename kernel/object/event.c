#include <jam/event.h>
#include <jam/mm.h>

#define EVENT_SIGNALS (SIG_SIGNALED | SIG_USER_ALL)

static void event_destroy(struct kobject *obj)
{
    kfree(container_of(obj, struct event, base));
}

static const struct kobject_ops event_ops = {
    .name = "event",
    .destroy = event_destroy,
};

status_t event_create(struct event **out)
{
    struct event *e = kzalloc(sizeof(*e));
    if (!e)
        return ERR_NO_MEMORY;
    kobject_init(&e->base, OBJ_EVENT, &event_ops, "event", 0);
    *out = e;
    return OK;
}

status_t event_signal(struct event *e, signals_t clear, signals_t set)
{
    if ((clear | set) & ~EVENT_SIGNALS)
        return ERR_INVALID_ARGS;
    kobject_signal(&e->base, clear, set);
    return OK;
}
