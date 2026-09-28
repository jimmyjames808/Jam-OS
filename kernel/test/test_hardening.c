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
