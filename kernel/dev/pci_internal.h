/* What pci.c (config access, the walk, BARs, capabilities, the command
 * register), pci_msi.c (MSI and MSI-X programming) and pci_report.c (names,
 * the boot log, the Devices report) share. Nothing else includes it. */
#pragma once

#include <stddef.h>
#include <jam/pci.h>
#include <jam/spinlock.h>

/* Config space offsets and bits. */
#define CFG_VENDOR     0x00
#define CFG_DEVICE     0x02
#define CFG_COMMAND    0x04
#define CFG_STATUS     0x06
#define CFG_REVISION   0x08
#define CFG_HEADER     0x0e
#define CFG_BAR0       0x10
#define CFG_SECONDARY  0x19
#define CFG_SUBORD     0x1a
#define CFG_CAP_PTR    0x34
#define CFG_CAP_PTR_CB 0x14   /* CardBus header */

#define CMD_IO         (1u << 0)
#define CMD_MEMORY     (1u << 1)
#define CMD_MASTER     (1u << 2)
#define CMD_INTX_OFF   (1u << 10)
#define STATUS_CAPS    (1u << 4)

#define CAP_ID_MSI     0x05
#define CAP_ID_PCIE    0x10
#define CAP_ID_MSIX    0x11

#define MSI_CTL_ENABLE   (1u << 0)
#define MSI_CTL_MMC(c)   (((c) >> 1) & 7)
#define MSI_CTL_MME_MASK (7u << 4)
#define MSI_CTL_64       (1u << 7)
#define MSI_CTL_MASKABLE (1u << 8)

#define MSIX_CTL_SIZE(c) (((c) & 0x7ff) + 1)
#define MSIX_CTL_FMASK   (1u << 14)
#define MSIX_CTL_ENABLE  (1u << 15)
#define MSIX_ENTRY_CTL_MASK 1u


/* Every config space and MSI-X table access holds it (IRQ-safe, a leaf). */
extern spinlock_t pci_lock;

/* ---- raw config access (callers hold pci_lock or run single-threaded) ------- */

static inline uint32_t raw_read(volatile void *cfg, uint32_t off, uint32_t width)
{
    volatile uint8_t *p = (volatile uint8_t *)cfg + off;
    switch (width) {
    case 1:  return *p;
    case 2:  return *(volatile uint16_t *)p;
    default: return *(volatile uint32_t *)p;
    }
}

static inline void raw_write(volatile void *cfg, uint32_t off, uint32_t width, uint32_t v)
{
    volatile uint8_t *p = (volatile uint8_t *)cfg + off;
    switch (width) {
    case 1:  *p = (uint8_t)v; break;
    case 2:  *(volatile uint16_t *)p = (uint16_t)v; break;
    default: *(volatile uint32_t *)p = v; break;
    }
}

static inline uint32_t rd(struct pci_dev *d, uint32_t off, uint32_t w) { return raw_read(d->cfg, off, w); }
static inline void wr(struct pci_dev *d, uint32_t off, uint32_t w, uint32_t v) { raw_write(d->cfg, off, w, v); }

/* The display and every bridge: never sized, never reprogrammed. */
static inline bool untouchable(const struct pci_dev *d)
{
    return d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY);
}

/* pci_report.c: what pci_init logs and reports. */
int pci_fmt_bdf(char *buf, size_t n, const struct pci_dev *d);
void pci_log_function(const struct pci_dev *d);
/* RESULTS lines for the functions of one class (prog_if -1: any). */
void pci_report_class(const char *what, uint8_t c, uint8_t s, int prog_if);
