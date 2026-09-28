/* M4.5 hardening checks that don't fit one object's test file: W^X on
 * every alias of the kernel image, and the guard pages under IST stacks. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/sched.h>

#define IST_STACK_SIZE (16 * 1024)   /* as in arch/x86_64/gdt.c */

extern char __text_start[], __text_end[], __rodata_start[], __rodata_end[],
    __data_start[], __data_end[];

static int access_of(const void *va)
{
    return vmm_access(vmm_kernel_pml4(), (uint64_t)va);
}

/* Last byte of a section, computed as an integer so the compiler doesn't
 * see an out-of-bounds pointer into a linker symbol. */
static const void *last(const char *end)
{
    return (const void *)((uintptr_t)end - 1);
}

/* The HHDM view of the same physical page. */
static int alias_access(const void *va)
{
    uint64_t pa = vmm_translate(vmm_kernel_pml4(), (uint64_t)va);
    KT_ASSERT(pa != UINT64_MAX);
    return access_of(phys_to_virt(pa));
}

KTEST(wx_kernel_image)
{
    /* Kernel mapping: text RX, rodata R, data RW+NX. */
    KT_EQ(access_of(__text_start), VM_EXEC);
    KT_EQ(access_of(last(__text_end)), VM_EXEC);
    KT_EQ(access_of(__rodata_start), 0);
    KT_EQ(access_of(last(__rodata_end)), 0);
    KT_EQ(access_of(__data_start), VM_WRITE);
    KT_EQ(access_of(last(__data_end)), VM_WRITE);

    /* HHDM aliases: never writable AND never executable, and text/rodata
     * not writable at all. */
    KT_EQ(alias_access(__text_start), 0);
    KT_EQ(alias_access(last(__text_end)), 0);
    KT_EQ(alias_access(__rodata_start), 0);
    KT_EQ(alias_access(__data_start) & VM_EXEC, 0);
}

KTEST(wx_stacks_and_heap)
{
    int local = 0;
    KT_EQ(access_of(&local), VM_WRITE);   /* thread stack: RW + NX */
    void *p = kmalloc(64);
    KT_EQ(access_of(p), VM_WRITE);
    kfree(p);
    uint64_t pa = pmm_alloc_page_phys(0);
    KT_ASSERT(pa);
    KT_EQ(access_of(phys_to_virt(pa)), VM_WRITE);   /* free RAM in the HHDM */
    pmm_free_page_phys(pa);
}

/* Every CPU's IST stacks (#DF, NMI, #MC) have an unmapped page below them,
 * so an overflow faults instead of corrupting whatever lies below. */
KTEST(ist_stack_guards)
{
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        for (int s = 0; s < 3; s++) {
            uint64_t top = c->tss.ist[s];
            KT_ASSERT(top);
            KT_EQ(access_of((void *)(top - 8)), VM_WRITE);
            KT_EQ(access_of((void *)(top - IST_STACK_SIZE)), VM_WRITE);
            KT_EQ(access_of((void *)(top - IST_STACK_SIZE - 1)), -1);
        }
    }
}

/* Cost of a nested spin_lock/unlock pair with the checker on, on every CPU
 * at once: the pair's edge is already known, so after the first round the
 * checker should write nothing shared (M4.5 fast path). */
#include <jam/kprintf.h>
#include <jam/time.h>

#define LOCK_ROUNDS 200000

/* One cache line per CPU: with 16-byte locks packed in arrays, four CPUs
 * shared each line and the first PC run (274 ns avg on 28 CPUs) measured
 * that line bouncing between cores, not the checker. */
static struct {
    spinlock_t outer, inner;
    uint64_t   ns;
} __attribute__((aligned(64))) speed[MAX_CPUS];

static void lock_speed_worker(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    uint64_t t0 = uptime_ns();
    for (int r = 0; r < LOCK_ROUNDS; r++) {
        spin_lock(&speed[i].outer);
        spin_lock(&speed[i].inner);
        spin_unlock(&speed[i].inner);
        spin_unlock(&speed[i].outer);
    }
    speed[i].ns = uptime_ns() - t0;
}

KTEST(lock_speed_all_cpus)
{
    struct thread *th[MAX_CPUS];
    for (uint32_t i = 0; i < cpu_count; i++) {
        spin_init(&speed[i].outer, "speed outer");
        spin_init(&speed[i].inner, "speed inner");
    }
    for (uint32_t i = 0; i < cpu_count; i++) {
        cpumask_t m;
        cpumask_one(&m, i);
        th[i] = thread_create_on("lock-speed", lock_speed_worker, (void *)(uintptr_t)i,
                                 PRIO_DEFAULT, &m);
    }
    uint64_t worst = 0, sum = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        thread_join(th[i]);
        sum += speed[i].ns;
        if (speed[i].ns > worst)
            worst = speed[i].ns;
    }
    kprintf("locks: nested lock+unlock pair on %u CPUs at once: avg %lu ns, worst CPU %lu ns\n",
            cpu_count, sum / cpu_count / LOCK_ROUNDS, worst / LOCK_ROUNDS);
}
