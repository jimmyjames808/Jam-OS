/* crash: the kernel's crash tests (debug_command "crash"). Alone, the
 * list; each one stops the machine with a panic screen (bp excepted), so a
 * name must be confirmed: crash <name> yes. */
#include "../sh.h"

SH_CMD(crash)
{
    if (argc == 1) {
        sh_kcmd("crash");
        sh_say("usage: crash <name> yes   (each one panics the kernel on purpose, bp excepted)\n");
        return 0;
    }
    if (argc != 3 || strcmp(argv[2], "yes")) {
        sh_say("crash %s: this stops the machine on purpose; type \"crash %s yes\" to go ahead\n",
               argv[1], argv[1]);
        return 0;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "crash %s", argv[1]);
    sh_say("crash %s: here goes\n", argv[1]);
    int64_t r = sh_kcmd(cmd);
    if (r == 0)
        sh_say("crash %s: came back%s\n", argv[1],
               strcmp(argv[1], "bp") ? " (it should have panicked!)" : ", as a breakpoint must");
    else if (r == ERR_NOT_FOUND)
        sh_say("crash: no test called %s (try crash alone)\n", argv[1]);
    return 0;
}
