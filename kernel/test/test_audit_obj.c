/* Audit repro tests for the M4 object layer. Each is expected to FAIL
 * (panic) on the code as audited; run one at a time:
 *   ktest=auditA   indirect channel cycle leaks both endpoints
 *   ktest=auditB   deep chain of queued endpoints overflows the kernel stack
 *   ktest=auditC   same cycle built through the sys_ layer (hostile caller) */
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sys.h>

#define CH(p) ((struct kobject *)(p))
#define CRIGHTS (RIGHTS_BASIC | RIGHTS_IO)

/* A1 is queued on B1 and B1 on A1: once A0 and B0 close, nothing can reach
 * either endpoint, yet each keeps the other's handle count above zero. */
KTEST(auditA_channel_indirect_cycle)
{
    uint64_t live = channel_live_count();
    struct channel *a0, *a1, *b0, *b1;
    KT_EQ(channel_create(&a0, &a1), OK);
    KT_EQ(channel_create(&b0, &b1), OK);
    struct khandle ka0 = khandle_from_new(CH(a0), CRIGHTS);
    struct khandle ka1 = khandle_from_new(CH(a1), CRIGHTS);
    struct khandle kb0 = khandle_from_new(CH(b0), CRIGHTS);
    struct khandle kb1 = khandle_from_new(CH(b1), CRIGHTS);
    KT_EQ(channel_write(b0, "aaaa", 4, &ka1, 1), OK);   /* A1 -> B1's queue */
    KT_EQ(channel_write(a0, "bbbb", 4, &kb1, 1), OK);   /* B1 -> A1's queue */
    khandle_release(&ka0);
    khandle_release(&kb0);
    kprintf("auditA: live endpoints before %lu, after closing every reachable handle %lu\n",
            live, channel_live_count());
    KT_EQ(channel_live_count(), live);   /* fails: 2 endpoints leaked for good */
}

/* e[i]'s queue holds e[i+1]; releasing e[0] closes the chain recursively:
 * channel_close -> msg_drop -> khandle_release -> on_zero_handles ->
 * channel_close ... one set of frames per level on a 64 KiB stack. */
enum { AUDIT_DEPTH = 1000 };

KTEST(auditB_channel_deep_close_recursion)
{
    struct khandle prev = { 0 };
    for (int i = 0; i < AUDIT_DEPTH; i++) {
        struct channel *a, *b;
        KT_EQ(channel_create(&a, &b), OK);
        struct khandle ka = khandle_from_new(CH(a), CRIGHTS);
        struct khandle kb = khandle_from_new(CH(b), CRIGHTS);
        if (prev.obj)
            KT_EQ(channel_write(b, "next", 4, &prev, 1), OK);   /* queued on a */
        khandle_release(&kb);
        prev = ka;
    }
    kprintf("auditB: chain of %d endpoints built, releasing the head\n", AUDIT_DEPTH);
    khandle_release(&prev);   /* expected: kernel stack overflow */
    kprintf("auditB: survived (no overflow at depth %d)\n", AUDIT_DEPTH);
}

/* The same cycle through the handle-level API a process will use in M5. */
KTEST(auditC_sys_channel_cycle)
{
    uint64_t live = channel_live_count();
    struct handle_table t;
    handle_table_init(&t);
    handle_t a0, a1, b0, b1;
    KT_EQ(sys_channel_create(&t, &a0, &a1), OK);
    KT_EQ(sys_channel_create(&t, &b0, &b1), OK);
    KT_EQ(sys_channel_write(&t, b0, "aaaa", 4, &a1, 1), OK);
    KT_EQ(sys_channel_write(&t, a0, "bbbb", 4, &b1, 1), OK);
    handle_table_destroy(&t);   /* process exit */
    kprintf("auditC: live endpoints before %lu, after table destroyed %lu\n", live,
            channel_live_count());
    KT_EQ(channel_live_count(), live);
}

/* Handle generations are 8 bits and the free list is LIFO, so after 256
 * close/insert cycles on one slot a stale handle value names a new object. */
static void audit_dummy_destroy(struct kobject *o) { kfree(o); }
static const struct kobject_ops audit_dummy_ops = { .name = "audit dummy",
                                                    .destroy = audit_dummy_destroy };

KTEST(auditD_handle_generation_wraps)
{
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *first = kzalloc(sizeof(*first));
    kobject_init(first, OBJ_EVENT, &audit_dummy_ops, "audit dummy", 0);
    struct khandle kh = khandle_from_new(first, RIGHTS_BASIC);
    handle_t stale, h = 0;
    KT_EQ(handle_insert(&t, &kh, &stale), OK);
    KT_EQ(handle_close(&t, stale), OK);
    struct kobject *o = NULL;
    for (int i = 0; i < 256; i++) {
        o = kzalloc(sizeof(*o));
        kobject_init(o, OBJ_EVENT, &audit_dummy_ops, "audit dummy", 0);
        kh = khandle_from_new(o, RIGHTS_BASIC | RIGHT_SIGNAL);
        KT_EQ(handle_insert(&t, &kh, &h), OK);
        if (i < 255)
            KT_EQ(handle_close(&t, h), OK);
    }
    struct kobject *got = NULL;
    status_t st = handle_get(&t, stale, OBJ_NONE, 0, &got, NULL);
    kprintf("auditD: stale %x new %x lookup of stale -> %s (%s)\n", stale, h, status_str(st),
            st == OK && got == o ? "names the NEW object" : "-");
    if (st == OK)
        kobject_unref(got);
    handle_table_destroy(&t);
    KT_EQ(st, ERR_BAD_HANDLE);
}
