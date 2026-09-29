/* libfun: the CPUs (CPUID) and the thread pool (fun.h). */
#include "fun.h"

/* ---- CPUs and the thread pool ---------------------------------------------------------- */

uint32_t fun_cpu_count(void)
{
    uint32_t r[4], n = 0;
    cpu_cpuid(0, 0, r);
    uint32_t max = r[0];
    /* V2 extended topology (0x1F) or extended topology (0xB): the last level
     * before "invalid" counts every logical CPU in the package. */
    for (uint32_t leaf = 0x1f; leaf >= 0xb && !n; leaf = leaf == 0x1f ? 0xb : 0) {
        if (max < leaf)
            continue;
        for (uint32_t sub = 0; sub < 8; sub++) {
            cpu_cpuid(leaf, sub, r);
            if (((r[2] >> 8) & 0xff) == 0)
                break;
            if (r[1] & 0xffff)
                n = r[1] & 0xffff;
        }
    }
    if (!n) {
        cpu_cpuid(1, 0, r);
        n = (r[1] >> 16) & 0xff;
    }
    if (n < 1)
        n = 1;
    if (n > FUN_MAX_THREADS)
        n = FUN_MAX_THREADS;
    return n;
}

bool fun_has_avx2(void)
{
    uint32_t r[4];
    cpu_cpuid(0, 0, r);
    if (r[0] < 7)
        return false;
    cpu_cpuid(1, 0, r);
    bool fma = r[2] >> 12 & 1, osxsave = r[2] >> 27 & 1, avx = r[2] >> 28 & 1;
    if (!fma || !osxsave || !avx)
        return false;
    if ((cpu_xcr0() & 6) != 6)   /* the OS saves SSE and AVX state */
        return false;
    cpu_cpuid(7, 0, r);
    return r[1] >> 5 & 1;
}

bool fun_is_tcg(void)
{
    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    if (!(r[2] >> 31 & 1))   /* no hypervisor */
        return false;
    cpu_cpuid(0x40000000, 0, r);
    return r[1] == 0x54474354 && r[2] == 0x43544743 && r[3] == 0x47435447;   /* "TCGTCGTCGTCG" */
}

#define POOL_STACK (64u << 10)
#define POOL_SPIN  20000   /* pauses (~1 ms) before a worker sleeps */

static struct {
    uint32_t n;                            /* threads, the caller's included */
    handle_t ev[FUN_MAX_THREADS];          /* worker i sleeps on ev[i] */
    volatile uint32_t sleeping[FUN_MAX_THREADS];   /* worker i sleeps (or is about to) */
    volatile uint32_t phase;               /* bumped by pool_run: a new batch */
    volatile uint32_t next, done, items;   /* the next item to take; threads done; items */
    void (*fn)(uint32_t, uint32_t, void *);   /* the batch's work: fn(item, thread, arg) */
    void *arg;                             /* its argument */
} pool = { .n = 1 };
uint32_t pool_items_by[FUN_MAX_THREADS];

static void pool_work(uint32_t me)
{
    uint32_t i, did = 0;
    while ((i = __atomic_fetch_add(&pool.next, 1, __ATOMIC_RELAXED)) < pool.items) {
        pool.fn(i, me, pool.arg);
        did++;
    }
    pool_items_by[me] = did;
    __atomic_fetch_add(&pool.done, 1, __ATOMIC_RELEASE);
}

static void pool_worker(void *a)
{
    uint32_t me = (uint32_t)(uintptr_t)a, seen = 0;
    for (;;) {
        uint32_t ph, spins = 0;
        while ((ph = __atomic_load_n(&pool.phase, __ATOMIC_ACQUIRE)) == seen) {
            if (++spins < POOL_SPIN) {
                __builtin_ia32_pause();
                continue;
            }
            /* Sleep: announce it, clear the event, look once more (pool_run
             * bumps the phase, then signals every worker that announced). */
            __atomic_store_n(&pool.sleeping[me], 1, __ATOMIC_SEQ_CST);
            jam_event_signal(pool.ev[me], SIG_SIGNALED, 0);
            if (__atomic_load_n(&pool.phase, __ATOMIC_SEQ_CST) == seen)
                jam_object_wait_one(pool.ev[me], SIG_SIGNALED, DEADLINE_NEVER, NULL);
            __atomic_store_n(&pool.sleeping[me], 0, __ATOMIC_SEQ_CST);
            spins = 0;
        }
        seen = ph;
        pool_work(me);
    }
}

uint32_t pool_start(uint32_t n)
{
    if (pool.n > 1)
        return pool.n;   /* already running */
    if (!n)
        n = fun_cpu_count();
    if (n > FUN_MAX_THREADS)
        n = FUN_MAX_THREADS;
    pool.n = 1;
    for (uint32_t i = 1; i < n; i++) {
        void *stack = malloc(POOL_STACK);
        handle_t th;
        if (!stack || jam_event_create(&pool.ev[i]) != OK)
            break;
        if (thread_spawn("worker", pool_worker, (void *)(uintptr_t)i, stack, POOL_STACK, &th) != OK)
            break;
        jam_handle_close(th);
        pool.n = i + 1;
    }
    return pool.n;
}

uint32_t pool_threads(void) { return pool.n; }

void pool_run(void (*fn)(uint32_t, uint32_t, void *), void *arg, uint32_t items)
{
    pool.fn = fn;
    pool.arg = arg;
    pool.items = items;
    pool.next = 0;
    pool.done = 0;
    __atomic_add_fetch(&pool.phase, 1, __ATOMIC_SEQ_CST);
    for (uint32_t i = 1; i < pool.n; i++)
        if (__atomic_load_n(&pool.sleeping[i], __ATOMIC_SEQ_CST))
            jam_event_signal(pool.ev[i], 0, SIG_SIGNALED);
    pool_work(0);
    while (__atomic_load_n(&pool.done, __ATOMIC_ACQUIRE) < pool.n)
        __builtin_ia32_pause();
}
