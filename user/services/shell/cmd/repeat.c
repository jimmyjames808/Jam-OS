/* repeat: run a command n times (Ctrl+C stops); the last one's status. */
#include "sh.h"

SH_CMD(repeat)
{
    uint64_t n;
    if (argc < 3 || !sh_parse_u64(argv[1], &n)) {
        sh_tty("usage: repeat <n> <command...>\n");
        return 2;
    }
    int st = 0;
    for (uint64_t i = 0; i < n && !sh_interrupted(); i++)
        st = sh_run_words(argc - 2, argv + 2);
    return st;
}
