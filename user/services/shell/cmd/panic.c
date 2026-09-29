/* panic: panic the kernel (a test: its screen must show over the console). */
#include "../sh.h"

SH_CMD(panic)
{
    (void)argc;
    (void)argv;
    sh_kcmd("panic");
    return 0;
}
