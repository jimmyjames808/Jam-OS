/* M5.5 performance pass: spin before idle, placement, per-CPU kmalloc
 * magazines, per-CPU one-shot timers, the serial transmit ring, PCIDs. */
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/time.h>

#define MS 1000000ull

static uint32_t pin_self(uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    thread_set_affinity(current_thread(), &m);
    return cpu;
}

static void unpin_self(void)
{
    cpumask_t m;
    cpumask_all(&m);
    thread_set_affinity(current_thread(), &m);
}

/* ---- spin before idle ---------------------------------------------------- */

#define PP_ROUNDS 200

static struct {
    spinlock_t       lock;
    struct waitqueue wq;
    volatile int     turn;   /* 0: pinger's move, 1: ponger's */
    volatile bool    stop;
} pp;

static void ponger(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&pp.lock);
    for (;;) {
        while (pp.turn != 1 && !pp.stop)
            waitqueue_wait(&pp.wq, &pp.lock, &f);
        if (pp.stop)
            break;
        pp.turn = 0;
        waitqueue_wake_all(&pp.wq);
    }
    spin_unlock_irqrestore(&pp.lock, f);
}

/* PP_ROUNDS block+wake round trips between this thread (on cpu a) and one
 * on cpu b; returns the polled wakeups cpu b saw meanwhile. */
static uint64_t pingpong(uint32_t a, uint32_t b)
{
    pin_self(a);
    spin_init(&pp.lock, "m55 pingpong");
    waitqueue_init(&pp.wq, "m55 pingpong waiters");
    pp.turn = 0;
    pp.stop = false;
    cpumask_t m;
    cpumask_one(&m, b);
    uint64_t polled0 = cpus[b]->polled_wakes;
    struct thread *t = thread_create_on("m55-pong", ponger, NULL, PRIO_DEFAULT + 2, &m);
    uint64_t f = spin_lock_irqsave(&pp.lock);
    for (int i = 0; i < PP_ROUNDS; i++) {
        pp.turn = 1;
        waitqueue_wake_all(&pp.wq);
        while (pp.turn != 0)
            waitqueue_wait(&pp.wq, &pp.lock, &f);
    }
    pp.stop = true;
    waitqueue_wake_all(&pp.wq);
    spin_unlock_irqrestore(&pp.lock, f);
    thread_join(t);
    uint64_t polled = cpus[b]->polled_wakes - polled0;
    unpin_self();
    return polled;
}

/* With a long spin window, the ponger's CPU is polling whenever it is woken:
 * nearly every wakeup skips the IPI. With the spin off none can. */
KTEST(spin_idle_skips_ipi)
{
    if (cpu_count < 3)
        return;
    uint64_t keep = sched_idle_spin_ns;
    sched_idle_spin_ns = 50 * MS;   /* QEMU is slow: make the window cover a round trip */
    uint64_t polled_on = pingpong(1, 2);
    sched_idle_spin_ns = 0;
    thread_sleep_ms(60);   /* let the window cpu 2 already opened run out */
    uint64_t polled_off = pingpong(1, 2);
    sched_idle_spin_ns = keep;
    kprintf("spin-idle: %lu of %d wakeups polled with a 50 ms window, %lu with none\n",
            polled_on, PP_ROUNDS, polled_off);
    KT_ASSERT(polled_on >= PP_ROUNDS / 2);
    KT_EQ(polled_off, 0);
}
