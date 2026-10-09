/* history: the lines typed, numbered, oldest first (up/down recall them;
 * sh_history.c keeps them in /data/etc/history, shared by the terminals);
 * `history -c` forgets them, the file too. */
#include "sh.h"

SH_CMD(history)
{
    if (argc == 2 && !strcmp(argv[1], "-c")) {
        sh_history_clear();
        return 0;
    }
    if (argc != 1) {
        sh_tty("usage: history [-c]\n");
        return 2;
    }
    unsigned n = sh_history_count();
    for (unsigned i = 0; i < n; i++)
        sh_say("%5u  %s\n", i + 1, sh_history_at(i));
    return 0;
}
