/* memmap: the loader's memory map, from the kernel, into its log. */
#include "sh.h"

SH_CMD(memmap)
{
    (void)argc;
    (void)argv;
    sh_kcmd("memmap");
    return 0;
}
