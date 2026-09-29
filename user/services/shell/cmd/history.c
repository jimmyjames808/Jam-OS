/* history: the last 32 lines typed, numbered (up/down recall them). */
#include "../sh.h"

SH_CMD(history)
{
    (void)argc;
    (void)argv;
    unsigned n = sh_history_count();
    for (unsigned i = n > 32 ? n - 32 : 0; i < n; i++) {
        const char *l = sh_history_at(i);
        if (l)
            sh_say("%5u  %s\n", i + 1, l);
    }
    return 0;
}
