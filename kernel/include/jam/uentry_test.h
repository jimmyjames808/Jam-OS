/* TEST ONLY (compiled out with make KTESTS=0): hooks that let
 * test/test_uentry.c run ring-3 code before processes and the generated
 * syscall table exist. Each is NULL (off) unless a test installs it. */
#pragma once

#ifndef JAM_NO_KTESTS
#include <stdbool.h>
#include <stdint.h>

struct syscall_frame;
struct thread;
struct trap_frame;

/* Consulted before syscall_dispatch: return true and set *ret to handle
 * the call (test syscall numbers only). */
extern bool (*uentry_test_syscall)(struct syscall_frame *f, int64_t *ret);
/* The CR3 to load when switching TO thread `next`: the test's hand-built
 * PML4 (physical) for a registered user thread, the kernel PML4 when this
 * CPU is leaving a test thread, or 0 to leave CR3 unchanged. The hook
 * tracks per-CPU state itself. Called from arch_thread_switch with
 * interrupts off. */
extern uint64_t (*uentry_test_cr3)(struct thread *t);
/* Told about every user thread killed for a fault (before it exits). */
extern void (*uentry_test_fault)(struct thread *t, uint64_t vector, uint64_t rip,
                                          uint64_t addr);
/* First look at every NMI (on the NMI's own context, GS already the
 * kernel's): return true to swallow it. */
extern bool (*uentry_test_nmi)(struct trap_frame *f);
#endif
