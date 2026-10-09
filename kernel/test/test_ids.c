/* Small ids (<jam/idspace.h>): the allocator on a space of its own (the
 * order, the lap, the hold, a full space), then the kernel's pids and
 * tids under many creates and kills on every CPU at once, checked
 * unique among the living. */
#include <jam/idspace.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>

#define SMALL_WORDS 2   /* a test space: the values 1 .. 127 */

static uint32_t take(struct idspace *s)
{
    uint32_t v = 0;
    KT_EQ(idspace_alloc(s, &v), OK);
    return v;
}

/* In rising order from the cursor; given back behind it: free again but
 * reached only after the wrap; a full space refuses. */
KTEST(ids_order_and_wrap)
{
    uint64_t used[SMALL_WORDS], held[SMALL_WORDS];
    struct idspace s;
    idspace_init(&s, "kt id space", used, held, SMALL_WORDS);
    for (uint32_t v = 1; v < 128; v++)
        KT_EQ(take(&s), v);
    uint32_t x;
    KT_EQ(idspace_alloc(&s, &x), ERR_NO_RESOURCES);   /* full, wrapped once for nothing */
    KT_EQ(idspace_live(&s), 127);
    idspace_free(&s, 3);
    idspace_free(&s, 100);
    KT_EQ(take(&s), 3);     /* the next lap, from the bottom */
    KT_EQ(take(&s), 100);
    KT_EQ(idspace_alloc(&s, &x), ERR_NO_RESOURCES);
    /* The refusal wrapped the cursor back to 1: everything given back now
     * is ahead of it, held, and free again only at the next wrap. */
    for (uint32_t v = 1; v < 128; v++)
        idspace_free(&s, v);
    KT_EQ(idspace_live(&s), 0);
    KT_EQ(s.nheld, 127);
    uint64_t laps = s.laps;
    KT_EQ(take(&s), 1);
    KT_EQ(s.laps, laps + 1);
    KT_EQ(s.nheld, 0);
    KT_EQ(take(&s), 2);
    idspace_free(&s, 1);
    idspace_free(&s, 2);
}

/* A value given back ahead of the cursor waits for the next lap: the
 * values after it come first. */
KTEST(ids_held_for_the_lap)
{
    uint64_t used[SMALL_WORDS], held[SMALL_WORDS];
    struct idspace s;
    idspace_init(&s, "kt id space", used, held, SMALL_WORDS);
    for (uint32_t v = 1; v < 128; v++)
        take(&s);
    idspace_free(&s, 10);
    idspace_free(&s, 60);
    KT_EQ(take(&s), 10);      /* wraps; the cursor is at 11 */
    uint64_t laps = s.laps;
    idspace_free(&s, 50);     /* ahead of the cursor: held */
    KT_EQ(take(&s), 60);      /* not 50 */
    KT_EQ(take(&s), 50);      /* after the wrap */
    KT_EQ(s.laps, laps + 1);
    for (uint32_t v = 1; v < 128; v++)
        idspace_free(&s, v);
}

/* ---- pids and tids under load -------------------------------------------------- */

#define IDS_THREADS 8      /* at least: one per CPU past 8 */
#define IDS_BATCH   16     /* processes a thread holds at once */
#define IDS_TOTAL   10000  /* creates in all */

/* The ids alive right now, the test's own: a value in the table twice is
 * a duplicate among the living. */
static spinlock_t live_lock = SPINLOCK_INIT("kt live ids");
static uint32_t live_pid[IDS_THREADS * 4][IDS_BATCH], live_tid[IDS_THREADS * 4][IDS_BATCH];
static uint32_t dup_found;   /* a duplicate (RELAXED; the test checks it after the join) */

/* live_lock held: is v in the table (but at [me][slot] itself)? */
static bool seen_locked(uint32_t (*tab)[IDS_BATCH], unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = 0; k < IDS_BATCH; k++)
            if (tab[i][k] == v)
                return true;
    return false;
}

static void note(uint32_t (*tab)[IDS_BATCH], unsigned n, unsigned me, unsigned slot, uint32_t v)
{
    uint64_t f = spin_lock_irqsave(&live_lock);
    if (!v || v >= IDS_MAX || seen_locked(tab, n, v))
        __atomic_store_n(&dup_found, v ? v : UINT32_MAX, __ATOMIC_RELAXED);
    tab[me][slot] = v;
    spin_unlock_irqrestore(&live_lock, f);
}

static void forget(uint32_t (*tab)[IDS_BATCH], unsigned me, unsigned slot)
{
    uint64_t f = spin_lock_irqsave(&live_lock);
    tab[me][slot] = 0;
    spin_unlock_irqrestore(&live_lock, f);
}

struct ids_worker {
    struct job *job;   /* where its processes go */
    unsigned    me;    /* its row in the tables */
    unsigned    n;     /* threads in all */
    unsigned    rounds;
    status_t    st;    /* the first failure */
};

/* One round: IDS_BATCH processes, each with a thread object, made, noted,
 * then killed, forgotten and dropped. */
static void ids_round(struct ids_worker *w)
{
    struct process *ps[IDS_BATCH];
    struct uthread *us[IDS_BATCH];
    for (unsigned k = 0; k < IDS_BATCH; k++) {
        status_t st = process_create(w->job, "kt-ids", &ps[k]);
        if (st == OK && (st = uthread_create(ps[k], "kt-ids", &us[k])) != OK)
            kobject_unref(process_kobject(ps[k]));
        if (st != OK) {
            w->st = st;
            ps[k] = NULL;
            continue;
        }
        struct thread_info ti;
        uthread_get_info(us[k], &ti);
        note(live_pid, w->n, w->me, k, process_pid(ps[k]));
        note(live_tid, w->n, w->me, k, ti.tid);
    }
    for (unsigned k = 0; k < IDS_BATCH; k++) {
        if (!ps[k])
            continue;
        /* Forgotten before the drop that gives the ids back: another CPU
         * may be handed them the moment after. */
        forget(live_pid, w->me, k);
        forget(live_tid, w->me, k);
        process_kill(ps[k], PROCESS_KILLED_CODE, true);
        kobject_unref(uthread_kobject(us[k]));
        kobject_unref(process_kobject(ps[k]));
    }
}

static void ids_main(void *arg)
{
    struct ids_worker *w = arg;
    for (unsigned r = 0; r < w->rounds; r++)
        ids_round(w);
}

KTEST(ids_unique_many_cpus)
{
    unsigned n = cpu_count > IDS_THREADS ? cpu_count : IDS_THREADS;
    if (n > IDS_THREADS * 4)
        n = IDS_THREADS * 4;
    uint32_t pids0 = idspace_live(&pid_space), tids0 = idspace_live(&tid_space);
    memset(live_pid, 0, sizeof(live_pid));
    memset(live_tid, 0, sizeof(live_tid));
    dup_found = 0;
    struct job *j = kt_fresh_job();
    struct ids_worker w[IDS_THREADS * 4];
    struct thread *t[IDS_THREADS * 4];
    unsigned rounds = (IDS_TOTAL + n * IDS_BATCH - 1) / (n * IDS_BATCH);
    for (unsigned i = 0; i < n; i++) {
        w[i] = (struct ids_worker){ j, i, n, rounds, OK };
        cpumask_t m;
        cpumask_one(&m, i % cpu_count);
        t[i] = thread_create_on("kt-ids", ids_main, &w[i], PRIO_DEFAULT, &m);
    }
    for (unsigned i = 0; i < n; i++)
        thread_join(t[i]);
    for (unsigned i = 0; i < n; i++)
        KT_EQ(w[i].st, OK);
    KT_EQ(__atomic_load_n(&dup_found, __ATOMIC_RELAXED), 0);
    kprintf("ids: %u processes and threads made and killed on %u CPUs\n",
            n * rounds * IDS_BATCH, cpu_count);
    kt_job_is_empty(j);
    job_unref(j);
    KT_GLOBAL_EQ(idspace_live(&pid_space), pids0);   /* every id given back */
    KT_GLOBAL_EQ(idspace_live(&tid_space), tids0);
}
