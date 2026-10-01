/* reboot: restart the machine, through init (abi/idl/initctl.idl): by
 * kexec into the kernel on the stick, or with -f through the firmware;
 * init syncs /data first either way, and falls back to the firmware by
 * itself. By ourselves (the firmware) if init doesn't do it. */
#include <idl/initctl.h>
#include "sh.h"

/* init reads the kernel and boot image, syncs (2 s at most), flushes the
 * log (1 s) and stops every driver (30 s at most) before the jump. */
#define INIT_WAIT (60 * NS_PER_S)

SH_CMD(reboot)
{
    bool firmware = argc == 2 && !strcmp(argv[1], "-f");
    if (argc > 2 || (argc == 2 && !firmware)) {
        sh_tty("usage: reboot [-f]   (-f: through the firmware, not kexec)\n");
        return 2;
    }
    sh_say("rebooting%s...\n", firmware ? " through the firmware" : "");
    sh_flush();
    /* Both answer only if the restart failed. */
    if (sh_initctl()) {
        uint64_t deadline = now() + INIT_WAIT;
        status_t st = firmware ? initctl_reboot_firmware_until(sh_initctl(), deadline)
                               : initctl_reboot_until(sh_initctl(), deadline);
        sh_say("reboot: init: %s\n", status_str(st));
    }
    status_t st = jam_reboot(sh_root());
    sh_say("reboot: %s\n", status_str(st));
    return 0;
}
