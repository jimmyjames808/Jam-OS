/* sync: everything written is on its medium, on every mount. (`reboot`
 * does it for /data by itself.) */
#include "sh.h"

SH_CMD(sync)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: sync\n");
        return 2;
    }
    char point[NS_NAME_MAX];
    int status = 0;
    for (unsigned i = 0; ns_mount_at(i, point); i++) {
        status_t st = fs_sync(point);
        if (st != OK) {
            sh_tty("sync: %s: %s\n", point, sh_why(st));
            status = 1;
        }
    }
    return status;
}
