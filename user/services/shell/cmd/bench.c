/* bench: the kernel benchmark; its results go to the kernel log. */
#include "sh.h"

SH_CMD(bench)
{
    (void)argc;
    (void)argv;
    if (sh_kcmd("bench") >= 0)
        sh_say("shell: bench: done (results in the log above)\n");
    return 0;
}
