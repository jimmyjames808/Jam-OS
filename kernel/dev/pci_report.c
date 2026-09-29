/* PCI names and reports: a log line per function at boot, the RESULTS
 * lines pci_init writes, and the Devices list (pci_report). */
#include <jam/kprintf.h>
#include <jam/pci.h>
#include <jam/report.h>

#include "pci_internal.h"

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
        return p == 0x30 ? "xHCI" : p == 0x20 ? "EHCI" : p == 0x10 ? "OHCI"
             : p == 0 ? "UHCI" : "USB";
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

int pci_fmt_bdf(char *buf, size_t n, const struct pci_dev *d)
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

void pci_log_function(const struct pci_dev *d)
{
    char bdf[16], irqs[96], bars[400];
    pci_fmt_bdf(bdf, sizeof(bdf), d);
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
    pci_fmt_bdf(bdf, sizeof(bdf), d);
    fmt_irqs(irqs, sizeof(irqs), d);
    fmt_bars(bars, sizeof(bars), d);
    report("pci: %s %s %04x:%04x rev %02x %s", what, bdf, d->info.vendor, d->info.device,
           d->info.revision, irqs);
    report("pci:   %s %s", bdf, bars[0] ? bars : "no BARs");
}

void pci_report_class(const char *what, uint8_t c, uint8_t s, int prog_if)
{
    uint32_t any = 0;
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
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

/* The RESULTS box holds 48 lines; the boot's other lines (topology, timer,
 * fpu, the pci_init lines, "run complete", maybe irq/serial) take about
 * ten to twelve, and the list's own header one. Up to 32 functions get a
 * line each, up to 64 two per line, beyond that three (compact). */
#define LIST_LINES 32

static int fmt_entry(char *buf, size_t n, const struct pci_dev *d, bool compact)
{
    char bdf[16];
    pci_fmt_bdf(bdf, sizeof(bdf), d);
    const char *fl = (d->info.flags & PCI_INFO_DISPLAY)  ? " D"
                   : (d->info.flags & PCI_INFO_BRIDGE) ? " B" : "";
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
    uint32_t ndevs = pci_count();
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
                len += fmt_entry(line + len, sizeof(line) - len, pci_get(i + k), per == 3);
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
