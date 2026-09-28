/* Events: the simplest kernel object, a set of signal bits that threads set
 * and clear explicitly and others wait on (object_wait_one or a port).
 *
 * Signals: SIG_SIGNALED and the user bits SIG_USER_ALL. Nothing else is
 * ever set on an event. Lock class "event". */
#pragma once

#include <jam/object.h>
#include <jam/status.h>

struct event {
    struct kobject base;
};

/* New event with no signals set; *out holds the creator's reference. */
status_t event_create(struct event **out);
/* signals = (signals & ~clear) | set. Only SIG_SIGNALED | SIG_USER_ALL may
 * be named, anything else is ERR_INVALID_ARGS (and nothing changes). */
status_t event_signal(struct event *e, signals_t clear, signals_t set);
