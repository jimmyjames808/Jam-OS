/* The handles init gives the shell (shell mode), besides the console's
 * (main.c):
 *   SR_RESOURCE   the root resource with RIGHT_READ (log) and RIGHT_MANAGE
 *                 (ktest, bench, stress, ps, mem through debug_command;
 *                 reboot); no RIGHT_MAP / RIGHT_SLICE
 *   SR_USER + 1   RES_PCI, RIGHTS_BASIC only (pci_enum for `devices`)
 *   SR_USER + 2   a channel from init: on the boot after a panic the first
 *                 shell finds a line to print on it (INIT_SHELL_NOTE:
 *                 sh_boot_note)
 *   SR_NS         init's whole namespace, as it is (every mount writable,
 *                 /data's etc included: the shell is where `allow` marks
 *                 programs) and kept up to date, with every service under
 *                 /svc (<os.h> SVC_*): devmgr's query and control channels
 *                 (`devices`, `usb`; control only passed on to the test
 *                 programs whose lists ask for it), init's control channel
 *                 (`kill`, `sync`, `reboot`, `mount`), the mixer's `audio`
 *                 and `audioctl` (`vol`), the music player (`music`: a
 *                 channel of the shell's own), logd's control channel
 * Each service's channel is libos's (svc_get): asked for again after its
 * service restarted (a new devmgr), it is the new one. What the shell
 * passes on to a program is that program's list (sh_program.c). */
#include <devmgr.h>
#include "sh_core.h"

static handle_t root, pci, from_init;
static char note[INIT_SHELL_NOTE_MAX + 1];   /* INIT_SHELL_NOTE's line, until taken */

void sh_handles_init(void)
{
    root = startup_handle(SR_RESOURCE);
    pci = startup_handle(SR_USER + 1);
    from_init = startup_handle(SR_USER + 2);
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

const char *sh_boot_note(void)
{
    static char taken[INIT_SHELL_NOTE_MAX + 1];
    while (from_init) {
        struct { uint32_t kind; char text[INIT_SHELL_NOTE_MAX]; } m = { 0, { 0 } };
        uint32_t n = 0, nh = 0;
        handle_t h[2] = { HANDLE_INVALID, HANDLE_INVALID };
        struct channel_read_args a = {
            .h = from_init, .bytes_cap = sizeof(m), .bytes = (uint64_t)(uintptr_t)&m,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)h,
            .handles_cap = 2, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        if (jam_channel_read(&a) != OK)
            break;   /* nothing new (or init's end is gone) */
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(h[i]);
        if (n > sizeof(m.kind) && m.kind == INIT_SHELL_NOTE && nh == 0) {
            memcpy(note, m.text, n - sizeof(m.kind));
            note[n - sizeof(m.kind)] = '\0';
        }
    }
    memcpy(taken, note, sizeof(taken));
    note[0] = '\0';
    return taken;
}

/* 0 while there is none: a restart in progress, or no devmgr at all. */
handle_t sh_devmgr(void)
{
    return svc_get(SVC_DEVMGR);
}

handle_t sh_devmgr_ctl(void)
{
    return svc_get(SVC_DEVMGR_CTL);
}
