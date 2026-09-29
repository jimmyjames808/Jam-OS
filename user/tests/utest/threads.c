/* utest: threads. FPU, SSE and AVX state survives preemption (more
 * threads than CPUs, each checking its registers after being preempted),
 * many threads come and go, a job kill takes threads that spin or never
 * started, and a job kill reaps a dead parent's orphans. */
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

static bool avx_usable(void)
{
    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    if (!(r[2] & (1u << 27)) || !(r[2] & (1u << 28)))   /* OSXSAVE, AVX */
        return false;
    return (cpu_xcr0() & 6) == 6;   /* the kernel saves SSE and AVX state */
}

/* Load 16 registers from pat, spin (preemption happens here), store them
 * to got: all in one asm block, so the compiler can't touch them between. */
static void sse_round(const uint8_t *pat, uint8_t *got, uint64_t spins)
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
__attribute__((target("avx"))) static void avx_round(const uint8_t *pat, uint8_t *got,
                                                     uint64_t spins)
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

bool wait_threads(handle_t *th, unsigned n)
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
