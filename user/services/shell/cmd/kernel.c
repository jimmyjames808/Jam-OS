/* kernel load: make /esp's kernel and boot image the stored copy now
 * (init's initctl.kernel_load, which holds the right to: kexec_load), so a
 * stick flashed while Jam OS runs (`make flash` on the Mac, the stick
 * plugged back in) is loaded before the reboot; `reboot` then starts it
 * without reading anything, and a panic comes back in the new build too. */
#include <idl/initctl.h>
#include "sh.h"

/* Reading two files from the ESP: one-sector clusters, so seconds. */
#define LOAD_WAIT (60 * NS_PER_S)

SH_CMD(kernel)
{
    if (argc != 2 || strcmp(argv[1], "load")) {
        sh_tty("usage: kernel load   (/esp's kernel and boot image, stored for the next reboot)\n");
        return 2;
    }
    if (!sh_initctl()) {
        sh_tty("kernel: no channel to init\n");
        return 1;
    }
    sh_say("kernel: reading /esp/boot/jamos.elf and /esp/boot/bootfs.img...\n");
    sh_flush();
    uint64_t kb = 0, bb = 0;
    uint32_t ms = 0;
    status_t st = initctl_kernel_load_until(sh_initctl(), now() + LOAD_WAIT, &kb, &bb, &ms);
    char k[16], b[16];
    if (st == OK) {
        sh_say("kernel: loaded (%s + %s read in %u ms): `reboot` starts it, without reading "
               "the stick again\n", sh_human(kb, k, sizeof(k)), sh_human(bb, b, sizeof(b)), ms);
        return 0;
    }
    sh_tty("kernel: not loaded: %s; the stored kernel is the one it was\n",
           st == ERR_NOT_FOUND           ? "no /esp, or not both files on it"
           : st == ERR_NOT_SUPPORTED     ? "there is no stored kernel (crashkernel=0)"
           : st == ERR_INVALID_ARGS      ? "they are not a Jam OS kernel and boot image"
           : st == ERR_NO_RESOURCES      ? "they are too big for the stored kernel's region"
                                         : status_str(st));
    return 1;
}
