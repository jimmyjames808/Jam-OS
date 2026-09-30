/* The kernel's PCI core (kernel/dev/pci.c, pci_msi.c, pci_report.c). The kernel keeps
 * everything a buggy or hostile driver must not do itself: ECAM access, BAR
 * sizing, MSI/MSI-X programming and Bus Master Enable. devmgr (a process)
 * decides which driver gets which device, through resource handles
 * (jam/resource.h); drivers see only <jam/driver.h>.
 *
 * Enumeration runs once at boot (pci_init, after ACPI and before userboot)
 * and the table never changes afterwards (no hotplug), so `struct pci_dev`
 * pointers stay valid forever and lookups need no lock. Config accesses go
 * through one IRQ-safe leaf lock.
 *
 * Never touched: the function whose BAR holds the boot framebuffer
 * (PCI_INFO_DISPLAY) and every bridge. Their command registers are never
 * written and their BARs never sized (sizing disables decode). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/status.h>

#define PCI_MAX_DEVS 256

struct pci_dev {
    uint32_t index;                 /* position in the table (pci_enum's index) */
    struct pci_dev_info info;       /* what pci_enum reports */
    volatile void *cfg;             /* this function's 4 KiB of ECAM */
    /* Capabilities (config offsets; 0 = absent). */
    uint16_t cap_msi, cap_msix, cap_pcie;
    bool     msi_64, msi_maskable;
    uint8_t  msix_table_bar, msix_pba_bar;
    uint32_t msix_table_off, msix_pba_off;
    volatile uint32_t *msix_table;  /* kernel UC mapping of the table, or NULL */
    /* RES_PCI_DEV resources made through system calls (devmgr's, for a
     * driver) that are alive: the function belongs to a driver process. */
    uint32_t proc_users;
    /* A process opened it with RIGHT_MANAGE (devmgr binding a driver to
     * it). Sticky for the rest of the boot: while devmgr (or its driver)
     * restarts, proc_users can be 0 for a moment, and a `ktest` from the
     * shell must not grab the function in that gap. */
    bool driver_managed;
};

/* Enumerate every ECAM segment in acpi.ecam[]; logs one line per function. */
void pci_init(void);
/* The "Devices" boot entry (`pcilist`): every function into the RESULTS
 * box, one line each (BDF, ids, class, MSI/MSI-X vector counts). */
void pci_report(void);
uint32_t pci_count(void);
struct pci_dev *pci_get(uint32_t index);            /* NULL past the end */
/* The n-th function with this vendor/device (0xffff = any), or NULL.
 * While pci_hide_in_use is set (`ktest` run from the shell, with
 * devmgr's drivers running), functions in use by drivers (pci_in_use) are
 * left out, so the kernel tests that drive a device skip it instead of
 * fighting its driver. */
struct pci_dev *pci_find(uint16_t vendor, uint16_t device, uint32_t n);
extern bool pci_hide_in_use;
/* A process holds it (proc_users), or devmgr ever opened it to bind a
 * driver (driver_managed, sticky). */
bool pci_in_use(const struct pci_dev *d);

/* Config space, width 1, 2 or 4, offset < 4096 and aligned to width. */
uint32_t pci_cfg_read(struct pci_dev *d, uint32_t off, uint32_t width);
void pci_cfg_write(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t v);
/* Capability offset by id: standard list (id < 0x100) or extended (0x100 +
 * PCIe extended id); 0 if absent. */
uint16_t pci_find_cap(struct pci_dev *d, uint32_t id);

/* MSI / MSI-X. `index` is the MSI-X table entry, or 0 for MSI (only a
 * single MSI vector is used). pci_msi_set writes address/data (entry masked
 * while it changes); pci_msi_enable turns MSI or MSI-X on/off for the
 * function and disables INTx (turning the last one off leaves INTx
 * disabled).
 * ERR_NOT_SUPPORTED if the function
 * lacks the capability, ERR_OUT_OF_RANGE for a bad index. */
status_t pci_msi_set(struct pci_dev *d, bool msix, uint32_t index, uint64_t addr, uint32_t data);
status_t pci_msi_enable(struct pci_dev *d, bool msix, bool on);
/* Per-vector mask: MSI-X always, MSI only if msi_maskable (else a no-op
 * returning ERR_NOT_SUPPORTED). */
status_t pci_msi_mask(struct pci_dev *d, bool msix, uint32_t index, bool masked);

/* Bus Master Enable, with a config read-back so it has taken effect when
 * this returns. Refused (ERR_ACCESS_DENIED) for the display and bridges. */
status_t pci_set_bus_master(struct pci_dev *d, bool on);
/* Memory decode on (a driver needs its BARs to answer). Same refusals. */
status_t pci_enable_memory(struct pci_dev *d);

/* Around a power-state change (D3hot -> D0 resets a function without
 * No_Soft_Reset): save the command register and the BAR registers, then
 * put back whatever the change lost. Restore returns true if the BARs had
 * been lost (the function was reset); INTx Disable ends up set. Bus Master
 * Enable is never put back: it stays as it is NOW (off after a reset, or
 * whatever its owner set while the caller slept outside pci_cmd_lock), so
 * bus mastering can't come back without its owner (the dma_cap) turning
 * it on. I/O and memory decode come back as saved (the kernel never turns
 * them off after boot, and the BARs they decode were just put back). */
struct pci_saved_config {
    uint16_t command;   /* command register (bus mastering left off) */
    uint32_t bar[6];    /* the six BAR registers as read */
};
void pci_save_config(struct pci_dev *d, struct pci_saved_config *out);
bool pci_restore_config(struct pci_dev *d, const struct pci_saved_config *in);

/* Does [phys, phys + len) touch a page holding any function's MSI-X table
 * or PBA? Such pages are never mapped for anyone but the kernel. */
bool pci_phys_protected(uint64_t phys, uint64_t len);
