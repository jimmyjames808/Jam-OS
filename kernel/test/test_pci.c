/* Tests for the PCI core (kernel/dev/pci*.c) on QEMU q35 as tools/qemu-test.sh
 * starts it: host bridge 8086:29c0, std VGA 1234:1111 (the boot display),
 * qemu-xhci 1b36:000d (MSI-X, the boot stick hangs off it), edu 1234:11e8
 * (MSI). Everything a test changes on a device it puts back. xHCI is never
 * reset or started: only its MSI-X table and enable bits are touched, with
 * every entry masked. */
#include <jam/fbcon.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/string.h>

/* pci.c internals for tests. */
status_t pci_size_bars(struct pci_dev *d, uint64_t sizes[6]);
uint32_t pci_cfg_read_bdf(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t fn,
                          uint32_t off, uint32_t width);
uint64_t pci_msix_table_phys(struct pci_dev *d);
uint64_t pci_msix_pba_phys(struct pci_dev *d);

#define CMD          0x04
#define CMD_MEMORY   (1u << 1)
#define CMD_MASTER   (1u << 2)
#define CMD_INTX_OFF (1u << 10)

/* The QEMU q35 device this test checks, or skip the test (return from it)
 * on hardware that doesn't have it, such as the real PC. A GNU statement
 * expression, so `return` leaves the KTEST function. */
#define need(vendor, device) ({                                                   \
    struct pci_dev *need_d_ = pci_find((vendor), (device), 0);                   \
    if (!need_d_) {                                                              \
        kprintf("ktest %s: no PCI %04x:%04x (a QEMU device), skipped\n",         \
                ktest_current, (unsigned)(vendor), (unsigned)(device));           \
        return;                                                                  \
    }                                                                            \
    need_d_;                                                                     \
})

static uint64_t bdf_key(const struct pci_dev *d)
{
    return ((uint64_t)d->info.segment << 16) | ((uint32_t)d->info.bus << 8) |
           ((uint32_t)d->info.dev << 3) | d->info.fn;
}

KTEST(pci_table)
{
    uint32_t n = pci_count();
    KT_ASSERT(n >= 5 && n <= PCI_MAX_DEVS);
    for (uint32_t i = 0; i < n; i++) {
        struct pci_dev *d = pci_get(i);
        KT_ASSERT(d && d->index == i && d->cfg);
        KT_ASSERT(d->info.vendor != 0xffff);
        KT_EQ(pci_cfg_read(d, 0, 2), d->info.vendor);
        KT_EQ(pci_cfg_read(d, 2, 2), d->info.device);
        if (i)
            KT_ASSERT(bdf_key(pci_get(i - 1)) < bdf_key(d));   /* sorted, no duplicates */
    }
    KT_ASSERT(pci_get(n) == NULL);
    KT_ASSERT(pci_find(0xffff, 0xffff, n) == NULL);
    KT_ASSERT(pci_find(0xffff, 0xffff, 0) == pci_get(0));

    struct pci_dev *h = need(0x8086, 0x29c0);
    KT_EQ(h->info.bus, 0);
    KT_EQ(h->info.dev, 0);
    KT_EQ(h->info.fn, 0);
    KT_EQ(h->info.class_code, 0x06);
    KT_EQ(h->info.subclass, 0x00);
    KT_ASSERT(h->info.flags & PCI_INFO_BRIDGE);
    KT_ASSERT(!(h->info.flags & PCI_INFO_DISPLAY));
    /* The host bridge's command register is never ours to write. */
    KT_EQ(pci_set_bus_master(h, true), ERR_ACCESS_DENIED);
    KT_EQ(pci_enable_memory(h), ERR_ACCESS_DENIED);
    uint64_t sizes[6];
    KT_EQ(pci_size_bars(h, sizes), ERR_ACCESS_DENIED);
    struct pci_dev *lpc = pci_find(0x8086, 0x2918, 0);
    if (lpc)
        KT_ASSERT(lpc->info.flags & PCI_INFO_BRIDGE);
}

KTEST(pci_edu)
{
    struct pci_dev *d = need(0x1234, 0x11e8);
    KT_ASSERT(d->cap_msi && !d->cap_msix);
    KT_EQ(d->info.msi_vectors, 1);
    KT_EQ(d->info.msix_vectors, 0);
    KT_ASSERT(d->msi_64);
    KT_ASSERT(!d->msi_maskable);
    KT_EQ(pci_find_cap(d, 0x05), d->cap_msi);
    KT_EQ(pci_find_cap(d, 0x11), 0);
    KT_ASSERT(!(d->info.flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY)));
    /* BAR0: 1 MiB of 32-bit, non-prefetchable MMIO; nothing else. */
    KT_EQ(d->info.bar[0].flags, PCI_BAR_MMIO);
    KT_EQ(d->info.bar[0].size, 1u << 20);
    KT_ASSERT(d->info.bar[0].phys && !(d->info.bar[0].phys & ((1u << 20) - 1)));
    KT_ASSERT(d->info.bar[0].phys < (1ull << 32));
    for (int b = 1; b < 6; b++) {
        KT_EQ(d->info.bar[b].flags, 0);
        KT_EQ(d->info.bar[b].size, 0);
    }
}

KTEST(pci_xhci)
{
    struct pci_dev *d = need(0x1b36, 0x000d);
    KT_EQ(d->info.class_code, 0x0c);
    KT_EQ(d->info.subclass, 0x03);
    KT_EQ(d->info.prog_if, 0x30);
    KT_ASSERT(d->cap_msix);
    KT_EQ(d->info.msix_vectors, 16);
    KT_EQ(pci_find_cap(d, 0x11), d->cap_msix);
    KT_EQ(pci_find_cap(d, 0x10), d->cap_pcie);
    KT_EQ(d->msix_table_bar, 0);
    KT_EQ(d->msix_table_off, 0x3000);
    KT_EQ(d->msix_pba_bar, 0);
    KT_EQ(d->msix_pba_off, 0x3800);
    KT_ASSERT(d->msix_table != NULL);
    /* BAR0: 16 KiB of 64-bit MMIO; bar[1] is its upper half, empty. */
    KT_EQ(d->info.bar[0].flags, PCI_BAR_MMIO | PCI_BAR_64);
    KT_EQ(d->info.bar[0].size, 0x4000);
    KT_ASSERT(d->info.bar[0].phys && !(d->info.bar[0].phys & 0x3fff));
    KT_EQ(d->info.bar[1].flags, 0);
    KT_EQ(pci_msix_table_phys(d), d->info.bar[0].phys + 0x3000);
    KT_EQ(pci_msix_pba_phys(d), d->info.bar[0].phys + 0x3800);
}

KTEST(pci_display_untouched)
{
    struct pci_dev *d = need(0x1234, 0x1111);
    KT_EQ(d->info.class_code, 0x03);
    KT_ASSERT(d->info.flags & PCI_INFO_DISPLAY);
    uint32_t displays = 0;
    for (uint32_t i = 0; i < pci_count(); i++)
        displays += !!(pci_get(i)->info.flags & PCI_INFO_DISPLAY);
    KT_EQ(displays, 1);
    /* The framebuffer is in one of its BARs; none of them was sized. */
    uint64_t fb = fbcon_phys(NULL);
    bool holds = false;
    for (int b = 0; b < 6; b++) {
        if (!d->info.bar[b].flags)
            continue;
        KT_ASSERT(d->info.bar[b].flags & PCI_BAR_UNSIZED);
        KT_EQ(d->info.bar[b].size, 0);
        holds |= (d->info.bar[b].flags & PCI_BAR_MMIO) && d->info.bar[b].phys <= fb &&
                 fb - d->info.bar[b].phys < (256ull << 20);
    }
    KT_ASSERT(holds);
    uint16_t cmd = pci_cfg_read(d, CMD, 2);
    KT_ASSERT(cmd & CMD_MEMORY);
    uint64_t sizes[6];
    KT_EQ(pci_size_bars(d, sizes), ERR_ACCESS_DENIED);
    KT_EQ(pci_set_bus_master(d, true), ERR_ACCESS_DENIED);
    KT_EQ(pci_set_bus_master(d, false), ERR_ACCESS_DENIED);
    KT_EQ(pci_enable_memory(d), ERR_ACCESS_DENIED);
    KT_EQ(pci_msi_enable(d, false, true), ERR_ACCESS_DENIED);
    KT_EQ(pci_msi_set(d, true, 0, 0xfee00000, 0x40), ERR_ACCESS_DENIED);
    KT_EQ(pci_msi_mask(d, false, 0, true), ERR_ACCESS_DENIED);
    KT_EQ(pci_cfg_read(d, CMD, 2), cmd);
}

static void check_sizing(struct pci_dev *d)
{
    uint32_t bars[6];
    uint16_t cmd = pci_cfg_read(d, CMD, 2);
    for (int b = 0; b < 6; b++)
        bars[b] = pci_cfg_read(d, 0x10 + 4 * b, 4);
    uint64_t sizes[6];
    KT_EQ(pci_size_bars(d, sizes), OK);
    for (int b = 0; b < 6; b++) {
        KT_EQ(sizes[b], d->info.bar[b].size);
        KT_EQ(pci_cfg_read(d, 0x10 + 4 * b, 4), bars[b]);
    }
    KT_EQ(pci_cfg_read(d, CMD, 2), cmd);
}

KTEST(pci_sizing_restores)
{
    struct pci_dev *edu = need(0x1234, 0x11e8), *x = need(0x1b36, 0x000d);
    check_sizing(edu);
    check_sizing(x);
    /* What init recorded is what the BAR registers hold. */
    KT_EQ(pci_cfg_read(edu, 0x10, 4) & ~0xfu, edu->info.bar[0].phys);
    uint64_t xbar = (pci_cfg_read(x, 0x10, 4) & ~0xfull) | (uint64_t)pci_cfg_read(x, 0x14, 4) << 32;
    KT_EQ(xbar, x->info.bar[0].phys);

    /* edu still answers at its BAR: identification register 0x010000ed. */
    uint16_t cmd = pci_cfg_read(edu, CMD, 2);
    KT_EQ(pci_enable_memory(edu), OK);
    KT_ASSERT(pci_cfg_read(edu, CMD, 2) & CMD_MEMORY);
    volatile uint32_t *mmio = vmm_map_mmio(edu->info.bar[0].phys, PAGE_SIZE);
    KT_EQ(mmio[0], 0x010000ed);
    check_sizing(edu);
    KT_EQ(mmio[0], 0x010000ed);
    pci_cfg_write(edu, CMD, 2, cmd);
}

KTEST(pci_missing_function)
{
    /* A device slot on bus 0 with nothing in it reads all-ones. */
    int empty = -1;
    for (int dev = 31; dev > 0 && empty < 0; dev--) {
        bool used = false;
        for (uint32_t i = 0; i < pci_count(); i++)
            used |= pci_get(i)->info.bus == 0 && pci_get(i)->info.dev == dev;
        if (!used)
            empty = dev;
    }
    KT_ASSERT(empty > 0);
    KT_EQ(pci_cfg_read_bdf(0, 0, empty, 0, 0, 4), 0xffffffffu);
    KT_EQ(pci_cfg_read_bdf(0, 0, empty, 0, 2, 2), 0xffff);
    KT_EQ(pci_cfg_read_bdf(0, 0, empty, 3, 0x40, 1), 0xff);
    /* A single-function device's function 1 is absent too. */
    struct pci_dev *edu = need(0x1234, 0x11e8);
    KT_EQ(pci_cfg_read_bdf(0, 0, edu->info.dev, 1, 0, 4), 0xffffffffu);
    KT_EQ(pci_cfg_read_bdf(0, 0, edu->info.dev, 0, 0, 4), 0x11e81234u);
    /* Buses the walk never reached, segments that don't exist, bad args. */
    KT_EQ(pci_cfg_read_bdf(0, 200, 0, 0, 0, 4), 0xffffffffu);
    KT_EQ(pci_cfg_read_bdf(7, 0, 0, 0, 0, 4), 0xffffffffu);
    KT_EQ(pci_cfg_read(edu, 2, 4), 0xffffffffu);       /* misaligned */
    KT_EQ(pci_cfg_read(edu, 4096, 1), 0xff);           /* past the end */
    KT_EQ(pci_cfg_read(edu, 0, 3), 0xffffffffu);       /* bad width */
    KT_EQ(pci_cfg_read(NULL, 0, 2), 0xffff);
    KT_EQ(pci_find_cap(NULL, 5), 0);
}

KTEST(pci_msix_pages_protected)
{
    struct pci_dev *x = need(0x1b36, 0x000d);
    uint64_t bar = x->info.bar[0].phys, t = bar + 0x3000;
    KT_ASSERT(pci_phys_protected(t, 1));
    KT_ASSERT(pci_phys_protected(t + 0x800, 8));       /* the PBA, same page */
    KT_ASSERT(pci_phys_protected(t + 0xfff, 1));
    KT_ASSERT(!pci_phys_protected(t - 1, 1));
    KT_ASSERT(pci_phys_protected(t - 1, 2));
    KT_ASSERT(!pci_phys_protected(bar, 0x3000));
    KT_ASSERT(pci_phys_protected(bar, 0x3001));
    KT_ASSERT(!pci_phys_protected(t + 0x1000, PAGE_SIZE));
    KT_ASSERT(pci_phys_protected(0, UINT64_MAX));
    KT_ASSERT(!pci_phys_protected(need(0x1234, 0x11e8)->info.bar[0].phys, 1u << 20));
    /* Any other function with MSI-X (QEMU's default NIC, e1000e) too. */
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (!d->cap_msix || !pci_msix_table_phys(d))
            continue;
        KT_ASSERT(pci_phys_protected(pci_msix_table_phys(d), 16));
        KT_ASSERT(pci_phys_protected(pci_msix_pba_phys(d), 8));
    }
}

KTEST(pci_config_write)
{
    /* edu's MSI address register is plain read/write storage (low 2 bits
     * reserved). */
    struct pci_dev *d = need(0x1234, 0x11e8);
    uint32_t off = d->cap_msi + 4, save = pci_cfg_read(d, off, 4);
    pci_cfg_write(d, off, 4, 0xfee12344);
    KT_EQ(pci_cfg_read(d, off, 4), 0xfee12344u);
    pci_cfg_write(d, off + 2, 2, 0xfeef);
    KT_EQ(pci_cfg_read(d, off, 4), 0xfeef2344u);
    pci_cfg_write(d, off + 1, 1, 0x56);
    KT_EQ(pci_cfg_read(d, off, 4), 0xfeef5644u);
    KT_EQ(pci_cfg_read(d, off + 1, 1), 0x56);
    KT_EQ(pci_cfg_read(d, off + 2, 2), 0xfeef);
    pci_cfg_write(d, off + 1, 4, 0);   /* misaligned: ignored */
    KT_EQ(pci_cfg_read(d, off, 4), 0xfeef5644u);
    pci_cfg_write(d, off, 4, save);
}

KTEST(pci_msi_roundtrip)
{
    struct pci_dev *d = need(0x1234, 0x11e8);
    uint32_t c = d->cap_msi;
    uint16_t cmd0 = pci_cfg_read(d, CMD, 2), ctl0 = pci_cfg_read(d, c + 2, 2);
    uint32_t alo = pci_cfg_read(d, c + 4, 4), ahi = pci_cfg_read(d, c + 8, 4);
    uint16_t data0 = pci_cfg_read(d, c + 12, 2);
    KT_ASSERT(!(ctl0 & 1));   /* nobody has turned edu's MSI on */

    KT_EQ(pci_msi_set(d, false, 0, 0xfee01000, 0x4031), OK);
    KT_EQ(pci_cfg_read(d, c + 4, 4), 0xfee01000u);
    KT_EQ(pci_cfg_read(d, c + 8, 4), 0);
    KT_EQ(pci_cfg_read(d, c + 12, 2), 0x4031);
    KT_EQ(pci_msi_set(d, false, 1, 0xfee01000, 0x4031), ERR_OUT_OF_RANGE);
    KT_EQ(pci_msi_set(d, true, 0, 0xfee01000, 0x4031), ERR_NOT_SUPPORTED);
    KT_EQ(pci_msi_mask(d, false, 0, true), ERR_NOT_SUPPORTED);   /* not maskable */
    KT_EQ(pci_msi_mask(d, true, 0, true), ERR_NOT_SUPPORTED);
    KT_EQ(pci_msi_enable(d, true, true), ERR_NOT_SUPPORTED);

    KT_EQ(pci_msi_enable(d, false, true), OK);
    uint16_t ctl = pci_cfg_read(d, c + 2, 2);
    KT_ASSERT(ctl & 1);
    KT_EQ((ctl >> 4) & 7, 0);   /* one vector */
    KT_ASSERT(pci_cfg_read(d, CMD, 2) & CMD_INTX_OFF);
    /* Changing the message while enabled (not maskable: MSI goes off for
     * the change and comes back on). */
    KT_EQ(pci_msi_set(d, false, 0, 0xfee02000, 0x4032), OK);
    KT_ASSERT(pci_cfg_read(d, c + 2, 2) & 1);
    KT_EQ(pci_cfg_read(d, c + 4, 4), 0xfee02000u);
    KT_EQ(pci_cfg_read(d, c + 12, 2), 0x4032);

    KT_EQ(pci_msi_enable(d, false, false), OK);
    KT_ASSERT(!(pci_cfg_read(d, c + 2, 2) & 1));
    KT_ASSERT(pci_cfg_read(d, CMD, 2) & CMD_INTX_OFF);   /* INTx stays disabled */
    /* Put it all back. */
    pci_cfg_write(d, c + 4, 4, alo);
    pci_cfg_write(d, c + 8, 4, ahi);
    pci_cfg_write(d, c + 12, 2, data0);
    pci_cfg_write(d, c + 2, 2, ctl0);
    pci_cfg_write(d, CMD, 2, cmd0);
}

KTEST(pci_msix_roundtrip)
{
    struct pci_dev *d = need(0x1b36, 0x000d);
    uint32_t n = d->info.msix_vectors, c = d->cap_msix;
    uint16_t cmd0 = pci_cfg_read(d, CMD, 2), ctl0 = pci_cfg_read(d, c + 2, 2);
    KT_ASSERT(!(ctl0 & 0x8000));   /* MSI-X off: nobody drives this xHCI */
    KT_ASSERT(cmd0 & CMD_MEMORY);
    uint32_t saved[16][4];
    for (uint32_t i = 0; i < n; i++)
        for (int w = 0; w < 4; w++)
            saved[i][w] = d->msix_table[4 * i + w];

    /* Every entry masked for the whole test. */
    for (uint32_t i = 0; i < n; i++)
        KT_EQ(pci_msi_mask(d, true, i, true), OK);
    for (uint32_t i = 0; i < n; i++)
        KT_ASSERT(d->msix_table[4 * i + 3] & 1);

    KT_EQ(pci_msi_set(d, true, 3, 0xfee03000, 0x4033), OK);
    KT_EQ(d->msix_table[12], 0xfee03000u);
    KT_EQ(d->msix_table[13], 0);
    KT_EQ(d->msix_table[14], 0x4033);
    KT_ASSERT(d->msix_table[15] & 1);   /* still masked afterwards */
    KT_EQ(pci_msi_set(d, true, n, 0xfee03000, 0x4033), ERR_OUT_OF_RANGE);
    KT_EQ(pci_msi_mask(d, true, n, false), ERR_OUT_OF_RANGE);
    KT_EQ(pci_msi_set(d, false, 0, 0xfee03000, 0x4033), d->cap_msi ? OK : ERR_NOT_SUPPORTED);

    /* Unmask/mask round trip on entry 3 (MSI-X is off: nothing can fire). */
    KT_EQ(pci_msi_mask(d, true, 3, false), OK);
    KT_ASSERT(!(d->msix_table[15] & 1));
    KT_EQ(pci_msi_set(d, true, 3, 0xfee04000, 0x4034), OK);   /* masked while it changes */
    KT_ASSERT(!(d->msix_table[15] & 1));                      /* then unmasked again */
    KT_EQ(d->msix_table[14], 0x4034);
    KT_EQ(pci_msi_mask(d, true, 3, true), OK);
    KT_ASSERT(d->msix_table[15] & 1);

    /* Enable with every entry masked, then off again. */
    KT_EQ(pci_msi_enable(d, true, true), OK);
    uint16_t ctl = pci_cfg_read(d, c + 2, 2);
    KT_ASSERT(ctl & 0x8000);
    KT_ASSERT(!(ctl & 0x4000));   /* function mask lifted */
    KT_ASSERT(pci_cfg_read(d, CMD, 2) & CMD_INTX_OFF);
    KT_EQ(pci_msi_enable(d, true, false), OK);
    ctl = pci_cfg_read(d, c + 2, 2);
    KT_ASSERT(!(ctl & 0xc000));

    for (uint32_t i = 0; i < n; i++)
        for (int w = 0; w < 4; w++)
            d->msix_table[4 * i + w] = saved[i][w];
    pci_cfg_write(d, c + 2, 2, ctl0);
    pci_cfg_write(d, CMD, 2, cmd0);
    KT_EQ(pci_cfg_read(d, CMD, 2), cmd0);
}

KTEST(pci_bus_master)
{
    struct pci_dev *d = need(0x1234, 0x11e8);
    uint16_t cmd0 = pci_cfg_read(d, CMD, 2);
    KT_EQ(pci_set_bus_master(d, true), OK);
    KT_ASSERT(pci_cfg_read(d, CMD, 2) & CMD_MASTER);
    KT_EQ(pci_set_bus_master(d, true), OK);   /* idempotent */
    KT_EQ(pci_set_bus_master(d, false), OK);
    KT_ASSERT(!(pci_cfg_read(d, CMD, 2) & CMD_MASTER));
    KT_EQ(pci_cfg_read(d, CMD, 2), cmd0 & ~CMD_MASTER);
    KT_EQ(pci_set_bus_master(NULL, true), ERR_INVALID_ARGS);
    pci_cfg_write(d, CMD, 2, cmd0);
}

/* Capability walks over a fake config space in RAM: loops and garbage end. */
KTEST(pci_cap_walk_bounded)
{
    uint8_t *cfg = kzalloc(4096);
    KT_ASSERT(cfg);
    struct pci_dev fake;
    memset(&fake, 0, sizeof(fake));
    fake.cfg = cfg;
    cfg[0x06] = 0x10;            /* status: capability list */
    cfg[0x34] = 0x40;
    cfg[0x40] = 0x09; cfg[0x41] = 0x50;
    cfg[0x50] = 0x01; cfg[0x51] = 0x60;
    cfg[0x60] = 0x10; cfg[0x61] = 0x40;   /* back to 0x40: a cycle */
    KT_EQ(pci_find_cap(&fake, 0x09), 0x40);
    KT_EQ(pci_find_cap(&fake, 0x01), 0x50);
    KT_EQ(pci_find_cap(&fake, 0x10), 0x60);
    KT_EQ(pci_find_cap(&fake, 0x05), 0);   /* ends despite the cycle */
    cfg[0x61] = 0x60;                       /* self-loop */
    KT_EQ(pci_find_cap(&fake, 0x05), 0);
    cfg[0x61] = 0x20;                       /* points into the header: stop */
    KT_EQ(pci_find_cap(&fake, 0x05), 0);
    cfg[0x61] = 0x70;
    cfg[0x70] = 0xff; cfg[0x71] = 0xff;     /* all-ones: stop */
    KT_EQ(pci_find_cap(&fake, 0x05), 0);
    cfg[0x06] = 0;                          /* no list at all */
    KT_EQ(pci_find_cap(&fake, 0x09), 0);

    /* Extended list: only walked for PCIe functions. */
    uint32_t *ext = (uint32_t *)cfg;
    ext[0x100 / 4] = 0x0001 | (1u << 16) | (0x140u << 20);
    ext[0x140 / 4] = 0x000b | (1u << 16) | (0x180u << 20);
    ext[0x180 / 4] = 0x0018 | (1u << 16) | (0x100u << 20);   /* cycle to 0x100 */
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x0b), 0);             /* not PCIe */
    fake.cap_pcie = 0x60;
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x01), 0x100);
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x0b), 0x140);
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x18), 0x180);
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x10), 0);             /* ends despite the cycle */
    ext[0x180 / 4] = 0x0018 | (1u << 16) | (0x080u << 20);   /* next below 0x100 */
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x10), 0);
    ext[0x180 / 4] = 0x0018 | (1u << 16) | (0x180u << 20);   /* self-loop */
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x10), 0);
    ext[0x140 / 4] = 0xffffffffu;                            /* garbage */
    KT_EQ(pci_find_cap(&fake, 0x100 + 0x18), 0);
    kfree(cfg);
}
