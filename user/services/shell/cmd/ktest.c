/* ktest: the kernel tests (all, or those whose names start with a prefix)
 * on the live system; they report into the kernel log. The other words go
 * to the kernel as typed (kernel/include/jam/ktest.h, struct ktest_opts):
 * loops=N, seed=S, shuffle, keep, load. */
#include "sh.h"

SH_CMD(ktest)
{
    char cmd[64];
    size_t n = (size_t)snprintf(cmd, sizeof(cmd), "ktest");
    for (int i = 1; i < argc; i++) {
        if (n + 1 + strlen(argv[i]) >= sizeof(cmd)) {
            sh_tty("ktest: too many words\n");
            return 2;
        }
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, " %s", argv[i]);
    }
    int64_t r = sh_kcmd(cmd);
    if (r >= 0)
        sh_say("shell: %s: %ld passed\n", cmd, (long)r);
    return r < 0;
}
