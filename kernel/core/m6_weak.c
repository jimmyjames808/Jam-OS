/* M6 foundation: a weak stub for every kernel interface between the M6
 * tracks (M6-PLAN.md), so each track builds and boots alone. A track
 * replaces the stubs it owns by defining the real functions (strong
 * symbols win); phase 2 deletes this file. Syscall stubs are generated
 * (kernel/abi/syscall_weak.c). */
#include <jam/interrupt.h>
#include <jam/pci.h>
#include <jam/resource.h>

#define WEAK __attribute__((weak))

/* ---- Track A: PCI core ---------------------------------------------------- */
WEAK void pci_init(void) {}
WEAK void pci_report(void) {}
WEAK uint32_t pci_count(void) { return 0; }
WEAK struct pci_dev *pci_get(uint32_t index) { (void)index; return NULL; }
WEAK struct pci_dev *pci_find(uint16_t vendor, uint16_t device, uint32_t n)
{
    (void)vendor; (void)device; (void)n;
    return NULL;
}
WEAK uint32_t pci_cfg_read(struct pci_dev *d, uint32_t off, uint32_t width)
{
    (void)d; (void)off; (void)width;
    return 0xffffffffu;
}
WEAK void pci_cfg_write(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t v)
{
    (void)d; (void)off; (void)width; (void)v;
}
WEAK uint16_t pci_find_cap(struct pci_dev *d, uint32_t id) { (void)d; (void)id; return 0; }
WEAK status_t pci_msi_set(struct pci_dev *d, bool msix, uint32_t index, uint64_t addr,
                          uint32_t data)
{
    (void)d; (void)msix; (void)index; (void)addr; (void)data;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t pci_msi_enable(struct pci_dev *d, bool msix, bool on)
{
    (void)d; (void)msix; (void)on;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t pci_msi_mask(struct pci_dev *d, bool msix, uint32_t index, bool masked)
{
    (void)d; (void)msix; (void)index; (void)masked;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t pci_set_bus_master(struct pci_dev *d, bool on) { (void)d; (void)on; return ERR_NOT_SUPPORTED; }
WEAK status_t pci_enable_memory(struct pci_dev *d) { (void)d; return ERR_NOT_SUPPORTED; }
WEAK bool pci_phys_protected(uint64_t phys, uint64_t len) { (void)phys; (void)len; return false; }

/* ---- Track B: vectors and interrupt objects ------------------------------- */
WEAK status_t vector_alloc(vector_fn_t fn, void *ctx, uint32_t *cpu, uint8_t *vec)
{
    (void)fn; (void)ctx; (void)cpu; (void)vec;
    return ERR_NOT_SUPPORTED;
}
WEAK void vector_free(uint32_t cpu, uint8_t vec) { (void)cpu; (void)vec; }
WEAK uint64_t msi_address(uint32_t cpu) { (void)cpu; return 0; }
WEAK uint32_t msi_data(uint8_t vec) { (void)vec; return 0; }
WEAK status_t interrupt_create_msi(struct pci_dev *d, uint32_t index, uint32_t flags,
                                   struct kobject **out)
{
    (void)d; (void)index; (void)flags; (void)out;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t interrupt_ack(struct kobject *irq) { (void)irq; return ERR_NOT_SUPPORTED; }
WEAK uint64_t interrupt_fire_count(struct kobject *irq) { (void)irq; return 0; }
WEAK status_t interrupt_create_virtual(struct kobject **out) { (void)out; return ERR_NOT_SUPPORTED; }
WEAK void interrupt_fire_virtual(struct kobject *irq) { (void)irq; }

/* ---- Track C: resources and bound DMA capabilities ------------------------- */
WEAK struct kobject *resource_root(void) { return NULL; }
WEAK void resource_init(void) {}
WEAK status_t resource_create(struct kobject *parent, uint32_t kind, uint64_t base, uint64_t size,
                              struct kobject **out)
{
    (void)parent; (void)kind; (void)base; (void)size; (void)out;
    return ERR_NOT_SUPPORTED;
}
WEAK uint32_t resource_kind(struct kobject *res) { (void)res; return 0; }
WEAK status_t resource_check_mmio(struct kobject *res, uint64_t phys, uint64_t len)
{
    (void)res; (void)phys; (void)len;
    return ERR_NOT_SUPPORTED;
}
WEAK struct pci_dev *resource_pci_dev(struct kobject *res) { (void)res; return NULL; }
WEAK status_t resource_pci_device(struct kobject *pci, uint32_t index, struct kobject **out)
{
    (void)pci; (void)index; (void)out;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t resource_pci_bar(struct kobject *dev, uint32_t bar, struct kobject **out)
{
    (void)dev; (void)bar; (void)out;
    return ERR_NOT_SUPPORTED;
}
WEAK status_t dma_cap_create_for(struct pci_dev *d, struct kobject **out)
{
    (void)d; (void)out;
    return ERR_NOT_SUPPORTED;
}
WEAK struct pci_dev *dma_cap_device(struct kobject *cap) { (void)cap; return NULL; }
