/* The kernel's own startup of the application processors, so the kernel
 * needs no boot loader to wake them (after a kexec there is none). It is
 * the default on every boot; `smp=loader` has Limine release its parked
 * CPUs instead (smp.c). The design, with its reasons, is in
 * docs/history/M8.5-AP-STARTUP.md.
 *
 * The protocol is the Intel SDM's (vol. 3A, 9.4.4.1 "Typical BSP
 * Initialization Sequence"): INIT, 10 ms, SIPI, 200 us, SIPI, 200 us. It
 * is sent to each CPU of the CPU list by its APIC ID, never broadcast, so
 * CPUs the firmware disabled stay asleep; the three passes go to every AP
 * in turn, so all of them start at once and the whole startup takes about
 * 10.5 ms whatever the count.
 *
 * Memory: one page in [64 KiB, 640 KiB) for the trampoline, taken out of
 * the memory map at early boot (apboot_reserve) and never given back; four
 * pages below 4 GiB for the transition page table (the trampoline's page
 * identity-mapped, plus the kernel half), freed once every AP is in or has
 * been stopped.
 *
 * A late CPU: an AP the BSP gave up on is sent INIT (apboot_stop), which
 * stops it wherever it is. Before its claim (smp.c) an AP writes nothing
 * but its own registers and struct cpu, so being stopped half way breaks
 * nothing, and the tables are freed only after the INIT has landed. */
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/smp.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>
#include "smp_internal.h"

/* Where the trampoline may live: a SIPI vector names a page below 1 MiB;
 * 640 KiB up is the VGA hole and ROMs (and vectors A0h-BFh are reserved);
 * below 64 KiB some firmware scribbles (Linux keeps it reserved too). */
#define TRAMP_FLOOR (64 * 1024)
#define TRAMP_LIMIT (640 * 1024)

/* SDM vol. 3A 9.4.4.1: after the INIT, and after each SIPI. */
#define INIT_DELAY_US 10000
#define SIPI_DELAY_US 200

/* Paging bits for the transition table (vmm.c has the whole set). */
#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_ADDR 0x000ffffffffff000ull
#define EFER_LME (1ull << 8)

#define TABLES 4   /* PML4, PDPT, PD, PT */

extern const char ap_tramp_start[], ap_tramp_end[], ap_park_start[], ap_park_end[];
void ap_boot64(void);
_Noreturn void ap_boot_main(struct ap_slot *s);

/* Read by ap_boot64 (trampoline.S). */
struct ap_slot ap_slots[MAX_CPUS];
uint32_t ap_slot_count;
uint64_t ap_boot_cr3;

static uint64_t tramp_pa;          /* the trampoline page, 0 if the memory map had none */
static uint64_t tables[TABLES];    /* the transition page table's pages, 0 = none */
static bool used;                  /* the startup ran on this boot */

void apboot_reserve(void)
{
    tramp_pa = pmm_early_alloc_low(TRAMP_FLOOR, TRAMP_LIMIT);
}

uint64_t smp_trampoline_page(void)
{
    return tramp_pa;
}

bool smp_own_startup(void)
{
    return used;
}

uint32_t smp_trampoline_tables(void)
{
    uint32_t n = 0;
    for (int i = 0; i < TABLES; i++)
        n += tables[i] != 0;
    return n;
}

static void free_tables(void)
{
    for (int i = 0; i < TABLES; i++) {
        if (tables[i])
            pmm_free_page_phys(tables[i]);
        tables[i] = 0;
    }
}

/* The PML4 must be below 4 GiB (the trampoline loads CR3 in 32-bit mode);
 * the other three are DMA32 too, for simplicity. The kernel half is shared
 * with the kernel's PML4 the way every address space shares it: its PDPTs
 * exist from vmm_init on and are never freed. */
static bool build_tables(void)
{
    for (int i = 0; i < TABLES; i++) {
        tables[i] = pmm_alloc_page_phys(PMM_ZERO | PMM_DMA32);
        if (!tables[i]) {
            free_tables();
            return false;
        }
    }
    uint64_t *pml4 = phys_to_virt(tables[0]);
    const uint64_t *kernel = phys_to_virt(vmm_kernel_pml4());
    for (int i = 256; i < 512; i++)
        pml4[i] = kernel[i];
    /* Exactly one 4 KiB page identity-mapped, read-only and executable. */
    uint64_t va = tramp_pa;
    pml4[(va >> 39) & 511] = tables[1] | PTE_P | PTE_W;
    ((uint64_t *)phys_to_virt(tables[1]))[(va >> 30) & 511] = tables[2] | PTE_P | PTE_W;
    ((uint64_t *)phys_to_virt(tables[2]))[(va >> 21) & 511] = tables[3] | PTE_P | PTE_W;
    ((uint64_t *)phys_to_virt(tables[3]))[(va >> 12) & 511] = (tramp_pa & PTE_ADDR) | PTE_P;
    return true;
}

static void put32(uint8_t *p, uint32_t off, uint32_t v)
{
    memcpy(p + off, &v, sizeof(v));
}

static uint32_t get32(const uint8_t *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

/* Copy the blob and relocate it to the page. */
static void write_trampoline(void)
{
    uint8_t *p = phys_to_virt(tramp_pa);
    memcpy(p, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));
    uint32_t base = (uint32_t)tramp_pa;
    put32(p, TR_GDTR + 2, get32(p, TR_GDTR + 2) + base);
    put32(p, TR_FAR32, get32(p, TR_FAR32) + base);
    put32(p, TR_FAR64, get32(p, TR_FAR64) + base);
    put32(p, TR_CR3, (uint32_t)tables[0]);
    put32(p, TR_EFER, (uint32_t)(EFER_LME | (cpu_features.nx ? EFER_NXE : 0)));
    uint64_t entry = (uint64_t)ap_boot64;
    memcpy(p + TR_ENTRY, &entry, sizeof(entry));
}

bool apboot_prepare(struct cpu *const *aps, uint32_t n)
{
    _Static_assert(TR_END <= PAGE_SIZE, "the trampoline fits its page");
    if (!tramp_pa) {
        kprintf("smp: no free page in [%u KiB, %u KiB) for the AP trampoline\n",
                TRAMP_FLOOR / 1024, TRAMP_LIMIT / 1024);
        return false;
    }
    if (!build_tables()) {
        kprintf("smp: no memory below 4 GiB for the AP trampoline's page table\n");
        return false;
    }
    ap_boot_cr3 = vmm_kernel_pml4();
    write_trampoline();
    for (uint32_t i = 0; i < n; i++)
        ap_slots[i] = (struct ap_slot){ .apic_id = aps[i]->lapic_id,
                                        .stack_top = aps[i]->kstack_top, .cpu = aps[i] };
    /* Release: an AP reads the slots after its SIPI, and the ICR write
     * comes after an mfence (lapic.c), so they are visible by then. */
    __atomic_store_n(&ap_slot_count, n, __ATOMIC_RELEASE);
    used = true;
    kprintf("smp: AP trampoline at %lx (SIPI vector %02lx), transition PML4 at %lx\n",
            tramp_pa, tramp_pa >> PAGE_SHIFT, tables[0]);
    return true;
}

/* xAPIC addresses only 8-bit IDs, and FFh is the broadcast. */
static bool reachable(const struct cpu *c)
{
    return lapic_x2apic() || c->lapic_id < 0xff;
}

static void send_pass(struct cpu *const *aps, uint32_t n, uint32_t skip, bool init)
{
    for (uint32_t i = 0; i < n; i++) {
        const struct cpu *c = aps[i];
        if (c->index == skip || !reachable(c))
            continue;
        bool ok = init ? lapic_send_init(c->lapic_id)
                       : lapic_send_sipi(c->lapic_id, (uint8_t)(tramp_pa >> PAGE_SHIFT));
        if (!ok)
            kprintf("smp: %s to lapic %u: the APIC never reported it sent\n",
                    init ? "INIT" : "SIPI", c->lapic_id);
    }
}

void apboot_kick(struct cpu *const *aps, uint32_t n, uint32_t skip)
{
    for (uint32_t i = 0; i < n; i++)
        if (!reachable(aps[i]))
            kprintf("smp: lapic %u is beyond xAPIC's 8-bit IDs: not started\n",
                    aps[i]->lapic_id);
    if (skip)
        kprintf("smp: test: no startup IPIs for cpu %u\n", skip);
    (void)lapic_read_esr();   /* start from a clean error status */
    send_pass(aps, n, skip, true);
    udelay(INIT_DELAY_US);
    send_pass(aps, n, skip, false);
    udelay(SIPI_DELAY_US);
    /* An AP the first SIPI started is no longer waiting for one, so the
     * second is ignored there; it is for an AP that missed the first. */
    send_pass(aps, n, skip, false);
    udelay(SIPI_DELAY_US);
    uint32_t esr = lapic_read_esr();
    if (esr)
        kprintf("smp: APIC error status %x after the startup IPIs\n", esr);
}

void apboot_stop(const struct cpu *c)
{
    if (used && reachable(c) && !lapic_send_init(c->lapic_id))
        kprintf("smp: INIT to stop lapic %u: the APIC never reported it sent\n", c->lapic_id);
}

void apboot_finish(bool stopped_any)
{
    if (!used)
        return;
    /* INIT acts at once, but nothing tells the BSP when (x2APIC has no
     * delivery status): give it the SDM's INIT delay before the memory a
     * stopped CPU might still be reading goes away. */
    if (stopped_any)
        udelay(INIT_DELAY_US);
    uint8_t *p = phys_to_virt(tramp_pa);
    memset(p, 0, PAGE_SIZE);
    memcpy(p, ap_park_start, (size_t)(ap_park_end - ap_park_start));
    free_tables();
    __atomic_store_n(&ap_slot_count, 0, __ATOMIC_RELEASE);
    memset(ap_slots, 0, sizeof(ap_slots));
}

/* ap_boot64 has loaded the kernel's page table and this CPU's kernel
 * stack. The rest of the BSP's paging setup (WP, PGE, the PAT; NXE is on
 * already), then the bring-up the Limine path shares. */
_Noreturn void ap_boot_main(struct ap_slot *s)
{
    struct cpu *c = s->cpu;
    cpu_enable_paging_features();
    smp_ap_main(c);
}

bool smp_trampoline_parked(void)
{
    if (!tramp_pa)
        return false;
    return !memcmp(phys_to_virt(tramp_pa), ap_park_start,
                   (size_t)(ap_park_end - ap_park_start));
}
