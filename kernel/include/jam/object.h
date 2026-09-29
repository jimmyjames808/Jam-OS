/* Kernel objects.
 *
 * Every object type embeds struct kobject first. An object has:
 *   - a reference count (kobject_ref/unref): the memory lives while > 0
 *   - a handle count: how many handles (in tables or in transit inside
 *     channel messages) point at it. When it drops to 0 the type's
 *     on_zero_handles runs (e.g. a channel tells its peer PEER_CLOSED).
 *   - 32 signal bits, set and cleared with kobject_signal. Observers
 *     registered on the object are told whenever the signals change; that
 *     is how object_wait_one and ports learn about state changes.
 *
 * Locking: each object has one spinlock, named per type ("channel",
 * "port", ...) so the lock-order checker can tell types apart. Observer
 * callbacks run with the object's lock held and interrupts off, so they
 * must be short and may only take locks that rank after object locks
 * (a port's queue lock, a run queue lock via thread_wake). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/list.h>
#include <jam/spinlock.h>
#include <jam/status.h>

enum obj_type {
    OBJ_NONE = 0,       /* "any type" in lookups */
    OBJ_EVENT,
    OBJ_TIMER,
    OBJ_CHANNEL,
    OBJ_PORT,
    OBJ_VMO,
    OBJ_DMA_CAP,        /* permission to pin memory for device DMA */
    OBJ_PROCESS,
    OBJ_THREAD,
    OBJ_INTERRUPT,      /* a device interrupt vector */
    OBJ_RESOURCE,       /* authority over a piece of hardware */
    OBJ_VMAR,           /* a handle to an address space */
    OBJ_JOB,            /* resource limits for a group of processes */
    OBJ_KLOG,           /* a reader of the kernel log */
    OBJ_SERIAL,         /* COM1 input */
    OBJ_SCREEN,         /* ownership of the boot framebuffer */
    OBJ_TYPE_COUNT,
};

/* signals_t and the common SIG_* bits are in <jam/abi.h> (user code needs
 * them too). Types document which ones they use. */

struct kobject;

struct kobject_ops {
    const char *name;   /* type name for logs and the lock checker */
    /* The last reference went away: free the object. Required. */
    void (*destroy)(struct kobject *obj);
    /* The last handle went away (references may remain). Optional. */
    void (*on_zero_handles)(struct kobject *obj);
};

/* Something that wants to hear about signal changes. `fire` runs under the
 * object's lock whenever the signals change while the observer is
 * registered, and once from kobject_observe if they already match. It is
 * told the current signals; it decides for itself whether they matter. */
struct observer {
    struct list_node node;                                 /* on the object's observer list */
    signals_t        mask;                                 /* signals it cares about */
    void (*fire)(struct observer *o, signals_t current);   /* called under the object's lock */
};

struct kobject {
    const struct kobject_ops *ops;  /* destroy / on_zero_handles */
    enum obj_type     type;         /* OBJ_* */
    uint32_t          refs;         /* kernel references; the last destroys it */
    uint32_t          handles;      /* handles to it; the last calls on_zero_handles */
    signals_t         signals;      /* SIG_* now; lock */
    spinlock_t        lock;         /* guards signals and observers */
    struct list_node  observers;    /* struct observer */
    uint64_t          koid;         /* unique id, never reused */
    /* Iterative teardown: when the last handle or last reference goes while a
     * teardown is already running on this CPU, the object is pushed onto a
     * per-CPU pending list (td_next) with the work still owed (td_pending)
     * instead of recursing. See kobject_unref / kobject_handle_drop. */
    struct kobject   *td_next;
    uint8_t           td_pending;
};

/* refs = 1 (the creator's reference), handles = 0, signals = initial. */
void kobject_init(struct kobject *obj, enum obj_type type, const struct kobject_ops *ops,
                  const char *lock_name, signals_t initial);
void kobject_ref(struct kobject *obj);
void kobject_unref(struct kobject *obj);
/* A new reference unless the last one is already gone (the object is being
 * destroyed): for lists that hold objects without a reference and unlist
 * them in destroy. Returns whether it took one. */
bool kobject_tryref(struct kobject *obj);

/* Handle-count bookkeeping, used by the handle layer (handle.c). */
void kobject_handle_gain(struct kobject *obj);
void kobject_handle_drop(struct kobject *obj);   /* may run on_zero_handles */

/* signals = (signals & ~clear) | set, then notify observers if changed. */
void kobject_signal(struct kobject *obj, signals_t clear, signals_t set);
/* Same, with obj->lock already held by the caller. */
void kobject_signal_locked(struct kobject *obj, signals_t clear, signals_t set);
signals_t kobject_signals(struct kobject *obj);

/* Register / unregister an observer. kobject_observe fires it immediately
 * (under the lock) if (signals & o->mask) != 0 already. */
void kobject_observe(struct kobject *obj, struct observer *o);
void kobject_unobserve(struct kobject *obj, struct observer *o);

/* Block until (signals & mask) != 0 or uptime_ns() >= deadline_ns
 * (DEADLINE_NEVER from sched.h for no limit). Returns OK, ERR_TIMED_OUT, or
 * ERR_CANCELED if the thread is cancelled (thread_cancel) first; *observed
 * (if non-NULL) gets the signals at that moment. */
status_t object_wait_one(struct kobject *obj, signals_t mask, uint64_t deadline_ns,
                         signals_t *observed);

const char *obj_type_name(enum obj_type t);
