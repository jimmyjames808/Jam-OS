/* utest: threads. FPU, SSE and AVX state survives preemption (more
 * threads than CPUs, each checking its registers after being preempted),
 * many threads come and go, the FS base (thread_start's and
 * thread_set_fs_base's), futexes from ring 3, lock_take under
 * contention, a job kill takes threads that spin or never started, and a
 * job kill reaps a dead parent's orphans. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

/* ---- FPU state across preemption ---------------------------------------------- */

/* More threads than the PC has CPUs (28), so they must take turns: a
 * round that takes far longer than the fastest one was preempted (or
 * interrupted) with the registers loaded, which is the case under test. */
#define FPU_THREADS 48
#define FPU_RUN_NS  (400 * NS_PER_MS)
#define FPU_SPINS   4000000

static bool have_avx;
/* Counted by the workers (RELAXED adds), read once they have all ended. */
static uint32_t fpu_errors, fpu_rounds, fpu_preempted;

bool avx_usable(void)
{
    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    if (!(r[2] & (1u << 27)) || !(r[2] & (1u << 28)))   /* OSXSAVE, AVX */
        return false;
    return (cpu_xcr0() & 6) == 6;   /* the kernel saves SSE and AVX state */
}

/* Load 16 registers from pat, spin (preemption happens here), store them
 * to got: all in one asm block, so the compiler can't touch them between. */
void sse_round(const uint8_t *pat, uint8_t *got, uint64_t spins)
{
    __asm__ volatile(
        "movdqu 0(%0), %%xmm0\n movdqu 16(%0), %%xmm1\n movdqu 32(%0), %%xmm2\n"
        "movdqu 48(%0), %%xmm3\n movdqu 64(%0), %%xmm4\n movdqu 80(%0), %%xmm5\n"
        "movdqu 96(%0), %%xmm6\n movdqu 112(%0), %%xmm7\n movdqu 128(%0), %%xmm8\n"
        "movdqu 144(%0), %%xmm9\n movdqu 160(%0), %%xmm10\n movdqu 176(%0), %%xmm11\n"
        "movdqu 192(%0), %%xmm12\n movdqu 208(%0), %%xmm13\n movdqu 224(%0), %%xmm14\n"
        "movdqu 240(%0), %%xmm15\n"
        "1: dec %2\n jnz 1b\n"
        "movdqu %%xmm0, 0(%1)\n movdqu %%xmm1, 16(%1)\n movdqu %%xmm2, 32(%1)\n"
        "movdqu %%xmm3, 48(%1)\n movdqu %%xmm4, 64(%1)\n movdqu %%xmm5, 80(%1)\n"
        "movdqu %%xmm6, 96(%1)\n movdqu %%xmm7, 112(%1)\n movdqu %%xmm8, 128(%1)\n"
        "movdqu %%xmm9, 144(%1)\n movdqu %%xmm10, 160(%1)\n movdqu %%xmm11, 176(%1)\n"
        "movdqu %%xmm12, 192(%1)\n movdqu %%xmm13, 208(%1)\n movdqu %%xmm14, 224(%1)\n"
        "movdqu %%xmm15, 240(%1)\n"
        : "+r"(pat), "+r"(got), "+r"(spins)
        :
        : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

/* The same with the full 256-bit ymm registers (upper halves are AVX
 * state, saved only by XSAVE). */
__attribute__((target("avx"))) void avx_round(const uint8_t *pat, uint8_t *got, uint64_t spins)
{
    __asm__ volatile(
        "vmovdqu 0(%0), %%ymm0\n vmovdqu 32(%0), %%ymm1\n vmovdqu 64(%0), %%ymm2\n"
        "vmovdqu 96(%0), %%ymm3\n vmovdqu 128(%0), %%ymm4\n vmovdqu 160(%0), %%ymm5\n"
        "vmovdqu 192(%0), %%ymm6\n vmovdqu 224(%0), %%ymm7\n vmovdqu 256(%0), %%ymm8\n"
        "vmovdqu 288(%0), %%ymm9\n vmovdqu 320(%0), %%ymm10\n vmovdqu 352(%0), %%ymm11\n"
        "vmovdqu 384(%0), %%ymm12\n vmovdqu 416(%0), %%ymm13\n vmovdqu 448(%0), %%ymm14\n"
        "vmovdqu 480(%0), %%ymm15\n"
        "1: dec %2\n jnz 1b\n"
        "vmovdqu %%ymm0, 0(%1)\n vmovdqu %%ymm1, 32(%1)\n vmovdqu %%ymm2, 64(%1)\n"
        "vmovdqu %%ymm3, 96(%1)\n vmovdqu %%ymm4, 128(%1)\n vmovdqu %%ymm5, 160(%1)\n"
        "vmovdqu %%ymm6, 192(%1)\n vmovdqu %%ymm7, 224(%1)\n vmovdqu %%ymm8, 256(%1)\n"
        "vmovdqu %%ymm9, 288(%1)\n vmovdqu %%ymm10, 320(%1)\n vmovdqu %%ymm11, 352(%1)\n"
        "vmovdqu %%ymm12, 384(%1)\n vmovdqu %%ymm13, 416(%1)\n vmovdqu %%ymm14, 448(%1)\n"
        "vmovdqu %%ymm15, 480(%1)\n vzeroupper\n"
        : "+r"(pat), "+r"(got), "+r"(spins)
        :
        : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static void fpu_worker(void *arg)
{
    uint64_t seed = (uint64_t)(uintptr_t)arg;
    _Alignas(32) uint8_t pat[512], got[512];
    for (unsigned k = 0; k < sizeof(pat); k++)
        pat[k] = (uint8_t)(seed * 131 + k * 7 + (k >> 5));
    uint64_t end = now() + FPU_RUN_NS, fastest = UINT64_MAX;
    uint64_t took[64];
    unsigned rounds = 0;
    while (now() < end) {
        size_t n = have_avx && (seed & 1) ? 512 : 256;   /* a mix of SSE and AVX threads */
        memset(got, 0, n);
        uint64_t t0 = cpu_tsc();
        if (n == 512)
            avx_round(pat, got, FPU_SPINS);
        else
            sse_round(pat, got, FPU_SPINS);
        uint64_t t = cpu_tsc() - t0;
        if (t < fastest)
            fastest = t;
        took[rounds++ % 64] = t;
        if (memcmp(pat, got, n))
            __atomic_add_fetch(&fpu_errors, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&fpu_rounds, 1, __ATOMIC_RELAXED);
    }
    for (unsigned i = 0; i < rounds && i < 64; i++)
        if (took[i] > 3 * fastest)
            __atomic_add_fetch(&fpu_preempted, 1, __ATOMIC_RELAXED);
}

bool wait_threads(const handle_t *th, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        signals_t seen;
        CHECK_ST(jam_object_wait_one(th[i], SIG_TERMINATED, now() + 20 * NS_PER_S, &seen), OK);
        CHECK_ST(jam_handle_close(th[i]), OK);
    }
    return true;
}

bool t_fpu_state_survives_preemption(void)
{
    static uint8_t stacks[FPU_THREADS][16384] __attribute__((aligned(64)));
    have_avx = avx_usable();
    handle_t th[FPU_THREADS];
    for (unsigned i = 0; i < FPU_THREADS; i++)
        CHECK_ST(thread_spawn("fpu", fpu_worker, (void *)(uintptr_t)(i + 1), stacks[i],
                              sizeof(stacks[i]), &th[i]),
                 OK);
    if (!wait_threads(th, FPU_THREADS))
        return false;
    printf("utest: fpu: %u threads, %u rounds (%u preempted mid-round), %s, %u mismatches\n",
           FPU_THREADS, __atomic_load_n(&fpu_rounds, __ATOMIC_RELAXED),
           __atomic_load_n(&fpu_preempted, __ATOMIC_RELAXED),
           have_avx ? "SSE + AVX" : "SSE only", __atomic_load_n(&fpu_errors, __ATOMIC_RELAXED));
    CHECK(__atomic_load_n(&fpu_rounds, __ATOMIC_RELAXED) >= FPU_THREADS);
    CHECK(__atomic_load_n(&fpu_preempted, __ATOMIC_RELAXED) > 0);   /* else it proved nothing */
    CHECK_EQ(__atomic_load_n(&fpu_errors, __ATOMIC_RELAXED), 0);
    return true;
}

/* ---- lock_take ---------------------------------------------------------------- */

#define LOCK_THREADS 6
#define LOCK_ROUNDS  20000
static bool     lock_l;
static uint64_t lock_count;   /* plain reads and writes, only under lock_l */

static void lock_worker(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < LOCK_ROUNDS; i++) {
        lock_take(&lock_l);
        uint64_t v = lock_count;
        if (i % 64 == 0)
            jam_nanosleep(now() + 50 * NS_PER_US);   /* a holder off the CPU: the others wait */
        lock_count = v + 1;
        lock_give(&lock_l);
    }
}

bool t_lock_take(void)
{
    static uint8_t stacks[LOCK_THREADS][8192] __attribute__((aligned(64)));
    handle_t th[LOCK_THREADS];
    lock_count = 0;
    for (unsigned i = 0; i < LOCK_THREADS; i++)
        CHECK_ST(thread_spawn("lock", lock_worker, NULL, stacks[i], sizeof(stacks[i]), &th[i]),
                 OK);
    if (!wait_threads(th, LOCK_THREADS))
        return false;
    lock_take(&lock_l);
    uint64_t n = lock_count;
    lock_give(&lock_l);
    CHECK_EQ(n, (uint64_t)LOCK_THREADS * LOCK_ROUNDS);   /* no increment lost */
    return true;
}

/* ---- the FS base (thread-local storage) -------------------------------------- */

#define USER_TOP_ADDR 0x7fffffffe000ull   /* the user range's top (kernel/include/jam/uentry.h) */
#define FS_READS      200

static uint64_t fs_words[4] __attribute__((aligned(64)));
static uint32_t fs_reads, fs_wrong;   /* the thread's; read once it has ended */

static uint64_t fs_word0(void)
{
    uint64_t v;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v) :: "memory");
    return v;
}

/* Started by jam_thread_start itself (rdi = the word it must see). */
static _Noreturn void fs_thread(uint64_t want)
{
    for (unsigned i = 0; i < FS_READS; i++) {
        if (fs_word0() != want)
            fs_wrong++;
        fs_reads++;
        if (i % 16 == 0)
            jam_nanosleep(now() + 20 * NS_PER_US);   /* off the CPU, maybe onto another */
    }
    jam_thread_exit();
}

/* thread_start's FS base is the thread's from its first instruction, and
 * stays across sleeps; thread_set_fs_base changes the caller's; a base or
 * entry past the user range is refused with nothing started. */
bool t_thread_fs_base(void)
{
    static uint8_t stack[8192] __attribute__((aligned(64)));
    fs_words[1] = 0x7e57f5b1ull;
    fs_words[2] = 0x5e7f5b2ull;
    fs_reads = fs_wrong = 0;
    handle_t t;
    CHECK_ST(jam_thread_create(startup_handle(SR_SELF_PROCESS), "fs", 2, 0, &t), OK);
    uint64_t top = (((uint64_t)(uintptr_t)stack + sizeof(stack)) & ~15ull) - 8;
    uint64_t entry = (uint64_t)(uintptr_t)fs_thread, base = (uint64_t)(uintptr_t)&fs_words[1];
    CHECK_ST(jam_thread_start(t, entry, top, fs_words[1], 0, USER_TOP_ADDR), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_start(t, entry, top, fs_words[1], 0, 1ull << 63), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_start(t, USER_TOP_ADDR, top, fs_words[1], 0, base), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_start(t, entry, top, fs_words[1], 0, base), OK);
    if (!wait_threads(&t, 1))
        return false;
    CHECK_EQ(fs_reads, FS_READS);
    CHECK_EQ(fs_wrong, 0);

    CHECK_ST(jam_thread_set_fs_base((uint64_t)(uintptr_t)&fs_words[2]), OK);
    CHECK_EQ(fs_word0(), fs_words[2]);
    jam_nanosleep(now() + 100 * NS_PER_US);
    CHECK_EQ(fs_word0(), fs_words[2]);
    CHECK_ST(jam_thread_set_fs_base(USER_TOP_ADDR), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_set_fs_base(0xffff800000000000ull), ERR_INVALID_ARGS);
    CHECK_EQ(fs_word0(), fs_words[2]);   /* unchanged */
    CHECK_ST(jam_thread_set_fs_base(0), OK);   /* as libos runs */
    return true;
}

/* ---- futexes ------------------------------------------------------------------ */

static uint32_t fx_word, fx_seen;   /* the waiter's word; what it saw (RELAXED) */
static status_t fx_status;

static void fx_waiter(void *arg)
{
    (void)arg;
    /* Wait while the word is 0; the main thread stores 1, then wakes. */
    while (!__atomic_load_n(&fx_word, __ATOMIC_ACQUIRE)) {
        status_t st = jam_futex_wait(&fx_word, 0, now() + 10 * NS_PER_S, 0);
        if (st != OK && st != ERR_BAD_STATE) {
            fx_status = st;
            return;
        }
    }
    __atomic_store_n(&fx_seen, 1, __ATOMIC_RELEASE);
    fx_status = OK;
}

/* The system calls from ring 3: a changed value, a deadline, bad
 * addresses and flags, a wake with nobody waiting, and a thread woken. */
bool t_futex_calls(void)
{
    static uint8_t stack[8192] __attribute__((aligned(64)));
    static uint32_t words[4];
    words[0] = 5;
    CHECK_ST(jam_futex_wait(&words[0], 4, DEADLINE_NEVER, 0), ERR_BAD_STATE);
    uint64_t t0 = now();
    CHECK_ST(jam_futex_wait(&words[0], 5, t0 + 2 * NS_PER_MS, 0), ERR_TIMED_OUT);
    CHECK(now() - t0 >= 2 * NS_PER_MS);
    CHECK_ST(jam_futex_wait(&words[0], 5, 0, 0), ERR_TIMED_OUT);
    CHECK_ST(jam_futex_wait((const uint32_t *)((uint8_t *)words + 1), 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(jam_futex_wait((const uint32_t *)8, 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(jam_futex_wait((const uint32_t *)0xffff800000000000ull, 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(jam_futex_wait(&words[0], 5, 0, 1), ERR_INVALID_ARGS);
    CHECK_EQ(jam_futex_wake(&words[0], 1, 0), 0);
    CHECK_EQ(jam_futex_wake(&words[0], 1, 2), ERR_INVALID_ARGS);
    CHECK_EQ(jam_futex_requeue(&words[0], 1, &words[1], 1, 0), 0);
    CHECK_EQ(jam_futex_requeue(&words[0], 1, (const uint32_t *)3, 1, 0), ERR_INVALID_ARGS);

    fx_word = fx_seen = 0;
    fx_status = ERR_IO;
    handle_t t;
    CHECK_ST(thread_spawn("futex", fx_waiter, NULL, stack, sizeof(stack), &t), OK);
    jam_nanosleep(now() + 2 * NS_PER_MS);   /* most likely asleep by now (either way works) */
    __atomic_store_n(&fx_word, 1, __ATOMIC_RELEASE);
    CHECK(jam_futex_wake(&fx_word, UINT32_MAX, 0) >= 0);
    if (!wait_threads(&t, 1))
        return false;
    CHECK_ST(fx_status, OK);
    CHECK_EQ(fx_seen, 1);
    return true;
}

/* ---- many threads ------------------------------------------------------------ */

#define MANY 64
static uint32_t many_count;   /* the workers' RELAXED adds; read once they have ended */

static void many_worker(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&many_count, 1, __ATOMIC_RELAXED);
}

bool t_many_threads(void)
{
    static uint8_t stacks[MANY][8192] __attribute__((aligned(64)));
    handle_t th[MANY];
    for (unsigned round = 0; round < 3; round++) {
        __atomic_store_n(&many_count, 0, __ATOMIC_RELAXED);
        for (unsigned i = 0; i < MANY; i++)
            CHECK_ST(thread_spawn("many", many_worker, NULL, stacks[i], sizeof(stacks[i]),
                                  &th[i]),
                     OK);
        if (!wait_threads(th, MANY))
            return false;
        CHECK_EQ(__atomic_load_n(&many_count, __ATOMIC_RELAXED), MANY);
        struct job_info ji;
        CHECK_ST(info_of(own_job(), &ji), OK);
        CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 1);   /* just us again */
    }
    /* A process outlives its first thread. */
    struct process_info info;
    if (!run_child("main-exits", 0, 0, &info))
        return false;
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 11);
    return true;
}

bool t_kill_spinning_and_unstarted(void)
{
    handle_t job, proc, p2, v2;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("spin", NULL, job, HANDLE_INVALID, &proc), OK);
    jam_nanosleep(now() + 30 * NS_PER_MS);
    CHECK_ST(jam_process_kill(proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_process_kill(proc), OK);   /* killing the dead is a no-op */
    CHECK_ST(jam_handle_close(proc), OK);
    /* A process that never started: killing it tears it down at once. */
    CHECK_ST(jam_process_create(job, "never", 5, 0, &p2, &v2), OK);
    CHECK_ST(jam_process_kill(p2), OK);
    CHECK_ST(spawn_wait(p2, NS_PER_S, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_handle_close(v2), OK);
    CHECK_ST(jam_handle_close(p2), OK);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* A child that leaves a grandchild behind (spinning, in a job of its own,
 * with no handle to it anywhere): job_kill on the child's job reaps it,
 * returns only once it is dead, and the job is empty and closed to new
 * processes and jobs afterwards. */
bool t_job_kill_reaps_orphans(void)
{
    handle_t job, proc, p2, v2, j2;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("orphan", NULL, job, HANDLE_INVALID, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 5 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 1);   /* the orphan, still spinning */
    CHECK_ST(jam_job_kill(own_job()), ERR_ACCESS_DENIED);   /* no RIGHT_MANAGE on our own */
    CHECK_ST(jam_job_kill(job), OK);
    CHECK_ST(info_of(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_process_create(job, "late", 4, 0, &p2, &v2), ERR_BAD_STATE);
    CHECK_ST(jam_job_create(job, 0, &j2), ERR_BAD_STATE);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}
