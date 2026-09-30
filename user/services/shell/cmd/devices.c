/* devices (lspci): every PCI function, and whether devmgr runs a driver
 * for it; then devmgr's counts. */
#include <devmgr.h>
#include "sh.h"

/* devmgr names a device by ids and instance: the n-th with these ids. */
static uint32_t instance(handle_t pci, uint32_t i, const struct pci_dev_info *info)
{
    uint32_t inst = 0;
    struct pci_dev_info o;
    for (uint32_t j = 0; j < i; j++)
        if (jam_pci_enum(pci, j, &o) == OK && o.vendor == info->vendor && o.device == info->device)
            inst++;
    return inst;
}

/* "  driver running", "  driver gone" or "". */
static const char *driver_of(const struct pci_dev_info *info, uint32_t inst)
{
    handle_t dm = sh_devmgr();
    if (!dm)
        return "";
    struct devmgr_rep rep;
    handle_t hs[DEVMGR_MAX_HANDLES];
    uint32_t nh = 0;
    status_t st = devmgr_call(dm, DEVMGR_GET_DRIVER, info->vendor, info->device, inst, &rep, hs,
                              DEVMGR_MAX_HANDLES, &nh, now() + 5 * NS_PER_S);
    for (uint32_t k = 0; k < nh; k++)
        jam_handle_close(hs[k]);
    return st == OK ? "  driver running" : st == ERR_BAD_STATE ? "  driver gone" : "";
}

static void devmgr_counts(void)
{
    handle_t dm = sh_devmgr();
    if (!dm) {
        sh_say("devmgr: not running (restarting?)\n");
        return;
    }
    struct devmgr_rep rep;
    status_t st = devmgr_call(dm, DEVMGR_STATUS, 0, 0, 0, &rep, NULL, 0, NULL,
                              now() + 5 * NS_PER_S);
    if (st == OK)
        sh_say("devmgr: %u bound, %u failed, %u skipped\n", rep.a, rep.b, rep.c);
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
        const char *drv = driver_of(&info, instance(pci, i, &info));
        sh_say("  %02x:%02x.%x %04x:%04x class %02x.%02x.%02x%s%s%s\n", info.bus, info.dev, info.fn,
               info.vendor, info.device, info.class_code, info.subclass, info.prog_if,
               info.flags & PCI_INFO_BRIDGE ? " bridge" : "",
               info.flags & PCI_INFO_DISPLAY ? " display" : "", drv);
    }
    sh_say("%u PCI function%s\n", i, i == 1 ? "" : "s");
    devmgr_counts();
    return 0;
}
