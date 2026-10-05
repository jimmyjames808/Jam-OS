/* Resources (kernel/object/resource.c): the authority over
 * hardware, as handles. Kinds RES_* in <jam/abi.h>.
 *
 * The root resource is made at boot; userboot puts a handle to it in init's
 * startup message (SR_RESOURCE). resource_create slices a smaller one out
 * of a parent (needs RIGHT_SLICE on the parent): a RES_MMIO range must lie
 * inside a RES_ROOT or RES_MMIO parent and never cover RAM; RES_PCI comes
 * from the root; RES_PCI_DEV from RES_PCI (pci_device_open), and a BAR's
 * RES_MMIO from its RES_PCI_DEV (pci_bar_resource). A resource is a small
 * object: one JOB_LIMIT_HANDLES unit of its creator's job. */
#pragma once

#include <stdint.h>
#include <jam/object.h>
#include <jam/status.h>

struct job;
struct pci_dev;

struct kobject *resource_root(void);      /* made by resource_init; a reference */
void resource_init(void);                  /* after pmm and pci_init */

status_t resource_create(struct kobject *parent, uint32_t kind, uint64_t base, uint64_t size,
                         struct kobject **out);
uint32_t resource_kind(struct kobject *res);
/* OK if res (RES_ROOT or RES_MMIO) covers [phys, phys + len). */
status_t resource_check_mmio(struct kobject *res, uint64_t phys, uint64_t len);
/* The function a RES_PCI_DEV stands for, or NULL for any other kind. */
struct pci_dev *resource_pci_dev(struct kobject *res);
/* RES_PCI_DEV for table entry `index`, and a BAR's RES_MMIO. */
status_t resource_pci_device(struct kobject *pci, uint32_t index, struct kobject **out);
status_t resource_pci_bar(struct kobject *dev, uint32_t bar, struct kobject **out);

/* DMA capabilities bound to a function (the plain dma_cap_create makes an
 * unbound one, for kernel tests). A new cap becomes the function's current
 * one and turns its Bus Master Enable off; the driver turns it on with
 * dma_cap_bus_master (<jam/resource_impl.h>) once its device is quiet.
 * When a VT-d unit translates the function (iommu=on), the cap gets an
 * IOMMU domain of its own and the function is switched to it before this
 * returns: the device reaches only what the cap pins (and its RMRRs).
 * Closing the last handle to the current cap clears Bus Master Enable and
 * reads config back; pins still held then are freed once the IOMMU took
 * them away, or quarantined without one, never dropped (see
 * kernel/object/dma_cap.c). job (may be NULL) is charged one handle unit
 * for the cap and a page for each of its domain's table pages.
 * ERR_INVALID_ARGS, ERR_ACCESS_DENIED (a bridge or display function, or,
 * while the IOMMU translates, one whose requester id other functions
 * share),
 * ERR_NO_MEMORY, ERR_NO_RESOURCES (the unit has no domain id left),
 * ERR_TIMED_OUT / ERR_IO (the unit didn't confirm the switch).
 * Thread context, interrupts on, no spinlock held. */
status_t dma_cap_create_for(struct pci_dev *d, struct job *job, struct kobject **out);
struct pci_dev *dma_cap_device(struct kobject *cap);   /* NULL if unbound */
