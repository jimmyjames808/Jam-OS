/* Channels that hold channels (endpoints sent in messages): reference
 * cycles (direct, 2-cycle, 3-cycle) are refused while the legitimate sends
 * that look similar still work, and a deep chain of channels, or of
 * channels and ports alternating, is torn down iteratively instead of
 * overflowing the kernel stack. The audit* test names stay as they are:
 * tests are run by name. */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/port.h>
#include <jam/sys.h>

#define CH(p) ((struct kobject *)(p))
#define CRIGHTS (RIGHTS_BASIC | RIGHTS_IO)

/* ---- cycles refused, legitimate sends allowed ---------------------------- */

/* A1 in B1's queue and B1 in A1's queue would be a 2-cycle nothing can reach.
 * The second send carries B1, whose queue already holds the channel A1, so it
 * is refused; nothing leaks. */
KTEST(auditA_channel_indirect_cycle_refused)
{
    uint64_t live = channel_live_count();
    struct channel *a0, *a1, *b0, *b1;
    KT_EQ(channel_create(&a0, &a1), OK);
    KT_EQ(channel_create(&b0, &b1), OK);
    struct khandle ka0 = khandle_from_new(CH(a0), CRIGHTS);
    struct khandle ka1 = khandle_from_new(CH(a1), CRIGHTS);
    struct khandle kb0 = khandle_from_new(CH(b0), CRIGHTS);
    struct khandle kb1 = khandle_from_new(CH(b1), CRIGHTS);
    KT_EQ(channel_write(b0, "aaaa", 4, &ka1, 1), OK);              /* A1 -> B1's queue */
    KT_EQ(channel_write(a0, "bbbb", 4, &kb1, 1), ERR_NOT_SUPPORTED);  /* would close it */
    /* The rejected write left kb1 untouched; release everything reachable. */
    khandle_release(&ka0);
    khandle_release(&kb0);
    khandle_release(&kb1);
    kprintf("auditA: cycle refused; live endpoints %lu -> %lu\n", live, channel_live_count());
    KT_GLOBAL_EQ(channel_live_count(), live);   /* no leak */
}

/* A 3-cycle (A1->B1q, B1->C1q, C1->A1q) is refused as soon as an edge would
 * carry an endpoint whose queue already holds a channel (here the second
 * send, carrying B1 whose queue holds A1). */
KTEST(auditA3_channel_three_cycle_refused)
{
    uint64_t live = channel_live_count();
    struct channel *a0, *a1, *b0, *b1, *c0, *c1;
    KT_EQ(channel_create(&a0, &a1), OK);
    KT_EQ(channel_create(&b0, &b1), OK);
    KT_EQ(channel_create(&c0, &c1), OK);
    struct khandle ka0 = khandle_from_new(CH(a0), CRIGHTS);
    struct khandle ka1 = khandle_from_new(CH(a1), CRIGHTS);
    struct khandle kb0 = khandle_from_new(CH(b0), CRIGHTS);
    struct khandle kb1 = khandle_from_new(CH(b1), CRIGHTS);
    struct khandle kc0 = khandle_from_new(CH(c0), CRIGHTS);
    struct khandle kc1 = khandle_from_new(CH(c1), CRIGHTS);
    KT_EQ(channel_write(b0, "aaaa", 4, &ka1, 1), OK);                 /* A1 -> B1q */
    KT_EQ(channel_write(c0, "bbbb", 4, &kb1, 1), ERR_NOT_SUPPORTED);  /* B1q holds A1 */
    khandle_release(&ka0);
    khandle_release(&kb0);
    khandle_release(&kb1);
    khandle_release(&kc0);
    khandle_release(&kc1);
    kprintf("auditA3: 3-cycle refused; live endpoints %lu -> %lu\n", live, channel_live_count());
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* Legitimate: sending an endpoint whose queue holds only plain messages, or
 * only non-channel handles, must still work. */
KTEST(auditA_legit_sends_allowed)
{
    uint64_t live = channel_live_count();
    struct channel *carrier0, *carrier1;
    KT_EQ(channel_create(&carrier0, &carrier1), OK);
    struct khandle kc0 = khandle_from_new(CH(carrier0), CRIGHTS);
    struct khandle kc1 = khandle_from_new(CH(carrier1), CRIGHTS);

    /* (1) an endpoint with a plain (handle-free) message queued on it */
    struct channel *x0, *x1;
    KT_EQ(channel_create(&x0, &x1), OK);
    struct khandle kx0 = khandle_from_new(CH(x0), CRIGHTS);
    struct khandle kx1 = khandle_from_new(CH(x1), CRIGHTS);
    KT_EQ(channel_write(x0, "hello", 5, NULL, 0), OK);   /* queued on x1 */
    KT_EQ(channel_write(carrier0, "m", 1, &kx1, 1), OK); /* send x1: allowed */
    khandle_release(&kx0);

    /* (2) an endpoint whose queue holds a non-channel handle (an event) */
    struct channel *y0, *y1;
    KT_EQ(channel_create(&y0, &y1), OK);
    struct event *e;
    KT_EQ(event_create(&e), OK);
    struct khandle ky0 = khandle_from_new(CH(y0), CRIGHTS);
    struct khandle ky1 = khandle_from_new(CH(y1), CRIGHTS);
    struct khandle ke = khandle_from_new(&e->base, RIGHTS_BASIC);
    KT_EQ(channel_write(y0, "e", 1, &ke, 1), OK);        /* event queued on y1 */
    KT_EQ(channel_write(carrier0, "m", 1, &ky1, 1), OK); /* send y1: allowed */
    khandle_release(&ky0);

    /* Tear it all down; nothing should leak. */
    khandle_release(&kc0);
    khandle_release(&kc1);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* The 2-cycle again, through the handle-level API a process uses. */
KTEST(auditC_sys_channel_cycle_refused)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t a0, a1, b0, b1;
    KT_EQ(sys_channel_create(&t, &a0, &a1), OK);
    KT_EQ(sys_channel_create(&t, &b0, &b1), OK);
    KT_EQ(sys_channel_write(&t, b0, "aaaa", 4, &a1, 1), OK);
    KT_EQ(sys_channel_write(&t, a0, "bbbb", 4, &b1, 1), ERR_NOT_SUPPORTED);
    handle_table_destroy(&t);
    kprintf("auditC: cycle refused; live endpoints %lu -> %lu\n", live, channel_live_count());
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* ---- iterative teardown ------------------------------------------------- */

/* e[i]'s queue holds e[i+1]; releasing e[0] closes the whole chain. This is a
 * legitimate (acyclic) structure, so it is built top-down -- each endpoint is
 * sent while its own queue is still empty, which is allowed (only cycles
 * are refused). channel_close used to recurse one stack frame per level
 * and overflow the 64 KiB kernel stack; now it drains iteratively. */
enum { AUDIT_DEPTH = 1000 };

KTEST(auditB_channel_deep_close_iterative)
{
    uint64_t live = channel_live_count();
    struct channel *e0, *e0p;
    KT_EQ(channel_create(&e0, &e0p), OK);
    struct khandle head = khandle_from_new(CH(e0), CRIGHTS);   /* kept: the chain head */
    struct khandle peer = khandle_from_new(CH(e0p), CRIGHTS);  /* write here to queue on e0 */

    for (int i = 1; i < AUDIT_DEPTH; i++) {
        struct channel *e, *ep;
        KT_EQ(channel_create(&e, &ep), OK);
        struct khandle ke = khandle_from_new(CH(e), CRIGHTS);
        struct khandle kep = khandle_from_new(CH(ep), CRIGHTS);
        /* Queue e onto the previous endpoint (via its peer). e's queue is
         * empty here, so it may be sent; the previous peer then closes. */
        KT_EQ(channel_write((struct channel *)peer.obj, "n", 1, &ke, 1), OK);
        khandle_release(&peer);
        peer = kep;
    }
    khandle_release(&peer);
    khandle_release(&head);   /* was a kernel stack overflow at this depth */
    kprintf("auditB: chain of %d endpoints torn down iteratively\n", AUDIT_DEPTH);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* A deep chain that alternates channel and port: channel head[i]'s queue holds
 * port p[i], and p[i] is bound to head[i+1] (so it holds the only reference to
 * it). Releasing head[0] cascades channel_close -> release port -> port_destroy
 * -> unref next channel -> ..., which used to recurse through BOTH object
 * types. It now drains iteratively too. */
enum { ALT_DEPTH = 500 };

KTEST(auditB2_alternating_channel_port_iterative)
{
    static struct channel *head[ALT_DEPTH], *tail[ALT_DEPTH];
    static struct port *p[ALT_DEPTH];
    uint64_t live = channel_live_count();
    struct port_stats ps0;
    port_get_stats(&ps0);

    for (int i = 0; i < ALT_DEPTH; i++) {
        KT_EQ(channel_create(&head[i], &tail[i]), OK);   /* each refs = 1 */
        KT_EQ(port_create(&p[i]), OK);                    /* refs = 1 */
    }
    /* Queue p[i] onto head[i] (write to its peer tail[i]); the khandle's ref
     * is moved into the message, so p[i] then lives only in that queue. */
    for (int i = 0; i < ALT_DEPTH; i++) {
        struct khandle kp = khandle_from_new(&p[i]->base, CRIGHTS);
        KT_EQ(channel_write(tail[i], "p", 1, &kp, 1), OK);
    }
    /* Bind p[i] to head[i+1]: the binding takes a reference on head[i+1]. */
    for (int i = 0; i < ALT_DEPTH - 1; i++)
        KT_EQ(port_bind(p[i], CH(head[i + 1]), 1, SIG_READABLE, PORT_BIND_PERSISTENT), OK);
    /* Drop our scaffolding references: the tails, and every head except the
     * head of the chain (head[i>=1] is then held only by p[i-1]'s binding). */
    for (int i = 0; i < ALT_DEPTH; i++)
        kobject_unref(CH(tail[i]));
    for (int i = 1; i < ALT_DEPTH; i++)
        kobject_unref(CH(head[i]));

    kobject_unref(CH(head[0]));   /* was ~2*depth deep recursion */
    kprintf("auditB2: alternating channel/port chain of %d torn down iteratively\n", ALT_DEPTH);

    struct port_stats ps1;
    port_get_stats(&ps1);
    KT_GLOBAL_EQ(channel_live_count(), live);
    KT_EQ(ps1.ports, ps0.ports);
    KT_EQ(ps1.bindings, ps0.bindings);
}
