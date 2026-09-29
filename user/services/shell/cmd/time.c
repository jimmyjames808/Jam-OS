/* time: run a command and say how long it took (on the screen). */
#include "sh.h"

SH_CMD(time)
{
    if (argc < 2) {
        sh_tty("usage: time <command...>\n");
        return 2;
    }
    uint64_t t0 = (uint64_t)jam_clock_get();
    int st = sh_run_words(argc - 1, argv + 1);
    uint64_t us = ((uint64_t)jam_clock_get() - t0) / 1000;
    sh_tty("time: %lu.%06lu s (status %d)\n", (unsigned long)(us / 1000000),
           (unsigned long)(us % 1000000), st);
    return st;
}
