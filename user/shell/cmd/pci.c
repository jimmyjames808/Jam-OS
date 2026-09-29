/* pci: the kernel's PCI report (BARs, MSI/MSI-X), into the kernel log. */
#include "../sh.h"

SH_CMD(pci)
{
    (void)argc;
    (void)argv;
    sh_kcmd("devices");
    return 0;
}
