/* dmesg: the whole kernel log (what the kernel still holds, up to 64 KiB). */
#include "sh.h"

SH_CMD(dmesg)
{
    (void)argc;
    (void)argv;
    char *buf;
    size_t got;
    uint64_t start_pos;
    status_t st = sh_klog_read(0, &buf, &got, &start_pos);
    if (st == ERR_NO_MEMORY) {
        sh_tty("dmesg: out of memory\n");
        return 1;
    }
    if (st != OK) {
        sh_tty("dmesg: %s\n", status_str(st));
        return 1;
    }
    /* Start at a whole line (the ring may have cut the first). */
    size_t start = 0;
    if (start_pos > 0) {
        while (start < got && buf[start] != '\n')
            start++;
        if (start < got)
            start++;
    }
    sh_put(buf + start, got - start);
    free(buf);
    return 0;
}
