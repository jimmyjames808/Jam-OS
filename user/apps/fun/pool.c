/* libfun: the CPUs (CPUID) and the thread pool (fun.h). */
#include "fun.h"

/* ---- CPUs and the thread pool ---------------------------------------------------------- */

/* The pool's shared words are touched only with __atomic builtins. A batch
 * is published by pool_run's SEQ_CST bump of phase (after it stores fn,
 * arg, items, next and done), which the workers' ACQUIRE load of phase
 * pairs with; the RELEASE add to done pairs with pool_run's ACQUIRE load of
 * it. sleeping[] and the second load of phase are SEQ_CST so that a worker
 * about to sleep and pool_run about to signal can't both miss each other.
 *
 * Between batches a worker spins (up to POOL_SPIN pauses) so that a batch
 * coming right after the last one finds it awake, then sleeps. pool_rest
 * (the app is about to wait: gfx_key does it) bumps rest, and a worker
 * that sees rest move since its last batch sleeps at once: without it an
 * app drawing 60 frames a second kept every worker spinning about 1 ms a
 * batch, some 3 CPUs' worth on a 28-CPU machine. A worker reads rest
 * before it adds itself to done, so pool_run's caller, which bumps rest
 * only after it has seen done complete, can't bump it before the read. */

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
    uint32_t sleeping[FUN_MAX_THREADS];    /* worker i sleeps (or is about to) */
    uint32_t phase;                        /* bumped by pool_run: a new batch */
    uint32_t rest;                         /* bumped by pool_rest: stop spinning */
    uint64_t spun[FUN_MAX_THREADS];        /* worker i's pauses so far (pool_spins) */
    uint32_t next, done, items;            /* the next item to take; threads done; items */
    void (*fn)(uint32_t, uint32_t, void *);   /* the batch's work: fn(item, thread, arg) */
    void *arg;                             /* its argument */
} pool = { .n = 1 };
uint32_t pool_items_by[FUN_MAX_THREADS];

static void pool_work(uint32_t me)
{
    uint32_t i, did = 0;
    while ((i = __atomic_fetch_add(&pool.next, 1, __ATOMIC_RELAXED)) <
           __atomic_load_n(&pool.items, __ATOMIC_RELAXED)) {
        pool.fn(i, me, pool.arg);
        did++;
    }
    pool_items_by[me] = did;
    __atomic_fetch_add(&pool.done, 1, __ATOMIC_RELEASE);
}

/* Worker me's spins so far, for pool_spins (it alone writes them). */
static void count_spins(uint32_t me, uint32_t spins)
{
    uint64_t was = __atomic_load_n(&pool.spun[me], __ATOMIC_RELAXED);
    __atomic_store_n(&pool.spun[me], was + spins, __ATOMIC_RELAXED);
}

static void pool_worker(void *a)
{
    uint32_t me = (uint32_t)(uintptr_t)a, seen = 0, rest = 0;
    for (;;) {
        uint32_t ph, spins = 0;
        while ((ph = __atomic_load_n(&pool.phase, __ATOMIC_ACQUIRE)) == seen) {
            if (spins < POOL_SPIN && __atomic_load_n(&pool.rest, __ATOMIC_RELAXED) == rest) {
                spins++;
                __builtin_ia32_pause();
                continue;
            }
            /* Sleep: announce it, clear the event, look once more (pool_run
             * bumps the phase, then signals every worker that announced). */
            count_spins(me, spins);
            spins = 0;
            __atomic_store_n(&pool.sleeping[me], 1, __ATOMIC_SEQ_CST);
            jam_event_signal(pool.ev[me], SIG_SIGNALED, 0);
            if (__atomic_load_n(&pool.phase, __ATOMIC_SEQ_CST) == seen)
                jam_object_wait_one(pool.ev[me], SIG_SIGNALED, DEADLINE_NEVER, NULL);
            __atomic_store_n(&pool.sleeping[me], 0, __ATOMIC_SEQ_CST);
            rest = __atomic_load_n(&pool.rest, __ATOMIC_RELAXED);   /* woken: spin again */
        }
        count_spins(me, spins);
        seen = ph;
        rest = __atomic_load_n(&pool.rest, __ATOMIC_RELAXED);   /* before done: see the top */
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
        if (!stack)
            break;
        if (jam_event_create(&pool.ev[i]) != OK) {
            free(stack);
            break;
        }
        void *me = (void *)(uintptr_t)i;
        if (thread_spawn("worker", pool_worker, me, stack, POOL_STACK, &th) != OK) {
            jam_handle_close(pool.ev[i]);
            free(stack);
            break;
        }
        jam_handle_close(th);
        pool.n = i + 1;
    }
    return pool.n;
}

uint32_t pool_threads(void) { return pool.n; }

void pool_rest(void)
{
    __atomic_add_fetch(&pool.rest, 1, __ATOMIC_RELAXED);
}

uint64_t pool_spins(void)
{
    uint64_t n = 0;
    for (uint32_t i = 1; i < pool.n; i++)
        n += __atomic_load_n(&pool.spun[i], __ATOMIC_RELAXED);
    return n;
}

uint32_t pool_asleep(void)
{
    uint32_t n = 0;
    for (uint32_t i = 1; i < pool.n; i++)
        n += __atomic_load_n(&pool.sleeping[i], __ATOMIC_SEQ_CST);
    return n;
}

void pool_run(void (*fn)(uint32_t, uint32_t, void *), void *arg, uint32_t items)
{
    pool.fn = fn;
    pool.arg = arg;
    __atomic_store_n(&pool.items, items, __ATOMIC_RELAXED);
    __atomic_store_n(&pool.next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pool.done, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pool.phase, 1, __ATOMIC_SEQ_CST);   /* publishes the batch */
    for (uint32_t i = 1; i < pool.n; i++)
        if (__atomic_load_n(&pool.sleeping[i], __ATOMIC_SEQ_CST))
            jam_event_signal(pool.ev[i], 0, SIG_SIGNALED);
    pool_work(0);
    while (__atomic_load_n(&pool.done, __ATOMIC_ACQUIRE) < pool.n)
        __builtin_ia32_pause();
}
