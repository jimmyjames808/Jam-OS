/* Timer objects: assert SIG_SIGNALED once uptime_ns() reaches a deadline.
 *
 * Expiry is driven by one kernel thread, the timer service, started with
 * the first timer. It keeps armed timers in a deadline-ordered list and
 * sleeps until the earliest deadline. Sleepers are woken by CPU 0's 100 Hz
 * tick, so a timer fires up to ~10 ms (plus scheduling delay) late, never
 * early. Timers that expire in the same pass signal in deadline order.
 *
 * Locking: "timer service" (the armed list) ranks before "timer" (the
 * object lock): the service signals expired timers while holding the list
 * lock, so set/cancel/expire and destroy are all serialised by it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/list.h>
#include <jam/object.h>
#include <jam/status.h>

struct ktimer {
    struct kobject   base;       /* OBJ_TIMER; SIG_SIGNALED once the deadline passes */
    /* Guarded by the timer service lock. */
    struct list_node node;       /* in the armed list while armed */
    uint64_t         deadline_ns;
    bool             armed;
};

/* New disarmed timer; *out holds the creator's reference. */
status_t timer_create(struct ktimer **out);
/* Arm (or re-arm) for deadline_ns (uptime_ns() scale) and clear
 * SIG_SIGNALED. A deadline already passed signals at once. */
status_t timer_set(struct ktimer *t, uint64_t deadline_ns);
/* Disarm and clear SIG_SIGNALED. Cancelling a disarmed timer is fine. */
status_t timer_cancel(struct ktimer *t);
