/* What the scheduler's three files share, and nothing else includes:
 * sched.c (run queues, placement, the switch, waking, idle, the tick),
 * thread.c (thread lifecycle and the stack cache) and wait.c (blocking,
 * sleepers, wait queues, mutexes). */
#pragma once

#include <stdint.h>
#include <jam/sched.h>

/* switch.S: the first return address of a new thread (calls thread_entry). */
void thread_start(void);

/* sched.c: first thing on the far side of every switch_context, and in a
 * new thread before it runs its function. */
void finish_switch(void);

/* thread.c */
void thread_cache_init(void);
/* A zeroed thread with one reference, no stack. NULL when out of memory. */
struct thread *thread_alloc(const char *name, int prio);
/* A kernel stack's top from the cache, or a new one; NULL when out of
 * memory. */
void *thread_stack_get(void);
/* After a dead thread's last switch (finish_switch, interrupts off): free
 * what it still holds and drop its reference to itself. */
void thread_reap(struct thread *t);

/* wait.c: set up CPU cpu's sleeper queue. */
void sleepq_init(uint32_t cpu);
