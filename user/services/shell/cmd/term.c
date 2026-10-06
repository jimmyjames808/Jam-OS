/* term: open another terminal, through init (abi/idl/initctl.idl
 * terminal): a window of its own on the compositor, with a shell of its
 * own, which init supervises as it does this one. Super+Enter in a
 * terminal does the same. With a command, the new terminal's shell runs
 * it first, as if typed at its prompt, and stays. Without a compositor (a
 * boot without one) there is one terminal. */
#include <idl/initctl.h>
#include "sh.h"

#define INIT_WAIT (5 * NS_PER_S)
#define CMD_MAX   128   /* initctl.terminal's command, its NUL included */

SH_CMD(term)
{
    uint8_t cmd[CMD_MAX] = { 0 };
    size_t o = 0;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (o + n + (o ? 1 : 0) >= CMD_MAX) {
            sh_tty("term: the command is longer than %u characters\n", CMD_MAX - 1);
            return 2;
        }
        if (o)
            cmd[o++] = ' ';
        memcpy(cmd + o, argv[i], n);
        o += n;
    }
    if (!sh_initctl()) {
        sh_tty("term: no init to ask (a shell run from a shell?)\n");
        return 1;
    }
    uint8_t n = 0;
    status_t st = initctl_terminal_until(sh_initctl(), now() + INIT_WAIT, cmd, &n);
    if (st == ERR_NOT_SUPPORTED) {
        sh_tty("term: more terminals need the compositor, and this boot has none\n");
        return 1;
    }
    if (st == ERR_NO_RESOURCES) {
        sh_tty("term: as many terminals are open as there may be\n");
        return 1;
    }
    if (st != OK) {
        sh_tty("term: init: %s\n", st == ERR_INVALID_ARGS ? "not one line of text" : sh_why(st));
        return 1;
    }
    sh_say("terminal %u opens\n", (unsigned)n);
    return 0;
}
