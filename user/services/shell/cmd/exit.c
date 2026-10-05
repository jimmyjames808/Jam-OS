/* exit: end this shell, and with it its terminal (and every program it
 * started with &), in a terminal other than the first (`term`): init
 * closes the terminal when its shell ends with code 0. The first
 * terminal is the system's: init would only start its shell again, so
 * exit says so and does nothing there. */
#include "sh.h"

SH_CMD(exit)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: exit\n");
        return 2;
    }
    if (sh_term_no <= 1) {
        sh_tty("exit: this is the first terminal: it stays (exit closes the others)\n");
        return 1;
    }
    sh_flush();
    jam_process_exit(0);
    return 0;   /* not reached */
}
