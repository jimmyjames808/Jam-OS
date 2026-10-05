/* The `iommu` debug command's output (kernel/debug/dbgcmd.c calls
 * iommu_report, <jam/iommu.h>): what the VT-d units, domains, faults and
 * interrupt remapping table hold right now. Read-only; it writes no
 * register and changes nothing. It takes each unit's context mutex while
 * it walks that unit's functions and domains (thread context), and reads
 * the invalidation counters and the per-requester fault counts without a
 * lock (a benign race with the queue waiters and the fault log thread: a
 * debug dump, not a measurement). */
#include <jam/iommu.h>
#include <jam/irq_remap.h>
#include <jam/kprintf.h>
#include <jam/pci.h>
#include <jam/vtd.h>

#include "vtd_domain.h"
#include "vtd_internal.h"

/* One domain, printed once (deduplicated by pointer within a unit). */
static void report_domain(const struct vtd_dom *d)
{
    if (d->kind == VTD_DOM_PASS)
        kprintf("iommu:     domain %u (%s): pass-through, %u function(s)\n", d->ud.did, d->what,
                d->users);
    else
        kprintf("iommu:     domain %u (%s): %lu page(s) mapped, %u table page(s), %u "
                "function(s)\n", d->ud.did, d->what, (unsigned long)d->pt.mapped, d->pt.tables,
                d->users);
}

/* The distinct domains the unit's functions name, and each function's. */
static void report_unit_domains(struct vtd_ctl *ctl)
{
    const struct vtd_dom *seen[64];
    unsigned nseen = 0;
    mutex_lock(&ctl->lock);
    uint32_t ids = 0;
    for (uint32_t w = 0; w < (ctl->ndid + 63) / 64; w++)
        for (uint64_t bits = ctl->did_used[w]; bits; bits &= bits - 1)
            ids++;
    kprintf("iommu:   domains: %u of %u ids in use; %u function(s) covered, %u boot domain(s)\n",
            ids, ctl->ndid, ctl->nfn, ctl->nboot);
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *dev = pci_get(i);
        struct vtd_fn *f = dev ? vtd_fn_of(dev) : NULL;
        if (!f || f->ctl != ctl || !f->cur)
            continue;
        unsigned k = 0;
        while (k < nseen && seen[k] != f->cur)
            k++;
        if (k == nseen && nseen < 64)
            seen[nseen++] = f->cur;
        kprintf("iommu:     %02x:%02x.%x -> domain %u (%s)%s, %u DMA fault(s) since attached\n",
                dev->info.bus,
                dev->info.dev, dev->info.fn, f->cur->ud.did, f->cur->what,
                f->muted ? ", muted" : "", f->dma_faults);
    }
    for (unsigned k = 0; k < nseen; k++)
        report_domain(seen[k]);
    mutex_unlock(&ctl->lock);
}

static void report_unit(struct vtd_unit *u)
{
    uint32_t g = vtd_rd32(u, VTD_GSTS);
    kprintf("iommu:   unit %u: registers %#lx, version %u.%u\n", u->index,
            (unsigned long)u->base, u->ver >> 4 & 0xf, u->ver & 0xf);
    kprintf("iommu:     translation %s, interrupt remapping %s, queued invalidation %s\n",
            g & VTD_GSTS_TES ? "on" : "off", g & VTD_GSTS_IRES ? "on" : "off",
            g & VTD_GSTS_QIES ? "on" : "off");
    const struct vtd_unit_stats *s = &u->stats;
    kprintf("iommu:     queue: %lu submission(s), %lu descriptor(s), %lu wrap(s), %lu refused, "
            "%lu timed out\n", (unsigned long)s->submissions, (unsigned long)s->descriptors,
            (unsigned long)s->wraps, (unsigned long)s->refused, (unsigned long)s->timeouts);
    kprintf("iommu:     faults: %lu event interrupt(s), %lu record(s) read, %lu lost\n",
            (unsigned long)s->fault_irqs, (unsigned long)s->faults, (unsigned long)s->faults_lost);
    if (u->fault_on)
        kprintf("iommu:     fault event interrupt on cpu %u, vector %u\n", u->fault_cpu,
                u->fault_vec);
    struct vtd_ctl *ctl = vtd_ctl_get(u->index);
    if (ctl && ctl->unit == u)
        report_unit_domains(ctl);
}

/* The DMA faults counted per requester (vtd_fault.c): the requesters with
 * no PCI function of their own (a DMA engine's writes QEMU gives no id,
 * ff:1f.7) show here where the per-function lines above do not. */
static void report_faults(void)
{
    const struct vtd_fault_counts *c = vtd_fault_counts_get();
    if (!c->ndev && !c->other) {
        kprintf("iommu:   no DMA faults recorded\n");
        return;
    }
    kprintf("iommu:   DMA faults per requester:\n");
    for (uint32_t i = 0; i < c->ndev; i++)
        kprintf("iommu:     unit %u %02x:%02x.%x: %lu\n", c->dev[i].unit, c->dev[i].sid >> 8,
                c->dev[i].sid >> 3 & 0x1f, c->dev[i].sid & 7, (unsigned long)c->dev[i].count);
    if (c->other)
        kprintf("iommu:     other requesters (counts table full): %lu\n", (unsigned long)c->other);
}

void iommu_report(void)
{
    if (!iommu_translating() && !irq_remap_on()) {
        kprintf("iommu: DMA translation and interrupt remapping are off (booted without "
                "iommu=on)\n");
        return;
    }
    kprintf("iommu: DMA translation %s, interrupt remapping %s\n",
            iommu_translating() ? "on" : "off", irq_remap_on() ? "on" : "off");
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++) {
        struct vtd_unit *u = vtd_unit_get(i);
        if (u)
            report_unit(u);
    }
    report_faults();
    kprintf("iommu: interrupt remapping table: %u entry(ies) in use\n", irq_remap_used());
}
