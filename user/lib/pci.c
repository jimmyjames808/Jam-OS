/* PCI helpers for programs that hold a function's handle (devmgr, and
 * utest's checks of what a driver's handles can reach). Config reads go
 * through the kernel (pci_config_read), which filters them by the
 * handle's rights. */
#include <os.h>

uint32_t pci_find_cap(handle_t dev, uint32_t id)
{
    uint32_t status = 0, p = 0, v = 0;
    /* Status bit 4: the function has a capability list at 0x34. */
    if (jam_pci_config_read(dev, 0x06, 2, &status) != OK || !(status & 0x10) ||
        jam_pci_config_read(dev, 0x34, 1, &p) != OK)
        return 0;
    for (int guard = 0; p >= 0x40 && p < 0x100 && guard < 48; guard++) {
        p &= ~3u;
        if (jam_pci_config_read(dev, p, 2, &v) != OK)
            return 0;
        if ((v & 0xff) == id)
            return p;
        p = v >> 8;
    }
    return 0;
}
