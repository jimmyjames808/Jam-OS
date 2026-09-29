/* unalias: remove aliases. */
#include "sh.h"

SH_CMD(unalias)
{
    int st = 0;
    for (int i = 1; i < argc; i++) {
        if (!sh_unalias(argv[i])) {
            sh_tty("unalias: %s: not found\n", argv[i]);
            st = 1;
        }
    }
    return st;
}
