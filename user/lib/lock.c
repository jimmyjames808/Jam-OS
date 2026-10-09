/* A lock for the threads of one program (<os.h> lock_take), on a futex:
 * the word is 0 (free), 1 (held) or 2 (held, and someone may be asleep
 * waiting for it). Holds are short (a copy, a list walk), so a taker
 * first spins a little; then it marks the word 2 and sleeps in the
 * kernel (futex_wait) until a giver that finds 2 wakes one sleeper. A
 * holder preempted on a busy machine costs its waiters nothing but the
 * wait. Not fair, not recursive, no kernel object (Drepper's "Futexes Are
 * Tricky", mutex 2). */
#include <os.h>

#define LOCK_SPINS 64   /* pauses before sleeping */

void lock_take(lock_t *l)
{
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&l->word, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    for (unsigned n = 0; n < LOCK_SPINS; n++) {
        __builtin_ia32_pause();
        c = 0;
        if (__atomic_load_n(&l->word, __ATOMIC_RELAXED) == 0 &&
            __atomic_compare_exchange_n(&l->word, &c, 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
    }
    /* Taken as 2 from here on: whoever gives it next wakes a sleeper, one
     * that may be us or not, which is all that is needed. A wait that ends
     * for any reason (a wake, the word already changed) just tries again. */
    while (__atomic_exchange_n(&l->word, 2, __ATOMIC_ACQUIRE) != 0)
        jam_futex_wait(&l->word, 2, DEADLINE_NEVER, 0);
}

void lock_give(lock_t *l)
{
    if (__atomic_exchange_n(&l->word, 0, __ATOMIC_RELEASE) == 2)
        jam_futex_wake(&l->word, 1, 0);
}
