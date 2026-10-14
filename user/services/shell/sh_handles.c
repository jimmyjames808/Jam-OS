/* The handles init gives the shell (shell mode), besides the console's
 * (main.c):
 *   SR_RESOURCE   the root resource with RIGHT_ROOT_KLOG (the log),
 *                 RIGHT_ROOT_SYSINFO (ps, top), RIGHT_ROOT_CLOCK (date),
 *                 RIGHT_ROOT_DEBUG (ktest, bench, stress, mem through
 *                 debug_command), RIGHT_ROOT_REBOOT (reboot when init
 *                 doesn't answer), RIGHT_ROOT_VMEX (programs from /data)
 *                 and RIGHT_ROOT_JIT (passed on to a program whose list
 *                 says `right jit`);
 *                 no RIGHT_MAP / RIGHT_SLICE
 *   SHELL_SR_PCI   RES_PCI, RIGHTS_BASIC only (pci_enum for `devices`)
 *   SR_NS         init's whole namespace, as it is (every mount writable,
 *                 /data's etc included: the shell is where `allow` marks
 *                 programs) and kept up to date, with every service under
 *                 /svc (<os.h> SVC_*): devmgr's query and control channels
 *                 (`devices`, `usb`; control only passed on to the test
 *                 programs whose lists ask for it), init's control channel
 *                 (`kill`, `sync`, `reboot`, `mount`), the mixer's `audio`
 *                 and `audioctl` (`vol`), the music player (`music`: a
 *                 channel of the shell's own), logd's control channel,
 *                 netstack's /svc/net (`net`, `ping`: <net.h>), the
 *                 resolver's /svc/dns (`host`, `ping <name>`: sh_lookup)
 * Each service's channel is libos's (svc_get): asked for again after its
 * service restarted (a new devmgr), it is the new one. What the shell
 * passes on to a program is that program's list (sh_program.c). */
#include <devmgr.h>
#include "sh_core.h"

static handle_t root, pci;

void sh_handles_init(void)
{
    root = startup_handle(SR_RESOURCE);
    pci = startup_handle(SHELL_SR_PCI);
}

handle_t sh_music(void)
{
    return svc_get(SVC_MUSIC);
}

handle_t sh_audio(void)
{
    return svc_get(SVC_AUDIO);
}

handle_t sh_audio_ctl(void)
{
    return svc_get(SVC_AUDIOCTL);
}

handle_t sh_initctl(void)
{
    return svc_get(SVC_INIT);
}

handle_t sh_root(void)
{
    return root;
}

handle_t sh_pci(void)
{
    return pci;
}

/* 0 while there is none: a restart in progress, or no devmgr at all. */
handle_t sh_devmgr(void)
{
    return svc_get(SVC_DEVMGR);
}

