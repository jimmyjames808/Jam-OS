/* Internals shared by kernel/object/resource.c, dma_cap.c,
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

struct iommu_domain;
struct job;
struct pci_dev;
struct q_batch;
struct vmo;

/* The highest physical address the architecture allows (MAXPHYADDR <= 52):
 * the root resource covers [0, RES_PHYS_LIMIT). */
#define RES_PHYS_LIMIT (1ull << 52)

/* Rights a resource handle can carry; a derived resource's handle gets the
 * rights of the handle it was made from, masked to these. */
#define RES_RIGHTS (RIGHTS_BASIC | RIGHTS_IO | RIGHT_MAP | RIGHT_SLICE | RIGHT_MANAGE)
/* The root's handle (userboot gives it to init): a resource's rights and
 * every power over the system (RIGHTS_ROOT, <jam/abi.h>). */
#define ROOT_RIGHTS (RES_RIGHTS | RIGHTS_ROOT)
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
/* The same for a writer with RIGHT_MANAGE on the function (`manage`):
 * it may also change the PM PowerState. */
status_t pci_cfg_write_allowed_as(struct pci_dev *d, uint32_t off, uint32_t width,
                                  uint32_t value, pci_cfg_reader_t read, bool manage);
/* Does this write change the function's PM PowerState? */
bool pci_cfg_write_changes_power(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t value,
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
    struct kobject       base;    /* its lock guards `pins` and `closed` */
    struct pci_dev      *dev;     /* the function it is bound to; NULL: unbound */
    struct job          *job;     /* charged one handle unit and its domain's tables, or NULL */
    struct list_node     pins;    /* vmo.c's struct vmo_range (cap_node) */
    bool                 closed;  /* last handle gone: no new pins */
    /* The IOMMU domain the device sees memory through (a bound cap of a
     * function a VT-d unit translates), or NULL. Set before the cap is
     * handed out and never changed: vmo_pin maps into it, vmo_unpin
     * unmaps. dma_cap.c's release thread destroys it once the cap has
     * closed and no pin is still being made or undone with it. */
    struct iommu_domain *dom;
    /* Bound: the batch its close (or its destroy, if it never had a
     * handle) hands its pins and domain over in, made with the cap so that
     * closing allocates nothing; NULL once handed over. */
    struct q_batch      *batch;
};

static inline struct dma_cap *dma_cap_from_kobject(struct kobject *o)
{
    return o && o->type == OBJ_DMA_CAP ? (struct dma_cap *)o : NULL;
}

/* vmo.c: release every pin made with c (the close path; interrupts on,
 * no spinlock held, may run with preemption off). Pins still being set up
 * or torn down elsewhere finish by themselves. */
void vmo_release_cap_pins(struct dma_cap *c);
/* vmo.c: the close path of a BOUND cap. Every pin still made with c
 * goes onto the list `out` (through the range's own list node) instead of
 * being released: its pages stay put, each with a checksum; *pins and
 * *pages count what went on (pins of physical VMOs are released at once:
 * no RAM behind them). Same context rules as vmo_release_cap_pins. */
void vmo_quarantine_cap_pins(struct dma_cap *c, struct list_node *out, uint64_t *pins,
                             uint64_t *pages);
/* Release every pin on such a list (emptied): the pages may go back to
 * their VMO. *pages += how many; *changed += how many no longer match the
 * checksum taken when they were quarantined (something -- the device, if
 * the VMO had no other writer -- wrote them in between). Interrupts on,
 * no spinlock held. */
void vmo_release_quarantined(struct list_node *list, uint64_t *pages, uint64_t *changed);
/* Pins currently recorded on the cap (tests, and the release thread). */
uint64_t dma_cap_pin_count(struct kobject *cap);
/* As resource_set_job, for a dma_cap made with no job (dma_cap_create's
 * unbound ones, in the tests): ERR_BAD_STATE if it has one. A bound cap's
 * domain is charged to the job given to dma_cap_create_for, not this. */
status_t dma_cap_set_job(struct kobject *cap, struct job *job);
/* Does the cap have an IOMMU domain (its pins mapped there, its device
 * reaching nothing else)? False for an unbound cap, with iommu=off or a
 * function no VT-d unit translates: then its close quarantines. */
bool dma_cap_translated(struct kobject *cap);
/* May the cap pin right now? Unbound: always; bound: it is its function's
 * current cap (the last one made for it, not closed) and the function's
 * Bus Master Enable is on (read from config space; all-ones counts as
 * off). */
bool dma_cap_bus_master_on(struct kobject *cap);

/* ---- DMA ownership and the quarantine (dma_cap.c) ----------------------------
 * Each function has at most one CURRENT dma_cap: the last one made for it
 * (dma_cap_create_for), until it closes. Making one turns the function's
 * Bus Master Enable off (and, with the IOMMU, points the function at the
 * new cap's empty domain); only the current cap turns it on again
 * (dma_cap_bus_master), which its driver does once it has quiesced the
 * device; the current cap's close turns it off, an older cap's close
 * doesn't touch it. A bound cap closing with pins still held: with an
 * IOMMU domain, a kernel thread ("dma quarantine") takes the domain away
 * from the device, waits for the unit to confirm it, and frees the pages
 * at once (until the unit confirms, they stay held and the thread tries
 * again every second); without one it quarantines them (see vmo_quarantine_cap_pins):
 * they are released DMA_QUARANTINE_GRACE_NS after the function's current
 * cap next turns bus mastering on, or DMA_QUARANTINE_TIMEOUT_NS after the
 * close if nobody does. The same thread releases them. */
#define DMA_QUARANTINE_GRACE_NS   1000000000ull    /* 1 s */
#define DMA_QUARANTINE_TIMEOUT_NS 30000000000ull   /* 30 s */

/* Bus Master Enable on or off through the function's current cap.
 * ERR_WRONG_TYPE: not a dma_cap; ERR_NOT_SUPPORTED: unbound;
 * ERR_BAD_STATE: not (or no longer) the function's current cap. */
status_t dma_cap_bus_master(struct kobject *cap, bool on);
/* Start the release thread (idempotent; the first dma_cap_create_for does
 * it, the ktest runner too, before its leak baseline). */
void dma_quarantine_start(void);
struct dma_quarantine_stats {
    uint64_t pins, pages;     /* held now: quarantined, or waiting for their domain to go
                               * (a batch being released counts until it is) */
    uint64_t released;        /* pages released from the quarantine so far (since boot) */
    uint64_t freed;           /* pages freed at once after the IOMMU took them away */
    uint64_t changed;         /* of released and freed, found changed at release */
};
/* One consistent snapshot: a batch leaves pins/pages in the same step it
 * enters released or freed (and changed), so pins at 0 means all of d's
 * pages are back. */
void dma_quarantine_stats(struct pci_dev *d, struct dma_quarantine_stats *out);
/* Tests: release d's batches now (quarantined ones whatever their
 * deadlines; a closed cap's domain taken away first), each batch listed
 * when it is called tried once; returns once that is done and the release
 * thread is in the middle of none. Nothing of d's is held afterwards
 * unless a domain the unit didn't confirm gone keeps its pages. Interrupts
 * on, no spinlock held. */
void dma_quarantine_flush(struct pci_dev *d);

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
status_t sys_dma_cap_bus_master(struct handle_table *t, handle_t dma, uint32_t on);
/* addrs: kernel array of at least len / PAGE_SIZE entries. */
status_t sys_vmo_pin(struct handle_table *t, handle_t vmo, handle_t dma, uint64_t offset,
                     uint64_t len, uint64_t *addrs, uint64_t *pin_id);
/* Only with the dma_cap the pin was made with (vmo_unpin). */
status_t sys_vmo_unpin(struct handle_table *t, handle_t vmo, handle_t dma, uint64_t pin_id);
