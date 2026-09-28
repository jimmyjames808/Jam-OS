#pragma once

#include <jam/boot.h>

#define TICK_HZ 100

/* Set up the BSP's per-CPU state (GDT/TSS/GS/LAPIC). */
void smp_init_bsp(const struct boot_info *bi);
/* Release every AP, wait for all of them to come online, then give the
 * loader's memory back to the allocator. */
void smp_start_aps(const struct boot_info *bi);
/* Run fn(arg) on every online CPU (the caller included) and wait for all
 * of them. APs pick it up on their next timer tick. */
void smp_run_on_all(void (*fn)(void *), void *arg);

/* Print one line per CPU with its type and tick count, plus a topology
 * summary. Returns false if any CPU's tick rate is far off. */
bool smp_report(uint64_t window_ms);
