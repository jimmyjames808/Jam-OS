/* Tests for PCIe Access Control Services (kernel/dev/pci_acs.c).
 *
 * Pure: the control register's new value from the capability register.
 * Live, on whatever machine runs them: every ACS capability found is one
 * (its header's id), and with the IOMMU translating (iommu on, the
 * default), every root port, switch downstream port and function of a
 * multi-function device that has the capability has the bits it offers
 * of SV, RR, CR and UF on. QEMU's q35 as tools/qemu-test.sh starts it has
 * no port; QEMU 10's pcie-root-port offers ACS (SV TB RR CR UF DT; not
 * with disable-acs=on), its xio3130-downstream switch port none. */
#include <jam/iommu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/pci.h>

#define ACS_TB 0x02   /* translation blocking: offered by QEMU's ports, never turned on */
#define ACS_EC 0x20   /* egress control: the same */
#define ACS_DT 0x40   /* direct translated peer-to-peer: the same */

KTEST(pci_acs_ctl_wanted_bits)
{
    /* Everything offered (QEMU's ports offer SV TB RR CR UF DT): the four
     * on, translation blocking and the others not. */
    KT_EQ(pci_acs_ctl_wanted(0x7f, 0), PCI_ACS_WANT);
    KT_EQ(PCI_ACS_WANT, 0x1d);
    /* Only what is offered; what was on stays on. */
    KT_EQ(pci_acs_ctl_wanted(PCI_ACS_RR | PCI_ACS_CR, ACS_DT), PCI_ACS_RR | PCI_ACS_CR | ACS_DT);
    KT_EQ(pci_acs_ctl_wanted(0, ACS_TB), ACS_TB);
    KT_EQ(pci_acs_ctl_wanted(ACS_TB | ACS_EC, 0), 0);
    KT_EQ(pci_acs_ctl_wanted(0xffff, 0x8000), 0x8000 | PCI_ACS_WANT);
}

/* Two functions of one device (same segment, bus and device number). */
static bool one_device(const struct pci_dev *a, const struct pci_dev *b)
{
    return a && b && a->info.segment == b->info.segment && a->info.bus == b->info.bus &&
           a->info.dev == b->info.dev;
}

KTEST(pci_acs_on_where_offered)
{
    uint32_t found = 0, checked = 0;
    bool translating = iommu_translating();
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (!d->cap_acs)
            continue;
        found++;
        KT_ASSERT(d->cap_pcie);   /* an extended capability: PCIe functions only */
        KT_ASSERT(d->cap_acs >= 0x100 && !(d->cap_acs & 3));
        KT_EQ(pci_cfg_read(d, d->cap_acs, 2), 0x000d);
        struct pci_acs_fn a;
        pci_acs_read(d, &a);
        KT_ASSERT(a.acs);
        bool multi = one_device(d, pci_get(i - 1)) || one_device(d, pci_get(i + 1));
        if (!translating ||
            (a.type != PCIE_TYPE_ROOT_PORT && a.type != PCIE_TYPE_DOWNSTREAM && !multi))
            continue;
        uint16_t offered = a.acs_cap & PCI_ACS_WANT;
        KT_EQ(a.acs_ctl & offered, offered);
        checked++;
    }
    kprintf("ktest %s: %u ACS capabilit%s, %u checked on%s\n", ktest_current, found,
            found == 1 ? "y" : "ies", checked, translating ? "" : " (the IOMMU is off: none)");
}
