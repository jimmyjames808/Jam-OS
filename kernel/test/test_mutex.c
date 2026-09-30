/* Mutexes (kernel/sched/wait.c): the hand-off to a waiter that has waited
 * too long. */
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/sched.h>
#include <jam/time.h>
#include <jam/x86.h>

/* ---- mutex hand-off (stress saw a waiter "made no progress" without it) ---- */

/* Up to three threads on other CPUs take a mutex back to back (hold 20 us, re-lock
 * at once). A third, alone on its CPU, wants it too: once woken it needs a
 * microsecond or so to run, by which time a hammer has it again, so without
 * the hand-off it could wait forever. With it, it gets the mutex within
 * about MUTEX_HANDOFF_NS each time. */
static struct mutex mh_mutex;
static volatile bool mh_stop;

static void mh_hammer(void *arg)
{
    (void)arg;
    while (!mh_stop) {
        mutex_lock(&mh_mutex);
        uint64_t until = uptime_ns() + 20000;
        while (uptime_ns() < until)
            cpu_relax();
        mutex_unlock(&mh_mutex);
    }
}

KTEST(mutex_handoff_prevents_starvation)
{
    KT_NEEDS_IDLE("measures how long a waiter waits for a contended mutex on a CPU of its own");
    if (cpu_count < 3)
        return;
    mutex_init(&mh_mutex, "kt handoff");
    mh_stop = false;
    struct thread *h[3];
    int nh = cpu_count - 1 < 3 ? (int)cpu_count - 1 : 3;
    for (int i = 0; i < nh; i++) {
        cpumask_t m;
        cpumask_one(&m, 1 + i);
        h[i] = thread_create_on("kt-hammer", mh_hammer, NULL, PRIO_DEFAULT, &m);
    }
    kt_pin_self(0);
    thread_sleep_ms(5);   /* let them get going */
    uint64_t handoffs0 = __atomic_load_n(&mutex_handoffs, __ATOMIC_RELAXED), worst = 0;
    int got = 0, slow = 0;
    uint64_t end = uptime_ns() + 500 * NS_PER_MS;
    while (got < 20 && uptime_ns() < end) {
        uint64_t t0 = uptime_ns();
        mutex_lock(&mh_mutex);
        uint64_t waited = uptime_ns() - t0;
        mutex_unlock(&mh_mutex);
        if (waited > worst)
            worst = waited;
        if (waited >= 10 * NS_PER_MS)
            slow++;
        got++;
    }
    mh_stop = true;
    for (int i = 0; i < nh; i++)
        thread_join(h[i]);
    kt_unpin_self();
    kprintf("mutex handoff: waiter got it %d times in <= 500 ms, worst wait %lu us (%d of 10 ms "
            "or more), %lu handoffs\n", got, worst / 1000, slow,
            __atomic_load_n(&mutex_handoffs, __ATOMIC_RELAXED) - handoffs0);
    KT_EQ(got, 20);
    /* ~1 ms with the hand-off (tens of ms without it, in QEMU; unbounded
     * on the PC). Under QEMU the host can take a virtual CPU away for a
     * time slice (about 10 ms) while its thread holds the mutex, and the
     * waiter's wait then measures the host, not the hand-off: two of the
     * twenty waits may be that long, none may reach 100 ms. */
    KT_ASSERT(slow <= 2);
    KT_ASSERT(worst < 100 * NS_PER_MS);
}
