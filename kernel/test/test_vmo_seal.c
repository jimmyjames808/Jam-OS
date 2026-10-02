/* Tests for sealing a VMO (vmo_seal, through vmo_make_exec): once sealed,
 * nothing changes its bytes or size, not even a call that took its VMO
 * reference before the seal (a vmo_write or a writable vmar_map from
 * another thread, already past its handle_get). */
#include <jam/aspace.h>
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/vmo.h>

#define PG PAGE_SIZE

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static uint8_t first_byte(struct vmo *v)
{
    uint8_t b = 0xee;
    KT_EQ(vmo_read(v, 0, &b, 1), OK);
    return b;
}

/* A sealed VMO refuses every write path and every way to write it later,
 * to a holder of a reference taken before the seal; reading and read-only
 * mappings still work. */
KTEST(vmo_seal_refuses_writers)
{
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, 0, &v), OK);
    KT_EQ(vmo_write(v, 0, "a", 1), OK);
    KT_EQ(vmo_seal(v), OK);
    KT_EQ(vmo_seal(v), OK);   /* again: still nothing maps it */

    KT_EQ(vmo_write(v, 0, "b", 1), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, PG), ERR_BAD_STATE);
    KT_EQ(vmo_set_size(v, 4 * PG), ERR_BAD_STATE);
    KT_EQ(vmo_commit(v, PG, PG), ERR_BAD_STATE);
    KT_EQ(vmo_decommit(v, 0, PG), ERR_BAD_STATE);
    void *va;
    KT_EQ(vmo_map_kernel(v, 0, PG, VM_WRITE, &va), ERR_BAD_STATE);
    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    uint64_t phys[1], pin;
    KT_EQ(vmo_pin(v, cap, 0, PG, phys, 1, &pin), ERR_BAD_STATE);
    kobject_unref(cap);

    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ | ASPACE_CAN_WRITE, &a), ERR_BAD_STATE);
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ | ASPACE_WRITE, &a), ERR_BAD_STATE);
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ | ASPACE_CAN_EXEC, &a), OK);
    KT_EQ(aspace_unmap(as, a, PG), OK);
    aspace_unref(as);

    KT_EQ(vmo_map_kernel(v, 0, PG, 0, &va), OK);
    KT_EQ(*(volatile uint8_t *)va, 'a');
    KT_EQ(vmo_seal(v), ERR_BAD_STATE);   /* mapped now */
    KT_EQ(vmo_unmap_kernel(v, va), OK);
    KT_EQ(first_byte(v), 'a');
    KT_EQ(vmo_size(v), 2 * PG);
    put(v);
}

/* What vmo_seal refuses: a mapping or pin of any kind, a VMO that isn't
 * paged. A refused seal changes nothing. */
KTEST(vmo_seal_refused_when_mapped)
{
    struct vmo *v;
    KT_EQ(vmo_create(PG, 0, &v), OK);
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    uint64_t a = 0;
    KT_EQ(aspace_map(as, v, 0, PG, ASPACE_READ, &a), OK);
    KT_EQ(vmo_seal(v), ERR_BAD_STATE);
    KT_EQ(aspace_unmap(as, a, PG), OK);
    aspace_unref(as);
    KT_EQ(vmo_write(v, 0, "c", 1), OK);   /* the refused seal left it writable */

    struct kobject *cap;
    KT_EQ(dma_cap_create(&cap), OK);
    uint64_t phys[1], pin;
    KT_EQ(vmo_pin(v, cap, 0, PG, phys, 1, &pin), OK);
    KT_EQ(vmo_seal(v), ERR_BAD_STATE);
    KT_EQ(vmo_unpin(v, cap, pin), OK);
    kobject_unref(cap);
    KT_EQ(vmo_seal(v), OK);
    put(v);

    struct vmo *c;
    KT_EQ(vmo_create(PG, VMO_CONTIGUOUS, &c), OK);
    KT_EQ(vmo_seal(c), ERR_BAD_STATE);
    put(c);
}

/* ---- vmo_make_exec against a write already running ------------------------ */

/* A writer thread holds its own VMO reference (a sys_vmo_write past its
 * handle_get) and is stopped by the hook in the middle of its copy; then
 * vmo_make_exec runs. Either it fails (the write is still in progress) or
 * the write must not land afterwards. A second writer starting after a
 * successful make_exec is refused. */
static struct vmo *mx_vmo;
static volatile int mx_in_copy, mx_go;
static status_t mx_write_st;

static void mx_hook(void *arg)
{
    if (arg != mx_vmo)
        return;   /* not the test's VMO */
    __atomic_store_n(&mx_in_copy, 1, __ATOMIC_RELEASE);
    uint64_t end = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while (!__atomic_load_n(&mx_go, __ATOMIC_ACQUIRE) && uptime_ns() < end)
        thread_sleep_ms(1);
}

static void mx_writer(void *arg)
{
    (void)arg;
    mx_write_st = vmo_write(mx_vmo, 0, "b", 1);
}

/* A handle table holding a fresh one-page VMO ("a" at 0) and a root handle
 * with RIGHT_ROOT_VMEX; mx_vmo gets a reference of its own. */
static void mx_setup(struct handle_table *t, handle_t *h, handle_t *root)
{
    handle_table_init(t);
    struct kobject *r = resource_root();
    kobject_ref(r);
    struct khandle kr = khandle_from_new(r, RIGHTS_BASIC | RIGHT_ROOT_VMEX);
    KT_EQ(handle_insert(t, &kr, root), OK);
    KT_EQ(sys_vmo_create(t, PG, 0, HANDLE_INVALID, h), OK);
    KT_EQ(sys_vmo_write(t, *h, 0, "a", 1), OK);
    struct kobject *obj;
    KT_EQ(handle_get(t, *h, OBJ_VMO, 0, &obj, NULL), OK);
    mx_vmo = vmo_from_kobject(obj);
}

KTEST(vmo_make_exec_races_writer)
{
    struct handle_table t;
    handle_t h, root, x;
    mx_setup(&t, &h, &root);
    mx_in_copy = mx_go = 0;
    mx_write_st = ERR_INTERNAL;
    __atomic_store_n(&dbg_hooks[DBG_VMO_WRITE_COPY], mx_hook, __ATOMIC_RELEASE);
    struct thread *w = thread_create_on("kt-vmo-writer", mx_writer, NULL, PRIO_DEFAULT, NULL);
    uint64_t end = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while (!__atomic_load_n(&mx_in_copy, __ATOMIC_ACQUIRE) && uptime_ns() < end)
        thread_sleep_ms(1);
    KT_ASSERT(mx_in_copy);
    status_t st = sys_vmo_make_exec(&t, h, root, &x);
    __atomic_store_n(&mx_go, 1, __ATOMIC_RELEASE);
    thread_join(w);
    __atomic_store_n(&dbg_hooks[DBG_VMO_WRITE_COPY], NULL, __ATOMIC_RELEASE);
    kprintf("vmo_make_exec_races_writer: make_exec %s, write %s, byte '%c'\n", status_str(st),
            status_str(mx_write_st), first_byte(mx_vmo));
    KT_EQ(mx_write_st, OK);   /* it was past every check when it stopped */
    KT_EQ(st, ERR_BAD_STATE);   /* so the seal must refuse: the byte is about to change */
    put(mx_vmo);
    handle_table_destroy(&t);

    /* Sealed first, then a writer holding an older reference starts. */
    mx_setup(&t, &h, &root);
    KT_EQ(sys_vmo_make_exec(&t, h, root, &x), OK);
    w = thread_create_on("kt-vmo-writer", mx_writer, NULL, PRIO_DEFAULT, NULL);
    thread_join(w);
    KT_EQ(mx_write_st, ERR_BAD_STATE);
    KT_EQ(first_byte(mx_vmo), 'a');
    put(mx_vmo);
    handle_table_destroy(&t);
}
