/* The handles init gives the shell (shell mode), besides the console's
 * (main.c):
 *   SR_RESOURCE   the root resource with RIGHT_READ (log) and RIGHT_MANAGE
 *                 (ktest, bench, stress, ps, mem through debug_command;
 *                 reboot); no RIGHT_MAP / RIGHT_SLICE
 *   SR_USER + 1   RES_PCI, RIGHTS_BASIC only (pci_enum for `devices`)
 *   SR_DEVMGR     devmgr's query channel, a client end (`devices`, `usb`)
 *   SR_DEVMGR_CTL devmgr's control channel, a client end: only handed on to
 *                 the test programs of `utest` and `usbtest`
 *   SR_USER + 2   a channel from init: when devmgr dies, init starts it
 *                 again (with its drivers) and sends the new client ends
 *                 here (INIT_SHELL_DEVMGR, <devmgr.h>); every command that
 *                 talks to devmgr takes the newest first. On the boot after
 *                 a panic the first shell also finds a line to print on it
 *                 (INIT_SHELL_NOTE: sh_boot_note)
 *   SR_USER + 3   init's control channel (abi/idl/initctl.idl): `kill`,
 *                 and `reboot` with /data synced first
 *   SR_AUDIO      the mixer's `audio` channel (abi/idl/audio.idl): every
 *                 program the shell runs gets a duplicate
 *   SR_AUDIO_CTL  its `audioctl` channel (`vol`; test programs get it too)
 *   SR_USER + 4   the music player's channel (abi/idl/music.idl): `music`
 * init keeps both mixer channels across a mixer's restart, and the
 * player's across the player's: they never change. Programs the shell starts get none of the others
 * (sh_program.c). */
#include <devmgr.h>
#include "sh_core.h"

static handle_t root, pci, devmgr, devmgr_ctl, from_init, initctl, audio, audio_ctl, music;
static char note[INIT_SHELL_NOTE_MAX + 1];   /* INIT_SHELL_NOTE's line, until taken */

void sh_handles_init(void)
{
    root = startup_handle(SR_RESOURCE);
    pci = startup_handle(SR_USER + 1);
    devmgr = startup_handle(SR_DEVMGR);
    devmgr_ctl = startup_handle(SR_DEVMGR_CTL);
    from_init = startup_handle(SR_USER + 2);
    initctl = startup_handle(SR_USER + 3);
    audio = startup_handle(SR_AUDIO);
    audio_ctl = startup_handle(SR_AUDIO_CTL);
    music = startup_handle(SR_USER + 4);
}

handle_t sh_music(void)
{
    return music;
}

handle_t sh_audio(void)
{
    return audio;
}

handle_t sh_audio_ctl(void)
{
    return audio_ctl;
}

handle_t sh_initctl(void)
{
    return initctl;
}

handle_t sh_root(void)
{
    return root;
}

handle_t sh_pci(void)
{
    return pci;
}

static void drop(handle_t *h)
{
    if (*h)
        jam_handle_close(*h);
    *h = HANDLE_INVALID;
}

/* Take what init sent: the newest devmgr client ends, a line to print;
 * and forget dead ends. */
static void devmgr_refresh(void)
{
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
        if (n == sizeof(m.kind) && m.kind == INIT_SHELL_DEVMGR && nh == 2) {
            drop(&devmgr);
            drop(&devmgr_ctl);
            devmgr = h[0];
            devmgr_ctl = h[1];
        } else if (n > sizeof(m.kind) && m.kind == INIT_SHELL_NOTE && nh == 0) {
            memcpy(note, m.text, n - sizeof(m.kind));
            note[n - sizeof(m.kind)] = '\0';
        } else {
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(h[i]);
        }
    }
    handle_t *ends[2] = { &devmgr, &devmgr_ctl };
    for (unsigned i = 0; i < 2; i++) {
        signals_t seen = 0;
        if (*ends[i] && jam_object_wait_one(*ends[i], SIG_PEER_CLOSED, 0, &seen) == OK &&
            (seen & SIG_PEER_CLOSED))
            drop(ends[i]);
    }
}

const char *sh_boot_note(void)
{
    static char taken[INIT_SHELL_NOTE_MAX + 1];
    devmgr_refresh();
    memcpy(taken, note, sizeof(taken));
    note[0] = '\0';
    return taken;
}

/* 0 while there is none: a restart in progress, or no devmgr at all. */
handle_t sh_devmgr(void)
{
    devmgr_refresh();
    return devmgr;
}

handle_t sh_devmgr_ctl(void)
{
    devmgr_refresh();
    return devmgr_ctl;
}
