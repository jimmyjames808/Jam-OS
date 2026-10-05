/* VT-d: which PCI functions share a requester id with others. A unit
 * tells devices apart only by the requester id their requests carry (its
 * source-id, VT-d 3.4), and a request from a conventional PCI device doesn't
 * carry its own: a PCIe-to-PCI/PCI-X bridge forwards it upstream under
 * its secondary bus's id (bus, 00.0) or its own, and a conventional
 * PCI-to-PCI bridge under its own. Every function below such a bridge
 * therefore uses the domain that id's context entry names, and can raise
 * the interrupt entries validated against that id: a driver given a
 * domain for one of them would reach the others' too, and the others'
 * drivers its. Rather than share a domain between their drivers, Jam OS
 * gives none of them a DMA capability while the IOMMU translates
 * (iommu_domain_create says ERR_ACCESS_DENIED); their DMA stays blocked.
 * The bridge itself is refused too: its own id stands for those below it.
 *
 * The check is pure (vtd_rid_shared, over a list of bridges, tested on a
 * made-up topology); vtd_rid_mark reads the real one from config space
 * once, at boot, and sets each function's `shared`. The PC has no such
 * bridge (its bridges are PCIe root ports), so nothing there is refused. */
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/report.h>

#include "vtd_domain.h"

#define PCI_SECONDARY      0x19   /* type 1 and 2 headers: the bus below */
#define PCI_SUBORDINATE    0x1a   /* the highest bus below */
#define PCIE_CAPS          0x02   /* the PCIe capability's own register, from the capability */
#define PCIE_TYPE(caps)    (((caps) >> 4) & 0xf)   /* device/port type, bits 7:4 */
#define PCIE_TYPE_TO_PCI   0x7    /* a PCI Express to PCI/PCI-X bridge */

/* A bridge with a valid secondary bus whose id is b's own. */
static bool leads_to(const struct vtd_bridge *b, uint16_t seg, uint8_t bus)
{
    return b->seg == seg && b->secondary == bus && b->secondary > (b->sid >> 8) &&
           b->secondary <= b->subordinate;
}

bool vtd_rid_shared(const struct vtd_bridge *br, uint32_t n, uint16_t seg, uint16_t sid)
{
    for (uint32_t i = 0; i < n; i++)
        if (br[i].seg == seg && br[i].sid == sid && br[i].conventional &&
            leads_to(&br[i], seg, br[i].secondary))
            return true;   /* an id the functions below it use */
    /* Up from the function's bus, one bridge at a time: each step goes to
     * a lower bus number (leads_to), so 256 steps reach the root. */
    uint8_t bus = (uint8_t)(sid >> 8);
    for (unsigned step = 0; step < 256; step++) {
        const struct vtd_bridge *up = NULL;
        for (uint32_t i = 0; i < n && !up; i++)
            if (leads_to(&br[i], seg, bus))
                up = &br[i];
        if (!up)
            return false;   /* a root bus: the id is the function's own */
        if (up->conventional)
            return true;
        bus = (uint8_t)(up->sid >> 8);
    }
    return true;   /* can't happen (see above); refuse rather than guess */
}

/* d as the check sees it, if it is a bridge with a bus below. */
static bool bridge_of(struct pci_dev *d, struct vtd_bridge *out)
{
    uint8_t type = d->info.header_type & 0x7f;
    if (type != 1 && type != 2)
        return false;
    uint32_t caps = d->cap_pcie ? pci_cfg_read(d, d->cap_pcie + PCIE_CAPS, 2) : 0;
    *out = (struct vtd_bridge){
        .seg = d->info.segment,
        .sid = (uint16_t)(d->info.bus << 8 | d->info.dev << 3 | d->info.fn),
        .secondary = (uint8_t)pci_cfg_read(d, PCI_SECONDARY, 1),
        .subordinate = (uint8_t)pci_cfg_read(d, PCI_SUBORDINATE, 1),
        .conventional = !d->cap_pcie || PCIE_TYPE(caps) == PCIE_TYPE_TO_PCI,
    };
    return true;
}

status_t vtd_rid_mark(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; vtd_fn_at(i); i++)
        n++;
    struct vtd_bridge *br = kmalloc((n ? n : 1) * sizeof(*br));
    if (!br)
        return ERR_NO_MEMORY;
    uint32_t nbr = 0;
    for (uint32_t i = 0; i < n; i++)
        nbr += bridge_of(vtd_fn_at(i)->dev, &br[nbr]);
    for (uint32_t i = 0; i < n; i++) {
        struct vtd_fn *f = vtd_fn_at(i);
        f->shared = vtd_rid_shared(br, nbr, f->dev->info.segment, f->sid);
        if (f->shared && f->ctl)
            report("vtd: %02x:%02x.%x shares a requester id (a PCIe-to-PCI or PCI bridge's): no "
                   "DMA for it with iommu=on", f->sid >> 8, (f->sid >> 3) & 0x1f, f->sid & 7);
    }
    kfree(br);
    return OK;
}
