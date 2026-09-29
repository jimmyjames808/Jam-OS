/* seq: the numbers first..last (first defaults to 1), one a line; at most
 * a million. */
#include "../sh.h"

SH_CMD(seq)
{
    uint64_t first = 1, last;
    if (argc == 2 && sh_parse_u64(argv[1], &last)) {
    } else if (argc == 3 && sh_parse_u64(argv[1], &first) && sh_parse_u64(argv[2], &last)) {
    } else {
        sh_tty("usage: seq [first] last\n");
        return 2;
    }
    if (last - first > 1000000 && last > first) {
        sh_tty("seq: at most a million numbers\n");
        return 1;
    }
    for (uint64_t v = first; v <= last; v++) {
        sh_say("%lu\n", (unsigned long)v);
        if ((v & 1023) == 0 && sh_interrupted())
            return 130;
    }
    return 0;
}
