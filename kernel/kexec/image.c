/* kexec's images: a kernel ELF and its bootfs laid out in the region with
 * everything the new kernel needs to start, so a jump copies nothing.
 *
 * The layout, from the region's base (docs/M8.5-PLAN.md has the table):
 *   [0, img_end)            the kernel's segments at base + (vaddr - virt_base),
 *                           the gaps and bss zero; img_end is the span
 *                           rounded up to 2 MiB
 *   [bootfs_off, ...)       bootfs.img; a reboot image also carries
 *                           jamos.elf, so it can load its own crash kernel
 *   [handoff_off, end)      the handoff, KX_TABLES pages of page tables, a
 *                           KX_STACK stack: loader-reclaimable to the new
 *                           kernel, which frees them once its CPUs are up
 *   [end, size)             free: the crash kernel's memory
 *
 * The page tables (built in a kernel buffer, then copied in) map exactly:
 * each segment at its link address with its own permissions, the
 * trampoline at T (region.c), and in the HHDM every RAM range of the new
 * memory map (2 MiB pages where they fit), the framebuffer (4 KiB,
 * write-combining) and a crash kernel's log ring and record (read-only).
 * The first mapping of a page wins, so the 4 KiB ones go first.
 *
 * The new memory map is this kernel's turned into the next one's: for a
 * reboot every RAM range is usable (this kernel is about to stop); for a
 * crash kernel this kernel's RAM is foreign (devices may still hold
 * addresses in it) and the region is its only usable memory. Then the
 * image's pieces are overlaid (memmap.c). */
#include <stddef.h>
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/elf.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/time.h>
#include "kexec_internal.h"

/* Page-table entry bits read back while building (vmm.c makes them). */
#define PTE_PS   (1ull << 7)              /* a 2 MiB leaf in a page directory */
#define PTE_ADDR 0x000ffffffffff000ull    /* the physical address bits */
#define SIZE_2M  (2ull << 20)

struct layout {
    uint64_t virt_base;     /* the kernel's link address: its lowest segment's page */
    uint64_t img_end;       /* the image's span, rounded up to 2 MiB */
    uint64_t bootfs_off;    /* where bootfs.img starts */
    uint64_t kfile_off;     /* where jamos.elf starts (a reboot image) */
    uint64_t handoff_off;   /* the loader-reclaimable part: handoff, tables, stack */
    uint64_t tables_off;
    uint64_t stack_off;
    uint64_t end;           /* the checksum covers [0, end) */
};

/* ---- checks and the layout ------------------------------------------------------- */

static status_t refuse(const char *what, const char *why)
{
    kprintf("kexec: %s refused: %s\n", what, why);
    return ERR_INVALID_ARGS;
}

static status_t lay_out(const struct kx_image *im, struct elf_plan *plan, struct layout *l)
{
    static const struct elf_range kernel_range = { KX_KERNEL_LO, KX_KERNEL_HI };
    const char *why;
    if (elf_check_range(im->kernel, im->kernel_size, &kernel_range, plan, &why) != OK)
        return refuse("the kernel", why);
    if (bootfs_validate(im->bootfs, im->bootfs_size, &why) != OK)
        return refuse("the bootfs image", why);
    const struct elf_segment *last = &plan->seg[plan->nseg - 1];
    l->virt_base = ALIGN_DOWN(plan->seg[0].vaddr, PAGE_SIZE);
    uint64_t span = last->vaddr + last->memsz - l->virt_base;
    if (span > KX_KERNEL_MAX)
        return refuse("the kernel", "its image is bigger than 64 MiB");
    l->img_end = ALIGN_UP(span, SIZE_2M);
    l->bootfs_off = l->img_end;
    l->kfile_off = l->bootfs_off + ALIGN_UP(im->bootfs_size, PAGE_SIZE);
    l->handoff_off = im->crash ? l->kfile_off
                               : l->kfile_off + ALIGN_UP(im->kernel_size, PAGE_SIZE);
    l->tables_off = l->handoff_off + ALIGN_UP(sizeof(struct kexec_handoff), PAGE_SIZE);
    l->stack_off = l->tables_off + (uint64_t)KX_TABLES * PAGE_SIZE;
    l->end = l->stack_off + KX_STACK;
    uint64_t need = l->end + (im->crash ? KX_MIN_FREE : 0);
    if (need > kx.size) {
        kprintf("kexec: the image needs %lu MiB, the region has %lu MiB\n", need >> 20,
                kx.size >> 20);
        return ERR_NO_RESOURCES;
    }
    return OK;
}

/* ---- the memory map ----------------------------------------------------------------- */

/* What this kernel's type t becomes for the next kernel. */
static enum boot_mem_type next_type(enum boot_mem_type t, bool crash)
{
    switch (t) {
    case BOOT_MEM_USABLE:
    case BOOT_MEM_LOADER_RECLAIMABLE:   /* reclaimed by now */
    case BOOT_MEM_KERNEL_AND_MODULES:   /* this kernel, about to stop */
        return crash ? BOOT_MEM_FOREIGN : BOOT_MEM_USABLE;
    case BOOT_MEM_FOREIGN:              /* the region */
    case BOOT_MEM_CRASH_LOG:
        return BOOT_MEM_USABLE;
    default:
        return t;
    }
}

static status_t build_memmap(const struct kx_image *im, const struct layout *l,
                             struct boot_mem_region *map, size_t *n)
{
    const struct boot_info *bi = kx_boot;
    *n = 0;
    for (size_t i = 0; i < bi->memmap_count && *n < KEXEC_MAX_MEMMAP; i++)
        map[(*n)++] = (struct boot_mem_region){ bi->memmap[i].base, bi->memmap[i].length,
                                                next_type(bi->memmap[i].type, im->crash) };
    kx_memmap_merge(map, n);
    const struct { uint64_t base, len; enum boot_mem_type type; } parts[] = {
        { kx.base, l->handoff_off, BOOT_MEM_KERNEL_AND_MODULES },
        { kx.base + l->handoff_off, l->end - l->handoff_off, BOOT_MEM_LOADER_RECLAIMABLE },
        { im->crash ? kx_record_phys() : 0, im->crash ? PAGE_SIZE : 0, BOOT_MEM_CRASH_LOG },
        { im->crash ? kx_kernel_phys(klog_ring()) : 0, im->crash ? KLOG_SIZE : 0,
          BOOT_MEM_CRASH_LOG },
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (!parts[i].len)
            continue;
        status_t st = kx_memmap_overlay(map, n, KEXEC_MAX_MEMMAP, parts[i].base, parts[i].len,
                                        parts[i].type);
        if (st != OK) {
            kprintf("kexec: the next kernel's memory map has more than %u entries\n",
                    KEXEC_MAX_MEMMAP);
            return st;
        }
    }
    kx_memmap_merge(map, n);
    return OK;
}

/* ---- the page tables --------------------------------------------------------------- */

struct tables {
    uint8_t *stage;   /* KX_TABLES zeroed pages in kernel memory; page 0 is the PML4 */
    unsigned n;       /* pages in use */
    uint64_t phys;    /* where page 0 will be in the region */
};

static uint64_t *table(struct tables *t, unsigned i)
{
    return (uint64_t *)(t->stage + (uint64_t)i * PAGE_SIZE);
}

/* The entry for va at `level` (2: page directory, 1: page table), making
 * the tables above it. NULL: out of pages, or a 2 MiB leaf is in the way
 * (level 1 only; *large then says so). */
static uint64_t *slot(struct tables *t, uint64_t va, int level, bool *large)
{
    uint64_t *tab = table(t, 0);
    *large = false;
    for (int l = 4; l > level; l--) {
        uint64_t *e = &tab[(va >> (12 + 9 * (l - 1))) & 511];
        if (!*e) {
            if (t->n == KX_TABLES)
                return NULL;
            *e = vmm_pte_table(t->phys + (uint64_t)t->n * PAGE_SIZE);
            tab = table(t, t->n++);
        } else if (*e & PTE_PS) {
            *large = true;
            return NULL;
        } else {
            tab = table(t, (unsigned)(((*e & PTE_ADDR) - t->phys) / PAGE_SIZE));
        }
    }
    return &tab[(va >> (12 + 9 * (level - 1))) & 511];
}

/* Map [va, va + len) to pa (both page-aligned) with VM_* flags; 2 MiB
 * pages where allowed and possible. A page mapped already keeps its
 * mapping. */
static status_t map_range(struct tables *t, uint64_t va, uint64_t pa, uint64_t len,
                          unsigned flags, bool allow_large)
{
    while (len) {
        bool large;
        uint64_t *e = slot(t, va, 2, &large);
        if (!e)
            return ERR_NO_RESOURCES;
        uint64_t step = SIZE_2M - (va & (SIZE_2M - 1));
        if (step > len)
            step = len;
        if (allow_large && !*e && step == SIZE_2M && !(pa & (SIZE_2M - 1))) {
            *e = vmm_pte(pa, flags, true);
        } else if (!(*e & PTE_PS)) {
            for (uint64_t off = 0; off < step; off += PAGE_SIZE) {
                uint64_t *p = slot(t, va + off, 1, &large);
                if (!p)
                    return ERR_NO_RESOURCES;
                if (!*p)
                    *p = vmm_pte(pa + off, flags, false);
            }
        }   /* else a 2 MiB page is there already */
        va += step;
        pa += step;
        len -= step;
    }
    return OK;
}

static status_t map_hhdm(struct tables *t, uint64_t base, uint64_t end, unsigned flags,
                         bool allow_large)
{
    base = ALIGN_DOWN(base, PAGE_SIZE);
    end = ALIGN_UP(end, PAGE_SIZE);
    return map_range(t, base + hhdm_offset, base, end - base, flags, allow_large);
}

/* The RAM of the new map in the HHDM: runs of touching RAM ranges
 * (whatever their types) as one, so only real holes cost a page table. */
static status_t map_ram(struct tables *t, const struct boot_mem_region *map, size_t n)
{
    status_t st = OK;
    for (size_t i = 0; i < n && st == OK;) {
        if (!boot_mem_is_ram(map[i].type)) {
            i++;
            continue;
        }
        uint64_t base = map[i].base, end = base + map[i].length;
        for (i++; i < n && boot_mem_is_ram(map[i].type) && map[i].base == end; i++)
            end = map[i].base + map[i].length;
        st = map_hhdm(t, base, end, VM_WRITE, true);
    }
    return st;
}

static status_t map_segments(struct tables *t, const struct elf_plan *plan,
                             const struct layout *l)
{
    status_t st = OK;
    for (unsigned i = 0; i < plan->nseg && st == OK; i++) {
        const struct elf_segment *s = &plan->seg[i];
        uint64_t va = ALIGN_DOWN(s->vaddr, PAGE_SIZE);
        uint64_t end = ALIGN_UP(s->vaddr + s->memsz, PAGE_SIZE);
        unsigned flags = (s->flags & ASPACE_WRITE ? VM_WRITE : 0) |
                         (s->flags & ASPACE_EXEC ? VM_EXEC : 0);
        st = map_range(t, va, kx.base + (va - l->virt_base), end - va, flags, false);
    }
    return st;
}

static status_t build_tables(struct tables *t, const struct kx_image *im,
                             const struct elf_plan *plan, const struct layout *l,
                             const struct boot_mem_region *map, size_t n)
{
    const struct boot_framebuffer *fb = &kx_boot->fb;
    t->n = 1;   /* the PML4 */
    t->phys = kx.base + l->tables_off;
    status_t st = map_range(t, kx_tramp_va(), kx_tramp_phys(), PAGE_SIZE, VM_EXEC, false);
    if (st == OK && im->crash)
        st = map_hhdm(t, kx_record_phys(), kx_record_phys() + PAGE_SIZE, 0, false);
    if (st == OK && im->crash)
        st = map_hhdm(t, kx_kernel_phys(klog_ring()), kx_kernel_phys(klog_ring()) + KLOG_SIZE,
                      0, false);
    if (st == OK && fb->virt)
        st = map_hhdm(t, fb->phys, fb->phys + (uint64_t)fb->pitch * fb->height,
                      VM_WRITE | VM_WC, false);
    if (st == OK)
        st = map_segments(t, plan, l);
    if (st == OK)
        st = map_ram(t, map, n);
    if (st != OK)
        kprintf("kexec: the next kernel's page tables need more than %u pages\n", KX_TABLES);
    return st;
}

/* ---- the handoff -------------------------------------------------------------------- */

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void fill_handoff(struct kexec_handoff *h, const struct kx_image *im,
                         const struct layout *l, const struct boot_mem_region *map, size_t n)
{
    const struct boot_info *bi = kx_boot;
    h->magic = KEXEC_HANDOFF_MAGIC;
    h->version = KEXEC_HANDOFF_VERSION;
    h->size = sizeof(*h);
    h->flags = im->crash ? KEXEC_ONE_CPU : 0;
    h->x2apic = bi->x2apic ? 1 : 0;
    h->hhdm_offset = hhdm_offset;
    h->kernel_phys_base = kx.base;
    h->kernel_virt_base = l->virt_base;
    h->rsdp_phys = bi->rsdp_phys;
    h->tsc_hz = tsc_hz ? tsc_hz : bi->tsc_hz_loader;
    if (bi->fb.virt)
        h->fb = (struct kexec_fb){
            .phys = bi->fb.phys, .width = bi->fb.width, .height = bi->fb.height,
            .pitch = bi->fb.pitch, .bpp = bi->fb.bpp, .red_shift = bi->fb.red_shift,
            .green_shift = bi->fb.green_shift, .blue_shift = bi->fb.blue_shift,
        };
    for (uint32_t i = 0; i < bi->cpu_count && i < KEXEC_MAX_CPUS; i++)
        h->cpus[h->cpu_count++] = (struct kexec_cpu){ bi->cpus[i].acpi_uid,
                                                      bi->cpus[i].lapic_id };
    for (size_t i = 0; i < n; i++)
        h->memmap[h->memmap_count++] = (struct kexec_mem){ map[i].base, map[i].length,
                                                           kexec_mem_to_wire(map[i].type), 0 };
    struct kexec_module *m = &h->modules[h->module_count++];
    *m = (struct kexec_module){ .phys = kx.base + l->bootfs_off, .size = im->bootfs_size };
    copy_str(m->path, KEXEC_STR, "kexec:/boot/" BOOTFS_MODULE);
    if (!im->crash) {
        m = &h->modules[h->module_count++];
        *m = (struct kexec_module){ .phys = kx.base + l->kfile_off, .size = im->kernel_size };
        copy_str(m->path, KEXEC_STR, "kexec:/boot/" KEXEC_KERNEL_MODULE);
    }
    copy_str(h->loader_name, KEXEC_STR, im->crash ? "Jam OS kexec (crash kernel)" : "Jam OS kexec");
    copy_str(h->cmdline, KEXEC_CMDLINE, im->cmdline);
    h->checksum = kexec_struct_sum(h, sizeof(*h), offsetof(struct kexec_handoff, checksum));
}

/* ---- writing it --------------------------------------------------------------------- */

static void write_image(const struct kx_image *im, const struct elf_plan *plan,
                        const struct layout *l, const struct kexec_handoff *h,
                        const struct tables *t)
{
    kx_zero(0, l->img_end);
    for (unsigned i = 0; i < plan->nseg; i++) {
        const struct elf_segment *s = &plan->seg[i];
        kx_write(s->vaddr - l->virt_base, (const uint8_t *)im->kernel + s->file_off, s->filesz);
    }
    kx_write(l->bootfs_off, im->bootfs, im->bootfs_size);
    if (!im->crash)
        kx_write(l->kfile_off, im->kernel, im->kernel_size);
    kx_write(l->handoff_off, h, sizeof(*h));
    kx_write(l->tables_off, t->stage, (uint64_t)KX_TABLES * PAGE_SIZE);
    kx_zero(l->stack_off, KX_STACK);
}

status_t kx_build(const struct kx_image *im)
{
    struct elf_plan plan;
    struct layout l;
    status_t st = lay_out(im, &plan, &l);
    if (st != OK)
        return st;
    struct boot_mem_region *map = kmalloc(KEXEC_MAX_MEMMAP * sizeof(*map));
    struct kexec_handoff *h = kzalloc(sizeof(*h));
    struct tables t = { .stage = kzalloc((uint64_t)KX_TABLES * PAGE_SIZE) };
    size_t n = 0;
    st = map && h && t.stage ? OK : ERR_NO_MEMORY;
    if (st == OK)
        st = build_memmap(im, &l, map, &n);
    if (st == OK)
        st = build_tables(&t, im, &plan, &l, map, n);
    if (st == OK) {
        fill_handoff(h, im, &l, map, n);
        write_image(im, &plan, &l, h, &t);
        kx.loaded = l.end;
        kx.sum = kx_sum_region(false);
        kx.tramp_sum = kx_tramp_sum();
        kx.cr3 = kx.base + l.tables_off;
        kx.entry = plan.entry;
        kx.handoff = hhdm_offset + kx.base + l.handoff_off;
        kx.stack_top = hhdm_offset + kx.base + l.stack_off + KX_STACK;
        kprintf("kexec: %s loaded: %lu KiB of %lu MiB (%u page tables, %zu memory ranges), "
                "checksum %016lx\n", im->crash ? "crash kernel" : "reboot image", l.end >> 10,
                kx.size >> 20, t.n, n, kx.sum);
    }
    kfree(map);
    kfree(h);
    kfree(t.stage);
    return st;
}
