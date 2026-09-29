/* M6 Track C internals shared by kernel/object/resource.c, dma_cap.c,
 * vmo.c and the hardware system calls (kernel/abi/sysc_hw.c). The public
 * contract is <jam/resource.h>; nothing outside those files and the tests
 * should need this header. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/handle.h>
#include <jam/list.h>
#include <jam/object.h>
#include <jam/status.h>

struct job;
struct pci_dev;
struct vmo;

/* The highest physical address the architecture allows (MAXPHYADDR <= 52):
 * the root resource covers [0, RES_PHYS_LIMIT). */
#define RES_PHYS_LIMIT (1ull << 52)

/* Rights a resource handle can carry; a derived resource's handle gets the
 * rights of the handle it was made from, masked to these. */
#define RES_RIGHTS (RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP | RIGHT_SLICE | RIGHT_MANAGE)
#define DMA_CAP_RIGHTS RIGHTS_BASIC

/* ---- resources ------------------------------------------------------------ */

/* Charge a fresh resource to job: one JOB_LIMIT_HANDLES unit until it is
 * destroyed. Once (ERR_BAD_STATE after); a NULL job charges nothing. */
status_t resource_set_job(struct kobject *res, struct job *job);
/* Can [phys, phys + len) be mapped for a process at all? ERR_ACCESS_DENIED
 * if it touches RAM, a page holding an MSI-X table or PBA
 * (pci_phys_protected), or MMIO the kernel owns (local APIC and the MSI
 * window 0xfee00000-0xfeefffff, I/O APICs, HPET, the PCIe ECAM windows). */
status_t resource_phys_mappable(uint64_t phys, uint64_t len);
/* The physical range of a RES_ROOT / RES_MMIO resource. */
status_t resource_range(struct kobject *res, uint64_t *base, uint64_t *size);

/* The config-space write filter (resource.c), see pci_cfg_write_allowed's
 * comment there. `read` reads the function's current config (pci_cfg_read
 * normally; tests pass a fake). OK or ERR_ACCESS_DENIED. Width and
 * alignment must already be checked. */
typedef uint32_t (*pci_cfg_reader_t)(struct pci_dev *d, uint32_t off, uint32_t width);
status_t pci_cfg_write_allowed(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t value,
                               pci_cfg_reader_t read);
/* Width 1, 2 or 4, offset aligned to it and < 4096: else ERR_INVALID_ARGS. */
status_t pci_cfg_access_ok(uint32_t off, uint32_t width);
/* Serialises every change of a command register made on a process's
 * behalf: filtered config writes (check + write), pci_bus_master and the
 * dma_cap close path (BME off + read-back). A spinlock (irqsave), ranked
 * above the PCI core's config lock. */
uint64_t pci_cmd_lock(void);
void     pci_cmd_unlock(uint64_t flags);

/* ---- DMA capabilities ------------------------------------------------------ */

struct dma_cap {
    struct kobject   base;      /* its lock guards `pins` and `closed` */
    struct pci_dev  *dev;       /* the function it is bound to; NULL: unbound */
    struct job      *job;       /* charged one handle unit, or NULL */
    struct list_node pins;      /* vmo.c's struct vmo_range (cap_node) */
    bool             closed;    /* last handle gone: no new pins */
};

static inline struct dma_cap *dma_cap_from_kobject(struct kobject *o)
{
    return o && o->type == OBJ_DMA_CAP ? (struct dma_cap *)o : NULL;
}

/* vmo.c: release every pin made with c (the close path; interrupts on,
 * no spinlock held, may run with preemption off). Pins still being set up
 * or torn down elsewhere finish by themselves. */
void vmo_release_cap_pins(struct dma_cap *c);
/* Pins currently recorded on the cap (tests). */
uint64_t dma_cap_pin_count(struct kobject *cap);
/* As resource_set_job, for a dma_cap. */
status_t dma_cap_set_job(struct kobject *cap, struct job *job);
/* May the cap pin right now? Unbound: always; bound: its function's Bus
 * Master Enable is on (read from config space; all-ones counts as off). */
bool dma_cap_bus_master_on(struct kobject *cap);

/* ---- the handle layer (kernel/abi/sysc_hw.c) -------------------------------
 * Same rules as <jam/sys.h>: kernel pointers, the caller's table. */

/* Syscall pins report at most this many pages per call (the address list
 * is built in a kernel buffer). 4096 pages = 16 MiB. */
#define VMO_PIN_SYS_MAX_PAGES 4096

status_t sys_resource_create(struct handle_table *t, handle_t parent, uint32_t kind,
                             uint64_t base, uint64_t size, handle_t *out);
status_t sys_pci_enum(struct handle_table *t, handle_t pci, uint32_t index,
                      struct pci_dev_info *out);
status_t sys_pci_device_open(struct handle_table *t, handle_t pci, uint32_t index, handle_t *out);
status_t sys_pci_config_read(struct handle_table *t, handle_t dev, uint32_t off, uint32_t width,
                             uint32_t *value);
status_t sys_pci_config_write(struct handle_table *t, handle_t dev, uint32_t off, uint32_t width,
                              uint32_t value);
status_t sys_pci_bar_resource(struct handle_table *t, handle_t dev, uint32_t bar, handle_t *out);
status_t sys_pci_bus_master(struct handle_table *t, handle_t dev, uint32_t enable);
status_t sys_vmo_create_physical(struct handle_table *t, handle_t res, uint64_t offset,
                                 uint64_t size, uint32_t cache, handle_t *out);
status_t sys_dma_cap_create(struct handle_table *t, handle_t dev, handle_t *out);
/* addrs: kernel array of at least len / PAGE_SIZE entries. */
status_t sys_vmo_pin(struct handle_table *t, handle_t vmo, handle_t dma, uint64_t offset,
                     uint64_t len, uint64_t *addrs, uint64_t *pin_id);
status_t sys_vmo_unpin(struct handle_table *t, handle_t vmo, uint64_t pin_id);
