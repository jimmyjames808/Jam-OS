#include <stdint.h>
#include <jam/cpu.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/string.h>
#include <jam/x86.h>

#define IST_STACK_SIZE (16 * 1024)
#define MSR_GS_BASE        0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

/* Early BSP tables, used only until gdt_init_cpu runs on the BSP. */
static uint64_t early_gdt[9];
static struct tss early_tss;
static uint8_t early_ist[3][IST_STACK_SIZE] __attribute__((aligned(16)));

#define SEG(access, flags) \
    (0xffffull | (uint64_t)(access) << 40 | (uint64_t)(flags) << 52 | 0xfull << 48)

static void build(uint64_t *gdt, struct tss *tss)
{
    gdt[0] = 0;
    gdt[1] = SEG(0x9a, 0xa);   /* kernel code: present, DPL0, exec/read, L=1 */
    gdt[2] = SEG(0x92, 0xc);   /* kernel data */
    gdt[3] = 0;                /* user 32-bit code: unused, keeps SYSRET layout */
    gdt[4] = SEG(0xf2, 0xc);   /* user data, DPL3 */
    gdt[5] = SEG(0xfa, 0xa);   /* user code, DPL3, L=1 */

    tss->iopb_offset = sizeof(*tss);   /* no I/O bitmap */
    uint64_t base = (uint64_t)tss, limit = sizeof(*tss) - 1;
    gdt[6] = (limit & 0xffff) | (base & 0xffffff) << 16 | 0x89ull << 40 |
             ((limit >> 16) & 0xf) << 48 | ((base >> 24) & 0xff) << 56;
    gdt[7] = base >> 32;
    gdt[8] = 0;
}

static void load(uint64_t *gdt, size_t size)
{
    struct gdtr gdtr = { size - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        /* FS/GS are left alone: loading a selector can clear the GS base,
         * and GS already points at this CPU's struct cpu. */
        "ltr %w3\n\t"
        :: "m"(gdtr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA), "r"(GDT_TSS)
        : "rax", "memory");
}

void gdt_init_bsp(void)
{
    memset(&early_tss, 0, sizeof(early_tss));
    for (int i = 0; i < 3; i++)
        early_tss.ist[i] = (uint64_t)&early_ist[i][IST_STACK_SIZE];
    build(early_gdt, &early_tss);
    load(early_gdt, sizeof(early_gdt));
}

void gdt_init_cpu(struct cpu *c)
{
    memset(&c->tss, 0, sizeof(c->tss));
    for (int i = 0; i < 3; i++)
        c->tss.ist[i] = (uint64_t)kstack_alloc(IST_STACK_SIZE);
    build(c->gdt, &c->tss);
}

void percpu_set_gs(struct cpu *c)
{
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void percpu_load(struct cpu *c)
{
    percpu_set_gs(c);
    load(c->gdt, sizeof(c->gdt));
    idt_load();
}
