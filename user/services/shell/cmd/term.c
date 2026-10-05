/* term: open another terminal, through init (abi/idl/initctl.idl
 * terminal): a window of its own on the compositor, with a shell of its
 * own, which init supervises as it does this one. Super+Enter in a
 * terminal does the same. Without a compositor (a boot without one)
 * there is one terminal. */
#include <idl/initctl.h>
#include "sh.h"

#define INIT_WAIT (5 * NS_PER_S)

SH_CMD(term)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: term\n");
        return 2;
    }
    if (!sh_initctl()) {
        sh_tty("term: no init to ask (a shell run from a shell?)\n");
        return 1;
    }
    uint8_t n = 0;
    status_t st = initctl_terminal_until(sh_initctl(), now() + INIT_WAIT, &n);
    if (st == ERR_NOT_SUPPORTED) {
        sh_tty("term: more terminals need the compositor, and this boot has none\n");
        return 1;
    }
    if (st == ERR_NO_RESOURCES) {
        sh_tty("term: as many terminals are open as there may be\n");
        return 1;
    }
    if (st != OK) {
        sh_tty("term: init: %s\n", status_str(st));
        return 1;
    }
    sh_say("terminal %u opens\n", (unsigned)n);
    return 0;
}
