/* Counters that one CPU (or one lock's holder) writes and others read
 * without a lock: statistics, run queue lengths, tick counts.
 *
 * Both halves of the update are explicit atomic accesses, so no plain C
 * access ever touches a variable another CPU reads. With a single writer
 * a load followed by a store is enough, and it costs a plain add: no
 * `lock` prefix on the scheduler's and the timer's hot paths. A counter
 * with several writers needs __atomic_add_fetch instead. */
#pragma once

#define COUNTER_ADD(p, d) \
    __atomic_store_n((p), __atomic_load_n((p), __ATOMIC_RELAXED) + (d), __ATOMIC_RELAXED)
#define COUNTER_SUB(p, d) \
    __atomic_store_n((p), __atomic_load_n((p), __ATOMIC_RELAXED) - (d), __ATOMIC_RELAXED)
