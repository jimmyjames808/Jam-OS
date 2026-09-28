/* Minimal M0 spinlock. M3 replaces this with ticket locks that save/restore
 * interrupt state and take part in lock-order checking. */
#pragma once

#include <stdbool.h>
#include <jam/x86.h>

typedef struct {
    volatile bool locked;
} spinlock_t;

#define SPINLOCK_INIT { false }

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            cpu_relax();
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
}
