/* PCIe Access Control Services (PCIe 6.0, 6.12 and 7.7.11): turned on at
 * boot where a port or a multi-function device offers it, so that a
 * device's requests to a peer go up to the root complex, where the IOMMU
 * translates (and blocks) them, instead of straight to the peer's BARs.
 *
 * What is turned on, the bits the capability register offers of:
 *   SV  source validation: a request coming up through the port must carry
 *       a requester id from below it (a device can't use another's id);
 *   RR  peer-to-peer request redirect: a request for a peer goes up;
 *   CR  peer-to-peer completion redirect: so do completions;
 *   UF  upstream forwarding: one that would go back down the port it came
 *       in by goes up too.
 * Where: every root port and switch downstream port that has the
 * capability, and every function of a multi-function device that has it
 * (a function offers RR and CR only if it can reach its device's other
 * functions). Translation blocking and egress control stay off: Jam OS
 * uses no device TLBs (ATS), and redirect already sends everything up.
 *
 * Linux does the same when its IOMMU is on (pci_std_enable_acs). So does
 * Jam OS: pci_acs_enable runs from the IOMMU's boot (vtd_rid_mark), before
 * translation goes on; with iommu=off the ports stay as the firmware left
 * them (no IOMMU sees the redirected requests). It runs once per boot and
 * only sets bits, so a kexec'd kernel finds them on already.
 *
 * What ACS can't fix, pci_acs_check finds: a function below a switch
 * downstream port without it (on its way up, or beside it on the
 * switch), or of a multi-function device below a port whose functions
 * don't all have it. vtd_rid.c marks those at boot and the IOMMU gives
 * them no domain, so no DMA capability (as it does for functions behind
 * a PCIe-to-PCI bridge). Root-complex integrated functions and the ports
 * themselves are never in question, nor is a function below a root port
 * without ACS (the owner's decision B, 2026-10-09): Jam OS doesn't treat
 * the root complex's routing between its root ports as a gap. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/report.h>
#include <jam/string.h>

#include "pci_internal.h"

#define PCIE_CAPS   0x02   /* the PCIe capability's own register, from the capability */
#define ACS_CAP_REG 0x04   /* 16 bits, from the ACS capability */
#define ACS_CTL_REG 0x06   /* 16 bits */

uint16_t pci_acs_ctl_wanted(uint16_t cap, uint16_t ctl)
{
    return (uint16_t)(ctl | (cap & PCI_ACS_WANT));
}

void pci_acs_read(struct pci_dev *d, struct pci_acs_fn *out)
{
    *out = (struct pci_acs_fn){
        .seg = d->info.segment,
        .sid = (uint16_t)(d->info.bus << 8 | d->info.dev << 3 | d->info.fn),
        .type = PCIE_TYPE_NONE,
    };
    uint64_t f = spin_lock_irqsave(&pci_lock);
    if (d->cap_pcie)
        out->type = (uint8_t)((rd(d, d->cap_pcie + PCIE_CAPS, 2) >> 4) & 0xf);
    uint8_t header = rd(d, CFG_HEADER, 1) & 0x7f;
    if (header == 1 || header == 2) {
        out->secondary = (uint8_t)rd(d, CFG_SECONDARY, 1);
        out->subordinate = (uint8_t)rd(d, CFG_SUBORD, 1);
    }
    if (d->cap_acs) {
        out->acs = true;
        out->acs_cap = (uint16_t)rd(d, d->cap_acs + ACS_CAP_REG, 2);
        out->acs_ctl = (uint16_t)rd(d, d->cap_acs + ACS_CTL_REG, 2);
    }
    spin_unlock_irqrestore(&pci_lock, f);
}

/* ---- the topology, pure ------------------------------------------------------------------ */

static uint8_t bus_of(const struct pci_acs_fn *a) { return (uint8_t)(a->sid >> 8); }

/* The bridge in fns[0..n) whose secondary bus is (seg, bus), or NULL: a
 * root bus. A bridge whose secondary bus isn't above its own, or past its
 * subordinate, leads nowhere (unconfigured). */
static const struct pci_acs_fn *bridge_above(const struct pci_acs_fn *fns, uint32_t n,
                                             uint16_t seg, uint8_t bus)
{
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_acs_fn *b = &fns[i];
        if (b->seg == seg && b->secondary == bus && b->secondary > bus_of(b) &&
            b->secondary <= b->subordinate)
            return &fns[i];
    }
    return NULL;
}

/* Are a and b functions of one device? Below a root port or a switch's
 * downstream port (a link: one device) every function on the bus is, at
 * any device number (ARI numbers a device's functions past 7 that way);
 * elsewhere those with the same device number. `up` is the bridge above
 * their bus (NULL: a root bus). */
static bool same_device(const struct pci_acs_fn *a, const struct pci_acs_fn *b,
                        const struct pci_acs_fn *up)
{
    if (a->seg != b->seg || bus_of(a) != bus_of(b))
        return false;
    if (up && (up->type == PCIE_TYPE_ROOT_PORT || up->type == PCIE_TYPE_DOWNSTREAM))
        return true;
    return ((a->sid >> 3) & 0x1f) == ((b->sid >> 3) & 0x1f);
}

/* How many functions fns[i]'s device has (itself included). */
static uint32_t device_functions(const struct pci_acs_fn *fns, uint32_t n, uint32_t i,
                                 const struct pci_acs_fn *up)
{
    uint32_t k = 0;
    for (uint32_t j = 0; j < n; j++)
        k += same_device(&fns[i], &fns[j], up);
    return k;
}

/* ---- isolation, pure ---------------------------------------------------------------------- */

/* A switch downstream port isolates the devices below it with all four on
 * (PCIe 6.12.1.1: a downstream port with ACS implements them all). */
static bool port_isolated(const struct pci_acs_fn *p)
{
    return p->acs && (p->acs_ctl & PCI_ACS_WANT) == PCI_ACS_WANT;
}

/* A function of a multi-function device with the bits it offers on (a
 * function offers RR and CR exactly when it can reach its device's other
 * functions, 6.12.1.2: what it doesn't offer it can't do). */
static bool function_isolated(const struct pci_acs_fn *f)
{
    uint16_t offered = f->acs_cap & PCI_ACS_WANT;
    return f->acs && (f->acs_ctl & offered) == offered;
}

enum pci_acs_gap pci_acs_check(const struct pci_acs_fn *fns, uint32_t n, uint32_t i,
                               uint16_t *where)
{
    const struct pci_acs_fn *f = &fns[i];
    const struct pci_acs_fn *up = bridge_above(fns, n, f->seg, bus_of(f));
    if (!up)
        return PCI_ACS_ISOLATED;   /* on a root bus: root-complex integrated, or a root port */
    if (device_functions(fns, n, i, up) > 1)
        for (uint32_t j = 0; j < n; j++)
            if (same_device(f, &fns[j], up) && !function_isolated(&fns[j])) {
                *where = fns[j].sid;
                return PCI_ACS_GAP_FUNCTION;
            }
    /* Up to the root, one bridge at a time: each step goes to a lower bus
     * number (bridge_above), so 256 steps reach a root bus. `via` is the
     * bridge the walk came up through onto `bus`. */
    const struct pci_acs_fn *via = NULL;
    uint8_t bus = bus_of(f);
    for (unsigned step = 0; step < 256 && up; step++) {
        if (up->type == PCIE_TYPE_DOWNSTREAM && !port_isolated(up)) {
            *where = up->sid;
            return PCI_ACS_GAP_PORT;
        }
        if (up->type == PCIE_TYPE_UPSTREAM)   /* `bus` is a switch's own: its downstream ports */
            for (uint32_t j = 0; j < n; j++) {
                const struct pci_acs_fn *p = &fns[j];
                if (p->seg == f->seg && bus_of(p) == bus && p->type == PCIE_TYPE_DOWNSTREAM &&
                    !port_isolated(p)) {
                    *where = p->sid;
                    return p == via ? PCI_ACS_GAP_PORT : PCI_ACS_GAP_PEER_PORT;
                }
            }
        via = up;
        bus = bus_of(up);
        up = bridge_above(fns, n, f->seg, bus);
    }
    if (up) {   /* can't happen (see above); refuse rather than guess */
        *where = up->sid;
        return PCI_ACS_GAP_PORT;
    }
    return PCI_ACS_ISOLATED;
}

const char *pci_acs_gap_text(enum pci_acs_gap gap)
{
    switch (gap) {
    case PCI_ACS_ISOLATED:      return "isolated";
    case PCI_ACS_GAP_PORT:      return "the switch port above it has no ACS on";
    case PCI_ACS_GAP_PEER_PORT: return "another downstream port of its switch has no ACS on";
    case PCI_ACS_GAP_FUNCTION:  return "a function of its multi-function device has no ACS on";
    }
    return "?";
}

/* ---- the boot ------------------------------------------------------------------------------ */

/* "SV RR CR UF", the bits of PCI_ACS_WANT in bits, or "none". */
static void fmt_bits(char *buf, size_t len, uint16_t bits)
{
    static const struct { uint16_t bit; const char *name; } names[] = {
        { PCI_ACS_SV, "SV" }, { PCI_ACS_RR, "RR" }, { PCI_ACS_CR, "CR" }, { PCI_ACS_UF, "UF" },
    };
    size_t at = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && at < len; i++)
        if (bits & names[i].bit)
            at += ksnprintf(buf + at, len - at, "%s%s", at ? " " : "", names[i].name);
    if (!at)
        ksnprintf(buf, len, "none");
}

enum { ROOT_PORT, DOWN_PORT, MULTI_FN, KINDS };
static const char *const kind_names[KINDS] = {
    "root port", "switch downstream port", "multi-function device",
};

status_t pci_acs_enable(void)
{
    uint32_t n = pci_count();
    struct pci_acs_fn *fns = kmalloc((n ? n : 1) * sizeof(*fns));
    if (!fns)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < n; i++)
        pci_acs_read(pci_get(i), &fns[i]);
    uint32_t on[KINDS] = { 0 }, without[KINDS] = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        struct pci_acs_fn *a = &fns[i];
        struct pci_dev *d = pci_get(i);
        const struct pci_acs_fn *up = bridge_above(fns, n, a->seg, bus_of(a));
        int kind = a->type == PCIE_TYPE_ROOT_PORT                ? ROOT_PORT
                 : a->type == PCIE_TYPE_DOWNSTREAM               ? DOWN_PORT
                 : device_functions(fns, n, i, up) > 1           ? MULTI_FN
                                                                 : -1;
        if (kind < 0)
            continue;
        char bdf[16];
        pci_fmt_bdf(bdf, sizeof(bdf), d);
        if (!a->acs) {
            /* A multi-function device on a root bus (the chipset's, a
             * root-complex integrated one) is not refused for it
             * (vtd_rid.c): not worth a line. */
            if (kind != MULTI_FN || up) {
                without[kind]++;
                kprintf("pci: no ACS at %s (%s%s)\n", bdf, kind_names[kind],
                        kind == MULTI_FN ? ", below a port" : "");
            }
            continue;
        }
        uint16_t want = pci_acs_ctl_wanted(a->acs_cap, a->acs_ctl);
        if (want != a->acs_ctl)
            pci_cfg_write(d, d->cap_acs + ACS_CTL_REG, 2, want);
        uint16_t back = (uint16_t)pci_cfg_read(d, d->cap_acs + ACS_CTL_REG, 2);
        on[kind]++;
        char bits[24], missing[24];
        uint16_t offered = a->acs_cap & PCI_ACS_WANT;
        fmt_bits(bits, sizeof(bits), offered);
        fmt_bits(missing, sizeof(missing), (uint16_t)(PCI_ACS_WANT & ~offered));
        kprintf("pci: ACS on at %s (%s): %s%s%s%s\n", bdf, kind_names[kind], bits,
                (a->acs_ctl & offered) == offered ? " (on already)" : "",
                kind != MULTI_FN && offered != PCI_ACS_WANT ? "; not offered: " : "",
                kind != MULTI_FN && offered != PCI_ACS_WANT ? missing : "");
        if ((back & offered) != offered) {
            char lost[24];
            fmt_bits(lost, sizeof(lost), (uint16_t)(offered & ~back));
            report("pci: ACS at %s (%s): %s didn't stay on", bdf, kind_names[kind], lost);
        }
        a->acs_ctl = back;
    }
    kprintf("pci: ACS on at %u root port%s, %u switch downstream port%s and %u function%s of "
            "multi-function devices; without it: %u, %u and %u below a port\n", on[ROOT_PORT],
            on[ROOT_PORT] == 1 ? "" : "s", on[DOWN_PORT], on[DOWN_PORT] == 1 ? "" : "s",
            on[MULTI_FN], on[MULTI_FN] == 1 ? "" : "s", without[ROOT_PORT], without[DOWN_PORT],
            without[MULTI_FN]);
    kfree(fns);
    return OK;
}
