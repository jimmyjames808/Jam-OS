/* The interrupt descriptor table: one gate per vector, all pointing into
 * isr.S's stubs (isr_table). NMI, #DB, double fault and machine check run
 * on their own IST stacks, since they can arrive when the current stack
 * can't be trusted. One table is shared by every CPU. */
#include <stdint.h>
#include <jam/cpu.h>

struct __attribute__((packed)) idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
};

struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};

extern const uint64_t isr_table[256];

static struct idt_entry idt[256];

static void set_gate(int vec, uint64_t handler, uint8_t ist)
{
    idt[vec] = (struct idt_entry){
        .offset_lo  = handler & 0xffff,
        .selector   = GDT_KERNEL_CODE,
        .ist        = ist,
        .type_attr  = 0x8e,   /* present, DPL0, 64-bit interrupt gate */
        .offset_mid = (handler >> 16) & 0xffff,
        .offset_hi  = handler >> 32,
    };
}

void idt_init(void)
{
    for (int v = 0; v < 256; v++)
        set_gate(v, isr_table[v], 0);
    /* These can arrive on a broken stack, so give them known-good ones. */
    set_gate(2, isr_table[2], IST_NMI);
    set_gate(8, isr_table[8], IST_DOUBLE_FAULT);
    set_gate(18, isr_table[18], IST_MACHINE_CHECK);
    /* #DB too: it can arrive on the first kernel instruction after a
     * user-controlled entry (the mov ss / pop ss trick), before the stack
     * is the kernel's. */
    set_gate(1, isr_table[1], IST_DEBUG);
    idt_load();
}

void idt_load(void)
{
    struct idtr idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}
