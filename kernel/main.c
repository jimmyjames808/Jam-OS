/* The boot sequence. kmain runs on Limine's stack: early console, CPU
 * features, GDT/IDT, the physical and virtual memory managers and the heap,
 * then switches to a kernel stack for kmain_stage2: bootfs, ACPI, the local
 * APIC and TSC, the scheduler (this code becomes thread "main"), IPIs,
 * the I/O APIC, the timer, the other CPUs, PCI, the VT-d probe and
 * resources. What runs after that depends on the command line
 * (boot/limine.conf): the tests,
 * the benchmark, the stress test, a crash test, or user space (init). The
 * RESULTS box at the end repeats every report() line. */
#include <stdint.h>
#include <jam/acpi.h>
#include <jam/boot.h>
#include <jam/bootfs.h>
#include <jam/cmdline.h>
#include <jam/cpu.h>
#include <jam/fbcon.h>
#include <jam/ioapic.h>
#include <jam/ipi.h>
#include <jam/irq.h>
#include <jam/kexec.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/pcid.h>
#include <jam/percpu.h>
#include <jam/random.h>
#include <jam/report.h>
#include <jam/resource.h>
#include <jam/sched.h>
#include <jam/selftest.h>
#include <jam/serial.h>
#include <jam/smp.h>
#include <jam/string.h>
#include <jam/sysinfo.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vtd.h>
#include <jam/wallclock.h>
#include <jam/x86.h>

#define JAMOS_VERSION   "0.0.28-m8.6"
#define KERNEL_STACK_SZ (64 * 1024)

_Noreturn void stack_switch_call(void *top, void (*fn)(void *), void *arg);

const char jamos_version[] = JAMOS_VERSION;   /* sys_info (sysc_sysinfo.c) */

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
    case BOOT_MEM_FOREIGN:            return "foreign";
    case BOOT_MEM_CRASH_LOG:          return "crash-log";
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
    kprintf("cpu id:      family %u model %x stepping %u, microcode %x\n", cpu_features.family,
            cpu_features.model, cpu_features.stepping, cpu_features.microcode);
    kprintf("cpu bits:    pcid=%d invpcid=%d pku=%d pks=%d waitpkg=%d cet-ss=%d cet-ibt=%d "
            "uintr=%d rdrand=%d rdseed=%d\n", cpu_features.pcid, cpu_features.invpcid,
            cpu_features.pku, cpu_features.pks, cpu_features.waitpkg, cpu_features.cet_ss,
            cpu_features.cet_ibt, cpu_features.uintr, cpu_features.rdrand, cpu_features.rdseed);
    pcid_report();
    kprintf("loader:      %s, cmdline \"%s\"\n", bi->loader_name, bi->cmdline);
    if (bi->fb.virt)
        kprintf("framebuffer: %ux%u %ubpp pitch %u @ phys %lx\n", bi->fb.width,
                bi->fb.height, bi->fb.bpp, bi->fb.pitch, bi->fb.phys);
    kprintf("kernel:      phys %lx virt %lx\n", bi->kernel_phys_base, bi->kernel_virt_base);
    kprintf("rsdp:        %lx\n", bi->rsdp_phys);
    kprintf("cpus:        %u from loader (bsp lapic %u, %s)\n", bi->cpu_count,
            bi->bsp_lapic_id, bi->x2apic ? "x2APIC" : "xAPIC");
}

static struct boot_info *boot;
static uint32_t boot_disk;   /* boot_disk_id(): set once in kmain_stage2 */

/* The network's mode (ARCHITECTURE.md "Networking"): the VLAN every frame
 * Jam OS sends is tagged with and the only one it receives, or untagged
 * (no frame ever tagged, only untagged ones received). The build chooses
 * the default (the Makefile's JAMOS_NET_DEFAULT: `JAMOS_VLAN` in the
 * git-ignored local.mk, untagged without one), and the boot word `vlan=`
 * can say otherwise. The one place the mode lives: init hears it from the
 * kernel, devmgr from init, each network driver from devmgr. */
#ifndef JAMOS_NET_DEFAULT
#error "JAMOS_NET_DEFAULT comes from the Makefile (local.mk's JAMOS_VLAN, or untagged)"
#endif
#define BOOT_VLAN_DEFAULT JAMOS_NET_DEFAULT
_Static_assert((BOOT_VLAN_DEFAULT >= 1 && BOOT_VLAN_DEFAULT <= 4094) ||
                   BOOT_VLAN_DEFAULT == CMDLINE_VLAN_UNTAGGED,
               "the network's default is a VLAN (1..4094) or untagged");
static uint32_t boot_vlan;   /* the mode, cmdline_vlan's: 0 is off; set once in kmain_stage2 */
static char vlan_word[16];   /* "vlan=<id>" or "vlan=none" for init ("" with the network off) */

uint32_t boot_disk_id(void)
{
    return boot_disk;
}

void kmain_print_memmap(void)
{
    for (size_t i = 0; i < boot->memmap_count; i++) {
        const struct boot_mem_region *r = &boot->memmap[i];
        kprintf("  %016lx - %016lx  %-14s %lu KiB\n", r->base, r->base + r->length,
                mem_type_name(r->type), r->length / 1024);
    }
}

/* "ktest=abc" -> "abc" (up to the next space); NULL if absent. */
static const char *ktest_prefix(void)
{
    static char buf[32];
    const char *p = boot->cmdline;
    for (; *p; p++) {
        if ((p == boot->cmdline || p[-1] == ' ') && !memcmp(p, "ktest=", 6)) {
            size_t n = 0;
            for (p += 6; *p && *p != ' ' && n + 1 < sizeof(buf); p++)
                buf[n++] = *p;
            buf[n] = '\0';
            return buf;
        }
    }
    return NULL;
}

/* Does the command line name a mode: a test, benchmark, report or crash
 * entry, or "init"? A boot without one is a plain boot and starts the
 * shell, whatever options it carries (verbose, nodeadline, ...):
 * an option must never decide what is booted. */
static bool mode_word_given(void)
{
    static const char *const modes[] = { "ktest", "bench", "selftest", "init", "keytest",
                                         "pcilist", "memmap" };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        if (cmdline_has(modes[i]))
            return true;
    if (ktest_prefix() || cmdline_get_u64("stress", 0, 1))
        return true;
    /* test<name>: a crash test at boot. */
    for (const char *p = boot->cmdline; *p; p++) {
        if ((p != boot->cmdline && p[-1] != ' ') || memcmp(p, "test", 4))
            continue;
        size_t n = 0;
        while (p[4 + n] && p[4 + n] != ' ')
            n++;
        if (selftest_crash_known(p + 4, n))
            return true;
    }
    return false;
}

/* The boot splash (bin/splash) plays on a plain boot: the shell mode
 * without the safe mode's `nousb` or the soak test's `soak`, and without
 * the boot words `verbose` or `nosplash`, which show the text log instead.
 * On such a boot fbcon draws no text (quiet) and init gets "splash". */
static bool splash_boot(void)
{
    if (cmdline_has("verbose") || cmdline_has("nosplash") || cmdline_has("nousb") ||
        cmdline_get_u64("soak", 0, 3))
        return false;
    return cmdline_has("shell") || !mode_word_given();
}

/* The timer check (smp_report: every CPU's ticks over 1 s). Test,
 * benchmark and regression entries run it first, synchronously: their
 * RESULTS box starts with it. A shell boot (the everyday one) runs it in
 * a kernel thread next to user space instead, so that second is not spent
 * before init starts; its lines come when it ends, and its result counts
 * only if init ever does. */
static bool timer_check_ok = true;   /* the thread's result: written before it exits */

static void timer_check_thread(void *arg)
{
    (void)arg;
    timer_check_ok = smp_report(1000, true);
}

/* The disk the machine booted from: its MBR id from the loader, or after a
 * kexec from the bootdisk= word (before the stored kernel is armed: its
 * command line carries it). */
static void boot_disk_init(void)
{
    boot_disk = boot->boot_disk_id ? boot->boot_disk_id
                                   : (uint32_t)cmdline_get_u64("bootdisk", 0, 0);
    if (boot_disk)
        kprintf("boot disk:   MBR disk id %08x (%s)\n", boot_disk,
                boot->boot_disk_id ? "from the loader" : "from the kernel before");
    else
        kprintf("boot disk:   no MBR disk id (devmgr takes the first Jam OS disk)\n");
}

/* The network's mode from the boot words (cmdline_vlan's rules). */
static void boot_vlan_init(void)
{
    const char *line = cmdline_get();
    boot_vlan = cmdline_vlan(line, BOOT_VLAN_DEFAULT);
    /* Two defaults give one answer only when a word decided it. */
    const char *from =
        cmdline_vlan(line, 1) == cmdline_vlan(line, 2) ? "the vlan= word" : "the build's default";
    if (boot_vlan == CMDLINE_VLAN_UNTAGGED) {
        ksnprintf(vlan_word, sizeof(vlan_word), "vlan=none");
        kprintf("network:     untagged (%s): no frame is sent tagged\n", from);
    } else if (boot_vlan) {
        ksnprintf(vlan_word, sizeof(vlan_word), "vlan=%u", boot_vlan);
        kprintf("network:     VLAN %u (%s)\n", boot_vlan, from);
    } else {
        kprintf("network:     off (%s): the network stays off\n", from);
    }
}

/* ACPI, the local APIC and the clocks (TSC, the wall clock), the random
 * number generator, then how fast the screen redraws. */
static void clocks_init(void)
{
    acpi_init(boot->rsdp_phys);
    lapic_init_bsp(boot->x2apic);
    tsc_calibrate_with_loader(boot->tsc_hz_loader);
    wallclock_init();   /* the date from the RTC (it waits with udelay: after the TSC) */
    random_init(boot);   /* after the TSC, ACPI and the date, which it mixes in */
    uint64_t redraw_us = fbcon_time_redraw(rdtsc) / (tsc_hz / 1000000);
    if (splash_boot())   /* quiet: nothing was drawn */
        kprintf("fbcon: %ux%u, quiet for the boot splash, mapped %s\n", boot->fb.width,
                boot->fb.height, vmm_cache_type(vmm_kernel_pml4(), (uint64_t)boot->fb.virt));
    else
        kprintf("fbcon: %ux%u, full-screen redraw takes %lu.%03lu ms, mapped %s\n",
                boot->fb.width, boot->fb.height, redraw_us / 1000, redraw_us % 1000,
                vmm_cache_type(vmm_kernel_pml4(), (uint64_t)boot->fb.virt));
}

/* The scheduler (this code becomes thread "main"), interrupts, the timer
 * and the other CPUs; then the next boot's stored kernel. */
static void cpus_init(void)
{
    smp_init_bsp(boot);
    sched_init_bsp();
    ipi_init();
    ioapic_init();
    serial_start_irq();   /* COM1 output from its transmit interrupt */
    lapic_timer_calibrate();
    lapic_timer_start(TICK_HZ);
    smp_start_aps(boot);
    irq_enable();
    kexec_load_stored();   /* the next boot's kernel into its region (kexec.h) */
}

/* The test, benchmark and stress entries the command line names, in
 * their order; false if one failed. */
static bool run_tests(void)
{
    bool ok = true;
#ifdef JAM_NO_KTESTS
    if (cmdline_has("ktest") || ktest_prefix() || cmdline_has("bench")) {
        kprintf("ktest: this kernel was built without tests or benchmarks (make KTESTS=0)\n");
        ok = false;
    }
#else
    if (cmdline_has("ktest") || ktest_prefix()) {
        /* loops=N seed=S shuffle keep load: the same words as the shell's ktest. */
        struct ktest_opts o;
        ktest_parse_opts(boot->cmdline, true, &o);
        ktest_run_opts(&o);
        ok &= ktest_last_failed() == 0;
    }
    if (cmdline_has("bench"))
        bench_run();
#endif
    uint64_t stress_s = cmdline_get_u64("stress", 0, 600);
    if (stress_s)
        ok &= stress_run(stress_s);
    selftest_crash_smp();
    return ok;
}

/* init's option words (at most INIT_WORDS_MAX): "splash" (splash_boot);
 * `hidboot`, which init passes on to devmgr and devmgr to every hid (mice
 * stay in the boot protocol); one of `netprobe`, `netsend` or `net` (in
 * that order of precedence), which init passes on to devmgr and devmgr to
 * the RTL8125's driver: its listen-only probe, its ARP send test, or its
 * netdev service for netstack (no other boot binds the network chip; a
 * reboot keeps `net` alone: kexec_next_cmdline); vlan=<id> or
 * vlan=none (only when the network is on: boot_vlan), which init passes
 * on to devmgr and devmgr to every network driver; bootdisk=0x<id>, which init passes on
 * to devmgr (the boot disk); `splashhang` (a test's: the splash never
 * finishes, and init must start the shell anyway). */
#define INIT_WORDS_MAX 6

static unsigned init_words(bool shell, const char *words[INIT_WORDS_MAX])
{
    unsigned n = 0;
    if (shell && splash_boot())
        words[n++] = "splash";
    if (cmdline_has("hidboot"))
        words[n++] = "hidboot";
    if (cmdline_has("netprobe"))
        words[n++] = "netprobe";
    else if (cmdline_has("netsend"))
        words[n++] = "netsend";
    else if (cmdline_has("net"))
        words[n++] = "net";
    if (boot_vlan)
        words[n++] = vlan_word;
    static char disk_word[24];
    if (boot_disk) {
        ksnprintf(disk_word, sizeof(disk_word), "bootdisk=0x%08x", boot_disk);
        words[n++] = disk_word;
    }
    if (shell && splash_boot() && cmdline_has("splashhang"))
        words[n++] = "splashhang";
    return n;
}

/* User space: init from bootfs, on "init" (init.cfg's programs: utest)
 * or on "shell" or a plain boot (no mode word: mode_word_given): devmgr,
 * the console, serial input and the shell, for good (no timeout; the
 * RESULTS box only comes if init ever ends). "nousb" (the safe mode
 * entry) is shell mode with devmgr leaving USB controllers alone.
 * soak[=minutes] (the Soak test entry) is a plain boot whose shell runs
 * `soak <minutes> halt` by itself: an option, like verbose, not a mode
 * word. The hidden `keytest` boot word: init starts devmgr alone
 * (usb-bus, a hid per HID interface, keys to the log) for 30 s. Test,
 * benchmark and crash entries start none of it. False if init (or
 * keytest's) reported a problem. */
static bool run_user_space(bool shell, bool nousb)
{
    static char soak_arg[16];
    uint64_t soak_min = cmdline_get_u64("soak", 0, 3);
    if (soak_min && !nousb)
        ksnprintf(soak_arg, sizeof(soak_arg), "soak=%lu", soak_min > 600 ? 600 : soak_min);
    const char *mode = nousb ? "shell-nousb" : soak_arg[0] ? soak_arg : "shell";
    const char *words[INIT_WORDS_MAX];
    unsigned nwords = init_words(shell, words);
    /* The regression run (init.cfg's programs, mode "init") gets the vlan=
     * word alone of the words, so its network drivers start as a plain boot's
     * would; the others describe a plain boot's devices and look. */
    const char *const run_words[1] = { vlan_word };
    bool ok = true;
    if (shell)
        ok &= userboot_run_init(0, mode, words, nwords);
    else if (cmdline_has("init"))
        ok &= userboot_run_init(cmdline_get_u64("init_timeout", 300, 300), "init", run_words,
                                boot_vlan ? 1 : 0);
    if (cmdline_has("keytest"))
        ok &= userboot_run_init(90, "keytest", words, nwords);
    return ok;
}

/* The end of a run (in shell mode only if init ended: something went
 * wrong): the RESULTS box, then CPU 0 idles. If the console never took
 * the screen the splash's quiet is still on, and the RESULTS would not be
 * drawn. */
_Noreturn static void finish(bool ok)
{
    fbcon_unquiet();
    sched_print_stats();
    uint64_t dropped = __atomic_load_n(&serial_dropped, __ATOMIC_RELAXED);
    if (dropped || serial_irq_broken())
        report("serial: %lu characters dropped (ring full)%s", dropped,
               serial_irq_broken() ? "; no transmit interrupt, output synchronous" : "");

    report("run %s", ok ? "complete: no problems" : "FINISHED WITH PROBLEMS");
    report_print(JAMOS_VERSION);
    kprintf("Idling.\n");
    thread_exit();   /* CPU 0 falls through to its idle thread */
}

/* Runs on the kernel's own stack, with its own page tables. */
_Noreturn static void kmain_stage2(void *arg)
{
    (void)arg;
    uint64_t total, free;
    pmm_stats(&total, &free);
    kprintf("pmm:         %lu MiB managed, %lu MiB free\n", total >> 8, free >> 8);
    boot_disk_init();
    boot_vlan_init();
    bootfs_init(boot);   /* only needs the heap; before the tests that use it */
    /* After a kexec: the previous kernel's record (did it panic?) and log,
     * before anything could panic into a stored kernel (kexec.h). */
    crashlog_init(boot);

    if (cmdline_has("selftest"))
        selftest_run();
    selftest_crash(boot->cmdline);
    clocks_init();
    cpus_init();

    bool nousb = cmdline_has("nousb");
    bool shell = cmdline_has("shell") || nousb || !mode_word_given();
    bool ok = true;
    struct thread *timer_check = NULL;
    if (shell) {
        kprintf("measuring ticks on every CPU for 1 s, next to user space...\n");
        timer_check = thread_create("timer check", timer_check_thread, NULL, PRIO_DEFAULT);
    } else {
        kprintf("measuring ticks on every CPU for 1 s...\n");
        ok = smp_report(1000, false);
    }
    /* PCI enumeration and the resource tree, once every CPU is online
     * (the vector allocator spreads MSIs over them). */
    pci_init();
    vtd_probe();   /* reads and logs the IOMMU's table and registers; writes nothing */
    resource_init();
    if (cmdline_has("pcilist"))
        pci_report();
    if (cmdline_has("selftest"))
        selftest_run_smp();
    ok &= run_tests();
    ok &= run_user_space(shell, nousb);
    if (timer_check) {
        thread_join(timer_check);
        ok &= timer_check_ok;
    }
    finish(ok);
}

_Noreturn void kmain(struct boot_info *bi)
{
    percpu_set_gs(&cpu0);   /* spinlocks need this_cpu() from here on */
    boot = bi;
    cmdline_set(bi->cmdline);
    bool has_serial = serial_init();
    fbcon_init(&bi->fb, splash_boot());

    fbcon_set_colors(0xffb000, 0x101018);
    kprintf("Jam OS %s\n", JAMOS_VERSION);
    fbcon_set_colors(0xd0d0d0, 0x101018);
    kprintf("serial:      %s\n", has_serial ? "COM1" : "none");

    cpu_detect();
    gdt_init_bsp();
    idt_init();
    print_boot_info(bi);
    kexec_reserve(bi);   /* before the memory managers: the stored kernel's region */
    if (cmdline_has("memmap"))
        kmain_print_memmap();

    pmm_early_init(bi);
    apboot_reserve();   /* the AP trampoline's page, below 640 KiB, before anything else */
    vmm_init(bi);
    pmm_init();
    vmm_use_buddy();
    heap_init();

    /* Loader-reclaimable memory (Limine's stack, page tables, and the code
     * the parked APs are spinning in until INIT resets them) is freed once
     * every AP has started (smp_start_aps). */
    stack_switch_call(kstack_alloc(KERNEL_STACK_SZ), kmain_stage2, NULL);
}
