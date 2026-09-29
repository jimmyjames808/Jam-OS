#pragma once

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline void cpu_relax(void) { __asm__ volatile("pause"); }

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

#define DEFINE_CR(n)                                                            \
    static inline uint64_t read_cr##n(void)                                     \
    { uint64_t v; __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v)); return v; } \
    static inline void write_cr##n(uint64_t v)                                  \
    { __asm__ volatile("mov %0, %%cr" #n :: "r"(v) : "memory"); }
DEFINE_CR(0)
DEFINE_CR(2)
DEFINE_CR(3)
DEFINE_CR(4)
#undef DEFINE_CR

static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }
static inline void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }

#define MSR_EFER           0xc0000080
#define MSR_STAR           0xc0000081
#define MSR_LSTAR          0xc0000082
#define MSR_CSTAR          0xc0000083
#define MSR_SFMASK         0xc0000084
#define MSR_FS_BASE        0xc0000100
#define MSR_GS_BASE        0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102
#define MSR_SYSENTER_CS    0x00000174
#define MSR_PAT            0x00000277
#define EFER_SCE (1ull << 0)
#define EFER_NXE (1ull << 11)
#define CR0_MP   (1ull << 1)
#define CR0_EM   (1ull << 2)
#define CR0_TS   (1ull << 3)
#define CR0_NE   (1ull << 5)
#define CR0_WP   (1ull << 16)
#define CR4_PGE        (1ull << 7)
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_UMIP       (1ull << 11)
#define CR4_FSGSBASE   (1ull << 16)
#define CR4_OSXSAVE    (1ull << 18)
#define CR4_SMEP       (1ull << 20)
#define CR4_SMAP       (1ull << 21)

#define RFLAGS_CF (1ull << 0)
#define RFLAGS_PF (1ull << 2)
#define RFLAGS_AF (1ull << 4)
#define RFLAGS_ZF (1ull << 6)
#define RFLAGS_SF (1ull << 7)
#define RFLAGS_TF (1ull << 8)
#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_OF (1ull << 11)
#define RFLAGS_NT (1ull << 14)
#define RFLAGS_AC (1ull << 18)
#define RFLAGS_ID (1ull << 21)

static inline void xsetbv(uint32_t reg, uint64_t v)
{
    __asm__ volatile("xsetbv" :: "c"(reg), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
