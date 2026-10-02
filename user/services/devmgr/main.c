/* devmgr: binds drivers to PCI functions and keeps them running
 * (supervise.c). A process in bootfs (bin/devmgr) that init
 * starts with a RES_PCI resource (SR_RESOURCE) sliced from the root, and
 * the server ends of its control and query channels (SR_DEVMGR_CTL,
 * SR_DEVMGR; chans.c serves them and the channels made from them; the
 * protocol, and the reconnect rule its clients follow, are in <devmgr.h>).
 *
 * It enumerates every function (pci_enum), matches each against the table
 * below and, for every match whose driver ELF is in bootfs, wakes the
 * function to D0 if it was left in D1-D3, makes the driver's handles and
 * starts drv/<name> in a job of its own (a child of devmgr's job, with
 * limits; bind.c). The policy is all here; the kernel only enforces
 * rights. A driver gets exactly (roles from <jam/driver.h>):
 *
 *   DR_PCIDEV  its function, RIGHT_READ | RIGHT_WRITE (+ wait, inspect):
 *              filtered config reads/writes, nothing else (no
 *              RIGHT_MANAGE: no bus mastering, dma_caps or interrupt
 *              objects; no RIGHT_SLICE: no BAR resources of its own)
 *   DR_BAR(n)  each memory BAR as a RES_MMIO, RIGHT_MAP (+ wait, inspect)
 *              (the kernel still refuses the MSI-X table / PBA pages)
 *   DR_IRQ(0)  an interrupt object: MSI-X entry 0 if the function has
 *              MSI-X, else its MSI
 *   DR_DMA     a dma_cap bound to the function: the function's new
 *              current cap, which turns Bus Master Enable OFF. The driver
 *              turns it on (drv_dma_bus_master; DMA and MSI both need it)
 *              once it has quiesced the device, so nothing a previous
 *              driver left queued reaches memory. Closing the cap --
 *              the driver exiting or being killed -- turns it off again;
 *              pins still held then are quarantined by the kernel
 *   DR_SERVE   a channel whose other end devmgr keeps (GET_SERVICE hands
 *              out duplicates of it)
 * The hardware handles (all but DR_SERVE) come without RIGHT_DUPLICATE and
 * RIGHT_TRANSFER (<devmgr.h> DEVMGR_DRV_*_RIGHTS): the driver can't pass
 * them on, so nothing of the device outlives the driver's job.
 *
 * A driver may also WRITE on DR_SERVE by itself (txid 0): usb-bus
 * sends `usbbus.interface_attached` (abi/idl/usbbus.idl) with the
 * interface's `usb` channel for each interface of a new device. devmgr
 * watches each driver's channel for these and keeps the channels (usb.c).
 *
 * A driver that dies is restarted with backoff, or given up on
 * (supervise.c). devmgr keeps its own handle to each function (with
 * RIGHT_MANAGE, from pci_device_open) and nothing else of the driver's.
 * Each binding is logged, with one RESULTS line per bound driver.
 *
 * The argument "nousb" (init passes it on for the safe mode boot entry)
 * leaves USB host controllers (class 0c03xx) without a driver: no USB at
 * all, the console's input is the serial port alone. The argument
 * "hidboot" (the boot word) is passed on to every drv/hid, which then
 * keeps mice in the boot protocol. A match-table row that names a word
 * binds only when devmgr was given that word, and its driver is started
 * with it: "netprobe" (the boot word) binds drv/rtl8125, the RTL8125's
 * listen-only probe; without it the network chip gets no driver at all.
 * The argument "vlan=<id>" (the boot's VLAN, from the kernel through
 * init) is passed on to the driver of every network card (PCI class 02):
 * the one way a network driver learns the VLAN; without it (or with one
 * that isn't valid) the drivers start without a VLAN and keep the
 * network off.
 *
 * DEVMGR_SHUTDOWN (a kexec reboot) stops everything the way the last
 * control client leaving does, without waiting for the shell's copies.
 *
 * USB interfaces usb-bus reports get class drivers (usb.c: class 3 ->
 * drv/hid, each in a job of its own, supervised the same way), connected
 * to the console when there is one (SR_CONSOLE, then DEVMGR_SET_CONSOLE).
 *
 * A mass-storage interface's driver makes a disk (disk.c): devmgr asks it
 * for its partitions and, if it is the disk Jam OS booted from, starts a
 * filesystem service on its ESP and its data partition, supervised the
 * same way, and hands their channels out through DEVMGR_MOUNTS (mounts.c).
 *
 * It runs until every client end of its control channel is gone (init
 * closes its own at the end of the boot): then it closes each driver's client end,
 * waits for the drivers to return, kills any that don't, and exits 0 if
 * every driver ended cleanly with its job at zero and nothing crashed or
 * was given up on meanwhile. */
#include <jam/netdev.h>
#include "internal.h"

/* ---- the match table ---------------------------------------------------------
 * vendor/device (0xffff = any) and/or class (class << 16 | subclass << 8 |
 * prog_if; ANY_CLASS = any). The first match wins; a driver whose ELF
 * isn't in bootfs is skipped with a log line. Adding a driver is one line
 * here and its directory under drivers/. */
#define ANY_CLASS 0xffffffffu
static const struct {
    uint16_t    vendor, device;   /* PCI ids, 0xffff: any */
    uint32_t    class_code;       /* class << 16 | subclass << 8 | prog_if, or ANY_CLASS */
    const char *path;             /* the driver in bootfs */
    const char *word;             /* bound only with this word, passed on (NULL: always) */
} matches[] = {
    { 0x1234, 0x11e8, ANY_CLASS, "drv/edu", NULL },      /* QEMU's edu test device */
    { 0xffff, 0xffff, 0x0c0330, "drv/usb-bus", NULL },   /* any xHCI controller */
    /* Intel HD Audio in HDA mode (class 04 03 00). Only Intel's: other
     * vendors' (the RTX's HDMI audio) are left without a driver, and 04 03 80
     * (Intel's audio DSP) needs firmware this driver doesn't have. Who may
     * use it is init's to say, not this table's: init asks for its device
     * channel and gives it to the mixer (<devmgr.h> "Trust"). */
    { 0x8086, 0xffff, 0x040300, "drv/hda", NULL },
    /* The board's Realtek RTL8125: only the listen-only probe so far
     * (docs/M9-PLAN.md stage 0), and only on a `netprobe` boot. */
    { 0x10ec, 0x8125, ANY_CLASS, "drv/rtl8125", "netprobe" },
};

struct binding devs[MAX_DEVS];
unsigned ndevs, problems;
handle_t pci_res, port;
unsigned nbound, nfailed, nskipped;
static bool nousb;
bool hidboot;
static bool netprobe;
uint32_t boot_mbr_id;
uint16_t net_vlan;
uint64_t devmgr_started;
bool shutdown_asked;

void say(bool report_it, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    if (report_it)
        jam_debug_report(buf, (uint64_t)n);
    else
        jam_debug_write(buf, (uint64_t)n);
}

const char *bdf(const struct binding *b)
{
    static char s[40];
    if (b->kind == BIND_USB) {
        /* its name without the driver's: "hid-6.1:0" (drv/hid) -> "usb 6.1:0" */
        snprintf(s, sizeof(s), "usb %s", b->name + (b->path ? strlen(b->path + 4) + 1 : 0));
        return s;
    }
    if (b->kind == BIND_FS)
        return b->name;
    if (b->kind != BIND_PCI)
        return "test";
    snprintf(s, sizeof(s), "%02x:%02x.%x", b->info.bus, b->info.dev, b->info.fn);
    return s;
}

/* Was devmgr given the boot word a match-table row asks for? */
static bool pci_word_given(const char *word)
{
    return !strcmp(word, "netprobe") && netprobe;
}

/* The driver for function i (NULL: none). */
static const char *match(const struct pci_dev_info *i)
{
    if (i->flags & (PCI_INFO_BRIDGE | PCI_INFO_DISPLAY))
        return NULL;   /* never a driver's */
    uint32_t cls = (uint32_t)i->class_code << 16 | (uint32_t)i->subclass << 8 | i->prog_if;
    if (nousb && (cls >> 8) == 0x0c03) {
        say(false, "devmgr: %02x:%02x.%x %04x:%04x: a USB controller, left alone (nousb)",
            i->bus, i->dev, i->fn, i->vendor, i->device);
        return NULL;
    }
    for (unsigned k = 0; k < sizeof(matches) / sizeof(matches[0]); k++) {
        if ((matches[k].vendor != 0xffff && matches[k].vendor != i->vendor) ||
            (matches[k].device != 0xffff && matches[k].device != i->device) ||
            (matches[k].class_code != ANY_CLASS && matches[k].class_code != cls))
            continue;
        if (matches[k].word && !pci_word_given(matches[k].word)) {
            say(false, "devmgr: %02x:%02x.%x %04x:%04x: left alone (no %s)", i->bus, i->dev,
                i->fn, i->vendor, i->device, matches[k].word);
            return NULL;
        }
        return matches[k].path;
    }
    return NULL;
}

const char *pci_driver_arg(const char *path)
{
    for (unsigned k = 0; path && k < sizeof(matches) / sizeof(matches[0]); k++)
        if (matches[k].word && !strcmp(matches[k].path, path) && pci_word_given(matches[k].word))
            return matches[k].word;
    return NULL;
}

bool in_bootfs(const char *path)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    return bootfs_default(&fs) == OK && bootfs_lookup(fs, path, &data, &size) == OK;
}

static void bind_all(void)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (!b->path)
            continue;
        if (!in_bootfs(b->path)) {
            say(false, "devmgr: %s %04x:%04x: %s is not in bootfs, skipped", bdf(b),
                b->info.vendor, b->info.device, b->path);
            nskipped++;
            b->path = NULL;
            continue;
        }
        b->last = start_driver(b);
        if (b->last != OK) {
            say(true, "devmgr: %s %04x:%04x -> %s: bind FAILED (%s)", bdf(b), b->info.vendor,
                b->info.device, b->path, status_str(b->last));
            nfailed++;
            continue;
        }
        nbound++;
        unsigned bars = mem_bars(b);
        say(true, "devmgr: %s %04x:%04x -> %s (%s, %u BAR%s, dma) bound", bdf(b), b->info.vendor,
            b->info.device, b->path,
            b->info.msix_vectors ? "MSI-X" : b->info.msi_vectors ? "MSI" : "no irq", bars,
            bars == 1 ? "" : "s");
    }
}

/* ---- main ------------------------------------------------------------------------ */

/* Every PCI function into devs[], each with the driver it matches. */
static status_t enumerate(void)
{
    for (uint32_t i = 0; ndevs < MAX_DEVS; i++) {
        struct binding *b = &devs[ndevs];
        status_t st = jam_pci_enum(pci_res, i, &b->info);
        if (st == ERR_OUT_OF_RANGE)
            break;
        if (st != OK) {
            say(true, "devmgr: pci_enum(%u): %s", i, status_str(st));
            return st;
        }
        b->kind = BIND_PCI;
        b->index = i;
        b->path = match(&b->info);
        ndevs++;
    }
    return OK;
}

/* Wait for one packet (or the next due restart) and act on it. */
static status_t wait_event(void)
{
    struct port_packet pkt;
    uint64_t deadline = sup_next_deadline();
    if (disk_next_deadline() < deadline)
        deadline = disk_next_deadline();
    if (mounts_next_deadline() < deadline)
        deadline = mounts_next_deadline();
    if (job_next_deadline() < deadline)
        deadline = job_next_deadline();
    status_t st = jam_port_wait(port, deadline, &pkt);
    if (st != OK)
        return st;
    if (chans_packet(pkt.key))
        return OK;   /* chans_serve reads what came */
    if ((pkt.key & KEY_DRIVER) && KEY_INDEX(pkt.key) < ndevs) {
        sup_died(&devs[KEY_INDEX(pkt.key)], KEY_GEN(pkt.key));
    } else if ((pkt.key & KEY_EVENTS) && KEY_INDEX(pkt.key) < ndevs) {
        struct binding *b = &devs[KEY_INDEX(pkt.key)];
        if (b->client_key != pkt.key)
            return OK;   /* stale */
        if (b->disk)
            disk_events(b);   /* a disk's driver, a filesystem service: answers */
        else
            usb_driver_events(b);
    } else if (pkt.key & KEY_USBIF) {
        usb_if_closed(pkt.key);
    } else if (pkt.key & KEY_DISK) {
        disk_key(pkt.key);
    }
    return OK;
}

/* Serve until the life channel's clients are all gone (ERR_PEER_CLOSED)
 * or something fails (its status). */
static status_t run(void)
{
    for (;;) {
        status_t st = chans_serve();
        if (shutdown_asked)
            return ERR_PEER_CLOSED;   /* as if every client had gone */
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = chans_arm();
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = wait_event();
        if (st != OK && st != ERR_TIMED_OUT)
            return st;
        sup_run_due();
        disk_run_due();
        mounts_run_due();
        job_run_due();
    }
}

/* Every client is gone: no more restarts; stop the drivers. True if each
 * ended cleanly and nothing went wrong while running.
 *
 * The drivers at the bottom (usb-bus) are told first and waited for: what
 * runs on them ends because its device is gone, as when a stick is pulled.
 * Then each is stopped and its job checked for leftovers, from the top
 * down: the filesystem services, the USB class drivers, and last the
 * bottom ones. A job is charged for what its driver shared with the one
 * above until that one has ended too (usb-bus makes the bulk buffer that
 * usb-storage maps), so checking usb-bus's job first was a race. */
static bool stop_all(void)
{
    static const int order[] = { BIND_FS, BIND_USB, -1 /* every other kind */ };
    bool ok = true;
    unsigned stopped = 0;
    disk_sync_all();   /* before the drivers under the filesystems go */
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        sup_reset(b);
        if (b->kind == BIND_FS || b->kind == BIND_USB || !b->proc)
            continue;
        signals_t seen;
        close_client(b);
        /* One that doesn't end is reported, and killed, by stop_driver below. */
        (void)jam_object_wait_one(b->proc, SIG_TERMINATED, now() + STOP_WAIT, &seen);
    }
    for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++)
        for (unsigned i = 0; i < ndevs; i++) {
            struct binding *b = &devs[i];
            bool bottom = b->kind != BIND_FS && b->kind != BIND_USB;
            if (order[k] < 0 ? !bottom : (int)b->kind != order[k])
                continue;
            if (b->proc) {
                if (!stop_driver(b, false, false)) {
                    say(true, "devmgr: %s %s did not end cleanly", bdf(b), b->path);
                    ok = false;
                }
                stopped++;
            }
            close_client(b);
        }
    for (unsigned i = 0; i < ndevs; i++) {
        if (devs[i].dev)
            jam_handle_close(devs[i].dev);
        usb_bus_gone(&devs[i]);   /* the interface channels we kept */
    }
    if (!job_settle())
        ok = false;   /* said which, and counted in problems */
    if (problems) {
        say(true, "devmgr: %u driver problem(s) while running (crashes, give-ups: see above)",
            problems);
        ok = false;
    }
    say(false, "devmgr: %u driver(s) stopped; exiting", stopped);
    return ok;
}

/* "0x1234abcd" (or without 0x) as a number; it stops at the first
 * character that isn't a hex digit. */
static uint32_t hex32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    uint32_t v = 0;
    for (int n = 0; n < 8; n++, s++) {
        char c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            break;
        v = v << 4 | (uint32_t)d;
    }
    return v;
}

int main(int argc, char **argv)
{
    devmgr_started = now();
    for (int i = 1; i < argc; i++) {
        nousb |= !strcmp(argv[i], "nousb");
        hidboot |= !strcmp(argv[i], "hidboot");
        netprobe |= !strcmp(argv[i], "netprobe");
        if (!strncmp(argv[i], "bootdisk=", 9))
            boot_mbr_id = hex32(argv[i] + 9);
    }
    net_vlan = netdev_vlan_args((const char *const *)argv + 1, (uint32_t)argc - 1);
    if (net_vlan)
        say(false, "devmgr: network drivers get vlan=%u", net_vlan);
    else
        say(false, "devmgr: no VLAN: network drivers keep the network off");
    /* devmgr runs until the control channel's clients are all gone (with
     * no control channel: the query channel's); a query or device channel
     * whose clients are gone is just dropped. */
    bool have_chans = chans_init();
    pci_res = startup_handle(SR_RESOURCE);
    if (!have_chans || !pci_res) {
        say(true, "devmgr: no %s in the startup message", have_chans ? "PCI resource" : "channel");
        return 1;
    }
    status_t st = jam_port_create(&port);
    if (st != OK) {
        say(true, "devmgr: no port (%s)", status_str(st));
        return 1;
    }
    /* With a console, class drivers send their input to it. */
    if (startup_handle(SR_CONSOLE))
        usb_new_console(startup_handle(SR_CONSOLE));
    if (netprobe)
        say(false, "devmgr: netprobe: an RTL8125 (10ec:8125) gets drv/rtl8125, which only "
            "listens");
    if (enumerate() != OK)
        return 1;
    bind_all();
    say(false, "devmgr: %u function(s), %u driver(s) bound, %u failed, %u skipped; serving",
        ndevs, nbound, nfailed, nskipped);

    st = run();
    bool ok = nfailed == 0;
    if (st != ERR_PEER_CLOSED) {
        say(true, "devmgr: serving failed (%s)", status_str(st));
        ok = false;
    }
    if (!stop_all())
        ok = false;
    return ok ? 0 : 1;
}
