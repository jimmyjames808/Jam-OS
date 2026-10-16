/* User FPU/SSE/AVX state. The kernel is built without SSE and never
 * touches these registers, so the only state to manage is the user
 * threads': each has an XSAVE area (FXSAVE on CPUs without XSAVE), saved
 * and restored eagerly by arch_thread_switch whenever a thread that has
 * one switches out or in. No lazy #NM tricks: CR0.TS stays clear.
 *
 * Two optimisations (switch fpu_opt, boot "nofpuopt"), both as Linux does
 * them:
 *   - XSAVEOPT instead of XSAVE where the CPU has it: it skips components
 *     not modified since the last XRSTOR from the same area on this CPU
 *     (and ones in their initial state). Every save follows a restore of
 *     the same thread's area on this CPU (the thread ran in between), or a
 *     skipped restore (below) whose registers came from that same XRSTOR,
 *     so the tracking always refers to the right area; components it skips
 *     hold exactly what the area already has.
 *   - Skip the restore when this CPU's registers still hold the incoming
 *     thread's state: it was the last thread restored here (fpu_owner) and
 *     it has not been restored on another CPU since (t->fpu_cpu == this
 *     CPU). A user thread that blocks and wakes on the same CPU with only
 *     kernel threads (idle) in between keeps its registers. Anything else
 *     that loads the registers must forget the owner: fpu_clobbered()
 *     (only the benchmark does). A new area starts with fpu_cpu = none, so
 *     a thread struct reused at a freed owner's address can't match.
 *
 * The system call rule (switch fpu_call, boot "nofpucall"). A system call
 * is a function call: the wrappers are out-of-line functions, and under
 * the x86-64 C calling convention every vector register and the x87 stack
 * are the caller's to save across a call; only MXCSR and the x87 control
 * word must survive it. So a thread switched out while it is inside a
 * system call (blocked, or preempted while the kernel ran the call for
 * it: t->in_syscall) has no live vector registers, and fpu_save_called
 * keeps just those two words: it writes them into the area, which it
 * turns into a clean state (XSTATE_BV says "initial" for every component,
 * except x87 when its control word isn't the default, and then the x87
 * part is empty), and forgets that the registers stand for the area. The
 * switch back in is the usual XRSTOR, of a state that is almost all
 * "initial", so the thread returns from its call with zeroed vector and
 * x87 registers and its own MXCSR and control word: never another
 * thread's values, nor stale ones of its own. A thread switched out of
 * ring 3 by an interrupt or an exception (in_syscall is clear) is saved
 * in full, as above.
 *
 * Components that must never be dropped: PKRU (user protection keys) is
 * per-thread state that says what memory the thread may touch, not a
 * scratch register, and CET's user state (shadow stacks) is the
 * thread's return addresses. Neither is enabled here (XCR0 is x87, SSE
 * and AVX at most; CET's state would be a supervisor component that only
 * XSAVES handles, which this file doesn't use). The rule applies only
 * while XCR0 holds nothing but x87, SSE and AVX (call_drop_ok), so turning
 * either on falls back to the full save until fpu_save_called keeps it. */
#include <jam/abi.h>
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pathstat.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/uentry.h>
#include <jam/x86.h>

#define XCR0_X87 (1ull << 0)
#define XCR0_SSE (1ull << 1)
#define XCR0_AVX (1ull << 2)

#define XCR0_DROPPABLE (XCR0_X87 | XCR0_SSE | XCR0_AVX)   /* what a system call may clobber */

/* The legacy (FXSAVE) area, Intel SDM vol. 1, 10.5.1: FCW at 0, MXCSR at
 * 24, the x87 environment and ST0-ST7 below 160, XMM0-XMM15 below 416.
 * The XSAVE header follows at 512 (SDM vol. 1, 13.4.2). */
#define FXSAVE_SIZE   512
#define FSW_OFF       2
#define FOP_OFF       6        /* FOP, then FIP (8) and FDP (16): the last x87 instruction */
#define MXCSR_OFF     24
#define MXCSR_MASK_OFF 28
#define X87_REGS      32       /* ST0-ST7 */
#define X87_END       160
#define XMM_END       416
#define XSTATE_BV_OFF 512
#define XSAVE_HDR_END 576      /* XCOMP_BV and 48 reserved bytes follow XSTATE_BV */
#define MXCSR_INIT    0x1f80   /* all SIMD exceptions masked, round to nearest */
#define FCW_INIT      0x037f   /* all x87 exceptions masked, 64-bit precision */
#define FSW_ES        0x0080   /* an unmasked x87 exception is pending */

_Static_assert(MXCSR_INIT == EXC_MXCSR_INIT, "the entry's MXCSR (abi.h) is the default");

static uint64_t xcr0;
static uint32_t area_size;
static uint32_t mxcsr_mask = 0xffbf;   /* the MXCSR bits this CPU takes (FXSAVE's MXCSR_MASK) */
static uint32_t avx_off, avx_size;     /* AVX's upper halves in the standard form (CPUID 0xd) */
static struct kmem_cache *area_cache;
static bool has_xsaveopt;
static bool call_drop_ok;   /* XCR0 holds only XCR0_DROPPABLE components */
bool fpu_opt = true;
bool fpu_call = true;
static struct thread *fpu_owner[MAX_CPUS];   /* whose state this CPU's registers hold */
#define FPU_CPU_NONE UINT32_MAX

/* Every CPU, from cpu_init_local. The BSP runs it first and sizes the
 * area; the APs only program their registers. */
void fpu_init_cpu(void)
{
    /* MP+NE: native x87 error reporting; EM and TS off: the FPU works. */
    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
    uint64_t cr4 = read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (cpu_features.xsave)
        cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

    if (!area_cache) {
        if (cpu_features.xsave) {
            uint32_t a, b, c, d;
            cpuid(0xd, 0, &a, &b, &c, &d);
            uint64_t supported = a | (uint64_t)d << 32;
            xcr0 = XCR0_X87 | XCR0_SSE;
            if (cpu_features.avx && (supported & XCR0_AVX))
                xcr0 |= XCR0_AVX;
            xsetbv(0, xcr0);
            cpuid(0xd, 0, &a, &b, &c, &d);
            area_size = b;   /* for the features now enabled in XCR0 */
            cpuid(0xd, 1, &a, &b, &c, &d);
            has_xsaveopt = a & 1;
            if (xcr0 & XCR0_AVX) {
                cpuid(0xd, 2, &a, &b, &c, &d);
                avx_size = a;
                avx_off = b;
            }
        } else {
            area_size = FXSAVE_SIZE;
        }
        /* FXSAVE's MXCSR_MASK says which MXCSR bits XRSTOR takes; 0 means
         * the original set (SDM vol. 1, 11.6.6). */
        static uint8_t probe[FXSAVE_SIZE] __attribute__((aligned(16)));
        __asm__ volatile("fxsave64 (%0)" :: "r"(probe) : "memory");
        uint32_t m = *(uint32_t *)(probe + MXCSR_MASK_OFF);
        if (m)
            mxcsr_mask = m;
        area_cache = kmem_cache_create("fpu state", area_size, 64);
        call_drop_ok = !(xcr0 & ~XCR0_DROPPABLE);
        __atomic_store_n(&fpu_opt, !cmdline_has("nofpuopt"), __ATOMIC_RELAXED);
        __atomic_store_n(&fpu_call, !cmdline_has("nofpucall"), __ATOMIC_RELAXED);
        report("fpu: %s, xcr0 %lx, %u-byte user state, calls keep %s; smep=%d smap=%d umip=%d "
               "pcid=%d/%d invpcid=%d",
               cpu_features.xsave ? (has_xsaveopt ? "XSAVEOPT" : "XSAVE") : "FXSAVE", xcr0,
               area_size, fpu_call_drop() ? "control words" : "everything", cpu_features.smep,
               cpu_features.smap, cpu_features.umip, cpu_features.pcid, pcid_is_on(),
               cpu_features.invpcid);
        return;
    }
    if (cpu_features.xsave)
        xsetbv(0, xcr0);
}

/* Default state: x87 and SSE masked and empty, AVX upper halves zero. For
 * XSAVE a zero header (XSTATE_BV = 0) means "initial state" for every
 * component, but MXCSR is always loaded from the legacy area. */
static void area_reset(void *area)
{
    memset(area, 0, area_size);
    *(uint16_t *)area = FCW_INIT;
    *(uint32_t *)((char *)area + MXCSR_OFF) = MXCSR_INIT;
}

void fpu_save(void *area)
{
    if (cpu_features.xsave && has_xsaveopt && __atomic_load_n(&fpu_opt, __ATOMIC_RELAXED))
        __asm__ volatile("xsaveopt64 (%0)" :: "r"(area), "a"((uint32_t)xcr0),
                         "d"((uint32_t)(xcr0 >> 32)) : "memory");
    else if (cpu_features.xsave)
        __asm__ volatile("xsave64 (%0)" :: "r"(area), "a"((uint32_t)xcr0),
                         "d"((uint32_t)(xcr0 >> 32)) : "memory");
    else
        __asm__ volatile("fxsave64 (%0)" :: "r"(area) : "memory");
}

void fpu_restore(const void *area)
{
    if (cpu_features.xsave)
        __asm__ volatile("xrstor64 (%0)" :: "r"(area), "a"((uint32_t)xcr0),
                         "d"((uint32_t)(xcr0 >> 32)) : "memory");
    else
        __asm__ volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
}

bool fpu_call_drop(void)
{
    return call_drop_ok && __atomic_load_n(&fpu_call, __ATOMIC_RELAXED);
}

/* The system call rule (see the top): keep MXCSR and the control word,
 * leave the area a clean state, and make the next switch in restore it. */
void fpu_save_called(struct thread *t)
{
    uint8_t *a = t->ustate;
    uint16_t fcw;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m"(fcw));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    bool own_fcw = fcw != FCW_INIT;
    if (!cpu_features.xsave)
        memset(a, 0, XMM_END);   /* FXRSTOR loads every field: empty x87, zero XMM */
    else if (own_fcw)
        memset(a, 0, X87_END);   /* x87 is loaded from the area: make it empty */
    if (cpu_features.xsave)      /* SSE and AVX (and x87 unless own_fcw): initial */
        *(uint64_t *)(a + XSTATE_BV_OFF) = own_fcw ? XCR0_X87 : 0;
    *(uint16_t *)a = fcw;
    *(uint32_t *)(a + MXCSR_OFF) = mxcsr;   /* XRSTOR loads MXCSR whatever XSTATE_BV says */
    t->fpu_cpu = FPU_CPU_NONE;   /* the registers no longer match the area */
}

/* Interrupts off: load t's state into this CPU's registers, unless they
 * still hold it (see the top). */
void fpu_load(struct thread *t)
{
    uint32_t cpu = this_cpu()->index;
    if (__atomic_load_n(&fpu_opt, __ATOMIC_RELAXED) && fpu_owner[cpu] == t && t->fpu_cpu == cpu) {
        PATH_COUNT(PATH_FPU_KEPT);
        return;
    }
    PATH_COUNT(PATH_FPU_RESTORE);
    fpu_restore(t->ustate);
    fpu_owner[cpu] = t;
    t->fpu_cpu = cpu;
}

/* Interrupts off: something other than fpu_load put state in this CPU's
 * registers. */
void fpu_clobbered(void)
{
    fpu_owner[this_cpu()->index] = NULL;
}

/* The registers hold whatever the last user thread on this CPU left;
 * arch_enter_user starts a thread from the default state instead.
 * Interrupts off. */
void fpu_reset_and_load(struct thread *t)
{
    area_reset(t->ustate);
    t->fpu_cpu = FPU_CPU_NONE;
    fpu_load(t);
}

uint32_t fpu_area_size(void)
{
    return area_size;
}

/* ---- the XSAVE area of a fault's frame (upcall.c) ---------------------------
 *
 * A frame (struct exc_frame, <jam/abi.h>) carries the thread's vector and
 * x87 registers in XSAVE's standard form with every component of XCR0
 * written out, whatever the save left in its initial state, so libc reads
 * an XMM register at its place without looking at XSTATE_BV. Only fields
 * the CPU defines are copied, from the thread's own area just saved: the
 * reserved and software bytes stay zero, and FOP, FIP and FDP stay zero
 * unless an x87 exception is pending (some AMD CPUs leave the previous
 * thread's there otherwise). */

uint32_t fpu_frame_size(void)
{
    return area_size <= FPU_FRAME_MAX ? area_size : 0;
}

/* Interrupts off; t is the current thread, interrupted in ring 3, so this
 * CPU's registers are t's: save them as a switch would (the XSAVEOPT
 * tracking stays true), then write the frame's form into out
 * (fpu_frame_size() bytes the caller zeroed). */
void fpu_frame_save(struct thread *t, uint8_t *out)
{
    uint8_t *a = t->ustate;
    fpu_save(a);
    uint64_t bv = cpu_features.xsave ? *(uint64_t *)(a + XSTATE_BV_OFF)
                                     : XCR0_X87 | XCR0_SSE;   /* FXSAVE writes everything */
    if (bv & XCR0_X87) {
        bool pending = *(uint16_t *)(a + FSW_OFF) & FSW_ES;
        memcpy(out, a, pending ? MXCSR_OFF : FOP_OFF);
        memcpy(out + X87_REGS, a + X87_REGS, X87_END - X87_REGS);
    } else {
        *(uint16_t *)out = FCW_INIT;   /* the rest of x87's initial state is 0 */
    }
    *(uint32_t *)(out + MXCSR_OFF) = *(uint32_t *)(a + MXCSR_OFF);   /* always the live one */
    *(uint32_t *)(out + MXCSR_MASK_OFF) = mxcsr_mask;
    if (bv & XCR0_SSE)
        memcpy(out + X87_END, a + X87_END, XMM_END - X87_END);
    if (!cpu_features.xsave)
        return;
    *(uint64_t *)(out + XSTATE_BV_OFF) = xcr0;
    if (bv & xcr0 & XCR0_AVX)
        memcpy(out + avx_off, a + avx_off, avx_size);
}

/* Can XRSTOR (FXRSTOR) take a frame's area (a kernel copy) without
 * faulting? MXCSR only with bits this CPU takes; the header (XSAVE) with
 * only XCR0's components, and XCOMP_BV and the reserved bytes 0 (the
 * standard form). Nothing else in an area makes the restore fault: an
 * exception it leaves pending faults in user mode, the thread's own. */
bool fpu_frame_ok(const uint8_t *area)
{
    if (*(const uint32_t *)(area + MXCSR_OFF) & ~mxcsr_mask)
        return false;
    if (!cpu_features.xsave)
        return true;
    if (*(const uint64_t *)(area + XSTATE_BV_OFF) & ~xcr0)
        return false;
    for (unsigned i = XSTATE_BV_OFF + 8; i < XSAVE_HDR_END; i++)
        if (area[i])
            return false;
    return true;
}

/* Interrupts off, t current and about to return to ring 3 (exception_done's
 * EXC_RESUME): the area (fpu_frame_ok said yes) becomes t's and is loaded
 * now, so the XSAVEOPT tracking and the owner shortcut (top) refer to what
 * the registers hold. */
void fpu_frame_load(struct thread *t, const uint8_t *area)
{
    memcpy(t->ustate, area, area_size);
    t->fpu_cpu = FPU_CPU_NONE;
    fpu_load(t);
}

int fpu_ustate_alloc(struct thread *t)
{
    if (t->ustate)
        return OK;
    void *area = kmem_cache_alloc(area_cache);
    if (!area)
        return ERR_NO_MEMORY;
    area_reset(area);
    t->fpu_cpu = FPU_CPU_NONE;
    t->ustate = area;
    return OK;
}

/* Only once t can no longer be switched in (reap), or by t itself before
 * it ever entered ring 3: arch_thread_switch saves into this area. */
void fpu_ustate_free(struct thread *t)
{
    void *area = t->ustate;
    if (!area)
        return;
    t->ustate = NULL;
    kmem_cache_free(area_cache, area);
}
