/* stress: the kernel's stress test for 1..600 seconds. */
#include "sh.h"

SH_CMD(stress)
{
    if (argc != 2) {
        sh_tty("usage: stress <seconds>   (1..600)\n");
        return 2;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "stress %s", argv[1]);
    int64_t r = sh_kcmd(cmd);   /* the kernel checks the number (said if it refuses) */
    if (r >= 0)
        sh_say("shell: %s: %s\n", cmd, r == 0 ? "PASSED" : "FAILED");
    return r == 0 ? 0 : 1;
}
