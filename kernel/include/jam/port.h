/* Ports: a queue of packets, and the way one thread waits on many objects.
 *
 * port_bind watches an object's signals and queues a PORT_PACKET_SIGNAL
 * when they match; port_queue_user queues arbitrary PORT_PACKET_USER
 * packets; port_wait takes the oldest packet, blocking if there is none.
 * The port asserts SIG_READABLE while any packet is queued, so a thread
 * can also object_wait_one on it. M6 adds interrupt packets.
 *
 * Lock order (lock classes):
 *   "port bindings"  the port's binding list; held while taking a watched
 *        |           object's lock (bind, unbind)
 *   object locks     "event", "timer", "channel", ... ("timer service" is
 *        |           above "timer")
 *   "port"           the port's own kobject lock, which also guards the
 *        |           packet queue: observer callbacks take it under the
 *        |           watched object's lock, so it ranks after every object
 *   "port waiters"   threads blocked in port_wait
 * A port can never be bound to a port (the "port" class would nest in
 * itself), and the last reference to a port must not be dropped while any
 * object lock is held (destroying it takes the bindings' object locks).
 *
 * Bindings do not hold a reference on the port: destroying the port (last
 * reference) detaches every binding from its object first, then frees the
 * queue. A binding holds a reference on the watched object until it is
 * unbound, or, for PORT_BIND_ONCE, until its packet is dequeued. */
#pragma once

#include <stdint.h>
#include <jam/list.h>
#include <jam/object.h>
#include <jam/status.h>
#include <jam/sched.h>

enum port_packet_type {
    PORT_PACKET_SIGNAL = 1,
    PORT_PACKET_USER = 2,
    /* M6: PORT_PACKET_INTERRUPT */
};

struct port_packet {
    uint64_t key;       /* chosen by whoever bound/queued it */
    uint32_t type;      /* enum port_packet_type */
    int32_t  status;    /* OK; ERR_CANCELED is reserved for binding teardown */
    union {
        struct {
            signals_t trigger;    /* the mask it was bound with */
            signals_t observed;   /* the object's signals at the last edge */
            uint64_t  count;      /* edges coalesced into this packet (>= 1) */
        } signal;
        struct {
            uint64_t data[4];
        } user;
    };
};

#define PORT_BIND_ONCE       0   /* fire once, then the binding is gone */
#define PORT_BIND_PERSISTENT 1   /* fire on every not-matching -> matching edge */

/* Most user packets a port holds before port_queue_user says
 * ERR_NO_RESOURCES. Signal packets never count: each binding owns one. */
#define PORT_MAX_USER_PACKETS 4096
/* Cap on live bindings per port: each is an observer walked with interrupts
 * off on every signal change, so an unbounded number is a DoS. (O3a) */
#define PORT_MAX_BINDINGS 4096

struct port {
    struct kobject   base;         /* base.lock ("port") guards the queue */
    struct list_node queue;        /* struct port_qentry, oldest first */
    uint32_t         user_queued;
    spinlock_t       bindings_lock;   /* "port bindings" */
    struct list_node bindings;        /* struct port_binding */
    uint32_t         nbindings;       /* live bindings; bindings_lock */
    struct waitqueue waiters;         /* "port waiters" */
};

status_t port_create(struct port **out);
/* Watch obj: when (signals & mask) becomes non-zero, queue a
 * PORT_PACKET_SIGNAL carrying `key`. If they already match, it fires at
 * once. ONCE fires a single time; PERSISTENT fires on every edge, and while
 * its packet is still queued further edges only raise that packet's count.
 * Binding a port to a port is ERR_NOT_SUPPORTED; mask 0 or unknown flags
 * are ERR_INVALID_ARGS. */
status_t port_bind(struct port *p, struct kobject *obj, uint64_t key, signals_t mask,
                   uint32_t flags);
/* Remove every live binding for (obj, key): ERR_NOT_FOUND if there were
 * none (a ONCE binding that already fired is gone). Queued packets stay. */
status_t port_unbind(struct port *p, struct kobject *obj, uint64_t key);
/* Queue a copy of *pkt with type PORT_PACKET_USER. */
status_t port_queue_user(struct port *p, const struct port_packet *pkt);
/* Take the oldest packet, blocking until one arrives or uptime_ns()
 * reaches deadline_ns (ERR_TIMED_OUT; DEADLINE_NEVER waits forever), or the
 * thread is cancelled with nothing queued (ERR_CANCELED). */
status_t port_wait(struct port *p, uint64_t deadline_ns, struct port_packet *out);

/* Live counts across all ports, for leak tests. */
struct port_stats {
    uint64_t ports;
    uint64_t bindings;       /* binding records, including ones only a queued packet keeps */
    uint64_t user_packets;
};
void port_get_stats(struct port_stats *s);
