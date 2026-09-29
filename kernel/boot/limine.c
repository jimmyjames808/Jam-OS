/* Limine glue: the ONLY file that includes limine.h. Converts Limine's
 * responses into a struct boot_info and jumps to kmain. */
#include <stdbool.h>
#include <limine.h>
#include <jam/boot.h>

#define REQ __attribute__((used, section(".limine_requests")))

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

REQ static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

REQ static volatile struct limine_bootloader_info_request info_req = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_module_request module_req = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_executable_address_request kaddr_req = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0 };
REQ static volatile struct limine_executable_cmdline_request cmdline_req = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0 };
/* Asking for MP makes Limine start the APs and park them until
 * boot_start_cpu releases them. x2APIC is enabled where the CPU has it. */
REQ static volatile struct limine_mp_request mp_req = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = LIMINE_MP_REQUEST_X86_64_X2APIC };
REQ static volatile struct limine_tsc_frequency_request tsc_req = {
    .id = LIMINE_TSC_FREQUENCY_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

void limine_entry(void);

static struct boot_info bi;

struct ap_start {
    void (*entry)(void *);   /* what the AP runs */
    void *arg;               /* entry's argument */
};
static struct ap_start ap_starts[BOOT_MAX_CPUS];

static void limine_ap_entry(struct limine_mp_info *info)
{
    struct ap_start *s = (struct ap_start *)info->extra_argument;
    s->entry(s->arg);
    for (;;)
        __asm__ volatile("cli; hlt");
}

void boot_start_cpu(const struct boot_cpu *cpu, void (*entry)(void *), void *arg)
{
    struct limine_mp_info *info = cpu->loader_handle;
    struct ap_start *s = &ap_starts[cpu - bi.cpus];
    s->entry = entry;
    s->arg = arg;
    info->extra_argument = (uint64_t)s;
    /* Release store: the AP must see extra_argument before goto_address. */
    __atomic_store_n(&info->goto_address, limine_ap_entry, __ATOMIC_SEQ_CST);
}

static enum boot_mem_type convert_mem_type(uint64_t t)
{
    switch (t) {
    case LIMINE_MEMMAP_USABLE:                 return BOOT_MEM_USABLE;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return BOOT_MEM_ACPI_RECLAIMABLE;
    case LIMINE_MEMMAP_ACPI_NVS:               return BOOT_MEM_ACPI_NVS;
    case LIMINE_MEMMAP_BAD_MEMORY:             return BOOT_MEM_BAD;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return BOOT_MEM_LOADER_RECLAIMABLE;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return BOOT_MEM_KERNEL_AND_MODULES;
    case LIMINE_MEMMAP_FRAMEBUFFER:            return BOOT_MEM_FRAMEBUFFER;
    default:                                   return BOOT_MEM_RESERVED;
    }
}

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    if (src)
        for (; src[i] && i + 1 < size; i++)
            dst[i] = src[i];
    dst[i] = '\0';
}

static void die(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

void limine_entry(void)
{
    /* Without HHDM we cannot even draw an error, so just stop. */
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision) || !hhdm_req.response ||
        !kaddr_req.response || !memmap_req.response)
        die();

    uint64_t hhdm = hhdm_req.response->offset;
    bi.hhdm_offset      = hhdm;
    bi.kernel_phys_base = kaddr_req.response->physical_base;
    bi.kernel_virt_base = kaddr_req.response->virtual_base;
    copy_str(bi.loader_name, sizeof(bi.loader_name),
             info_req.response ? info_req.response->name : "unknown");
    copy_str(bi.cmdline, sizeof(bi.cmdline),
             cmdline_req.response ? cmdline_req.response->cmdline : "");

    /* Base revision >= 4 returns the RSDP as an HHDM virtual address. */
    if (rsdp_req.response && rsdp_req.response->address)
        bi.rsdp_phys = (uint64_t)rsdp_req.response->address - hhdm;

    if (tsc_req.response)
        bi.tsc_hz_loader = tsc_req.response->frequency;

    if (mp_req.response) {
        struct limine_mp_response *mp = mp_req.response;
        bi.bsp_lapic_id = mp->bsp_lapic_id;
        bi.x2apic = (mp->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC) != 0;
        for (uint64_t i = 0; i < mp->cpu_count && bi.cpu_count < BOOT_MAX_CPUS; i++) {
            struct limine_mp_info *c = mp->cpus[i];
            bi.cpus[bi.cpu_count++] = (struct boot_cpu){
                .acpi_uid = c->processor_id, .lapic_id = c->lapic_id, .loader_handle = c };
        }
    } else {
        bi.cpu_count = 1;
        bi.cpus[0].lapic_id = bi.bsp_lapic_id;
    }

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        struct limine_framebuffer *f = fb_req.response->framebuffers[0];
        bi.fb = (struct boot_framebuffer){
            .virt = f->address, .phys = (uint64_t)f->address - hhdm,
            .width = (uint32_t)f->width, .height = (uint32_t)f->height,
            .pitch = (uint32_t)f->pitch, .bpp = f->bpp,
            .red_shift = f->red_mask_shift, .green_shift = f->green_mask_shift,
            .blue_shift = f->blue_mask_shift,
        };
    }

    uint64_t n = memmap_req.response->entry_count;
    for (uint64_t i = 0; i < n && bi.memmap_count < BOOT_MAX_MEMMAP; i++) {
        struct limine_memmap_entry *e = memmap_req.response->entries[i];
        bi.memmap[bi.memmap_count++] = (struct boot_mem_region){
            .base = e->base, .length = e->length, .type = convert_mem_type(e->type) };
    }

    if (module_req.response) {
        uint64_t n = module_req.response->module_count;
        for (uint64_t i = 0; i < n && bi.module_count < BOOT_MAX_MODULES; i++) {
            struct limine_file *m = module_req.response->modules[i];
            struct boot_module *bm = &bi.modules[bi.module_count++];
            bm->phys = (uint64_t)m->address - hhdm;
            bm->size = m->size;
            copy_str(bm->path, sizeof(bm->path), m->path);
            copy_str(bm->string, sizeof(bm->string), m->string);
        }
    }

    kmain(&bi);
}
