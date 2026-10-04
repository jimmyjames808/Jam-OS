/* iommu: the IOMMU (VT-d) state from the kernel, into its log: the units,
 * their translation, interrupt-remapping and queue state, each covered
 * function's domain and mapped pages, the DMA fault counts and the
 * interrupt remapping table's use. */
#include "sh.h"

SH_CMD(iommu)
{
    (void)argc;
    (void)argv;
    sh_kcmd("iommu");
    return 0;
}
