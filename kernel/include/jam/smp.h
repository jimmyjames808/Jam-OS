/* Bringing up the other CPUs (arch/x86_64/smp.c, apboot.c): the BSP's own
 * per-CPU state first, then every AP, started by the kernel itself with
 * INIT-SIPI-SIPI (or, with the boot word `smp=loader`, released from
 * where Limine parked it). */
#pragma once

#include <stdbool.h>
#include <jam/boot.h>

#define TICK_HZ 100

/* Set up the BSP's per-CPU state (GDT/TSS/GS/LAPIC). */
void smp_init_bsp(const struct boot_info *bi);
/* Early boot, after pmm_early_init and before pmm_init: take the AP
 * trampoline's page (below 640 KiB) out of the memory map for good. */
void apboot_reserve(void);
/* Start every AP, wait (bounded) for all of them to come online, stop and
 * report any that did not, then give the loader's memory back to the
 * allocator if every CPU came up. */
void smp_start_aps(const struct boot_info *bi);
/* Print one line per CPU with its type and tick count, plus a topology
 * summary. Returns false if any CPU's tick rate is far off. */
bool smp_report(uint64_t window_ms);

/* For the tests: the trampoline's page (0 if the memory map had none),
 * whether this boot started the APs with it, how many of its transition
 * page-table pages are still allocated (0 after the startup), and whether
 * the page now holds the halt stub. */
uint64_t smp_trampoline_page(void);
bool     smp_own_startup(void);
uint32_t smp_trampoline_tables(void);
bool     smp_trampoline_parked(void);
