/* reboot: restart the machine, through init (abi/idl/initctl.idl): by
 * kexec into a fresh copy of the system, or with -f through the firmware;
 * init syncs /data and stops the drivers first either way, and falls back
 * to the firmware by itself. By ourselves (the firmware) if init doesn't
 * answer within INIT_WAIT: then the kernel's reset lines on the screen
 * follow init's last ones, which say where it stopped.
 *
 * A kexec reboot looks like switching the PC on: the screen is blanked to
 * the boot splash's background first (console.blank), and nothing is drawn
 * until the next boot's splash. The lines below still reach the serial
 * port; if the restart fails the screen comes back to show them. */
#include <idl/console.h>
#include <idl/initctl.h>
#include "sh.h"

/* init may read the kernel and boot image, syncs (2 s at most), flushes
 * the log (1 s) and stops every driver (30 s at most) before the jump. */
#define INIT_WAIT  (60 * NS_PER_S)
#define BLANK_WAIT (2 * NS_PER_S)

SH_CMD(reboot)
{
    bool firmware = argc == 2 && !strcmp(argv[1], "-f");
    if (argc > 2 || (argc == 2 && !firmware)) {
        sh_tty("usage: reboot [-f]   (-f: through the firmware, not kexec)\n");
        return 2;
    }
    if (!firmware)
        (void)console_blank_until(sh_console(), now() + BLANK_WAIT, 1);   /* best effort */
    sh_say("Restarting%s...\n", firmware ? " through the firmware" : "");
    sh_flush();
    /* Both answer only if the restart failed. */
    if (sh_initctl()) {
        uint64_t deadline = now() + INIT_WAIT;
        status_t st = firmware ? initctl_reboot_firmware_until(sh_initctl(), deadline)
                               : initctl_reboot_until(sh_initctl(), deadline);
        if (!firmware)
            (void)console_blank_until(sh_console(), now() + BLANK_WAIT, 0);
        sh_say("reboot: the restart failed (%s): resetting the machine instead\n",
               status_str(st));
    }
    status_t st = jam_reboot(sh_root());
    sh_say("reboot: the machine couldn't be reset (%s): hold the power button to turn it "
           "off\n", status_str(st));
    return 0;
}
