/* The kernel's PCI core (M6 Track A): ECAM config access, the bus walk,
 * the device table, capability lists, BAR decode/sizing, MSI and MSI-X
 * programming and Bus Master Enable. See <jam/pci.h>.
 *
 * Enumeration (pci_init) runs once, on CPU 0, before any driver exists:
 *   1. every MCFG segment is walked from its start bus, following each
 *      bridge's secondary..subordinate numbers exactly as the firmware set
 *      them (nothing is ever renumbered); each bus's 1 MiB of ECAM is mapped
 *      uncached when the walk reaches it;
 *   2. the table is sorted by segment:bus:device.function;
 *   3. BARs are read as-is, and the boot display is found: the function
 *      whose memory BAR holds the boot framebuffer (the memory BAR with the
 *      highest base at or below the framebuffer, among functions decoding
 *      memory: BARs never overlap, so that is the one containing it);
 *   4. every other function's BARs are sized with its decode briefly off
 *      and restored exactly. Never sized, command register never written:
 *      the display, every bridge (header type 1/2 or class 06: host, ISA/LPC
 *      and PCI bridges), and as a belt-and-braces rule any display-class
 *      (03) function;
 *   5. capabilities (standard + extended) are walked with bounded,
 *      cycle-checked loops; MSI-X tables and PBAs become protected pages,
 *      and each table gets an uncached kernel mapping.
 *
 * After pci_init the table never changes, so lookups take no lock. Every
 * config space and MSI-X table access goes through `pci_lock` (IRQ-safe,
 * a leaf: nothing else is taken while it is held). */
#include <jam/acpi.h>
#include <jam/fbcon.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/report.h>
#include <jam/spinlock.h>
#include <jam/string.h>

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

/* Capability walks: a standard list has at most 48 entries in 0x40-0xff,
 * an extended one at most 960 in 0x100-0xfff. */
#define STD_CAP_MAX 48
#define EXT_CAP_MAX 960

/* ---- state ----------------------------------------------------------------- */

struct ecam_seg {
    uint64_t phys;            /* ECAM base (bus 0's address, per MCFG) */
    uint16_t segment;
    uint8_t  bus_start, bus_end;
    volatile uint8_t *bus_va[256];   /* uncached 1 MiB per bus, NULL = not walked */
    uint8_t  visited[32];            /* bus bitmap: loop guard for the walk */
};

static struct ecam_seg segs[ACPI_MAX_ECAM];
static uint32_t nsegs;
static struct pci_dev devs[PCI_MAX_DEVS];
static uint32_t ndevs;
static bool table_full;
static spinlock_t pci_lock = SPINLOCK_INIT("pci");

/* Pages holding an MSI-X table or PBA, [start, end) page aligned. */
#define MAX_PROT (2 * PCI_MAX_DEVS)
static struct { uint64_t start, end; } prot[MAX_PROT];
static uint32_t nprot;

/* Where each MSI-X table / PBA lives, per function (0 = unknown). */
static uint64_t msix_table_phys[PCI_MAX_DEVS], msix_pba_phys[PCI_MAX_DEVS];

static uint64_t display_fb;   /* framebuffer phys, 0 if none */

/* ---- raw config access (callers hold pci_lock or run single-threaded) ------- */

static bool cfg_args_ok(uint32_t off, uint32_t width)
{
    return (width == 1 || width == 2 || width == 4) && off < 4096 && !(off & (width - 1));
}

static uint32_t raw_read(volatile void *cfg, uint32_t off, uint32_t width)
{
    volatile uint8_t *p = (volatile uint8_t *)cfg + off;
    switch (width) {
    case 1:  return *p;
    case 2:  return *(volatile uint16_t *)p;
    default: return *(volatile uint32_t *)p;
    }
}

static void raw_write(volatile void *cfg, uint32_t off, uint32_t width, uint32_t v)
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

uint32_t pci_cfg_read(struct pci_dev *d, uint32_t off, uint32_t width)
{
    if (!d || !d->cfg || !cfg_args_ok(off, width))
        return width == 1 ? 0xff : width == 2 ? 0xffff : 0xffffffffu;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint32_t v = raw_read(d->cfg, off, width);
    spin_unlock_irqrestore(&pci_lock, f);
    return v;
}

void pci_cfg_write(struct pci_dev *d, uint32_t off, uint32_t width, uint32_t v)
{
    if (!d || !d->cfg || !cfg_args_ok(off, width))
        return;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    raw_write(d->cfg, off, width, v);
    spin_unlock_irqrestore(&pci_lock, f);
}

static struct ecam_seg *seg_of(uint16_t segment)
{
    for (uint32_t i = 0; i < nsegs; i++)
        if (segs[i].segment == segment)
            return &segs[i];
    return NULL;
}

static volatile uint8_t *fn_cfg(struct ecam_seg *s, uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (!s || bus < s->bus_start || bus > s->bus_end || !s->bus_va[bus] || dev > 31 || fn > 7)
        return NULL;
    return s->bus_va[bus] + ((uint32_t)dev << 15) + ((uint32_t)fn << 12);
}

/* Config read by address, for any function on a bus the walk mapped (tests:
 * a missing function reads all-ones). All-ones for an unmapped bus too. */
uint32_t pci_cfg_read_bdf(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t fn,
                          uint32_t off, uint32_t width)
{
    volatile uint8_t *cfg = fn_cfg(seg_of(segment), bus, dev, fn);
    if (!cfg || !cfg_args_ok(off, width))
        return width == 1 ? 0xff : width == 2 ? 0xffff : 0xffffffffu;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint32_t v = raw_read(cfg, off, width);
    spin_unlock_irqrestore(&pci_lock, f);
    return v;
}

/* ---- capabilities ------------------------------------------------------------ */

static bool untouchable(const struct pci_dev *d)
{
    return d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY);
}

/* Standard list: bounded and cycle-checked (a visited bitmap over the 64
 * dword slots), so a garbage or looping list ends. Caller holds the lock or
 * runs at init. */
static uint16_t find_std_cap(struct pci_dev *d, uint8_t id)
{
    if (!(raw_read(d->cfg, CFG_STATUS, 2) & STATUS_CAPS))
        return 0;
    uint8_t type = raw_read(d->cfg, CFG_HEADER, 1) & 0x7f;
    uint32_t ptr = raw_read(d->cfg, type == 2 ? CFG_CAP_PTR_CB : CFG_CAP_PTR, 1) & 0xfc;
    uint64_t seen = 0;   /* bit n = dword slot n of 0x00-0xff */
    for (int i = 0; i < STD_CAP_MAX && ptr >= 0x40; i++) {
        uint64_t bit = 1ull << (ptr >> 2);
        if (seen & bit)
            return 0;   /* cycle */
        seen |= bit;
        uint32_t hdr = raw_read(d->cfg, ptr, 2);
        if ((hdr & 0xff) == 0xff)
            return 0;   /* all-ones: gone or garbage */
        if ((hdr & 0xff) == id)
            return (uint16_t)ptr;
        ptr = (hdr >> 8) & 0xfc;
    }
    return 0;
}

/* Extended list (PCIe functions only): from 0x100, same guards over the
 * 960 dword slots of 0x100-0xfff. */
static uint16_t find_ext_cap(struct pci_dev *d, uint16_t id)
{
    if (!d->cap_pcie)
        return 0;
    uint64_t seen[15] = { 0 };   /* 960 bits */
    uint32_t ptr = 0x100;
    for (int i = 0; i < EXT_CAP_MAX && ptr >= 0x100 && ptr < 0x1000; i++) {
        uint32_t slot = (ptr - 0x100) >> 2;
        if (seen[slot / 64] & (1ull << (slot % 64)))
            return 0;
        seen[slot / 64] |= 1ull << (slot % 64);
        uint32_t hdr = raw_read(d->cfg, ptr, 4);
        if (hdr == 0 || hdr == 0xffffffffu)
            return 0;
        if ((hdr & 0xffff) == id)
            return (uint16_t)ptr;
        ptr = (hdr >> 20) & 0xffc;
    }
    return 0;
}

uint16_t pci_find_cap(struct pci_dev *d, uint32_t id)
{
    if (!d || !d->cfg)
        return 0;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint16_t off = id < 0x100 ? find_std_cap(d, (uint8_t)id)
                 : id < 0x10000 + 0x100 ? find_ext_cap(d, (uint16_t)(id - 0x100)) : 0;
    spin_unlock_irqrestore(&pci_lock, f);
    return off;
}

/* ---- the walk -------------------------------------------------------------- */

static bool bus_seen(struct ecam_seg *s, uint8_t bus) { return s->visited[bus / 8] & (1u << (bus % 8)); }

static void add_function(struct ecam_seg *s, uint8_t bus, uint8_t dev, uint8_t fn)
{
    if (ndevs == PCI_MAX_DEVS) {
        table_full = true;
        return;
    }
    struct pci_dev *d = &devs[ndevs++];
    memset(d, 0, sizeof(*d));
    d->cfg = fn_cfg(s, bus, dev, fn);
    struct pci_dev_info *in = &d->info;
    in->segment = s->segment;
    in->bus = bus;
    in->dev = dev;
    in->fn = fn;
    in->vendor = rd(d, CFG_VENDOR, 2);
    in->device = rd(d, CFG_DEVICE, 2);
    uint32_t rc = rd(d, CFG_REVISION, 4);
    in->revision = rc & 0xff;
    in->prog_if = (rc >> 8) & 0xff;
    in->subclass = (rc >> 16) & 0xff;
    in->class_code = rc >> 24;
    in->header_type = rd(d, CFG_HEADER, 1);
    if ((in->header_type & 0x7f) != 0 || in->class_code == 0x06)
        in->flags |= PCI_INFO_BRIDGE;
}

static void walk_bus(struct ecam_seg *s, uint8_t bus, int depth);

/* Follow each bridge on `bus` to its secondary bus, in device order. */
static void walk_children(struct ecam_seg *s, uint8_t bus, int depth)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        volatile uint8_t *cfg0 = fn_cfg(s, bus, dev, 0);
        if (raw_read(cfg0, CFG_VENDOR, 2) == 0xffff)
            continue;
        uint8_t nfn = (raw_read(cfg0, CFG_HEADER, 1) & 0x80) ? 8 : 1;
        for (uint8_t fn = 0; fn < nfn; fn++) {
            volatile uint8_t *cfg = fn_cfg(s, bus, dev, fn);
            if (raw_read(cfg, CFG_VENDOR, 2) == 0xffff)
                continue;
            uint8_t type = raw_read(cfg, CFG_HEADER, 1) & 0x7f;
            if (type != 1 && type != 2)
                continue;
            uint8_t sec = raw_read(cfg, CFG_SECONDARY, 1);
            uint8_t sub = raw_read(cfg, CFG_SUBORD, 1);
            /* The firmware's numbers only: an unconfigured (0) or
             * nonsensical range is skipped, never assigned. */
            if (sec > bus && sec <= sub)
                walk_bus(s, sec, depth + 1);
        }
    }
}

static void walk_bus(struct ecam_seg *s, uint8_t bus, int depth)
{
    if (bus < s->bus_start || bus > s->bus_end || bus_seen(s, bus) || depth > 64)
        return;
    s->visited[bus / 8] |= 1u << (bus % 8);
    if (!s->bus_va[bus])
        s->bus_va[bus] = vmm_map_mmio(s->phys + ((uint64_t)bus << 20), 1u << 20);

    /* This bus's functions first, then each bridge's secondary bus (the
     * table is sorted by address afterwards anyway). */
    for (uint8_t dev = 0; dev < 32; dev++) {
        volatile uint8_t *cfg0 = fn_cfg(s, bus, dev, 0);
        if (raw_read(cfg0, CFG_VENDOR, 2) == 0xffff)
            continue;
        uint8_t nfn = (raw_read(cfg0, CFG_HEADER, 1) & 0x80) ? 8 : 1;
        for (uint8_t fn = 0; fn < nfn; fn++)
            if (raw_read(fn_cfg(s, bus, dev, fn), CFG_VENDOR, 2) != 0xffff)
                add_function(s, bus, dev, fn);
    }
    walk_children(s, bus, depth);
}

static uint64_t bdf_key(const struct pci_dev *d)
{
    return ((uint64_t)d->info.segment << 16) | ((uint32_t)d->info.bus << 8) |
           ((uint32_t)d->info.dev << 3) | d->info.fn;
}

static void sort_table(void)
{
    for (uint32_t i = 1; i < ndevs; i++) {
        struct pci_dev t = devs[i];
        uint32_t j = i;
        for (; j > 0 && bdf_key(&devs[j - 1]) > bdf_key(&t); j--)
            devs[j] = devs[j - 1];
        devs[j] = t;
    }
    for (uint32_t i = 0; i < ndevs; i++)
        devs[i].index = i;
}

/* ---- BARs -------------------------------------------------------------------- */

static uint32_t bar_count(const struct pci_dev *d)
{
    switch (d->info.header_type & 0x7f) {
    case 0:  return 6;
    case 1:  return 2;
    case 2:  return 1;
    default: return 0;
    }
}

/* Read BARs as-is (no sizing): phys and flags. */
static void read_bars(struct pci_dev *d)
{
    uint32_t n = bar_count(d);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lo = rd(d, CFG_BAR0 + 4 * i, 4);
        if (lo == 0)
            continue;
        if (lo & 1) {
            d->info.bar[i].phys = lo & ~3u;
            d->info.bar[i].flags = PCI_BAR_IO;
            continue;
        }
        uint32_t fl = PCI_BAR_MMIO | ((lo & 8) ? PCI_BAR_PREFETCH : 0);
        uint64_t phys = lo & ~0xfu;
        if (((lo >> 1) & 3) == 2 && i + 1 < n) {
            fl |= PCI_BAR_64;
            phys |= (uint64_t)rd(d, CFG_BAR0 + 4 * (i + 1), 4) << 32;
        }
        d->info.bar[i].phys = phys;
        d->info.bar[i].flags = fl;
        if (fl & PCI_BAR_64)
            i++;
    }
}

/* Size every BAR with decode off, restoring each BAR and the command
 * register exactly. Refused for the display, bridges and display-class
 * functions. Returns the sizes found (0 = BAR not implemented). Also used
 * by the ktests to check that sizing leaves the function as it was. */
status_t pci_size_bars(struct pci_dev *d, uint64_t sizes[6])
{
    if (!d || untouchable(d) || d->info.class_code == 0x03)
        return ERR_ACCESS_DENIED;
    uint32_t n = bar_count(d);
    uint32_t lo_mask[6] = { 0 }, orig[6] = { 0 };
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint16_t cmd = rd(d, CFG_COMMAND, 2);
    if (cmd & (CMD_IO | CMD_MEMORY))
        wr(d, CFG_COMMAND, 2, cmd & ~(CMD_IO | CMD_MEMORY));
    for (uint32_t i = 0; i < n; i++) {
        orig[i] = rd(d, CFG_BAR0 + 4 * i, 4);
        wr(d, CFG_BAR0 + 4 * i, 4, 0xffffffffu);
        lo_mask[i] = rd(d, CFG_BAR0 + 4 * i, 4);
        wr(d, CFG_BAR0 + 4 * i, 4, orig[i]);
    }
    if (cmd & (CMD_IO | CMD_MEMORY))
        wr(d, CFG_COMMAND, 2, cmd);
    spin_unlock_irqrestore(&pci_lock, f);

    for (uint32_t i = 0; i < 6; i++)
        sizes[i] = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t m = lo_mask[i];
        if (m == 0 || m == 0xffffffffu)
            continue;   /* not implemented (all-ones: nothing answers) */
        if (orig[i] & 1) {                     /* I/O: 16 or 32 address bits */
            uint32_t a = m & ~3u;
            if (!(a & 0xffff0000u))
                a |= 0xffff0000u;
            sizes[i] = (uint32_t)(~a + 1);
            continue;
        }
        uint64_t a = m & ~0xfu;
        if (((orig[i] >> 1) & 3) == 2 && i + 1 < n) {
            /* An upper half that reads back 0 is hard-wired: the BAR can
             * only sit below 4 GiB, and the size comes from the low half
             * alone (the PC's VMD controller; read as size bits it gave
             * ~2^64). */
            uint32_t hi = lo_mask[i + 1];
            a |= hi ? (uint64_t)hi << 32 : 0xffffffff00000000ull;
            sizes[i] = a != 0xffffffff00000000ull ? ~a + 1 : 0;
            i++;
        } else {
            a |= 0xffffffff00000000ull;
            sizes[i] = a != 0xffffffff00000000ull ? ~a + 1 : 0;
        }
    }
    return OK;
}

/* The boot display: among functions decoding memory, the memory BAR with
 * the highest base at or below the framebuffer holds it. */
static void find_display(void)
{
    display_fb = fbcon_phys(NULL);
    if (!display_fb)
        return;
    struct pci_dev *best = NULL;
    uint64_t best_base = 0;
    for (uint32_t i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        if (!(rd(d, CFG_COMMAND, 2) & CMD_MEMORY))
            continue;
        for (int b = 0; b < 6; b++) {
            uint64_t base = d->info.bar[b].phys;
            if ((d->info.bar[b].flags & PCI_BAR_MMIO) && base && base <= display_fb &&
                base >= best_base) {
                best = d;
                best_base = base;
            }
        }
    }
    if (best)
        best->info.flags |= PCI_INFO_DISPLAY;
}

static void size_or_mark(struct pci_dev *d)
{
    uint64_t sizes[6];
    if (pci_size_bars(d, sizes) != OK) {
        for (int b = 0; b < 6; b++)
            if (d->info.bar[b].flags)
                d->info.bar[b].flags |= PCI_BAR_UNSIZED;
        return;
    }
    for (int b = 0; b < 6; b++) {
        uint32_t fl = d->info.bar[b].flags;
        if (!fl)
            continue;
        uint64_t phys = d->info.bar[b].phys;
        if (!sizes[b]) {   /* read non-zero but does not size: not a BAR */
            d->info.bar[b].flags = 0;
            d->info.bar[b].phys = 0;
        } else if ((sizes[b] & (sizes[b] - 1)) || sizes[b] > (1ull << 40) ||
                   phys + sizes[b] < phys || (phys & (sizes[b] - 1))) {
            /* Not a power of two, absurdly large, wrapping, or not aligned
             * to its size: the answer can't be right, so treat the BAR
             * like the display's (never handed out). */
            kprintf("pci: %02x:%02x.%u bar%d at %lx sizes to %lx: implausible, left unsized\n",
                    d->info.bus, d->info.dev, d->info.fn, b, phys, sizes[b]);
            d->info.bar[b].flags |= PCI_BAR_UNSIZED;
        } else {
            d->info.bar[b].size = sizes[b];
        }
        if (fl & PCI_BAR_64)
            b++;
    }
}

/* ---- MSI / MSI-X discovery ------------------------------------------------------- */

static void add_protected(uint64_t phys, uint64_t len)
{
    if (!phys || !len || nprot == MAX_PROT)
        return;
    prot[nprot].start = ALIGN_DOWN(phys, PAGE_SIZE);
    prot[nprot].end = ALIGN_UP(phys + len, PAGE_SIZE);
    nprot++;
}

/* Phys of BAR `bir` + off, or 0 if that BAR is not a memory BAR with an
 * address. */
static uint64_t bar_addr(const struct pci_dev *d, uint8_t bir, uint32_t off)
{
    if (bir > 5 || !(d->info.bar[bir].flags & PCI_BAR_MMIO) || !d->info.bar[bir].phys)
        return 0;
    return d->info.bar[bir].phys + off;
}

static void read_caps(struct pci_dev *d)
{
    d->cap_pcie = find_std_cap(d, CAP_ID_PCIE);
    d->cap_msi = find_std_cap(d, CAP_ID_MSI);
    d->cap_msix = find_std_cap(d, CAP_ID_MSIX);
    if (d->cap_msi) {
        uint16_t ctl = rd(d, d->cap_msi + 2, 2);
        d->msi_64 = ctl & MSI_CTL_64;
        d->msi_maskable = ctl & MSI_CTL_MASKABLE;
        uint32_t mmc = MSI_CTL_MMC(ctl);
        d->info.msi_vectors = 1u << (mmc > 5 ? 5 : mmc);
    }
    if (d->cap_msix) {
        uint16_t ctl = rd(d, d->cap_msix + 2, 2);
        uint32_t t = rd(d, d->cap_msix + 4, 4), p = rd(d, d->cap_msix + 8, 4);
        d->info.msix_vectors = MSIX_CTL_SIZE(ctl);
        d->msix_table_bar = t & 7;
        d->msix_table_off = t & ~7u;
        d->msix_pba_bar = p & 7;
        d->msix_pba_off = p & ~7u;
        uint32_t n = d->info.msix_vectors;
        uint64_t tp = bar_addr(d, d->msix_table_bar, d->msix_table_off);
        uint64_t pp = bar_addr(d, d->msix_pba_bar, d->msix_pba_off);
        msix_table_phys[d->index] = tp;
        msix_pba_phys[d->index] = pp;
        /* Protected even for the display and bridges when their BAR has an
         * address: being too careful here costs nothing. */
        add_protected(tp, (uint64_t)n * 16);
        add_protected(pp, ((n + 63) / 64) * 8);
        if (tp && !untouchable(d))
            d->msix_table = vmm_map_mmio(tp, (uint64_t)n * 16);
    }
}

bool pci_phys_protected(uint64_t phys, uint64_t len)
{
    if (!len)
        len = 1;
    uint64_t end = phys + len < phys ? UINT64_MAX : phys + len;
    for (uint32_t i = 0; i < nprot; i++)
        if (phys < prot[i].end && end > prot[i].start)
            return true;
    return false;
}

/* ---- names and logging ----------------------------------------------------------- */

static const char *class_name(const struct pci_dev *d)
{
    uint32_t c = d->info.class_code, s = d->info.subclass, p = d->info.prog_if;
    switch (c << 8 | s) {
    case 0x0100: return "SCSI";
    case 0x0101: return "IDE";
    case 0x0104: return "RAID";
    case 0x0106: return "SATA";
    case 0x0108: return "NVMe";
    case 0x0200: return "Ethernet";
    case 0x0280: return "network";
    case 0x0300: return "VGA";
    case 0x0302: return "3D";
    case 0x0380: return "display";
    case 0x0401: return "audio";
    case 0x0403: return "HD audio";
    case 0x0480: return "multimedia";
    case 0x0500: return "RAM ctl";
    case 0x0580: return "memory ctl";
    case 0x0600: return "host brg";
    case 0x0601: return "ISA brg";
    case 0x0604: return "PCI brg";
    case 0x0680: return "bridge";
    case 0x0700: return "serial";
    case 0x0780: return "comm";
    case 0x0805: return "SD host";
    case 0x0880: return "system";
    case 0x0c03:
        return p == 0x30 ? "xHCI" : p == 0x20 ? "EHCI" : p == 0x10 ? "OHCI" : p == 0 ? "UHCI" : "USB";
    case 0x0c05: return "SMBus";
    case 0x0c80: return "serial bus";
    case 0x1180: return "signal";
    }
    return "";
}

static void fmt_size(char *buf, size_t n, uint64_t size)
{
    if (size >= (1ull << 30) && !(size & ((1ull << 30) - 1)))
        ksnprintf(buf, n, "%luG", size >> 30);
    else if (size >= (1ull << 20) && !(size & ((1ull << 20) - 1)))
        ksnprintf(buf, n, "%luM", size >> 20);
    else if (size >= (1ull << 10) && !(size & ((1ull << 10) - 1)))
        ksnprintf(buf, n, "%luK", size >> 10);
    else
        ksnprintf(buf, n, "%lu", size);
}

static int fmt_bdf(char *buf, size_t n, const struct pci_dev *d)
{
    if (d->info.segment)
        return ksnprintf(buf, n, "%04x:%02x:%02x.%x", d->info.segment, d->info.bus, d->info.dev,
                         d->info.fn);
    return ksnprintf(buf, n, "%02x:%02x.%x", d->info.bus, d->info.dev, d->info.fn);
}

/* "bar0 mem64 pf 0x6000000000 256M bar2 io 0x3000 256 ..." */
static void fmt_bars(char *buf, size_t n, const struct pci_dev *d)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int b = 0; b < 6 && len < n; b++) {
        uint32_t fl = d->info.bar[b].flags;
        if (!fl)
            continue;
        char sz[24];
        if (fl & PCI_BAR_UNSIZED)
            ksnprintf(sz, sizeof(sz), "unsized");
        else
            fmt_size(sz, sizeof(sz), d->info.bar[b].size);
        len += ksnprintf(buf + len, n - len, "%sbar%d %s%s%s 0x%lx %s", len ? " " : "", b,
                         (fl & PCI_BAR_IO) ? "io" : "mem", (fl & PCI_BAR_64) ? "64" : "",
                         (fl & PCI_BAR_PREFETCH) ? " pf" : "", d->info.bar[b].phys, sz);
    }
}

static void fmt_irqs(char *buf, size_t n, const struct pci_dev *d)
{
    size_t len = ksnprintf(buf, n, "msi %u", d->info.msi_vectors);
    if (d->cap_msi && len < n)
        len += ksnprintf(buf + len, n - len, "%s%s", d->msi_64 ? " 64-bit" : " 32-bit",
                         d->msi_maskable ? " maskable" : "");
    if (len < n)
        len += ksnprintf(buf + len, n - len, ", msix %u", d->info.msix_vectors);
    if (d->cap_msix && len < n)
        ksnprintf(buf + len, n - len, " (table bar%u+0x%x, pba bar%u+0x%x)", d->msix_table_bar,
                  d->msix_table_off, d->msix_pba_bar, d->msix_pba_off);
}

static void log_function(const struct pci_dev *d)
{
    char bdf[16], irqs[96], bars[400];
    fmt_bdf(bdf, sizeof(bdf), d);
    fmt_irqs(irqs, sizeof(irqs), d);
    fmt_bars(bars, sizeof(bars), d);
    kprintf("pci: %s %04x:%04x class %02x%02x%02x rev %02x %s%s%s %s%s%s\n", bdf, d->info.vendor,
            d->info.device, d->info.class_code, d->info.subclass, d->info.prog_if,
            d->info.revision, class_name(d),
            (d->info.flags & PCI_INFO_DISPLAY) ? " [boot display]" : "",
            (d->info.flags & PCI_INFO_BRIDGE) ? " [bridge]" : "", irqs, bars[0] ? "; " : "", bars);
}

/* Two RESULTS lines for a function worth reading in full. */
static void report_full(const char *what, const struct pci_dev *d)
{
    char bdf[16], irqs[96], bars[400];
    fmt_bdf(bdf, sizeof(bdf), d);
    fmt_irqs(irqs, sizeof(irqs), d);
    fmt_bars(bars, sizeof(bars), d);
    report("pci: %s %s %04x:%04x rev %02x %s", what, bdf, d->info.vendor, d->info.device,
           d->info.revision, irqs);
    report("pci:   %s %s", bdf, bars[0] ? bars : "no BARs");
}

static void report_class(const char *what, uint8_t c, uint8_t s, int prog_if)
{
    uint32_t any = 0;
    for (uint32_t i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        if (d->info.class_code == c && d->info.subclass == s &&
            (prog_if < 0 || d->info.prog_if == prog_if) && any++ < 2)
            report_full(what, d);   /* the first two in full; the box is small */
    }
    if (any > 2)
        report("pci: %u more %s functions (see the log)", any - 2, what);
    if (!any && prog_if >= 0)
        report("pci: no %s function (class %02x%02x%02x)", what, c, s, prog_if);
    else if (!any)
        report("pci: no %s function (class %02x%02x)", what, c, s);
}

/* ---- init --------------------------------------------------------------------- */

void pci_init(void)
{
    for (uint32_t i = 0; i < acpi.ecam_count && nsegs < ACPI_MAX_ECAM; i++) {
        struct ecam_seg *s = &segs[nsegs++];
        s->phys = acpi.ecam[i].phys;
        s->segment = acpi.ecam[i].segment;
        s->bus_start = acpi.ecam[i].bus_start;
        s->bus_end = acpi.ecam[i].bus_end;
    }
    if (!nsegs) {
        report("pci: no MCFG (no ECAM): no PCI devices");
        return;
    }
    for (uint32_t i = 0; i < nsegs; i++)
        walk_bus(&segs[i], segs[i].bus_start, 0);
    sort_table();

    for (uint32_t i = 0; i < ndevs; i++)
        read_bars(&devs[i]);
    find_display();
    for (uint32_t i = 0; i < ndevs; i++) {
        size_or_mark(&devs[i]);
        read_caps(&devs[i]);
        log_function(&devs[i]);
    }

    uint32_t buses = 0;
    for (uint32_t i = 0; i < nsegs; i++)
        for (int b = 0; b < 256; b++)
            buses += bus_seen(&segs[i], (uint8_t)b);
    char disp[24] = "none";
    for (uint32_t i = 0; i < ndevs; i++)
        if (devs[i].info.flags & PCI_INFO_DISPLAY)
            fmt_bdf(disp, sizeof(disp), &devs[i]);
    report("pci: %u functions on %u bus%s (%u ECAM segment%s)%s; boot display %s (fb 0x%lx)",
           ndevs, buses, buses == 1 ? "" : "es", nsegs, nsegs == 1 ? "" : "s",
           table_full ? ", TABLE FULL" : "", disp, display_fb);
    report_class("xHCI", 0x0c, 0x03, 0x30);
    report_class("Ethernet", 0x02, 0x00, -1);
}

/* ---- lookups ---------------------------------------------------------------------- */

uint32_t pci_count(void) { return ndevs; }

struct pci_dev *pci_get(uint32_t index)
{
    return index < ndevs ? &devs[index] : NULL;
}

struct pci_dev *pci_find(uint16_t vendor, uint16_t device, uint32_t n)
{
    for (uint32_t i = 0; i < ndevs; i++)
        if ((vendor == 0xffff || devs[i].info.vendor == vendor) &&
            (device == 0xffff || devs[i].info.device == device) && n-- == 0)
            return &devs[i];
    return NULL;
}

/* ---- the Devices entry ------------------------------------------------------------ */

/* The RESULTS box holds 48 lines; the boot's other lines (topology, timer,
 * fpu, the pci_init lines, "run complete", maybe irq/serial) take about
 * ten to twelve, and the list's own header one. Up to 32 functions get a
 * line each, up to 64 two per line, beyond that three (compact). */
#define LIST_LINES 32

static int fmt_entry(char *buf, size_t n, const struct pci_dev *d, bool compact)
{
    char bdf[16];
    fmt_bdf(bdf, sizeof(bdf), d);
    const char *fl = (d->info.flags & PCI_INFO_DISPLAY) ? " D" : (d->info.flags & PCI_INFO_BRIDGE) ? " B" : "";
    if (compact)
        return ksnprintf(buf, n, "%s %04x:%04x %02x%02x%02x m%u x%u%s", bdf, d->info.vendor,
                         d->info.device, d->info.class_code, d->info.subclass, d->info.prog_if,
                         d->info.msi_vectors, d->info.msix_vectors, fl);
    return ksnprintf(buf, n, "%s %04x:%04x %02x%02x%02x %-10s msi %-2u msix %-3u%s", bdf,
                     d->info.vendor, d->info.device, d->info.class_code, d->info.subclass,
                     d->info.prog_if, class_name(d), d->info.msi_vectors, d->info.msix_vectors,
                     fl);
}

void pci_report(void)
{
    uint32_t per = ndevs <= LIST_LINES ? 1 : ndevs <= 2 * LIST_LINES ? 2 : 3;
    report("pci list: %u functions (msi/msix = vector counts; D boot display, B bridge)", ndevs);
    for (uint32_t i = 0; i < ndevs; i += per) {
        char line[128];
        size_t len = 0;
        line[0] = '\0';
        for (uint32_t k = 0; k < per && i + k < ndevs && len + 1 < sizeof(line); k++) {
            if (k)
                len += ksnprintf(line + len, sizeof(line) - len, " | ");
            size_t col = len + (per == 3 ? 36 : 54);
            if (len + 1 < sizeof(line))
                len += fmt_entry(line + len, sizeof(line) - len, &devs[i + k], per == 3);
            if (len >= sizeof(line))
                len = sizeof(line) - 1;
            /* Pad so the next column lines up. */
            while (k + 1 < per && i + k + 1 < ndevs && len < col && len + 1 < sizeof(line))
                line[len++] = ' ';
            line[len] = '\0';
        }
        while (len && line[len - 1] == ' ')
            line[--len] = '\0';
        report("%s", line);
    }
}

/* ---- MSI / MSI-X ------------------------------------------------------------------- */

static status_t check_irq(struct pci_dev *d, bool msix, uint32_t index)
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

static inline volatile uint32_t *msix_entry(struct pci_dev *d, uint32_t index)
{
    return d->msix_table + 4 * index;
}

/* MSI mask bits register (maskable MSI only). */
static uint32_t msi_mask_off(struct pci_dev *d)
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
    uint16_t cmd = rd(d, CFG_COMMAND, 2);
    if (on) {
        wr(d, CFG_COMMAND, 2, cmd | CMD_INTX_OFF);
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
        /* INTx Disable stays set (M7; review of M6 phase 2): clearing it
         * with the last MSI gone could let an INTx the device has pending
         * fire into a line nobody handles, and an unbound function has no
         * business interrupting. */
        (void)cmd;
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

/* ---- command register ---------------------------------------------------------------- */

static status_t set_cmd_bit(struct pci_dev *d, uint16_t bit, bool on)
{
    if (!d)
        return ERR_INVALID_ARGS;
    if (untouchable(d))
        return ERR_ACCESS_DENIED;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint16_t cmd = rd(d, CFG_COMMAND, 2);
    uint16_t want = on ? cmd | bit : cmd & ~bit;
    if (want != cmd)
        wr(d, CFG_COMMAND, 2, want);
    uint16_t back = rd(d, CFG_COMMAND, 2);   /* flushes the posted write */
    spin_unlock_irqrestore(&pci_lock, f);
    return !!(back & bit) == on ? OK : ERR_BAD_STATE;
}

void pci_save_config(struct pci_dev *d, struct pci_saved_config *out)
{
    uint64_t f = spin_lock_irqsave(&pci_lock);
    out->command = (uint16_t)rd(d, CFG_COMMAND, 2);
    for (uint32_t i = 0; i < 6; i++)
        out->bar[i] = i < bar_count(d) ? rd(d, CFG_BAR0 + 4 * i, 4) : 0;
    spin_unlock_irqrestore(&pci_lock, f);
}

bool pci_restore_config(struct pci_dev *d, const struct pci_saved_config *in)
{
    if (untouchable(d))
        return false;
    bool lost = false;
    uint64_t f = spin_lock_irqsave(&pci_lock);
    uint16_t cmd = (uint16_t)rd(d, CFG_COMMAND, 2);
    for (uint32_t i = 0; i < bar_count(d); i++)
        lost |= rd(d, CFG_BAR0 + 4 * i, 4) != in->bar[i];
    if (lost) {
        /* Decode off while the BARs go back, then the command register as
         * it was (INTx Disable set whatever it was: M7). */
        wr(d, CFG_COMMAND, 2, cmd & ~(CMD_IO | CMD_MEMORY));
        for (uint32_t i = 0; i < bar_count(d); i++)
            wr(d, CFG_BAR0 + 4 * i, 4, in->bar[i]);
    }
    if (lost || cmd != (in->command | CMD_INTX_OFF))
        wr(d, CFG_COMMAND, 2, in->command | CMD_INTX_OFF);
    (void)rd(d, CFG_COMMAND, 2);
    spin_unlock_irqrestore(&pci_lock, f);
    return lost;
}

status_t pci_set_bus_master(struct pci_dev *d, bool on)
{
    return set_cmd_bit(d, CMD_MASTER, on);
}

status_t pci_enable_memory(struct pci_dev *d)
{
    return set_cmd_bit(d, CMD_MEMORY, true);
}

/* For the ktests: the MSI-X table / PBA physical address of a function. */
uint64_t pci_msix_table_phys(struct pci_dev *d) { return d ? msix_table_phys[d->index] : 0; }
uint64_t pci_msix_pba_phys(struct pci_dev *d) { return d ? msix_pba_phys[d->index] : 0; }
