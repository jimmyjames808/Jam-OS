/* devices (lspci): every PCI function, and whether devmgr runs a driver
 * for it; then devmgr's counts. */
#include <idl/devmgr.h>
#include "sh.h"

/* "  driver running", "  driver gone" or "". */
static const char *driver_of(const struct pci_dev_info *info)
{
    handle_t dm = sh_devmgr();
    if (!dm)
        return "";
    /* devmgr names a function by its address on the bus */
    uint32_t bdf = (uint32_t)info->bus << 8 | (uint32_t)(info->dev & 31u) << 3 | (info->fn & 7u);
    handle_t process = HANDLE_INVALID, job = HANDLE_INVALID;
    uint32_t pci_index;
    status_t st = devmgr_get_driver_until(dm, now() + 5 * NS_PER_S, DEVMGR_KIND_PCI_BDF, bdf, 0, 0,
                                          &process, &job, &pci_index);
    if (st == OK) {
        jam_handle_close(process);
        jam_handle_close(job);
    }
    return st == OK ? "  driver running" : st == ERR_BAD_STATE ? "  driver gone" : "";
}

static void devmgr_counts(void)
{
    handle_t dm = sh_devmgr();
    if (!dm) {
        sh_say("devmgr: not running (restarting?)\n");
        return;
    }
    uint32_t bound, failed, skipped;
    status_t st = devmgr_status_until(dm, now() + 5 * NS_PER_S, &bound, &failed, &skipped);
    if (st == OK)
        sh_say("devmgr: %u bound, %u failed, %u skipped\n", bound, failed, skipped);
    else
        sh_say("devmgr: %s\n", status_str(st));
}

SH_CMD(devices)
{
    (void)argc;
    (void)argv;
    handle_t pci = sh_pci();
    if (!pci) {
        sh_say("devices: no PCI resource\n");
        return 0;
    }
    struct pci_dev_info info;
    uint32_t i;
    for (i = 0; jam_pci_enum(pci, i, &info) == OK; i++) {
        const char *drv = driver_of(&info);
        sh_say("  %02x:%02x.%x %04x:%04x class %02x.%02x.%02x%s%s%s\n", info.bus, info.dev, info.fn,
               info.vendor, info.device, info.class_code, info.subclass, info.prog_if,
               info.flags & PCI_INFO_BRIDGE ? " bridge" : "",
               info.flags & PCI_INFO_DISPLAY ? " display" : "", drv);
    }
    sh_say("%u PCI function%s\n", i, i == 1 ? "" : "s");
    devmgr_counts();
    return 0;
}
