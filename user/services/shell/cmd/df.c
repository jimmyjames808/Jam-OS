/* df: the mounts, with their size and free space. */
#include "sh.h"

SH_CMD(df)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: df\n");
        return 2;
    }
    sh_say("%-10s %10s %10s %10s  %s\n", "Mount", "Size", "Used", "Free", "Volume");
    char point[NS_NAME_MAX];
    int status = 0;
    for (unsigned i = 0; ns_mount_at(i, point); i++) {
        uint64_t total, free_bytes;
        bool ro;
        char label[17], a[24], b[24], c[24];
        status_t st = fs_statfs(point, &total, &free_bytes, &ro, label);
        if (st != OK) {
            sh_say("%-10s %s\n", point, sh_why(st));
            status = 1;
            continue;
        }
        uint64_t used = free_bytes < total ? total - free_bytes : 0;
        sh_say("%-10s %10s %10s %10s  %s%s\n", point, sh_human(total, a, sizeof(a)),
               sh_human(used, b, sizeof(b)), sh_human(free_bytes, c, sizeof(c)), label,
               ro ? " (read-only)" : "");
    }
    return status;
}
