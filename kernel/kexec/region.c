/* kexec's region: reserved at boot, unmapped, written only through a
 * window, checksummed.
 *
 * The reservation (kexec_reserve) runs on the loader's memory map before
 * the memory managers exist: the highest 2 MiB-aligned range of the asked
 * size below 4 GiB (the crash kernel's drivers need DMA32 memory) and
 * above 16 MiB, cut out of one usable entry and made BOOT_MEM_FOREIGN. So
 * the PMM never sees it, the HHDM skips it (vmm_init), and the vmemmap
 * has no pages for it; only pmm_range_has_ram still calls it RAM, so no
 * MMIO resource or physical VMO can ever cover it.
 *
 * The window is 2 MiB of the vmap area whose page table is made once
 * (kx_window_init). In thread context (kx_lock held) a piece of the region
 * is mapped there with vmm_map, used and unmapped with vmm_unmap, whose
 * TLB shootdown leaves no CPU a stale translation into the region. The
 * panic path can't take vmm's lock, so it stores the window's entries
 * itself (read-only) and flushes them with INVLPG: the other CPUs are
 * halted by then, and the entries are global, so the flush reaches them
 * whatever PCID this CPU runs with.
 *
 * The trampoline (tramp.S) is one page of kernel text. Besides its link
 * address it is mapped read-execute at T in the vmap area, and every image
 * maps the same page at T too, so the CR3 switch happens on a page mapped
 * the same in both page tables. Its HHDM alias is read-only (kernel
 * image), so W^X holds for all three. */
#include <jam/cmdline.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/x86.h>
#include "kexec_internal.h"

#define FOUR_GIB  (4ull << 30)
#define LOW_KEEP  (16ull << 20)   /* the region never starts below this */
#define MIB_MIN   32u
#define MIB_MAX   1024u
#define WIN_PAGES (KX_WINDOW / PAGE_SIZE)

struct kx_region kx;
int kx_state = KX_OFF;
struct mutex kx_lock;
const struct boot_info *kx_boot;

extern char kexec_tramp[];   /* tramp.S: the page */

static uint64_t window_va;    /* 2 MiB aligned */
static uint64_t *window_pte;  /* its page table's 512 entries */
static uint64_t tramp_va;     /* T */

/* ---- the reservation ---------------------------------------------------------- */

/* The highest 2 MiB-aligned start of `size` bytes inside a usable entry,
 * below 4 GiB and above LOW_KEEP; 0 if none. */
static uint64_t pick(const struct boot_info *bi, uint64_t size)
{
    uint64_t best = 0;
    for (size_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_region *r = &bi->memmap[i];
        if (r->type != BOOT_MEM_USABLE)
            continue;
        uint64_t lo = ALIGN_UP(r->base > LOW_KEEP ? r->base : LOW_KEEP, KX_WINDOW);
        uint64_t end = r->base + r->length;
        uint64_t hi = ALIGN_DOWN(end < FOUR_GIB ? end : FOUR_GIB, KX_WINDOW);
        if (hi > lo && hi - lo >= size && hi - size > best)
            best = hi - size;
    }
    return best;
}

void kexec_reserve(struct boot_info *bi)
{
    kx_boot = bi;
    mutex_init(&kx_lock, "kexec");
    if (kexec_is_crash_kernel())
        return;   /* a crash kernel never starts another */
    uint64_t mib = cmdline_get_u64("crashkernel", KEXEC_DEFAULT_MIB, KEXEC_DEFAULT_MIB);
    if (!mib) {
        kprintf("kexec:       off (crashkernel=0): no crash kernel, no kexec reboot\n");
        return;
    }
    mib = mib < MIB_MIN ? MIB_MIN : mib > MIB_MAX ? MIB_MAX : mib;
    uint64_t size = ALIGN_UP(mib << 20, KX_WINDOW);
    uint64_t base = pick(bi, size);
    if (!base) {
        kprintf("kexec:       no %lu MiB free below 4 GiB: no crash kernel\n", mib);
        return;
    }
    if (kx_memmap_overlay(bi->memmap, &bi->memmap_count, BOOT_MAX_MEMMAP, base, size,
                          BOOT_MEM_FOREIGN) != OK) {
        kprintf("kexec:       the memory map is full: no crash kernel\n");
        return;
    }
    kx.base = base;
    kx.size = size;
    __atomic_store_n(&kx_state, KX_EMPTY, __ATOMIC_RELEASE);
    kprintf("kexec:       %lu MiB at %lx-%lx reserved for the crash kernel, unmapped\n",
            size >> 20, base, base + size);
}

bool kexec_region(uint64_t *base, uint64_t *size)
{
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) == KX_OFF)
        return false;
    *base = kx.base;
    *size = kx.size;
    return true;
}

bool kexec_is_crash_kernel(void)
{
    return cmdline_has("crash");
}

/* ---- the window ------------------------------------------------------------------- */

void kx_window_init(void)
{
    /* Twice the window, so a 2 MiB-aligned one fits inside. */
    window_va = ALIGN_UP(vmm_reserve(2 * KX_WINDOW), KX_WINDOW);
    window_pte = vmm_kernel_ptes(window_va);
    tramp_va = vmm_reserve(PAGE_SIZE);
    vmm_map(vmm_kernel_pml4(), tramp_va, kx_tramp_phys(), PAGE_SIZE,
            VM_EXEC | VM_GLOBAL | VM_SMALL);
}

/* The window over the 2 MiB of the region holding byte `off`; returns
 * where `off` is in it. Unmap with window_close. */
static uint8_t *window_open(uint64_t off, bool write)
{
    uint64_t pa = ALIGN_DOWN(kx.base + off, KX_WINDOW);
    vmm_map(vmm_kernel_pml4(), window_va, pa, KX_WINDOW,
            (write ? VM_WRITE : 0) | VM_GLOBAL | VM_SMALL);
    return (uint8_t *)window_va + (kx.base + off - pa);
}

static void window_close(void)
{
    vmm_unmap(vmm_kernel_pml4(), window_va, KX_WINDOW);
}

/* The panic path's window: read-only, entries stored directly. */
static const uint8_t *window_open_nolock(uint64_t off)
{
    uint64_t pa = ALIGN_DOWN(kx.base + off, KX_WINDOW);
    for (uint64_t i = 0; i < WIN_PAGES; i++) {
        window_pte[i] = vmm_pte(pa + i * PAGE_SIZE, VM_GLOBAL, false);
        invlpg(window_va + i * PAGE_SIZE);
    }
    return (const uint8_t *)window_va + (kx.base + off - pa);
}

static void window_close_nolock(void)
{
    for (uint64_t i = 0; i < WIN_PAGES; i++) {
        window_pte[i] = 0;
        invlpg(window_va + i * PAGE_SIZE);
    }
}

/* Bytes from off to the end of its window, at most len. */
static uint64_t in_window(uint64_t off, uint64_t len)
{
    uint64_t left = KX_WINDOW - ((kx.base + off) & (KX_WINDOW - 1));
    return len < left ? len : left;
}

void kx_write(uint64_t off, const void *src, uint64_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t n = in_window(off, len);
        uint8_t *d = window_open(off, true);
        if (s)
            memcpy(d, s, n);
        else
            memset(d, 0, n);
        window_close();
        off += n;
        len -= n;
        if (s)
            s += n;
    }
}

void kx_zero(uint64_t off, uint64_t len)
{
    kx_write(off, NULL, len);
}

uint64_t kx_sum_region(bool nolock)
{
    uint64_t h = KEXEC_SUM_SEED;
    for (uint64_t off = 0; off < kx.loaded;) {
        uint64_t n = in_window(off, kx.loaded - off);
        const uint8_t *p = nolock ? window_open_nolock(off) : window_open(off, false);
        h = kexec_checksum(p, n, h);
        if (nolock)
            window_close_nolock();
        else
            window_close();
        off += n;
    }
    return h;
}

/* ---- the trampoline and the kernel image -------------------------------------- */

uint64_t kx_kernel_phys(const void *va)
{
    return (uint64_t)va - kx_boot->kernel_virt_base + kx_boot->kernel_phys_base;
}

uint64_t kx_tramp_va(void)
{
    return tramp_va;
}

uint64_t kx_tramp_phys(void)
{
    return kx_kernel_phys(kexec_tramp);
}

uint64_t kx_tramp_sum(void)
{
    return kexec_checksum((const void *)tramp_va, PAGE_SIZE, KEXEC_SUM_SEED);
}

/* ---- tests ---------------------------------------------------------------------------- */

bool kexec_verify(void)
{
    mutex_lock(&kx_lock);
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    bool ok = (s == KX_ARMED || s == KX_IMAGE) && kx_sum_region(false) == kx.sum &&
              kx_tramp_sum() == kx.tramp_sum;
    mutex_unlock(&kx_lock);
    return ok;
}

status_t kexec_test_corrupt(void)
{
    mutex_lock(&kx_lock);
    status_t st = ERR_BAD_STATE;
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) == KX_ARMED) {
        /* A byte of the new kernel's text (its first segments start
         * the region): the kind of damage a wild write would do. */
        uint8_t *p = window_open(2 * PAGE_SIZE + 123, true);
        *p ^= 0x5a;
        window_close();
        st = OK;
    }
    mutex_unlock(&kx_lock);
    return st;
}
