/* ktest: the kernel tests (all, or those whose names start with a prefix)
 * on the live system; they report into the kernel log. */
#include "sh.h"

SH_CMD(ktest)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "ktest%s%s", argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
    int64_t r = sh_kcmd(cmd);
    if (r >= 0)
        sh_say("shell: %s: %ld passed\n", cmd, (long)r);
    return 0;
}
