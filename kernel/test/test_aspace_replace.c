/* VMAR_REPLACE (aspace_map with ASPACE_REPLACE, mm/aspace.c): an unmap and
 * a map in one step, MAP_FIXED's call.
 *
 *   aspace_replace_shapes
 *       over nothing, over part of one mapping (a split), over the ends of
 *       two and a whole one between, over a hole and a mapping; the
 *       entries and mappings after each, the refusals (and a refused one
 *       leaves the old mapping and its entries as they were), and the
 *       job's charges back to nothing at the end.
 *   aspace_replace_under_faults
 *       threads on other CPUs copy from the range through the address
 *       space, as a system call does, while the test replaces all or part
 *       of it thousands of times with one VMO or another: every copy
 *       succeeds and reads that page's word from one of the two VMOs. (An
 *       unmap then a map leaves a moment with nothing there: such copies
 *       fail.) */
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/irq.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>
#include <jam/x86.h>

#define PG       PAGE_SIZE
#define RW       (ASPACE_READ | ASPACE_WRITE)
#define FIX      (RW | ASPACE_FIXED)
#define REPL     (RW | ASPACE_FIXED | ASPACE_REPLACE)
#define PTE_P    (1ull << 0)
#define PTE_ADDR 0x000ffffffffff000ull
#define BASE     0x30000000ull
#define ROUNDS   5000
#define COPIERS  3

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static uint64_t pte_pa(struct aspace *as, uint64_t va)
{
    uint64_t e = aspace_pte(as, va);
    return (e & PTE_P) ? (e & PTE_ADDR) : 0;
}

/* A VMO of n pages whose page i starts with the word tag + i. */
static struct vmo *tagged(unsigned n, uint64_t tag)
{
    struct vmo *v;
    KT_EQ(vmo_create(n * PG, 0, &v), OK);
    for (unsigned i = 0; i < n; i++) {
        uint64_t w = tag + i;
        KT_EQ(vmo_write(v, i * PG, &w, 8), OK);
    }
    return v;
}

/* Page `page` of the range faults in to page `idx` of v. */
static void maps_to(struct aspace *as, unsigned page, struct vmo *v, unsigned idx)
{
    KT_EQ(aspace_fault(as, BASE + page * PG, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, BASE + page * PG), vmo_page_phys(v, idx * PG));
}

static status_t replace(struct aspace *as, struct vmo *v, unsigned first, unsigned end,
                        unsigned vmo_page)
{
    uint64_t a = BASE + first * PG;
    status_t st = aspace_map(as, v, vmo_page * PG, (end - first) * PG, REPL, &a);
    if (st == OK)
        KT_EQ(a, BASE + first * PG);
    return st;
}

KTEST(aspace_replace_shapes)
{
    struct job *j = kt_fresh_job();
    struct aspace *as;
    KT_EQ(aspace_create_charged(j, &as), OK);
    struct vmo *a = tagged(8, 0xa000), *b = tagged(8, 0xb000), *c = tagged(16, 0xc000);
    uint64_t at = BASE;
    KT_EQ(aspace_map(as, a, 0, 8 * PG, FIX, &at), OK);
    for (unsigned i = 0; i < 8; i++)
        maps_to(as, i, a, i);

    /* The middle of one mapping: a[0,2) b[2,4) a[4,8). */
    KT_EQ(replace(as, b, 2, 4, 0), OK);
    KT_EQ(aspace_mapping_count(as), 3);
    KT_EQ(pte_pa(as, BASE + 2 * PG), 0);   /* the old entry went */
    maps_to(as, 1, a, 1);
    maps_to(as, 2, b, 0);
    maps_to(as, 3, b, 1);
    maps_to(as, 4, a, 4);

    /* The tail of one, all of the next, the head of the third:
     * a[0,1) c[1,6) a[6,8). */
    KT_EQ(replace(as, c, 1, 6, 1), OK);
    KT_EQ(aspace_mapping_count(as), 3);
    maps_to(as, 0, a, 0);
    for (unsigned i = 1; i < 6; i++)
        maps_to(as, i, c, i);
    maps_to(as, 6, a, 6);
    KT_EQ(vmo_kobject(b)->refs, 1);   /* b's mapping went whole */

    /* Over nothing it is a fixed map; then over a mapping's end, a hole
     * and another's head: a[0,1) c[1,6) a[6,7) b[7,11) c[11,12). */
    KT_EQ(replace(as, c, 10, 12, 10), OK);
    KT_EQ(aspace_mapping_count(as), 4);
    KT_EQ(replace(as, b, 7, 11, 0), OK);
    KT_EQ(aspace_mapping_count(as), 5);
    maps_to(as, 6, a, 6);
    maps_to(as, 7, b, 0);
    maps_to(as, 9, b, 2);
    maps_to(as, 10, b, 3);
    maps_to(as, 11, c, 11);

    /* Refusals, each with nothing changed: page 3 still maps c's page 3. */
    uint64_t pa3 = pte_pa(as, BASE + 3 * PG);
    KT_EQ(pa3, vmo_page_phys(c, 3 * PG));
    at = BASE;
    KT_EQ(aspace_map(as, b, 0, PG, RW | ASPACE_REPLACE, &at), ERR_INVALID_ARGS);   /* not FIXED */
    KT_EQ(aspace_map(as, b, 0, PG, ASPACE_READ | ASPACE_FIXED | ASPACE_REPLACE | ASPACE_KEPT_ONLY,
                     &at), ERR_INVALID_ARGS);
    KT_EQ(replace(as, b, 2, 4, 7), ERR_OUT_OF_RANGE);   /* past b's end */
    KT_EQ(aspace_mapping_count(as), 5);
    KT_EQ(pte_pa(as, BASE + 3 * PG), pa3);
    KT_EQ(aspace_fault(as, BASE + 3 * PG, ASPACE_READ), OK);
    KT_EQ(pte_pa(as, BASE + 3 * PG), pa3);

    KT_EQ(aspace_unmap(as, USER_BASE, USER_TOP - USER_BASE), OK);
    KT_EQ(aspace_mapping_count(as), 0);
    KT_EQ(vmo_kobject(a)->refs, 1);
    KT_EQ(vmo_kobject(b)->refs, 1);
    KT_EQ(vmo_kobject(c)->refs, 1);
    aspace_unref(as);
    kt_job_is_empty(j);
    job_unref(j);
    put(a);
    put(b);
    put(c);
}

/* A kernel thread copying from the range through the address space. */
struct copier {
    struct aspace *as;
    uint32_t       cpu;
    uint64_t       copies, failed, wrong;
    struct thread *t;
};

static uint32_t copy_stop;

static void copier_main(void *arg)
{
    struct copier *c = arg;
    struct thread *t = current_thread();
    kt_pin_self(c->cpu);
    aspace_ref(c->as);
    irq_disable();
    t->aspace = c->as;
    aspace_switch(NULL, c->as);
    irq_enable();
    while (!__atomic_load_n(&copy_stop, __ATOMIC_ACQUIRE)) {
        for (unsigned i = 0; i < 4; i++) {
            uint64_t w = 0;
            if (copy_from_user(&w, BASE + i * PG, sizeof(w)) != OK)
                c->failed++;
            else if (w != 0xa000 + i && w != 0xb000 + i)
                c->wrong++;
            c->copies++;
        }
    }
    irq_disable();
    t->aspace = NULL;
    aspace_switch(c->as, NULL);
    irq_enable();
    aspace_unref(c->as);
    kt_unpin_self();
}

KTEST(aspace_replace_under_faults)
{
    struct aspace *as;
    KT_EQ(aspace_create(&as), OK);
    struct vmo *v[2] = { tagged(4, 0xa000), tagged(4, 0xb000) };
    uint64_t at = BASE;
    KT_EQ(aspace_map(as, v[0], 0, 4 * PG, FIX, &at), OK);
    uint64_t faults0 = aspace_fault_count(as);

    uint32_t n = cpu_count > 1 ? cpu_count - 1 : 1;
    n = n < COPIERS ? n : COPIERS;
    static struct copier cs[COPIERS];
    memset(cs, 0, sizeof(cs));
    __atomic_store_n(&copy_stop, 0, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < n; i++) {
        cs[i].as = as;
        cs[i].cpu = (i + 1) % cpu_count;
        cs[i].t = thread_create("replace-copy", copier_main, &cs[i], PRIO_DEFAULT);
        KT_ASSERT(cs[i].t);
    }
    /* All of it, its middle, its first half, its last page. */
    static const uint8_t ranges[][2] = { { 0, 4 }, { 1, 3 }, { 0, 2 }, { 3, 4 } };
    for (unsigned r = 0; r < ROUNDS; r++) {
        const uint8_t *g = ranges[r % 4];
        KT_EQ(replace(as, v[(r / 4) & 1], g[0], g[1], g[0]), OK);
        if (r % 64 == 0)
            thread_yield();
    }
    __atomic_store_n(&copy_stop, 1, __ATOMIC_RELEASE);
    uint64_t copies = 0;
    for (uint32_t i = 0; i < n; i++) {
        thread_join(cs[i].t);
        copies += cs[i].copies;
        KT_EQ(cs[i].failed, 0);
        KT_EQ(cs[i].wrong, 0);
    }
    uint64_t faults = aspace_fault_count(as) - faults0;
    kprintf("ktest %s: %u replaces, %u copiers, %lu copies, %lu faults\n", ktest_current,
            ROUNDS, n, copies, faults);
    KT_ASSERT(copies > 0);
    KT_IDLE_ASSERT(faults > 0);   /* the copies did meet the replaces */
    aspace_unref(as);
    put(v[0]);
    put(v[1]);
}
