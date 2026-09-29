/* reboot: restart the machine. */
#include "sh.h"

SH_CMD(reboot)
{
    (void)argc;
    (void)argv;
    sh_say("rebooting...\n");
    sh_flush();
    status_t st = jam_reboot(sh_root());
    sh_say("reboot: %s\n", status_str(st));
    return 0;
}
