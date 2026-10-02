/* A lock for the threads of one program (<os.h> lock_take): a bool taken
 * with test-and-set. Holds are short (a copy, a list walk), so a waiter
 * first pauses a few times; then it sleeps 20 us between tries, so a
 * holder preempted on a busy machine doesn't leave its waiters spinning
 * through their time slices. Not fair, not recursive, no kernel object. */
#include <os.h>

#define LOCK_SPINS 64                  /* pauses before the first sleep */
#define LOCK_NAP   (20 * NS_PER_US)    /* a sleep between tries after them */

void lock_take(bool *l)
{
    for (unsigned n = 0; __atomic_test_and_set(l, __ATOMIC_ACQUIRE); n++) {
        if (n < LOCK_SPINS)
            __builtin_ia32_pause();
        else
            jam_nanosleep(now() + LOCK_NAP);
    }
}

void lock_give(bool *l)
{
    __atomic_clear(l, __ATOMIC_RELEASE);
}
