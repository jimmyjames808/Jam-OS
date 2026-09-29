/* stress: the kernel's stress test for 1..600 seconds. */
#include "sh.h"

SH_CMD(stress)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "stress %s", argc > 1 ? argv[1] : "");
    int64_t r = sh_kcmd(cmd);
    if (r >= 0)
        sh_say("shell: %s: %s\n", cmd, r == 0 ? "PASSED" : "FAILED");
    return 0;
}
