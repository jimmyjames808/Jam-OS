#include <stdint.h>
#include <jam/acpi.h>
#include <jam/boot.h>
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/ioapic.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/lapic.h>
#include <jam/fbcon.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/selftest.h>
#include <jam/serial.h>
#include <jam/smp.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define JAMOS_VERSION   "0.0.4-m3"
#define KERNEL_STACK_SZ (64 * 1024)

_Noreturn void stack_switch_call(void *top, void (*fn)(void *), void *arg);

static const char *mem_type_name(enum boot_mem_type t)
{
    switch (t) {
    case BOOT_MEM_USABLE:             return "usable";
    case BOOT_MEM_RESERVED:           return "reserved";
    case BOOT_MEM_ACPI_RECLAIMABLE:   return "acpi-reclaim";
    case BOOT_MEM_ACPI_NVS:           return "acpi-nvs";
    case BOOT_MEM_BAD:                return "bad";
    case BOOT_MEM_LOADER_RECLAIMABLE: return "loader-reclaim";
    case BOOT_MEM_KERNEL_AND_MODULES: return "kernel+modules";
    case BOOT_MEM_FRAMEBUFFER:        return "framebuffer";
    }
    return "?";
}

static void print_boot_info(const struct boot_info *bi)
{
    kprintf("cpu:         %s (%s)%s\n", cpu_features.brand, cpu_features.vendor,
            cpu_features.hybrid ? " hybrid P/E" : "");
    kprintf("features:    nx=%d 1g=%d pat=%d pge=%d x2apic=%d invariant-tsc=%d\n",
            cpu_features.nx, cpu_features.pages_1g, cpu_features.pat,
            cpu_features.pge, cpu_features.x2apic, cpu_features.tsc_invariant);
    kprintf("loader:      %s, cmdline \"%s\"\n", bi->loader_name, bi->cmdline);
    if (bi->fb.virt)
        kprintf("framebuffer: %ux%u %ubpp pitch %u @ phys %lx\n", bi->fb.width,
                bi->fb.height, bi->fb.bpp, bi->fb.pitch, bi->fb.phys);
    kprintf("kernel:      phys %lx virt %lx\n", bi->kernel_phys_base, bi->kernel_virt_base);
    kprintf("rsdp:        %lx\n", bi->rsdp_phys);
    kprintf("cpus:        %u from loader (bsp lapic %u, %s)\n", bi->cpu_count,
            bi->bsp_lapic_id, bi->x2apic ? "x2APIC" : "xAPIC");

    if (cmdline_has("memmap")) {
        for (size_t i = 0; i < bi->memmap_count; i++) {
            const struct boot_mem_region *r = &bi->memmap[i];
            kprintf("  %016lx - %016lx  %-14s %lu KiB\n", r->base, r->base + r->length,
                    mem_type_name(r->type), r->length / 1024);
        }
    }
}

static struct boot_info *boot;

/* Runs on the kernel's own stack, with its own page tables. */
_Noreturn static void kmain_stage2(void *arg)
{
    (void)arg;
    uint64_t total, free;
    pmm_stats(&total, &free);
    kprintf("pmm:         %lu MiB managed, %lu MiB free\n", total >> 8, free >> 8);

    if (cmdline_has("selftest"))
        selftest_run();
    selftest_crash(boot->cmdline);

    acpi_init(boot->rsdp_phys);
    lapic_init_bsp(boot->x2apic);
    tsc_calibrate_with_loader(boot->tsc_hz_loader);
    uint64_t redraw_us = fbcon_time_redraw(rdtsc) / (tsc_hz / 1000000);
    kprintf("fbcon: %ux%u, full-screen redraw takes %lu.%03lu ms, mapped %s\n",
            boot->fb.width, boot->fb.height, redraw_us / 1000, redraw_us % 1000,
            vmm_cache_type(vmm_kernel_pml4(), (uint64_t)boot->fb.virt));
    smp_init_bsp(boot);
    sched_init_bsp();   /* this code is now thread "main" */
    ipi_init();
    ioapic_init();
    lapic_timer_calibrate();
    lapic_timer_start(TICK_HZ);
    smp_start_aps(boot);
    irq_enable();

    kprintf("measuring ticks on every CPU for 1 s...\n");
    bool ok = smp_report(1000);
    if (cmdline_has("selftest"))
        selftest_run_smp();
    uint64_t stress_s = cmdline_get_u64("stress", 0);
    if (stress_s)
        ok &= stress_run(stress_s);
    selftest_crash_smp();
    sched_print_stats();

    kprintf("\nM3 %s. Idling.\n", ok ? "complete" : "FINISHED WITH PROBLEMS");
    thread_exit();   /* CPU 0 falls through to its idle thread */
}

_Noreturn void kmain(struct boot_info *bi)
{
    percpu_set_gs(&cpu0);   /* spinlocks need this_cpu() from here on */
    boot = bi;
    cmdline_set(bi->cmdline);
    int has_serial = serial_init();
    fbcon_init(&bi->fb);

    fbcon_set_colors(0xffb000, 0x101018);
    kprintf("Jam OS %s\n", JAMOS_VERSION);
    fbcon_set_colors(0xd0d0d0, 0x101018);
    kprintf("serial:      %s\n", has_serial ? "COM1" : "none");

    cpu_detect();
    gdt_init_bsp();
    idt_init();
    print_boot_info(bi);

    pmm_early_init(bi);
    vmm_init(bi);
    pmm_init();
    vmm_use_buddy();
    heap_init();

    /* Loader-reclaimable memory (Limine's stack, page tables, and the code
     * the parked APs are spinning in) is freed in M2, after the APs start. */
    stack_switch_call(kstack_alloc(KERNEL_STACK_SZ), kmain_stage2, NULL);
}
