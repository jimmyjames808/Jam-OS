/* fbbench: a crew of exactly n threads for one line. libfun's pool always
 * uses every thread it has; a line measured at 4 threads must have only 4
 * at work and nothing else spinning beside them (a spinning hyperthread
 * slows its sibling), so each line starts its own crew and ends it.
 *
 * The workers spin (with pause) between jobs, as the pool's do for its
 * first millisecond, so a sample never includes waking a thread: the
 * compositor's pool is awake while it paints a burst of frames. A job is
 * published by crew_run's SEQ_CST bump of phase after it stores fn, arg,
 * items, next and done; a worker's ACQUIRE load of phase pairs with it.
 * Each worker's RELEASE add to done pairs with crew_run's ACQUIRE load. */
#include "fbbench.h"

#define CREW_STACK (64u << 10)
#define STOP_NS    (5 * NS_PER_S)

static struct {
    uint32_t n;                                   /* threads, the caller's included */
    handle_t th[CREW_MAX];                        /* worker i's thread handle (i >= 1) */
    void    *stack[CREW_MAX];                     /* and its stack */
    uint32_t phase;                               /* bumped by crew_run: a new job */
    uint32_t base;                                /* phase when the workers were made */
    uint32_t quit;                                /* set by crew_stop */
    uint32_t next, done, items;                   /* next item; threads done; items */
    void   (*fn)(uint32_t, uint32_t, void *);     /* the job */
    void    *arg;                                 /* its argument */
} crew = { .n = 1 };

static void work(uint32_t me)
{
    uint32_t i;
    while ((i = __atomic_fetch_add(&crew.next, 1, __ATOMIC_RELAXED)) <
           __atomic_load_n(&crew.items, __ATOMIC_RELAXED))
        crew.fn(i, me, crew.arg);
    __atomic_fetch_add(&crew.done, 1, __ATOMIC_RELEASE);
}

static void worker(void *a)
{
    uint32_t me = (uint32_t)(uintptr_t)a;
    /* Not crew.phase itself: the first job may be published before this
     * thread first runs. */
    uint32_t seen = __atomic_load_n(&crew.base, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t ph;
        while ((ph = __atomic_load_n(&crew.phase, __ATOMIC_ACQUIRE)) == seen) {
            if (__atomic_load_n(&crew.quit, __ATOMIC_RELAXED))
                return;
            __builtin_ia32_pause();
        }
        seen = ph;
        work(me);
    }
}

uint32_t crew_start(uint32_t n)
{
    crew_stop();
    n = n < 1 ? 1 : n > CREW_MAX ? CREW_MAX : n;
    __atomic_store_n(&crew.quit, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&crew.base, __atomic_load_n(&crew.phase, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    for (uint32_t i = 1; i < n; i++) {
        void *stack = malloc(CREW_STACK);
        if (!stack)
            break;
        if (thread_spawn("fbbench", worker, (void *)(uintptr_t)i, stack, CREW_STACK,
                         &crew.th[i]) != OK) {
            free(stack);
            break;
        }
        crew.stack[i] = stack;
        crew.n = i + 1;
    }
    return crew.n;
}

void crew_run(void (*fn)(uint32_t, uint32_t, void *), void *arg, uint32_t items)
{
    crew.fn = fn;
    crew.arg = arg;
    __atomic_store_n(&crew.items, items, __ATOMIC_RELAXED);
    __atomic_store_n(&crew.next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&crew.done, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&crew.phase, 1, __ATOMIC_SEQ_CST);   /* publishes the job */
    work(0);
    while (__atomic_load_n(&crew.done, __ATOMIC_ACQUIRE) < crew.n)
        __builtin_ia32_pause();
}

void crew_stop(void)
{
    __atomic_store_n(&crew.quit, 1, __ATOMIC_RELAXED);
    for (uint32_t i = 1; i < crew.n; i++) {
        /* A worker that doesn't end in STOP_NS is left running and its
         * stack kept: never freed under a live thread. */
        if (jam_object_wait_one(crew.th[i], SIG_TERMINATED, now() + STOP_NS, NULL) == OK)
            free(crew.stack[i]);
        jam_handle_close(crew.th[i]);
        crew.th[i] = HANDLE_INVALID;
        crew.stack[i] = NULL;
    }
    crew.n = 1;
}
