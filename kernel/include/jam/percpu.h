/* Per-CPU state, reached through GS: gs:0 holds a pointer to the struct.
 * M5 adds swapgs on user entry; until then GS is always the kernel's. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_CPUS 256
#define MAX_HELD_LOCKS 16

struct spinlock;
struct thread;

enum core_type { CORE_UNKNOWN, CORE_PERFORMANCE, CORE_EFFICIENCY };

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

struct cpu {
    struct cpu    *self;          /* must stay first: this_cpu() reads gs:0 */
    uint32_t       index;         /* 0 = BSP, dense */
    uint32_t       lapic_id;
    uint32_t       acpi_uid;
    enum core_type type;
    uint32_t       core_id;       /* x2APIC id with the SMT bits dropped */
    uint32_t       smt_id;
    volatile bool  online;
    volatile uint64_t ticks;
    void          *kstack_top;

    /* Scheduling state. */
    uint32_t       preempt_count;   /* >0: this CPU must not switch threads */
    uint32_t       irq_depth;       /* >0: inside an interrupt handler */
    volatile bool  need_resched;    /* set locally or by a reschedule IPI */
    struct thread *current;

    /* Lock checker: locks held right now, innermost last. */
    uint32_t         held_depth;
    struct spinlock *held[MAX_HELD_LOCKS];
    uint8_t          held_cls[MAX_HELD_LOCKS];

    /* Watchdog: this CPU watches the next one's tick count. */
    uint64_t       wd_seen_ticks;
    uint32_t       wd_stale_seconds;

    /* Statistics. */
    volatile uint64_t switches, steals, ipis;

    uint64_t       gdt[9] __attribute__((aligned(16)));
    struct tss     tss;
};

extern struct cpu *cpus[MAX_CPUS];
extern uint32_t    cpu_count;
/* The BSP's struct cpu is static so GS can point at it from the first
 * instruction of kmain, before the heap exists (spinlocks need it). */
extern struct cpu  cpu0;

/* this_cpu() is only stable while the thread cannot migrate: with
 * preemption or interrupts disabled. From preemptible code, the thread may
 * move to another CPU between reading the pointer and using it, so fields
 * that preemptible code touches go through the single-instruction GS
 * accessors below, which an interrupt cannot split. */
static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#define PERCPU_OFF(field) __builtin_offsetof(struct cpu, field)

static inline struct thread *percpu_current(void)
{
    struct thread *t;
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(t) : "i"(PERCPU_OFF(current)));
    return t;
}

/* This CPU's index in one GS-relative load: safe from preemptible code,
 * though the answer may be stale by the time it is used. */
static inline uint32_t percpu_index(void)
{
    uint32_t i;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(i) : "i"(PERCPU_OFF(index)));
    return i;
}

static inline void percpu_preempt_inc(void)
{
    __asm__ volatile("incl %%gs:%c0" :: "i"(PERCPU_OFF(preempt_count)) : "memory");
}

/* Decrement; returns the new value (read on whatever CPU we are on then). */
static inline uint32_t percpu_preempt_dec(void)
{
    uint32_t v;
    __asm__ volatile("decl %%gs:%c1\n\tmovl %%gs:%c1, %0"
                     : "=r"(v) : "i"(PERCPU_OFF(preempt_count)) : "memory");
    return v;
}

/* Load this CPU's GDT/TSS/IDT and point GS at it. */
void percpu_load(struct cpu *c);
/* Point GS at c without touching descriptor tables (early boot). */
void percpu_set_gs(struct cpu *c);
