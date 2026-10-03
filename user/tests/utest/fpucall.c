/* utest: the system call rule for the FPU (ARCHITECTURE.md, "The system
 * call ABI"; kernel/arch/x86_64/fpu.c). A thread that blocks inside a
 * system call keeps its MXCSR and x87 control word and gets its vector
 * and x87 registers back zeroed, never another thread's values; a thread
 * switched out of ring 3 (an interrupt, or a page fault) keeps all of it.
 * Meanwhile more threads than CPUs keep every CPU busy with registers full
 * of their own patterns, so a leak would show.
 *
 * Each round is one asm block (the compiler can't touch the registers in
 * between): set our own MXCSR and control word, push 1.0 on the x87
 * stack, load the vector registers from a pattern, then either block in
 * nanosleep (the `syscall` instruction directly) or fault in fresh pages
 * and spin, then FXSAVE (and, with AVX, the ymm upper halves) to look at
 * what came back. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <stddef.h>
#include <check.h>
#include <os.h>
#include "utest.h"

#define OWN_FCW     0x027f   /* 53-bit precision, exceptions masked: not the default 0x037f */
#define OWN_MXCSR   0x7f80   /* round toward zero, exceptions masked: not the default 0x1f80 */
#define DEF_MXCSR   0x1f80

/* The FXSAVE image (Intel SDM vol. 1, 10.5.1). */
#define FX_FCW      0
#define FX_FSW      2
#define FX_FTW      4        /* abridged: one bit per register, 1 = in use */
#define FX_MXCSR    24
#define FX_ST       32       /* ST0-ST7, 16 bytes apart (10 used) */
#define FX_XMM      160      /* XMM0-XMM15 */
#define FX_END      416

#define RIVALS      40       /* at most; without the CPU count, more than the PC's 28 */
#define CALL_ROUNDS 16
#define RUN_NS      (300 * NS_PER_MS)
#define PAGES       16       /* fresh pages faulted in per ring-3 round */
#define SLICES      32       /* rounds before the pages are all faulted in */
#define SPINS       16384    /* per page */

/* One round, read and written by the asm through a pointer. */
struct round {
    const uint8_t *pat;      /* xmm0-15 from 256 bytes, or ymm0-15 from 512 */
    uint8_t       *fx;       /* FXSAVE image afterwards (512 bytes, 16-aligned) */
    uint8_t       *hi;       /* ymm0-15 upper halves afterwards (256 bytes; AVX only) */
    uint64_t       deadline; /* != 0: block in nanosleep until then */
    uint8_t       *pages;    /* else: fault in these PAGES pages, spinning on each */
    int64_t        st;       /* nanosleep's result */
    uint32_t       mxcsr;    /* OWN_MXCSR, then DEF_MXCSR to leave with */
    uint32_t       def_mxcsr;
    uint16_t       fcw;      /* OWN_FCW */
};

/* The middle of a round, shared by both shapes; %0 is the struct. */
#define ROUND_MIDDLE                                                                  \
    "mov %c[dl](%0), %%rdi\n test %%rdi, %%rdi\n jz 2f\n"                             \
    "mov %[nr], %%eax\n syscall\n mov %%rax, %c[st](%0)\n jmp 3f\n"                   \
    "2: mov %c[pg](%0), %%rdx\n mov %[np], %%esi\n"                                   \
    "4: movb $1, (%%rdx)\n add $4096, %%rdx\n mov %[sp], %%ecx\n"                     \
    "5: dec %%ecx\n jnz 5b\n dec %%esi\n jnz 4b\n"                                    \
    "3: mov %c[fx](%0), %%rax\n fxsave64 (%%rax)\n"

#define ROUND_OPERANDS                                                                \
    : /* outputs: through r */                                                        \
    : "r"(r), [pat] "i"(offsetof(struct round, pat)), [fx] "i"(offsetof(struct round, fx)), \
      [hi] "i"(offsetof(struct round, hi)), [dl] "i"(offsetof(struct round, deadline)),   \
      [pg] "i"(offsetof(struct round, pages)), [st] "i"(offsetof(struct round, st)),      \
      [mx] "i"(offsetof(struct round, mxcsr)), [dmx] "i"(offsetof(struct round, def_mxcsr)), \
      [cw] "i"(offsetof(struct round, fcw)), [nr] "i"(SYS_nanosleep), [np] "i"(PAGES),     \
      [sp] "i"(SPINS)                                                                  \
    : "rax", "rcx", "rdx", "rsi", "rdi", "r11", "memory", "cc", "xmm0", "xmm1", "xmm2",  \
      "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",  \
      "xmm13", "xmm14", "xmm15"

static void round_sse(struct round *r)
{
    __asm__ volatile(
        "ldmxcsr %c[mx](%0)\n fldcw %c[cw](%0)\n fld1\n"
        "mov %c[pat](%0), %%rax\n"
        "movdqu 0(%%rax), %%xmm0\n movdqu 16(%%rax), %%xmm1\n movdqu 32(%%rax), %%xmm2\n"
        "movdqu 48(%%rax), %%xmm3\n movdqu 64(%%rax), %%xmm4\n movdqu 80(%%rax), %%xmm5\n"
        "movdqu 96(%%rax), %%xmm6\n movdqu 112(%%rax), %%xmm7\n movdqu 128(%%rax), %%xmm8\n"
        "movdqu 144(%%rax), %%xmm9\n movdqu 160(%%rax), %%xmm10\n movdqu 176(%%rax), %%xmm11\n"
        "movdqu 192(%%rax), %%xmm12\n movdqu 208(%%rax), %%xmm13\n movdqu 224(%%rax), %%xmm14\n"
        "movdqu 240(%%rax), %%xmm15\n"
        ROUND_MIDDLE
        "fninit\n ldmxcsr %c[dmx](%0)\n"
        ROUND_OPERANDS);
}

__attribute__((target("avx"))) static void round_avx(struct round *r)
{
    __asm__ volatile(
        "ldmxcsr %c[mx](%0)\n fldcw %c[cw](%0)\n fld1\n"
        "mov %c[pat](%0), %%rax\n"
        "vmovdqu 0(%%rax), %%ymm0\n vmovdqu 32(%%rax), %%ymm1\n vmovdqu 64(%%rax), %%ymm2\n"
        "vmovdqu 96(%%rax), %%ymm3\n vmovdqu 128(%%rax), %%ymm4\n vmovdqu 160(%%rax), %%ymm5\n"
        "vmovdqu 192(%%rax), %%ymm6\n vmovdqu 224(%%rax), %%ymm7\n vmovdqu 256(%%rax), %%ymm8\n"
        "vmovdqu 288(%%rax), %%ymm9\n vmovdqu 320(%%rax), %%ymm10\n vmovdqu 352(%%rax), %%ymm11\n"
        "vmovdqu 384(%%rax), %%ymm12\n vmovdqu 416(%%rax), %%ymm13\n vmovdqu 448(%%rax), %%ymm14\n"
        "vmovdqu 480(%%rax), %%ymm15\n"
        ROUND_MIDDLE
        "mov %c[hi](%0), %%rax\n"
        "vextractf128 $1, %%ymm0, 0(%%rax)\n vextractf128 $1, %%ymm1, 16(%%rax)\n"
        "vextractf128 $1, %%ymm2, 32(%%rax)\n vextractf128 $1, %%ymm3, 48(%%rax)\n"
        "vextractf128 $1, %%ymm4, 64(%%rax)\n vextractf128 $1, %%ymm5, 80(%%rax)\n"
        "vextractf128 $1, %%ymm6, 96(%%rax)\n vextractf128 $1, %%ymm7, 112(%%rax)\n"
        "vextractf128 $1, %%ymm8, 128(%%rax)\n vextractf128 $1, %%ymm9, 144(%%rax)\n"
        "vextractf128 $1, %%ymm10, 160(%%rax)\n vextractf128 $1, %%ymm11, 176(%%rax)\n"
        "vextractf128 $1, %%ymm12, 192(%%rax)\n vextractf128 $1, %%ymm13, 208(%%rax)\n"
        "vextractf128 $1, %%ymm14, 224(%%rax)\n vextractf128 $1, %%ymm15, 240(%%rax)\n"
        "vzeroupper\n fninit\n ldmxcsr %c[dmx](%0)\n"
        ROUND_OPERANDS);
}

/* ---- rivals: every CPU busy with registers full of other values ----------- */

static bool rivals_stop;   /* RELEASE store by the test, ACQUIRE loads by the rivals */
static bool have_avx;

static void fill(uint8_t *pat, size_t n, uint32_t seed)
{
    for (size_t k = 0; k < n; k++)
        pat[k] = (uint8_t)(seed * 97 + k * 13 + 1);   /* never all zero */
}

static void rival(void *arg)
{
    _Alignas(32) uint8_t pat[512], got[512];
    fill(pat, sizeof(pat), (uint32_t)(uintptr_t)arg);
    while (!__atomic_load_n(&rivals_stop, __ATOMIC_ACQUIRE)) {
        if (have_avx)
            avx_round(pat, got, 20000);
        else
            sse_round(pat, got, 20000);
    }
}

static handle_t rival_th[RIVALS];
static unsigned nrivals;

/* Two more rivals than CPUs: every CPU busy, and the test thread still
 * gets a fair share of one (so its slice runs out mid-round). */
static unsigned rivals_wanted(void)
{
    handle_t h;
    struct sys_info si;
    if (jam_handle_duplicate(startup_handle(SR_RESOURCE), RIGHT_ROOT_SYSINFO, &h) != OK)
        return RIVALS;
    status_t st = jam_sys_info(h, &si);
    jam_handle_close(h);
    return st == OK && si.cpu_count + 2 < RIVALS ? si.cpu_count + 2 : RIVALS;
}

static bool rivals_start(void)
{
    static uint8_t stacks[RIVALS][8192] __attribute__((aligned(64)));
    have_avx = avx_usable();
    nrivals = rivals_wanted();
    __atomic_store_n(&rivals_stop, false, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < nrivals; i++)
        CHECK_ST(thread_spawn("fpu rival", rival, (void *)(uintptr_t)(100 + i), stacks[i],
                              sizeof(stacks[i]), &rival_th[i]),
                 OK);
    return true;
}

static bool rivals_end(void)
{
    __atomic_store_n(&rivals_stop, true, __ATOMIC_RELEASE);
    return wait_threads(rival_th, nrivals);
}

/* ---- what came back ----------------------------------------------------------- */

enum came_back { ZEROED, KEPT, OTHER };

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* 1.0 as an 80-bit x87 value: mantissa 1 << 63, exponent 0x3fff. */
static bool st_is_one(const uint8_t *st)
{
    static const uint8_t one[10] = { 0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f };
    return !memcmp(st, one, sizeof(one));
}

/* The vector and x87 registers after a round: all zeroed and empty, all
 * as we left them, or anything else (a leak, or a lost save). */
static enum came_back classify(const struct round *r)
{
    const uint8_t *fx = r->fx;
    bool zero = fx[FX_FTW] == 0 && fx[FX_FSW] == 0 && fx[FX_FSW + 1] == 0;
    for (unsigned i = 0; i < 8; i++)
        zero = zero && all_zero(fx + FX_ST + 16 * i, 10);
    zero = zero && all_zero(fx + FX_XMM, FX_END - FX_XMM) && (!have_avx || all_zero(r->hi, 256));
    if (zero)
        return ZEROED;
    /* One value pushed: TOP is 7, so physical register 7 is the one in use. */
    bool kept = fx[FX_FTW] == 0x80 && st_is_one(fx + FX_ST);
    for (unsigned i = 0; i < 16 && kept; i++) {
        size_t at = have_avx ? 32 * i : 16 * i;   /* ymm i's low half, or xmm i */
        kept = !memcmp(fx + FX_XMM + 16 * i, r->pat + at, 16) &&
               (!have_avx || !memcmp(r->hi + 16 * i, r->pat + at + 16, 16));
    }
    return kept ? KEPT : OTHER;
}

static bool control_words_kept(const struct round *r)
{
    uint16_t fcw;
    uint32_t mxcsr;
    memcpy(&fcw, r->fx + FX_FCW, sizeof(fcw));
    memcpy(&mxcsr, r->fx + FX_MXCSR, sizeof(mxcsr));
    if (fcw != OWN_FCW || mxcsr != OWN_MXCSR)
        FAIL("control words came back as fcw %x mxcsr %x (set %x %x)", fcw, mxcsr, OWN_FCW,
             OWN_MXCSR);
    return true;
}

static void round_run(struct round *r)
{
    if (have_avx)
        round_avx(r);
    else
        round_sse(r);
}

/* ---- the tests ------------------------------------------------------------------ */

/* Block in a system call (nanosleep straight through `syscall`): MXCSR and
 * the control word survive, the rest comes back zeroed. "kept" is allowed
 * for a round that didn't block (preempted past its deadline before the
 * call) and for a boot with the rule off ("nofpucall"); never anything
 * else. */
bool t_fpu_call_keeps_control_words(void)
{
    _Alignas(32) static uint8_t pat[512], fx[512], hi[256];
    fill(pat, sizeof(pat), 7);
    if (!rivals_start())
        return false;
    unsigned zeroed = 0, kept = 0;
    bool ok = true;
    for (unsigned i = 0; i < CALL_ROUNDS && ok; i++) {
        struct round r = { .pat = pat, .fx = fx, .hi = hi, .deadline = now() + 3 * NS_PER_MS,
                           .mxcsr = OWN_MXCSR, .def_mxcsr = DEF_MXCSR, .fcw = OWN_FCW };
        round_run(&r);
        enum came_back c = classify(&r);
        ok = r.st == OK && c != OTHER && control_words_kept(&r);
        if (r.st != OK || c == OTHER)
            printf("utest: fpu call round %u: nanosleep %s, xmm0 %x %x, ftw %x\n", i,
                   status_str((status_t)r.st), fx[FX_XMM], fx[FX_XMM + 1], fx[FX_FTW]);
        zeroed += c == ZEROED;
        kept += c == KEPT;
    }
    if (!rivals_end() || !ok)
        return false;
    printf("utest: fpu call: %u rounds blocked in a call, %u came back zeroed, %u kept, %s\n",
           CALL_ROUNDS, zeroed, kept, have_avx ? "SSE + AVX" : "SSE only");
    return true;
}

/* Switched out of ring 3 by an interrupt or a page fault (fresh pages of a
 * VMO, faulted in one by one): everything is kept, the control words and
 * the x87 stack too. A round far slower than the fastest was preempted. */
bool t_fpu_ring3_switch_keeps_all(void)
{
    _Alignas(32) static uint8_t pat[512], fx[512], hi[256];
    fill(pat, sizeof(pat), 9);
    handle_t vmo, vmar = startup_handle(SR_SELF_VMAR);
    uint64_t len = (uint64_t)SLICES * PAGES * 4096, addr = 0;
    CHECK_ST(jam_vmo_create(len, 0, HANDLE_INVALID, &vmo), OK);
    CHECK_ST(jam_vmar_map(vmar, vmo, 0, len, VMAR_READ | VMAR_WRITE, &addr), OK);
    if (!rivals_start())
        return false;
    uint64_t end = now() + RUN_NS, fastest = UINT64_MAX;
    unsigned rounds = 0, preempted = 0;
    bool ok = true;
    while (ok && now() < end) {
        struct round r = {
            .pat = pat, .fx = fx, .hi = hi,
            .pages = (uint8_t *)(uintptr_t)(addr + (uint64_t)(rounds % SLICES) * PAGES * 4096),
            .mxcsr = OWN_MXCSR, .def_mxcsr = DEF_MXCSR, .fcw = OWN_FCW,
        };
        uint64_t t0 = cpu_tsc();
        round_run(&r);
        uint64_t took = cpu_tsc() - t0;
        preempted += rounds > 0 && took > 3 * fastest;   /* against the fastest so far */
        if (took < fastest)
            fastest = took;
        enum came_back c = classify(&r);
        ok = c == KEPT && control_words_kept(&r);
        if (c != KEPT)
            printf("utest: fpu ring-3 round %u: registers came back %s\n", rounds,
                   c == ZEROED ? "zeroed" : "with other values");
        rounds++;
    }
    bool ended = rivals_end();
    CHECK_ST(jam_vmar_unmap(vmar, addr, len), OK);
    CHECK_ST(jam_handle_close(vmo), OK);
    if (!ended || !ok)
        return false;
    printf("utest: fpu ring 3: %u rounds (%u preempted mid-round), %u rivals, %s, all kept\n",
           rounds, preempted, nrivals, have_avx ? "SSE + AVX" : "SSE only");
    CHECK(preempted > 0);   /* else it proved nothing */
    return true;
}
