/* Bringing up the other CPUs (arch/x86_64/smp.c): the BSP's own per-CPU
 * state first, then every AP released through Limine's MP request. */
#pragma once

#include <jam/boot.h>

#define TICK_HZ 100

/* Set up the BSP's per-CPU state (GDT/TSS/GS/LAPIC). */
void smp_init_bsp(const struct boot_info *bi);
/* Release every AP, wait for all of them to come online, then give the
 * loader's memory back to the allocator. */
void smp_start_aps(const struct boot_info *bi);
/* Print one line per CPU with its type and tick count, plus a topology
 * summary. Returns false if any CPU's tick rate is far off. */
bool smp_report(uint64_t window_ms);
