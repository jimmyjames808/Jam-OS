/* watch: run a command every n seconds (default 2) on a cleared screen
 * until Ctrl+C. */
#include "../sh.h"

SH_CMD(watch)
{
    uint64_t every = 2 * SH_S;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        if (!sh_parse_seconds(argv[i + 1], &every) || every < 100 * SH_MS) {
            sh_tty("watch: -n: at least 0.1 seconds\n");
            return 2;
        }
        i += 2;
    }
    if (i >= argc) {
        sh_tty("usage: watch [-n seconds] <command...>\n");
        return 2;
    }
    int st = 0;
    bool screen = !sh_piped();
    while (!sh_interrupted()) {
        if (screen)
            sh_say("\033[2J\033[H");
        sh_say("Every %lu.%lus: ", (unsigned long)(every / SH_S),
               (unsigned long)(every % SH_S / (100 * SH_MS)));
        for (int k = i; k < argc; k++)
            sh_say("%s%s", argv[k], k + 1 < argc ? " " : "\n");
        st = sh_run_words(argc - i, argv + i);
        if (!sh_sleep(every))
            break;
    }
    return st;
}
