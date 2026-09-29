/* sleep: wait some seconds ("0.5" works); Ctrl+C stops it. */
#include "../sh.h"

SH_CMD(sleep)
{
    uint64_t ns;
    if (argc != 2 || !sh_parse_seconds(argv[1], &ns)) {
        sh_tty("usage: sleep <seconds>\n");
        return 2;
    }
    return sh_sleep(ns) ? 0 : 130;
}
