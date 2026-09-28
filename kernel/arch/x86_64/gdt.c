#include <stdint.h>
#include <jam/cpu.h>
#include <jam/string.h>

#define IST_STACK_SIZE (16 * 1024)

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

/* M2 turns these into per-CPU copies; for now only the BSP has them. */
static uint64_t gdt[9];
static struct tss tss;
static uint8_t ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));

#define SEG(access, flags) \
    (0xffffull | (uint64_t)(access) << 40 | (uint64_t)(flags) << 52 | 0xfull << 48)

void gdt_init_bsp(void)
{
    gdt[0] = 0;
    gdt[1] = SEG(0x9a, 0xa);   /* kernel code: present, DPL0, exec/read, L=1 */
    gdt[2] = SEG(0x92, 0xc);   /* kernel data */
    gdt[3] = 0;                /* user 32-bit code: unused, keeps SYSRET layout */
    gdt[4] = SEG(0xf2, 0xc);   /* user data, DPL3 */
    gdt[5] = SEG(0xfa, 0xa);   /* user code, DPL3, L=1 */

    memset(&tss, 0, sizeof(tss));
    for (int i = 0; i < 3; i++)
        tss.ist[i] = (uint64_t)&ist_stacks[i][IST_STACK_SIZE];
    tss.iopb_offset = sizeof(tss);   /* no I/O bitmap */

    uint64_t base = (uint64_t)&tss, limit = sizeof(tss) - 1;
    gdt[6] = (limit & 0xffff) | (base & 0xffffff) << 16 | 0x89ull << 40 |
             ((limit >> 16) & 0xf) << 48 | ((base >> 24) & 0xff) << 56;
    gdt[7] = base >> 32;
    gdt[8] = 0;

    struct gdtr gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };
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
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "ltr %w3\n\t"
        :: "m"(gdtr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA), "r"(GDT_TSS)
        : "rax", "memory");
}
