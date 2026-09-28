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

#define MSR_EFER 0xc0000080
#define MSR_PAT  0x00000277
#define EFER_NXE (1ull << 11)
#define CR0_WP   (1ull << 16)
#define CR4_PGE  (1ull << 7)
