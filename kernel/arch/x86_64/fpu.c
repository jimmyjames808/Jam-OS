/* User FPU/SSE/AVX state. The kernel is built without SSE and never
 * touches these registers, so the only state to manage is the user
 * threads': each has an XSAVE area (FXSAVE on CPUs without XSAVE), saved
 * and restored eagerly by arch_thread_switch whenever a thread that has
 * one switches out or in. No lazy #NM tricks: CR0.TS stays clear. */
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
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
        } else {
            area_size = FXSAVE_SIZE;
        }
        area_cache = kmem_cache_create("fpu state", area_size, 64);
        kprintf("fpu: %s, xcr0 %lx, %u-byte user state; smep=%d smap=%d umip=%d\n",
                cpu_features.xsave ? "XSAVE" : "FXSAVE", xcr0, area_size, cpu_features.smep,
                cpu_features.smap, cpu_features.umip);
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
    if (cpu_features.xsave)
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

/* The registers hold whatever the last user thread on this CPU left;
 * arch_enter_user starts a thread from the default state instead. */
void fpu_reset_and_load(void *area)
{
    area_reset(area);
    fpu_restore(area);
}

int fpu_ustate_alloc(struct thread *t)
{
    if (t->ustate)
        return OK;
    void *area = kmem_cache_alloc(area_cache);
    if (!area)
        return ERR_NO_MEMORY;
    area_reset(area);
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
