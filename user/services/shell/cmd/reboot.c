/* reboot: restart the machine, through init (which syncs /data first,
 * abi/idl/initctl.idl); by ourselves if init doesn't do it. */
#include <idl/initctl.h>
#include "sh.h"

#define INIT_WAIT (4 * NS_PER_S)   /* init's sync takes at most 2 s */

SH_CMD(reboot)
{
    (void)argc;
    (void)argv;
    sh_say("rebooting...\n");
    sh_flush();
    /* initctl.reboot answers only if the reset failed. */
    if (sh_initctl())
        sh_say("reboot: init: %s\n",
               status_str(initctl_reboot_until(sh_initctl(), now() + INIT_WAIT)));
    status_t st = jam_reboot(sh_root());
    sh_say("reboot: %s\n", status_str(st));
    return 0;
}
