/* User FPU/SSE/AVX state. The kernel is built without SSE and never
 * touches these registers, so the only state to manage is the user
 * threads': each has an XSAVE area (FXSAVE on CPUs without XSAVE), saved
 * and restored eagerly by arch_thread_switch whenever a thread that has
 * one switches out or in. No lazy #NM tricks: CR0.TS stays clear.
 *
 * M5.5 (switch fpu_opt, boot "nofpuopt"), both as Linux does them:
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
 *     a thread struct reused at a freed owner's address can't match. */
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/report.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/uentry.h>
#include <jam/x86.h>

#define XCR0_X87 (1ull << 0)
#define XCR0_SSE (1ull << 1)
#define XCR0_AVX (1ull << 2)

#define FXSAVE_SIZE  512
#define MXCSR_OFF    24
#define MXCSR_INIT   0x1f80   /* all SIMD exceptions masked, round to nearest */
#define FCW_INIT     0x037f   /* all x87 exceptions masked, 64-bit precision */

static uint64_t xcr0;
static uint32_t area_size;
static struct kmem_cache *area_cache;
static bool has_xsaveopt;
volatile bool fpu_opt = true;
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
        } else {
            area_size = FXSAVE_SIZE;
        }
        area_cache = kmem_cache_create("fpu state", area_size, 64);
        fpu_opt = !cmdline_has("nofpuopt");
        report("fpu: %s, xcr0 %lx, %u-byte user state; smep=%d smap=%d umip=%d pcid=%d/%d "
               "invpcid=%d", cpu_features.xsave ? (has_xsaveopt ? "XSAVEOPT" : "XSAVE") : "FXSAVE",
               xcr0, area_size, cpu_features.smep, cpu_features.smap, cpu_features.umip,
               cpu_features.pcid, pcid_is_on(), cpu_features.invpcid);
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
    if (cpu_features.xsave && has_xsaveopt && fpu_opt)
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

/* Interrupts off: load t's state into this CPU's registers, unless they
 * still hold it (see the top). */
void fpu_load(struct thread *t)
{
    uint32_t cpu = this_cpu()->index;
    if (fpu_opt && fpu_owner[cpu] == t && t->fpu_cpu == cpu)
        return;
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

bool fpu_has_xsaveopt(void)
{
    return has_xsaveopt;
}

uint32_t fpu_area_size(void)
{
    return area_size;
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

uint64_t fpu_xcr0(void)
{
    return xcr0;
}
