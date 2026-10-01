/* The process build's driver entry: libos's _start reads the startup
 * message and calls main(); this main() turns the message's handles into
 * the struct driver_start and runs driver_main. Linked into every driver
 * ELF (drv/<name> in bootfs), never into libos.a (other programs have a
 * main of their own).
 *
 * Roles: whoever starts a driver passes each handle with startup role
 * SR_DRIVER(DR_*) (= SR_USER + the driver role, <os.h>); the rest of the
 * startup message (process, vmar, job, bootfs) is libos's. The driver's
 * name is its argv[0] without the path ("drv/null" -> "null"); the rest of
 * argv (up to DRV_MAX_ARGS words) are its args. */
#include <jam/driver.h>
#include <os.h>
#include "driver_start.h"

static struct driver_start start;

int main(int argc, char **argv)
{
    const char *name = argc > 0 && argv[0] ? argv[0] : "driver";
    for (const char *p = name; *p; p++)
        if (*p == '/')
            name = p + 1;
    start.name = name;
    for (int i = 1; i < argc && start.nargs < DRV_MAX_ARGS; i++)
        start.args[start.nargs++] = argv[i];
    for (unsigned i = 0; i < startup_handle_count(); i++) {
        uint32_t role;
        handle_t h = startup_handle_at(i, &role);
        if (role < SR_USER)
            continue;
        if (start.nhandles == DRV_MAX_HANDLES) {
            printf("%s: more than %u driver handles; the rest are ignored\n", name,
                   DRV_MAX_HANDLES);
            break;
        }
        start.handles[start.nhandles].role = role - SR_USER;
        start.handles[start.nhandles].h = h;
        start.nhandles++;
    }
    libos_driver_start = &start;
    return driver_main(&start);
}
