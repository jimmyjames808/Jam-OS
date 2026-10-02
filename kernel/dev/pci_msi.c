/* MSI and MSI-X programming: message address/data, enable, per-vector
 * masks. Discovery (where the capabilities and the MSI-X table are) is
 * part of the walk in pci.c. Everything here holds pci_lock. */
#include <jam/pci.h>
#include <jam/spinlock.h>

#include "pci_internal.h"

static status_t check_irq(const struct pci_dev *d, bool msix, uint32_t index)
{
    if (!d)
        return ERR_INVALID_ARGS;
    if (untouchable(d))
        return ERR_ACCESS_DENIED;
    if (msix) {
        if (!d->cap_msix)
            return ERR_NOT_SUPPORTED;
        if (index >= d->info.msix_vectors)
            return ERR_OUT_OF_RANGE;
        if (!d->msix_table)
            return ERR_BAD_STATE;   /* table BAR has no address */
    } else {
        if (!d->cap_msi)
            return ERR_NOT_SUPPORTED;
        if (index != 0)
            return ERR_OUT_OF_RANGE;
    }
    return OK;
}

/* The MSI-X table only answers while memory decode is on. */
static void ensure_memory(struct pci_dev *d)
{
    uint16_t cmd = rd(d, CFG_COMMAND, 2);
    if (!(cmd & CMD_MEMORY))
        wr(d, CFG_COMMAND, 2, cmd | CMD_MEMORY);
}

static inline volatile uint32_t *msix_entry(const struct pci_dev *d, uint32_t index)
{
    return d->msix_table + 4 * index;
}

/* MSI mask bits register (maskable MSI only). */
static uint32_t msi_mask_off(const struct pci_dev *d)
{
    return d->cap_msi + (d->msi_64 ? 0x10 : 0x0c);
}

status_t pci_msi_set(struct pci_dev *d, bool msix, uint32_t index, uint64_t addr, uint32_t data)
{
    status_t st = check_irq(d, msix, index);
    if (st != OK)
        return st;
    if (!msix && !d->msi_64 && (addr >> 32))
        return ERR_INVALID_ARGS;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    if (msix) {
        ensure_memory(d);
        volatile uint32_t *e = msix_entry(d, index);
        uint32_t ctl = e[3];
        if (!(ctl & MSIX_ENTRY_CTL_MASK))
            e[3] = ctl | MSIX_ENTRY_CTL_MASK;
        e[0] = (uint32_t)addr;
        e[1] = (uint32_t)(addr >> 32);
        e[2] = data;
        if (!(ctl & MSIX_ENTRY_CTL_MASK))
            e[3] = ctl;
        (void)e[3];   /* flush the posted writes */
    } else {
        uint32_t c = d->cap_msi;
        uint16_t ctl = rd(d, c + 2, 2);
        uint32_t mask = 0;
        bool off_while = false;
        if (d->msi_maskable) {
            mask = rd(d, msi_mask_off(d), 4);
            wr(d, msi_mask_off(d), 4, mask | 1);
        } else if (ctl & MSI_CTL_ENABLE) {
            /* Not maskable: keep a half-written message from going out by
             * turning MSI off while it changes. */
            wr(d, c + 2, 2, ctl & ~MSI_CTL_ENABLE);
            off_while = true;
        }
        wr(d, c + 4, 4, (uint32_t)addr);
        if (d->msi_64) {
            wr(d, c + 8, 4, (uint32_t)(addr >> 32));
            wr(d, c + 0x0c, 2, data & 0xffff);
        } else {
            wr(d, c + 8, 2, data & 0xffff);
        }
        if (d->msi_maskable)
            wr(d, msi_mask_off(d), 4, mask);
        if (off_while)
            wr(d, c + 2, 2, ctl);
        (void)rd(d, c + 2, 2);
    }
    spin_unlock_irqrestore(&pci_lock, f);
    return OK;
}

status_t pci_msi_enable(struct pci_dev *d, bool msix, bool on)
{
    status_t st = check_irq(d, msix, 0);
    if (st != OK)
        return st;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint16_t msi_ctl = d->cap_msi ? rd(d, d->cap_msi + 2, 2) : 0;
    uint16_t msix_ctl = d->cap_msix ? rd(d, d->cap_msix + 2, 2) : 0;
    if (on && ((msix && (msi_ctl & MSI_CTL_ENABLE)) || (!msix && (msix_ctl & MSIX_CTL_ENABLE)))) {
        spin_unlock_irqrestore(&pci_lock, f);
        return ERR_BAD_STATE;   /* the other kind is on */
    }
    if (on) {
        wr(d, CFG_COMMAND, 2, rd(d, CFG_COMMAND, 2) | CMD_INTX_OFF);
        if (msix) {
            ensure_memory(d);
            /* Enable under the function mask, then lift it: the entries'
             * own mask bits decide from here. */
            wr(d, d->cap_msix + 2, 2, msix_ctl | MSIX_CTL_ENABLE | MSIX_CTL_FMASK);
            wr(d, d->cap_msix + 2, 2, (msix_ctl | MSIX_CTL_ENABLE) & ~MSIX_CTL_FMASK);
        } else {
            wr(d, d->cap_msi + 2, 2, (msi_ctl & ~MSI_CTL_MME_MASK) | MSI_CTL_ENABLE);
        }
    } else {
        if (msix) {
            wr(d, d->cap_msix + 2, 2, msix_ctl | MSIX_CTL_FMASK);
            wr(d, d->cap_msix + 2, 2, msix_ctl & ~(MSIX_CTL_ENABLE | MSIX_CTL_FMASK));
            msix_ctl &= ~MSIX_CTL_ENABLE;
        } else {
            wr(d, d->cap_msi + 2, 2, msi_ctl & ~MSI_CTL_ENABLE);
            msi_ctl &= ~MSI_CTL_ENABLE;
        }
        /* INTx Disable stays set: clearing it
         * with the last MSI gone could let an INTx the device has pending
         * fire into a line nobody handles, and an unbound function has no
         * business interrupting. */
    }
    (void)rd(d, CFG_COMMAND, 2);
    spin_unlock_irqrestore(&pci_lock, f);
    return OK;
}

status_t pci_msi_mask(struct pci_dev *d, bool msix, uint32_t index, bool masked)
{
    status_t st = check_irq(d, msix, index);
    if (st != OK)
        return st;
    if (!msix && !d->msi_maskable)
        return ERR_NOT_SUPPORTED;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    if (msix) {
        ensure_memory(d);
        volatile uint32_t *e = msix_entry(d, index);
        uint32_t ctl = e[3];
        e[3] = masked ? ctl | MSIX_ENTRY_CTL_MASK : ctl & ~MSIX_ENTRY_CTL_MASK;
        (void)e[3];
    } else {
        uint32_t m = rd(d, msi_mask_off(d), 4);
        wr(d, msi_mask_off(d), 4, masked ? m | 1 : m & ~1u);
        (void)rd(d, msi_mask_off(d), 4);
    }
    spin_unlock_irqrestore(&pci_lock, f);
    return OK;
}
