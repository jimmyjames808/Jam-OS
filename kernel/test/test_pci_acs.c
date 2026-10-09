/* Tests for PCIe Access Control Services (kernel/dev/pci_acs.c).
 *
 * Pure: the control register's new value from the capability register.
 * Live, on whatever machine runs them: every ACS capability found is one
 * (its header's id), and with the IOMMU translating (iommu on, the
 * default), every root port, switch downstream port and function of a
 * multi-function device that has the capability has the bits it offers
 * of SV, RR, CR and UF on. QEMU's q35 as tools/qemu-test.sh starts it has
 * no port; QEMU 10's pcie-root-port offers ACS (SV TB RR CR UF DT; not
 * with disable-acs=on), its xio3130-downstream switch port none.
 *
 * The refusal (pci_acs_check, vtd_rid.c, iommu_domain_create): pure, on a
 * made-up topology, every gap and every case that has none; live, with
 * translation on, every function marked at boot is refused a domain, and
 * every edu not marked (nor sharing a requester id) gets one.
 * tools/vtd-test.sh's vtd-acs run boots QEMU with edus below a switch
 * port without ACS, as a two-function device below a root port, and
 * alone below root ports with and without ACS. */
#include <jam/iommu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/object.h>
#include <jam/pci.h>
#include <jam/resource.h>
#include <jam/string.h>

#include "../dev/vtd_domain.h"

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

/* ---- the isolation check, pure ------------------------------------------------------------- */

#define SID(b, d, f) ((uint16_t)((b) << 8 | (d) << 3 | (f)))
#define ON  PCI_ACS_WANT

/* fns[i] is the function (seg, sid). */
static uint32_t at(const struct pci_acs_fn *fns, uint32_t n, uint16_t seg, uint16_t sid)
{
    for (uint32_t i = 0; i < n; i++)
        if (fns[i].seg == seg && fns[i].sid == sid)
            return i;
    ktest_fail("no %x in the topology", sid);
}

/* pci_acs_check for (seg 0, sid) is `want`, and names `by` when it isn't
 * isolated. */
static void expect(const struct pci_acs_fn *fns, uint32_t n, uint16_t sid, enum pci_acs_gap want,
                   uint16_t by)
{
    uint16_t where = 0xffff;
    enum pci_acs_gap got = pci_acs_check(fns, n, at(fns, n, 0, sid), &where);
    if (got != want || (want != PCI_ACS_ISOLATED && where != by))
        ktest_fail("%02x:%02x.%x: gap %d at %04x, want %d at %04x", sid >> 8, (sid >> 3) & 0x1f,
                   sid & 7, got, where, want, by);
}

/* A made-up topology (segment 0 unless said):
 *   00:1c.0 root port, no ACS -> 01:00.0 switch upstream port -> bus 2:
 *     02:01.0 downstream port, ACS on -> 03:00.0 an endpoint;
 *     02:02.0 downstream port, ACS on -> 04:00.0 and 04:00.1, a
 *       two-function device, each with RR and CR offered and on;
 *     02:03.0 an endpoint in the switch itself;
 *   00:1d.0 root port, no ACS -> 05:00.0 alone;
 *   00:1e.0 root port, ACS on -> 06:00.0 and 06:00.1, no ACS;
 *   00:1b.0 root port -> 07:00.0 and 07:01.0 (ARI: one device), the
 *     first offering nothing, the second RR and CR, on;
 *   00:1f.0 and 00:1f.3, a chipset device on the root bus, no ACS;
 *   00:1a.0 a bridge left unconfigured (secondary 0);
 *   segment 1: 03:00.0 on its root bus. */
static const struct pci_acs_fn topo[] = {
    { 0, SID(0, 0x1c, 0), PCIE_TYPE_ROOT_PORT, 1, 4, false, 0, 0 },
    { 0, SID(1, 0, 0), PCIE_TYPE_UPSTREAM, 2, 4, false, 0, 0 },
    { 0, SID(2, 1, 0), PCIE_TYPE_DOWNSTREAM, 3, 3, true, 0x7f, ON },
    { 0, SID(2, 2, 0), PCIE_TYPE_DOWNSTREAM, 4, 4, true, 0x7f, ON },
    { 0, SID(2, 3, 0), 0, 0, 0, false, 0, 0 },
    { 0, SID(3, 0, 0), 0, 0, 0, false, 0, 0 },
    { 0, SID(4, 0, 0), 0, 0, 0, true, PCI_ACS_RR | PCI_ACS_CR, PCI_ACS_RR | PCI_ACS_CR },
    { 0, SID(4, 0, 1), 0, 0, 0, true, PCI_ACS_RR | PCI_ACS_CR, PCI_ACS_RR | PCI_ACS_CR },
    { 0, SID(0, 0x1d, 0), PCIE_TYPE_ROOT_PORT, 5, 5, false, 0, 0 },
    { 0, SID(5, 0, 0), 0, 0, 0, false, 0, 0 },
    { 0, SID(0, 0x1e, 0), PCIE_TYPE_ROOT_PORT, 6, 6, true, 0x7f, ON },
    { 0, SID(6, 0, 0), 0, 0, 0, false, 0, 0 },
    { 0, SID(6, 0, 1), 0, 0, 0, false, 0, 0 },
    { 0, SID(0, 0x1b, 0), PCIE_TYPE_ROOT_PORT, 7, 7, false, 0, 0 },
    { 0, SID(7, 0, 0), 0, 0, 0, true, 0, 0 },
    { 0, SID(7, 1, 0), 0, 0, 0, true, PCI_ACS_RR | PCI_ACS_CR, PCI_ACS_RR | PCI_ACS_CR },
    { 0, SID(0, 0x1f, 0), PCIE_TYPE_NONE, 0, 0, false, 0, 0 },
    { 0, SID(0, 0x1f, 3), PCIE_TYPE_NONE, 0, 0, false, 0, 0 },
    { 0, SID(0, 0x1a, 0), PCIE_TYPE_ROOT_PORT, 0, 0, false, 0, 0 },
    { 1, SID(3, 0, 0), 0, 0, 0, false, 0, 0 },
};
#define NTOPO (sizeof(topo) / sizeof(topo[0]))

KTEST(pci_acs_check_topology)
{
    struct pci_acs_fn t[NTOPO];
    uint32_t n = NTOPO;
    memcpy(t, topo, sizeof(t));
    /* As made: below the switch, every port has ACS on and the
     * two-function device both functions; below root ports without ACS,
     * a device alone; a function offering nothing can't reach its
     * device's others; the chipset's device on the root bus; the ports. */
    expect(t, n, SID(3, 0, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(4, 0, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(4, 0, 1), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(2, 3, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(5, 0, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(7, 0, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(0, 0x1f, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(0, 0x1f, 3), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(0, 0x1c, 0), PCI_ACS_ISOLATED, 0);
    expect(t, n, SID(2, 1, 0), PCI_ACS_ISOLATED, 0);
    uint16_t where = 0;
    KT_EQ(pci_acs_check(t, n, at(t, n, 1, SID(3, 0, 0)), &where), PCI_ACS_ISOLATED);
    /* A two-function device below a root port, neither with ACS: both,
     * whatever the root port has. */
    expect(t, n, SID(6, 0, 0), PCI_ACS_GAP_FUNCTION, SID(6, 0, 0));
    expect(t, n, SID(6, 0, 1), PCI_ACS_GAP_FUNCTION, SID(6, 0, 0));

    /* One downstream port's ACS off: the devices below it (their own
     * requests go to a peer), and those below the other port and in the
     * switch (that port's device reaches them). Its own port is named
     * first when both are off. */
    t[at(t, n, 0, SID(2, 2, 0))].acs_ctl = 0;
    expect(t, n, SID(4, 0, 0), PCI_ACS_GAP_PORT, SID(2, 2, 0));
    expect(t, n, SID(4, 0, 1), PCI_ACS_GAP_PORT, SID(2, 2, 0));
    expect(t, n, SID(3, 0, 0), PCI_ACS_GAP_PEER_PORT, SID(2, 2, 0));
    expect(t, n, SID(2, 3, 0), PCI_ACS_GAP_PEER_PORT, SID(2, 2, 0));
    expect(t, n, SID(5, 0, 0), PCI_ACS_ISOLATED, 0);   /* another root port: not its switch */
    t[at(t, n, 0, SID(2, 1, 0))].acs = false;
    expect(t, n, SID(3, 0, 0), PCI_ACS_GAP_PORT, SID(2, 1, 0));
    memcpy(t, topo, sizeof(t));
    /* A downstream port with only some of the four (UF not offered): a gap. */
    t[at(t, n, 0, SID(2, 1, 0))].acs_cap = 0x0f;
    t[at(t, n, 0, SID(2, 1, 0))].acs_ctl = PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR;
    expect(t, n, SID(3, 0, 0), PCI_ACS_GAP_PORT, SID(2, 1, 0));
    memcpy(t, topo, sizeof(t));
    /* One function of the two offering RR and CR without them on (not
     * enabled yet): both are refused, it named. */
    t[at(t, n, 0, SID(4, 0, 1))].acs_ctl = PCI_ACS_RR;
    expect(t, n, SID(4, 0, 0), PCI_ACS_GAP_FUNCTION, SID(4, 0, 1));
    expect(t, n, SID(4, 0, 1), PCI_ACS_GAP_FUNCTION, SID(4, 0, 1));
    memcpy(t, topo, sizeof(t));
    /* ARI: below a root port, functions at other device numbers are one
     * device's. */
    t[at(t, n, 0, SID(7, 1, 0))].acs = false;
    expect(t, n, SID(7, 0, 0), PCI_ACS_GAP_FUNCTION, SID(7, 1, 0));
    /* A list of one: its function is on a root bus. */
    KT_EQ(pci_acs_check(t, 1, 0, &where), PCI_ACS_ISOLATED);
    /* Every gap has words. */
    KT_ASSERT(strcmp(pci_acs_gap_text(PCI_ACS_GAP_PORT), "?") != 0);
    KT_ASSERT(strcmp(pci_acs_gap_text(PCI_ACS_GAP_PEER_PORT), "?") != 0);
    KT_ASSERT(strcmp(pci_acs_gap_text(PCI_ACS_GAP_FUNCTION), "?") != 0);
}

/* ---- the refusal, live --------------------------------------------------------------------- */

/* With translation on: every function a driver could ask for that the
 * boot marked is refused a domain, and every edu not marked (nor sharing
 * a requester id) gets one. Then edu, made to look marked, is refused a
 * DMA capability and its context entry stays home. */
KTEST(pci_acs_gap_refused)
{
    if (!iommu_translating()) {
        kprintf("ktest %s: VT-d translation is off (iommu=on), skipped\n", ktest_current);
        return;
    }
    uint32_t refused = 0, marked = 0, edus = 0, allowed = 0;
    for (uint32_t i = 0; vtd_fn_at(i); i++) {
        struct vtd_fn *f = vtd_fn_at(i);
        struct pci_dev *d = f->dev;
        bool edu = d->info.vendor == 0x1234 && d->info.device == 0x11e8;
        if (!f->ctl || (d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY)) ||
            (!f->acs_gap && (!edu || f->shared)))
            continue;
        struct iommu_domain *dom = NULL;
        status_t st = iommu_domain_create(d, NULL, &dom);
        if (st == OK)
            KT_EQ(iommu_domain_destroy(dom), OK);
        if (f->acs_gap) {
            marked++;
            refused += st == ERR_ACCESS_DENIED;
        } else {
            edus++;
            allowed += st == OK;
        }
    }
    KT_EQ(refused, marked);
    KT_EQ(allowed, edus);
    kprintf("ktest %s: %u function(s) refused for an ACS gap, %u edu(s) allowed\n",
            ktest_current, refused, allowed);

    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    struct vtd_fn *f = vtd_fn_of(d);
    if (!f || f->acs_gap || f->shared)
        return;
    struct vtd_dom *home = f->cur;
    struct kobject *cap = NULL;
    f->acs_gap = PCI_ACS_GAP_FUNCTION;
    f->acs_where = f->sid;
    status_t st = dma_cap_create_for(d, NULL, &cap);
    f->acs_gap = PCI_ACS_ISOLATED;
    f->acs_where = 0;
    if (st == OK)
        kobject_unref(cap);
    KT_EQ(st, ERR_ACCESS_DENIED);
    KT_ASSERT(f->cur == home);
}
