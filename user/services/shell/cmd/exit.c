/* exit: end this shell, and with it its terminal (and every program it
 * started with &): init closes the terminal when its shell ends with
 * code 0, the first terminal like any other. A shell init gave no
 * "term=" (sh_term_no 0: started without a compositor) would only be
 * started again, so exit says so and does nothing there. */
#include "sh.h"

SH_CMD(exit)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: exit\n");
        return 2;
    }
    if (!sh_term_no) {
        sh_tty("exit: this is the only terminal (no desktop), so it stays open\n");
        return 1;
    }
    sh_flush();
    jam_process_exit(0);
    return 0;   /* not reached */
}
