/* run: start a program (sh_program.c), wait, say how it ended. */
#include "sh.h"

SH_CMD(run)
{
    if (argc < 2) {
        sh_tty("usage: run <prog|path> [args]\n");
        return 2;
    }
    return sh_run_program(argc - 1, argv + 1);
}
