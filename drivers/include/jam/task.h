/* <jam/task.h>: cooperative tasks, so one thread's loop can serve many
 * things side by side and still write a multi-step job as straight-line
 * code that waits at each step.
 *
 * A task is a function running on a stack of its own, inside the thread
 * of the loop that owns its set. It runs until it waits (task_wait); the
 * loop then goes on with its own work (its port, its channels) and, each
 * round, runs again every task whose wait may be over (task_run). Only one
 * thing runs at a time and a task gives the CPU up only where it waits, so
 * nothing a task shares with the loop or other tasks needs a lock, but
 * anything may have changed across a wait: a task's wait loop checks its
 * condition again after every wake.
 *
 * Waking: a task waits until a deadline or until something happens
 * (task_kick, which the loop calls when an event or a message came, or a
 * task when it gives back something others wait for). Every waiting task
 * looks again after a kick, and at least every wake_cap_ns if the set has
 * one: more wakes than strictly needed, but the code stays simple and a
 * missed kick costs time, not a hang.
 *
 * The loop's shape (usb-bus's serve.c is the full example):
 *
 *     for (;;) {
 *         take the port's packets and the channels' messages
 *             (start a task for work that will wait; task_kick if a
 *             waiting task may care);
 *         task_run(set);
 *         port wait until task_next_wake(set, my own next deadline);
 *     }
 *
 * A stack canary (the bottom 64 bytes of each stack) is checked every
 * time a task switches back to the loop: a task that ran over its stack is
 * reported once through the set's overflow callback and the set marks
 * itself overflowed. The memory next to that stack can't be trusted after
 * that: stop (usb-bus exits 7).
 *
 * Rules: a set belongs to one thread, and every call on it is made from
 * that thread (the loop or one of the set's tasks). The library is libos
 * (user/lib/task.c); programs and drivers include this header. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TASK_STACK_DEFAULT (32u * 1024)   /* bytes; the IDL buffers of a request take ~4 KiB */
#define TASK_STACK_MIN     4096u
#define TASK_MAX_TASKS     256u           /* tasks one set can hold */

struct task_set;   /* the tasks of one loop */
struct task;       /* one task: valid from task_start until its function returns */

struct task_opts {
    uint32_t max_tasks;            /* slots: 1..TASK_MAX_TASKS */
    uint32_t stack_bytes;          /* each task's stack, a multiple of 16 of at least
                                    * TASK_STACK_MIN; 0: TASK_STACK_DEFAULT */
    uint64_t wake_cap_ns;          /* a waiting task looks again at least this often;
                                    * 0: only at its deadline or a kick */
    void (*overflow)(void *arg);   /* a task ran over its stack: called once, with
                                    * that task's arg; NULL: nobody to tell */
};

/* A set of up to o->max_tasks tasks. NULL: bad options or no memory. */
struct task_set *task_set_create(const struct task_opts *o);
/* Free the set and its stacks: false if a task hadn't ended (it is
 * abandoned, never run again: whatever it holds stays held). From one of
 * its own tasks: refused (false, nothing freed). */
bool task_set_destroy(struct task_set *s);

/* Start fn(arg) as a task. From the loop it runs at once, until it first
 * waits or ends; from one of the set's tasks it runs at the loop's next
 * task_run. NULL if every slot is taken or there is no memory for its
 * stack (a slot's stack is kept for its next task). */
struct task *task_start(struct task_set *s, void (*fn)(void *arg), void *arg);
/* Run, once each in slot order, every task whose wait may be over. True
 * if any ran. From one of the set's tasks: does nothing (false). */
bool task_run(struct task_set *s);
/* The earliest uptime (ns) at which a task may go on, or `next` if that
 * is sooner; now if one may go on now. The loop's port wait ends there. */
uint64_t task_next_wake(const struct task_set *s, uint64_t next);

/* In one of the set's tasks: give the CPU back to the loop until deadline
 * (ns uptime; capped at wake_cap_ns from now) or the next task_kick, then
 * return. In the loop: returns at once (the loop waits on its port). */
void task_wait(struct task_set *s, uint64_t deadline);
/* task_wait with no deadline: let the loop and the other tasks run a round. */
void task_yield(struct task_set *s);
/* Something happened: every waiting task looks again at the next task_run. */
void task_kick(struct task_set *s);

/* The task running now, NULL in the loop. */
struct task *task_current(const struct task_set *s);
void *task_arg(const struct task *t);
/* Tasks started and not ended yet. */
unsigned task_live(const struct task_set *s);
/* Did a task run over its stack (the overflow callback was called)? */
bool task_overflowed(const struct task_set *s);
