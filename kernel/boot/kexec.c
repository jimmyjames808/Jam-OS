/* The second filler of struct boot_info (limine.c is the first): a kernel
 * started by another Jam OS kernel (kexec, kernel/kexec/) reads the
 * handoff (<jam/kexec_handoff.h>) instead of Limine's responses.
 *
 * _start (arch/x86_64/entry.S) comes here when rdi holds
 * KEXEC_ENTRY_MAGIC, with rsi pointing at the handoff. The handoff crosses
 * from one kernel build to another, so it is checked completely before
 * anything in it is used (kexec_handoff_check); a handoff that fails
 * resets the machine (an empty IDT and a breakpoint: a triple fault),
 * since without its framebuffer and memory map there is nothing to print
 * with, and a firmware reboot beats a silent hang.
 *
 * Everything is copied into the static boot_info, as limine.c does: the
 * handoff's pages are loader-reclaimable and freed once the other CPUs
 * are up. No CPU has a loader handle: the kernel's own AP startup
 * (INIT-SIPI-SIPI) starts them, whichever CPU entered. The crash record
 * the handoff points at is read later, with the heap up (crashlog.c). */
#include <stddef.h>
#include <jam/boot.h>
#include <jam/kexec_handoff.h>
#include <jam/string.h>
#include <jam/x86.h>

#define MSR_X2APIC_ID 0x802
#define UPPER_HALF    0xffff800000000000ull

_Static_assert(KEXEC_MAX_CPUS <= BOOT_MAX_CPUS && KEXEC_MAX_MEMMAP <= BOOT_MAX_MEMMAP &&
                   KEXEC_MAX_MODULES <= BOOT_MAX_MODULES,
               "a valid handoff fits struct boot_info");

_Noreturn void kexec_entry(const struct kexec_handoff *h);

static struct boot_info bi;

uint64_t kexec_checksum(const void *p, uint64_t len, uint64_t seed)
{
    const uint8_t *b = p;
    uint64_t h = seed;
    for (uint64_t i = 0; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, sizeof(w));
        /* Each step is a bijection of h for a given word (xor, an odd
         * multiplier, an xor-shift), so a changed word always changes the
         * result. */
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

uint64_t kexec_struct_sum(const void *p, uint64_t len, uint64_t at)
{
    static const uint64_t zero;
    uint64_t s = kexec_checksum(p, at, KEXEC_SUM_SEED);
    s = kexec_checksum(&zero, sizeof(zero), s);
    return kexec_checksum((const uint8_t *)p + at + 8, len - at - 8, s);
}

static uint64_t handoff_sum(const struct kexec_handoff *h)
{
    return kexec_struct_sum(h, sizeof(*h), offsetof(struct kexec_handoff, checksum));
}

/* The length of s, or cap if there is no NUL in s[0..cap). */
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n])
        n++;
    return n;
}

static bool terminated(const char *s, size_t cap)
{
    return bounded_len(s, cap) < cap;
}

static const char *check_memmap(const struct kexec_handoff *h)
{
    for (uint32_t i = 0; i < h->memmap_count; i++) {
        const struct kexec_mem *m = &h->memmap[i];
        enum boot_mem_type t;
        if (!kexec_mem_from_wire(m->type, &t))
            return "a memory range of an unknown type";
        if (m->reserved || m->base + m->length < m->base)
            return "a bad memory range";
    }
    return NULL;
}

static const char *check_modules(const struct kexec_handoff *h)
{
    for (uint32_t i = 0; i < h->module_count; i++) {
        const struct kexec_module *m = &h->modules[i];
        if (m->phys + m->size < m->phys)
            return "a module wraps around";
        if (!terminated(m->path, KEXEC_STR) || !terminated(m->string, KEXEC_STR))
            return "a module's path or string is not terminated";
    }
    return NULL;
}

const char *kexec_handoff_check(const struct kexec_handoff *h, uint64_t len)
{
    if (len < sizeof(*h))
        return "shorter than a handoff";
    if (h->magic != KEXEC_HANDOFF_MAGIC)
        return "bad magic";
    if (h->version != KEXEC_HANDOFF_VERSION || h->size != sizeof(*h))
        return "another version";
    if (h->checksum != handoff_sum(h))
        return "bad checksum";
    if ((h->flags & ~KEXEC_FLAGS) || h->x2apic > 1 || h->reserved)
        return "unknown flags";
    if (!h->cpu_count || h->cpu_count > KEXEC_MAX_CPUS || !h->memmap_count ||
        h->memmap_count > KEXEC_MAX_MEMMAP || h->module_count > KEXEC_MAX_MODULES)
        return "a count out of range";
    if (h->hhdm_offset < UPPER_HALF || h->kernel_virt_base < UPPER_HALF)
        return "an address outside the upper half";
    if (!terminated(h->loader_name, KEXEC_STR) || !terminated(h->cmdline, KEXEC_CMDLINE))
        return "a string is not terminated";
    const struct kexec_fb *f = &h->fb;
    if (f->reserved[0] || f->reserved[1] || f->reserved[2] ||
        (f->phys && (!f->width || !f->height || f->pitch < f->width)))
        return "a bad framebuffer";
    const char *bad = check_memmap(h);
    return bad ? bad : check_modules(h);
}

/* The entering CPU's APIC id: the x2APIC id register, or in xAPIC mode
 * (its registers are not mapped yet) the initial APIC id from CPUID. */
static uint32_t own_apic_id(bool x2apic)
{
    if (x2apic)
        return (uint32_t)rdmsr(MSR_X2APIC_ID);
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return b >> 24;
}

static void copy_str(char *dst, size_t size, const char *src, size_t src_cap)
{
    size_t n = bounded_len(src, src_cap);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void fill_cpus(const struct kexec_handoff *h)
{
    uint32_t me = own_apic_id(h->x2apic);
    bi.bsp_lapic_id = me;
    bi.x2apic = (int)h->x2apic;
    for (uint32_t i = 0; i < h->cpu_count; i++)
        bi.cpus[bi.cpu_count++] = (struct boot_cpu){
            .acpi_uid = h->cpus[i].acpi_uid, .lapic_id = h->cpus[i].lapic_id };
}

static void fill(const struct kexec_handoff *h)
{
    copy_str(bi.loader_name, sizeof(bi.loader_name), h->loader_name, KEXEC_STR);
    copy_str(bi.cmdline, sizeof(bi.cmdline), h->cmdline, KEXEC_CMDLINE);
    bi.hhdm_offset = h->hhdm_offset;
    bi.kernel_phys_base = h->kernel_phys_base;
    bi.kernel_virt_base = h->kernel_virt_base;
    bi.rsdp_phys = h->rsdp_phys;
    bi.tsc_hz_loader = h->tsc_hz;
    bi.kexec_record = h->record_phys;   /* untrusted: crashlog.c checks it */
    fill_cpus(h);
    if (h->fb.phys)
        bi.fb = (struct boot_framebuffer){
            .phys = h->fb.phys, .virt = (void *)(h->fb.phys + h->hhdm_offset),
            .width = h->fb.width, .height = h->fb.height, .pitch = h->fb.pitch,
            .bpp = h->fb.bpp, .red_shift = h->fb.red_shift, .green_shift = h->fb.green_shift,
            .blue_shift = h->fb.blue_shift,
        };
    for (uint32_t i = 0; i < h->memmap_count; i++) {
        enum boot_mem_type t = BOOT_MEM_RESERVED;
        (void)kexec_mem_from_wire(h->memmap[i].type, &t);   /* checked already */
        bi.memmap[bi.memmap_count++] = (struct boot_mem_region){
            .base = h->memmap[i].base, .length = h->memmap[i].length, .type = t };
    }
    for (uint32_t i = 0; i < h->module_count; i++) {
        struct boot_module *m = &bi.modules[bi.module_count++];
        m->phys = h->modules[i].phys;
        m->size = h->modules[i].size;
        copy_str(m->path, sizeof(m->path), h->modules[i].path, KEXEC_STR);
        copy_str(m->string, sizeof(m->string), h->modules[i].string, KEXEC_STR);
    }
}

_Noreturn void kexec_entry(const struct kexec_handoff *h)
{
    if (kexec_handoff_check(h, sizeof(*h))) {
        static const struct __attribute__((packed)) { uint16_t limit; uint64_t base; } none;
        __asm__ volatile("lidt %0; int3" ::"m"(none));   /* a triple fault: the firmware resets */
        for (;;)
            hlt();
    }
    fill(h);
    kmain(&bi);
}
