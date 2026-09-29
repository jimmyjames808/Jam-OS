/* Starting the xhci-noop driver without devmgr (M6 done test;
 * kernel/drivers/xhci_launch.c). The kernel builds the driver's handles
 * the way devmgr will (RES_PCI_DEV without RIGHT_MANAGE, BAR 0, MSI-X
 * entry 0 or MSI, a bound dma_cap with Bus Master Enable on) and starts it
 * as a kernel process or as the process drv/xhci-noop from bootfs.
 * Used by the ktests (xhci_noop_*) and the "xhcitest" boot entry. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct pci_dev;

/* The n-th function with class 0c0330 (xHCI), or NULL. */
struct pci_dev *xhci_find(uint32_t n);

/* Run xhci-noop on d once, as a kernel process (process = false) or as a
 * process, in a fresh job, and wait (bounded) for it to exit. Checks
 * afterwards that Bus Master Enable and MSI / MSI-X are off again and the
 * job is back to zero. Reports one RESULTS line; true if all of that held
 * and the driver exited 0. */
bool xhci_launch(struct pci_dev *d, bool process);
