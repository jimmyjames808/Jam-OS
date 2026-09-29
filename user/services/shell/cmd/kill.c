/* kill: kill the first process with that name (the kernel's kill reaches
 * the whole job tree), except init. */
#include "sh.h"

SH_CMD(kill)
{
    if (argc != 2) {
        sh_say("usage: kill <name>\n");
        return 0;
    }
    if (!strcmp(argv[1], "init")) {
        /* Nobody restarts init: it supervises everything else (killed, the
         * kernel prints its RESULTS box while the rest runs on
         * unsupervised). devmgr may go: init starts it again, with its
         * drivers. */
        sh_say("kill: %s is not restarted by anyone: not killing it\n", argv[1]);
        return 0;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "kill %s", argv[1]);
    int64_t r = sh_kcmd(cmd);
    if (r >= 0)
        sh_say("shell: killed process %ld (%s)\n", (long)r, argv[1]);
    return 0;
}
