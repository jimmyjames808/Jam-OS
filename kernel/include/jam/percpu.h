/* Per-CPU state, reached through GS: gs:0 holds a pointer to the struct.
 * In the kernel GS is always this CPU's struct; ring 3 runs with the
 * user's GS base, and every entry from ring 3 swaps it back (swapgs).
 * NMI, #MC and #DB can land in the swapgs window, so they check the GS
 * base MSR instead of trusting the saved CS (arch/x86_64/isr.S). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_CPUS 256
#define MAX_HELD_LOCKS 16

struct spinlock;
struct thread;

enum core_type { CORE_UNKNOWN, CORE_PERFORMANCE, CORE_EFFICIENCY };

struct __attribute__((packed)) tss {
    uint32_t reserved0;     /* 0 */
    uint64_t rsp[3];        /* stacks for entries from rings 0-2; rsp[0] = kernel_rsp */
    uint64_t reserved1;     /* 0 */
    uint64_t ist[7];        /* IST stacks (IST_*), picked by IDT gates */
    uint64_t reserved2;     /* 0 */
    uint16_t reserved3;     /* 0 */
    uint16_t iopb_offset;   /* I/O bitmap offset; past the end = none */
};

struct cpu {
    struct cpu    *self;          /* must stay first: this_cpu() reads gs:0 */
    /* The syscall entry reads these two before it has a stack or a free
     * register, at fixed offsets (jam/entry_asm.h). */
    uint64_t       user_rsp;      /* user RSP while the syscall frame is built */
    uint64_t       kernel_rsp;    /* current thread's kernel stack top (= TSS rsp0) */
    uint32_t       index;                                /* 0 = BSP, dense */
    uint32_t       lapic_id;                             /* local APIC id (x2APIC id on the PC) */
    uint32_t       acpi_uid;      /* ACPI processor UID (MADT), for its NMI pins */
    enum core_type type;                                 /* P-core or E-core (CPUID 0x1a) */
    uint32_t       core_id;                              /* x2APIC id with the SMT bits dropped */
    uint32_t       smt_id;                               /* thread within its core (the SMT bits) */
    bool           online;        /* running the scheduler: may take IPIs and threads */
    uint64_t          ticks;      /* scheduler ticks on this CPU since it came up */
    /* Timer (lapic.c), absolute TSC values, touched only by this CPU
     * with interrupts off: the next scheduler tick, the earliest sleeper
     * deadline (UINT64_MAX: none), and what the timer is armed for. */
    uint64_t       tick_deadline, timer_deadline, timer_armed;
    void          *kstack_top;    /* the stack it started on; its idle thread keeps it */

    /* Scheduling state. */
    uint32_t       preempt_count;                        /* >0: this CPU must not switch threads */
    uint32_t       irq_depth;                            /* >0: inside an interrupt handler */
    bool           need_resched;                         /* set locally or by a reschedule IPI */
    /* The idle thread is polling need_resched and its run queue (spin
     * before idle): a remote wakeup needs no IPI. See idle_loop. */
    bool           idle_polling;
    struct thread *current;                              /* the thread running here */

    /* Lock checker: locks held right now, innermost last. */
    uint32_t         held_depth;
    struct spinlock *held[MAX_HELD_LOCKS];
    uint8_t          held_cls[MAX_HELD_LOCKS];

    /* Watchdog: this CPU watches the next one's tick count. */
    uint64_t       wd_seen_ticks;
    uint32_t       wd_stale_seconds;

    /* Statistics. */
    uint64_t          switches, steals, ipis;
    uint64_t          polled_wakes;   /* wakeups that found this CPU polling: no IPI */
    /* CPU time (sched.c, for the shell): the TSC at the last switch (0 until
     * the run queue is online), the idle thread's cycles up to then, and
     * whether the idle thread runs now. Written by this CPU only. */
    uint64_t          switch_tsc, idle_tsc;
    bool              idle_now;

    uint64_t       gdt[9] __attribute__((aligned(16)));  /* this CPU's GDT (gdt.c) */
    struct tss     tss;           /* this CPU's TSS (its descriptor is in gdt) */
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

/* Fields of a CPU that other CPUs read without a lock. `online` is set once
 * by the CPU itself (a release store in ap_main), so it is read with an
 * acquire; the others are relaxed, as they only decide when to look again. */
static inline bool cpu_online(const struct cpu *c)
{
    return __atomic_load_n(&c->online, __ATOMIC_ACQUIRE);
}

static inline bool cpu_need_resched(const struct cpu *c)
{
    return __atomic_load_n(&c->need_resched, __ATOMIC_RELAXED);
}

static inline void cpu_set_need_resched(struct cpu *c, bool v)
{
    __atomic_store_n(&c->need_resched, v, __ATOMIC_RELAXED);
}

static inline uint64_t cpu_ticks(const struct cpu *c)
{
    return __atomic_load_n(&c->ticks, __ATOMIC_RELAXED);
}

/* Load this CPU's GDT/TSS/IDT and point GS at it. */
void percpu_load(struct cpu *c);
/* Point GS at c without touching descriptor tables (early boot). */
void percpu_set_gs(struct cpu *c);
