/* Hostile child modes for the kernel's tests (kernel/test/test_fuzz.c):
 * a program that does what no program should, from many threads at once,
 * until it exits or is killed. The kernel must neither crash nor leak:
 * once the child is dead its job is back to zero on every count.
 *
 *   utest fuzz-calls <seed> <ms>   random system calls with random
 *       arguments (numbers, handles from a pool of its own objects and
 *       stale or made-up values, good and bad pointers, structs of random
 *       fields), from FZ_THREADS threads at a time: a call with a far
 *       deadline may block its thread for good, so a parked thread takes
 *       over from each one that stopped making progress (FZ_MAX in all);
 *       after <ms> the main thread exits the process while the others are
 *       inside their calls.
 *   utest fuzz-chans <seed> <ms>   channel ends shared through a table of
 *       slots by FZ_THREADS threads that create, write (with handles,
 *       ends included), read, call, reply-and-wait, mark, bind, close and
 *       duplicate them at random, each racing the others' closes; exits
 *       after <ms> the same way (or is killed by the test first).
 *
 * The child holds no power: its startup handles are its own process,
 * thread, address space, job and bootfs, and fuzz-calls closes every one
 * once its threads exist (a number like 32768 is also a handle value: the
 * table's first slot), so nothing it does can reach another program, nor
 * end its own run early. It never calls
 * process_exit or thread_exit at random, nor debug_write (the log). */
#include <jam/syscall_nums.h>
#include <os.h>
#include "utest.h"

#define FZ_THREADS 12           /* running at once */
#define FZ_MAX     48           /* threads in all: the rest are parked */
#define FZ_POOL    48           /* handles of objects the fuzzer made */
#define FZ_SCRATCH (64 * 1024)  /* what pointer arguments may point into */

static uint8_t fz_scratch[FZ_SCRATCH] __attribute__((aligned(4096)));
static handle_t fz_pool[FZ_POOL];
static uint8_t fz_stacks[FZ_MAX][16384] __attribute__((aligned(64)));
static volatile uint32_t fz_calls;
static uint32_t fz_active[FZ_MAX];     /* 0: parked (a futex word), 1: fuzzing */
static uint32_t fz_progress[FZ_MAX];   /* calls each thread returned from */
static uint32_t fz_go;                 /* the startup handles are closed */

struct fz_arg {
    uint64_t seed;
    unsigned i;
};
static struct fz_arg fz_args[FZ_MAX];

/* xorshift64*: each thread its own state. */
static uint64_t rnd(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545f4914f6cdd1dull;
}

static int64_t raw_call(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5)
{
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret), "+r"(r10), "+r"(r8), "+r"(r9)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                       "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                       "xmm14", "xmm15");
    return ret;
}

/* A handle-shaped value: mostly one of the pool's (a live object, or one
 * a racing close just ended), sometimes its neighbour (a stale
 * generation, another slot), a word the kernel wrote into the scratch
 * area (a new handle an earlier call returned), or anything. */
static uint64_t a_handle(uint64_t *s)
{
    uint64_t r = rnd(s);
    switch (r % 8) {
    case 0: case 1: case 2: case 3:
        return __atomic_load_n(&fz_pool[(r >> 8) % FZ_POOL], __ATOMIC_RELAXED);
    case 4:
        return __atomic_load_n(&fz_pool[(r >> 8) % FZ_POOL], __ATOMIC_RELAXED) + ((r >> 20) % 3) - 1;
    case 5: {
        uint32_t w;
        memcpy(&w, fz_scratch + (((r >> 8) % (FZ_SCRATCH / 4)) * 4), 4);
        return w;
    }
    case 6:
        return 0;
    default:
        return (uint32_t)(r >> 16);
    }
}

/* A pointer-shaped value: into the scratch area (most), or null, kernel,
 * non-canonical, the last bytes below the user range's top, or random. */
static uint64_t a_pointer(uint64_t *s)
{
    uint64_t r = rnd(s);
    switch (r % 10) {
    case 0: case 1: case 2: case 3: case 4: case 5:
        return (uint64_t)(uintptr_t)fz_scratch + ((r >> 8) % FZ_SCRATCH);
    case 6:
        return 0;
    case 7:
        return 0xffffffff80000000ull + ((r >> 8) & 0xfffff);
    case 8:
        return 0x7fffffffe000ull - ((r >> 8) & 0xff);
    default:
        return (r >> 4) | ((r & 1) << 63);
    }
}

/* A number: small, a size or count near a limit, a deadline (now, soon,
 * past, forever), or anything. */
static uint64_t a_number(uint64_t *s)
{
    uint64_t r = rnd(s);
    switch (r % 12) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (r >> 8) % 64;
    case 3: return 4096ull << ((r >> 8) % 6);
    case 4: return (uint64_t)now() + ((r >> 8) % 2000000);   /* within 2 ms */
    case 5: return (uint64_t)now() - ((r >> 8) % 1000000);   /* just past */
    case 6: return (r >> 8) & 1 ? UINT64_MAX : (uint64_t)now() + 1000000;
    case 7: return 1ull << ((r >> 8) % 64);
    case 8: return (r >> 8) % 70000;                           /* near CHANNEL_MAX_BYTES */
    case 9: return 0xffffffffull;
    case 10: return (uint64_t)now() + ((r >> 8) % 100000);    /* within 0.1 ms */
    default: return r;
    }
}

static uint64_t an_arg(uint64_t *s)
{
    uint64_t r = rnd(s) % 3;
    return r == 0 ? a_handle(s) : r == 1 ? a_pointer(s) : a_number(s);
}

/* A struct argument filled with plausible fields (struct-taking calls). */
static void random_struct(uint64_t *s, uint64_t *w, unsigned words)
{
    for (unsigned i = 0; i < words; i++) {
        uint64_t r = rnd(s) % 4;
        w[i] = r == 0 ? a_handle(s) | a_handle(s) << 32 : r == 1 ? a_pointer(s) : a_number(s);
    }
    if (rnd(s) % 2)
        w[0] = (uint32_t)a_handle(s) | (rnd(s) % 512) << 32;   /* h, then a small length */
}

static bool excluded(uint64_t nr)
{
    return nr == SYS_process_exit || nr == SYS_thread_exit || nr == SYS_debug_write ||
           nr == SYS_debug_report;
}

static bool takes_struct(uint64_t nr)
{
    return nr == SYS_channel_read || nr == SYS_channel_call || nr == SYS_channel_reply_wait ||
           nr == SYS_vmo_pin;
}

static void fuzz_worker(void *arg)
{
    struct fz_arg *a0 = arg;
    uint64_t s = a0->seed;
    unsigned me = a0->i;
    uint64_t sbuf[16];
    while (!__atomic_load_n(&fz_go, __ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&fz_active[me], __ATOMIC_ACQUIRE))
        jam_futex_wait(&fz_active[me], 0, now() + 50 * NS_PER_MS, 0);
    for (;;) {
        uint64_t nr = rnd(&s) % 170;
        if (excluded(nr))
            continue;
        uint64_t a[6];
        for (unsigned i = 0; i < 6; i++)
            a[i] = an_arg(&s);
        if (takes_struct(nr) && rnd(&s) % 4) {
            random_struct(&s, sbuf, 16);
            a[0] = (uint64_t)(uintptr_t)sbuf;
        }
        raw_call(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
        __atomic_add_fetch(&fz_calls, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&fz_progress[me], 1, __ATOMIC_RELAXED);
        /* Now and then a fresh object into the pool, so closes don't
         * empty it: the old handle is simply forgotten (the kernel keeps
         * it in the table until we die). */
        if (rnd(&s) % 64 == 0) {
            handle_t h = HANDLE_INVALID, h2 = HANDLE_INVALID;
            switch (rnd(&s) % 5) {
            case 0: jam_channel_create(&h, &h2); break;
            case 1: jam_event_create(&h); break;
            case 2: jam_port_create(&h); break;
            case 3: jam_vmo_create(4096 * (1 + rnd(&s) % 8), 0, &h); break;
            default: jam_timer_create(&h); break;
            }
            __atomic_store_n(&fz_pool[rnd(&s) % FZ_POOL], h, __ATOMIC_RELAXED);
            if (h2)
                __atomic_store_n(&fz_pool[rnd(&s) % FZ_POOL], h2, __ATOMIC_RELAXED);
        }
    }
}

/* The pool's first objects: one of each kind it can make, a child job
 * (so process_create, job_kill and the limits have something to work on:
 * the fuzzer's own job handle is not in the pool), a process in it. */
static void fill_pool(void)
{
    unsigned n = 0;
    handle_t a, b;
    for (unsigned i = 0; i < 6 && n + 2 <= FZ_POOL; i++)
        if (jam_channel_create(&a, &b) == OK) {
            fz_pool[n++] = a;
            fz_pool[n++] = b;
        }
    for (unsigned i = 0; i < 4 && n < FZ_POOL; i++)
        if (jam_event_create(&a) == OK)
            fz_pool[n++] = a;
    for (unsigned i = 0; i < 4 && n < FZ_POOL; i++)
        if (jam_port_create(&a) == OK)
            fz_pool[n++] = a;
    for (unsigned i = 0; i < 4 && n < FZ_POOL; i++)
        if (jam_vmo_create(4096 * (i + 1), i == 3 ? VMO_KEEP_PAGES : 0, &a) == OK)
            fz_pool[n++] = a;
    for (unsigned i = 0; i < 2 && n < FZ_POOL; i++)
        if (jam_timer_create(&a) == OK)
            fz_pool[n++] = a;
    handle_t job;
    if (jam_job_create(startup_handle(SR_JOB), 0, &job) == OK && n < FZ_POOL) {
        fz_pool[n++] = job;
        handle_t p, v;
        if (jam_process_create(job, "fz", 2, 0, &p, &v) == OK && n + 2 <= FZ_POOL) {
            fz_pool[n++] = p;
            fz_pool[n++] = v;
        }
    }
    for (unsigned i = n; i < FZ_POOL; i++)
        fz_pool[i] = fz_pool[i % (n ? n : 1)];
}

/* ---- fuzz-chans ---------------------------------------------------------------- */

#define FC_SLOTS 24
static handle_t fc_slot[FC_SLOTS];
static handle_t fc_port;

static handle_t fc_take(uint64_t *s)
{
    return __atomic_exchange_n(&fc_slot[rnd(s) % FC_SLOTS], HANDLE_INVALID, __ATOMIC_ACQ_REL);
}

static void fc_put(uint64_t *s, handle_t h)
{
    if (!h)
        return;
    handle_t old = __atomic_exchange_n(&fc_slot[rnd(s) % FC_SLOTS], h, __ATOMIC_ACQ_REL);
    if (old)
        jam_handle_close(old);
}

static handle_t fc_peek(uint64_t *s)
{
    return __atomic_load_n(&fc_slot[rnd(s) % FC_SLOTS], __ATOMIC_RELAXED);
}

static void chans_worker(void *arg)
{
    uint64_t s = (uint64_t)(uintptr_t)arg;
    uint8_t buf[512];
    handle_t hs[4];
    for (;;) {
        uint64_t r = rnd(&s);
        handle_t h = fc_peek(&s);   /* used without owning it: a racing close may end it */
        uint64_t soon = (uint64_t)now() + (r >> 40) % 2000000;
        switch (r % 14) {
        case 0: case 1: {
            handle_t a, b;
            if (jam_channel_create(&a, &b) == OK) {
                fc_put(&s, a);
                fc_put(&s, b);
            }
            break;
        }
        case 2: case 3: {   /* a write carrying up to 3 handles taken from the table */
            unsigned n = (r >> 8) % 4, k = 0;
            for (unsigned i = 0; i < n; i++)
                if ((hs[k] = fc_take(&s)) != HANDLE_INVALID)
                    k++;
            memcpy(buf, &r, 8);
            jam_channel_write(h, buf, 8 + (r >> 16) % 200, hs, k);   /* consumed, whatever */
            break;
        }
        case 4: {
            uint32_t nb, nh;
            handle_t got[4];
            if (jam_channel_read(&(struct channel_read_args){
                    .h = h, .rd_cap = sizeof(buf), .rd_bytes = (uint64_t)(uintptr_t)buf,
                    .rd_handles = (uint64_t)(uintptr_t)got, .rd_hcap = 4,
                    .rd_actual = (uint64_t)(uintptr_t)&nb,
                    .rd_hactual = (uint64_t)(uintptr_t)&nh }) == OK)
                for (uint32_t i = 0; i < nh && i < 4; i++)
                    fc_put(&s, got[i]);
            break;
        }
        case 5: {
            uint32_t nb, nh;
            handle_t got[4];
            handle_t mine = fc_take(&s);
            unsigned k = mine ? 1 : 0;
            hs[0] = mine;
            if (jam_channel_call(&(struct channel_call_args){
                    .h = h, .wr_len = 16, .wr_bytes = (uint64_t)(uintptr_t)buf,
                    .wr_handles = (uint64_t)(uintptr_t)hs, .wr_nhandles = k,
                    .rd_cap = sizeof(buf), .rd_bytes = (uint64_t)(uintptr_t)buf,
                    .rd_handles = (uint64_t)(uintptr_t)got, .rd_hcap = 4,
                    .rd_actual = (uint64_t)(uintptr_t)&nb,
                    .rd_hactual = (uint64_t)(uintptr_t)&nh, .deadline_ns = soon }) == OK)
                for (uint32_t i = 0; i < nh && i < 4; i++)
                    fc_put(&s, got[i]);
            break;
        }
        case 6: {   /* answer whatever came last on h, then wait for the next request */
            uint32_t nb = 0, nh = 0;
            int32_t rst;
            handle_t got[4];
            handle_t give = fc_take(&s);
            jam_channel_reply_wait(&(struct channel_reply_wait_args){
                .h = r & (1ull << 20) ? h : HANDLE_INVALID, .wr_len = r & (1ull << 20) ? 8 : 0,
                .wr_bytes = r & (1ull << 20) ? (uint64_t)(uintptr_t)buf : 0,
                .wr_handles = give && (r & (1ull << 20)) ? (uint64_t)(uintptr_t)&give : 0,
                .wr_nhandles = give && (r & (1ull << 20)) ? 1 : 0, .wait = fc_peek(&s),
                .reply_status = r & (1ull << 20) ? (uint64_t)(uintptr_t)&rst : 0,
                .rd_cap = sizeof(buf), .rd_hcap = 4, .rd_bytes = (uint64_t)(uintptr_t)buf,
                .rd_handles = (uint64_t)(uintptr_t)got, .rd_actual = (uint64_t)(uintptr_t)&nb,
                .rd_hactual = (uint64_t)(uintptr_t)&nh, .deadline_ns = soon });
            if (give && !(r & (1ull << 20)))
                fc_put(&s, give);
            for (uint32_t i = 0; i < nh && i < 4; i++)
                fc_put(&s, got[i]);
            break;
        }
        case 7:
            jam_handle_close(fc_take(&s));
            break;
        case 8: {
            handle_t d;
            if (jam_handle_duplicate(h, RIGHT_SAME, &d) == OK)
                fc_put(&s, d);
            break;
        }
        case 9:
            jam_channel_set_flags(h, CHANNEL_NO_HANDLES);
            break;
        case 10: {
            struct handle_info info;
            jam_handle_info(h, &info);
            break;
        }
        case 11:
            jam_port_bind(fc_port, h, r >> 32, SIG_READABLE | SIG_PEER_CLOSED,
                          r & 2 ? PORT_BIND_ONCE : PORT_BIND_PERSISTENT);
            break;
        case 12: {
            struct port_packet pkt;
            jam_port_wait(fc_port, soon, &pkt);
            break;
        }
        default:
            jam_object_wait_one(h, SIG_READABLE, soon, NULL);
            break;
        }
        __atomic_add_fetch(&fz_calls, 1, __ATOMIC_RELAXED);
    }
}

/* ---- the modes ------------------------------------------------------------------ */

/* fuzz-calls' main thread: every FZ_THREADS-th of a second, a parked
 * thread takes over from each fuzzing one that hasn't come back from a
 * call since the last look (blocked for good, likely), until none is
 * parked; until `end`. */
static void shepherd(uint64_t end)
{
    uint32_t seen[FZ_MAX] = { 0 };
    unsigned next = FZ_THREADS;
    while ((uint64_t)now() < end) {
        jam_sleep_until(now() + 20 * NS_PER_MS);
        for (unsigned i = 0; i < next && i < FZ_MAX; i++) {
            uint32_t p = __atomic_load_n(&fz_progress[i], __ATOMIC_RELAXED);
            bool stuck = p == seen[i] && __atomic_load_n(&fz_active[i], __ATOMIC_RELAXED) == 1;
            seen[i] = p;
            if (stuck && next < FZ_MAX) {
                __atomic_store_n(&fz_active[i], 2, __ATOMIC_RELAXED);   /* replaced */
                __atomic_store_n(&fz_active[next], 1, __ATOMIC_RELEASE);
                jam_futex_wake(&fz_active[next], 1, 0);
                next++;
            }
        }
    }
}

/* Close every handle the program was started with: the fuzzer has its
 * threads and pool now, and must never reach itself (its process,
 * address space, job). */
static void close_startup(void)
{
    for (unsigned i = startup_handle_count(); i-- > 0;) {
        uint32_t role;
        handle_t h = startup_handle_at(i, &role);
        if (h)
            jam_handle_close(h);
    }
}

static uint64_t decimal(const char *p, uint64_t dflt)
{
    if (!p || !*p)
        return dflt;
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (uint64_t)(*p - '0');
    return v;
}

static int run(int argc, char **argv, void (*worker)(void *), bool calls)
{
    uint64_t seed = decimal(argc > 2 ? argv[2] : NULL, 1);
    uint64_t ms = decimal(argc > 3 ? argv[3] : NULL, 2000);
    if (!seed)
        seed = 1;
    unsigned n = calls ? FZ_MAX : FZ_THREADS;
    handle_t th;
    for (unsigned i = 0; i < n; i++) {
        fz_args[i] = (struct fz_arg){ seed * 0x9e3779b97f4a7c15ull + i + 1, i };
        fz_active[i] = i < FZ_THREADS;
        if (thread_spawn("fz", worker, calls ? (void *)&fz_args[i] : (void *)(uintptr_t)fz_args[i].seed,
                         fz_stacks[i], sizeof(fz_stacks[i]), &th) != OK)
            return 2;
        jam_handle_close(th);
    }
    uint64_t end = (uint64_t)now() + ms * NS_PER_MS;
    if (calls) {
        close_startup();
        __atomic_store_n(&fz_go, 1, __ATOMIC_RELEASE);
        shepherd(end);
    } else {
        jam_sleep_until(end);
    }
    unsigned used = 0;
    for (unsigned i = 0; i < n; i++)
        used += fz_active[i] != 0;
    printf("utest: %s: seed %lu, %u calls in %lu ms from %u threads\n", argv[1],
           (unsigned long)seed, __atomic_load_n(&fz_calls, __ATOMIC_RELAXED), (unsigned long)ms,
           used);
    jam_process_exit(0);   /* the workers are inside their calls: the kernel ends them */
}

int fuzz_child(int argc, char **argv)
{
    if (!strcmp(argv[1], "fuzz-calls")) {
        fill_pool();
        return run(argc, argv, fuzz_worker, true);
    }
    if (!strcmp(argv[1], "fuzz-chans")) {
        if (jam_port_create(&fc_port) != OK)
            return 2;
        return run(argc, argv, chans_worker, false);
    }
    printf("utest: unknown fuzz mode \"%s\"\n", argv[1]);
    return 127;
}
