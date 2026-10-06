/* launch: start one of the desktop's apps through init, as the
 * compositor's search box does (abi/idl/initctl.idl launch: only the
 * programs <deskapps.h> lists, from the boot image, each with what its
 * list asks for and /svc/wayland, no terminal). The shell's way to see
 * what init allows the compositor. */
#include <idl/initctl.h>
#include "sh.h"

#define INIT_WAIT (5 * NS_PER_S)

SH_CMD(launch)
{
    uint8_t app[16] = { 0 };
    if (argc != 2 || strlen(argv[1]) >= sizeof(app)) {
        sh_tty("usage: launch <app>\n");
        return 2;
    }
    if (!sh_initctl()) {
        sh_tty("launch: no init to ask (a shell run from a shell?)\n");
        return 1;
    }
    memcpy(app, argv[1], strlen(argv[1]));
    uint64_t koid = 0;
    status_t st = initctl_launch_until(sh_initctl(), now() + INIT_WAIT, app, &koid);
    if (st == ERR_NOT_FOUND) {
        sh_tty("launch: %s is not one of the desktop's apps\n", argv[1]);
        return 1;
    }
    if (st != OK) {
        sh_tty("launch: init: %s\n", sh_why(st));
        return 1;
    }
    sh_say("launch: %s started (process %lu)\n", argv[1], (unsigned long)koid);
    return 0;
}
