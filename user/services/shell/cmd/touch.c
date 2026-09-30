/* touch: make an empty file where there is none. One that exists is left
 * as it is. */
#include "sh.h"

SH_CMD(touch)
{
    if (argc < 2) {
        sh_tty("usage: touch <file>...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char abs[SH_PATH_MAX];
        bool dir;
        uint64_t size;
        status_t st = sh_resolve(argv[i], abs, sizeof(abs)) ? OK : ERR_INVALID_ARGS;
        if (st == OK && sh_stat(abs, &dir, &size) != OK)
            st = sh_write(abs, NULL, 0, 0);
        if (st != OK) {
            sh_tty("touch: %s: %s\n", argv[i], sh_why(st));
            status = 1;
        }
    }
    return status;
}
