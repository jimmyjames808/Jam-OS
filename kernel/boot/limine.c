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
/* Asking for MP makes Limine start the APs and park them; M2 wakes them. */
REQ static volatile struct limine_mp_request mp_req = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = 0 };

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

void limine_entry(void);

static struct boot_info bi;

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

    if (mp_req.response) {
        bi.cpu_count    = (uint32_t)mp_req.response->cpu_count;
        bi.bsp_lapic_id = mp_req.response->bsp_lapic_id;
    } else {
        bi.cpu_count = 1;
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
