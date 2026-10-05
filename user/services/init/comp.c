/* init's shell mode: the compositor (docs/G1-PLAN.md), on a boot with the
 * word `comp`; without it the console owns the screen as it always did.
 *
 *   compositor  bin/compositor, first (before the console): root with
 *               RIGHT_ROOT_SCREEN only (framebuffer_take; none of the
 *               console's other powers) and the server end of
 *               /svc/wayland's shared channel (SR_USER + 0), on which each
 *               opener's svc.connect makes its own connection. init makes
 *               that channel once and keeps both ends, so a restarted
 *               compositor serves the same channel and its clients (the
 *               consoles: terms.c gives each a duplicate of the client
 *               end) connect again through it. Like the console it is
 *               started again however often it ends (shell.c,
 *               never_given_up): the terminals have no screen without it.
 *
 * Not built here yet (the boot wiring's): the compositor on every plain
 * boot, /svc/wayland in the namespace for programs, its compctl channels
 * for devmgr's and serialin's input, the console without the input
 * sources. Until then the input still goes to the first console, and
 * through it to the first terminal only. */
#include <os.h>
#include "init.h"

static handle_t wl_srv, wl_cli;   /* /svc/wayland's shared channel (0: no compositor) */

bool comp_on(void)
{
    return wl_cli != HANDLE_INVALID;
}

handle_t comp_wayland(void)
{
    handle_t d = HANDLE_INVALID;
    if (!wl_cli || jam_handle_duplicate(wl_cli, RIGHT_SAME, &d) != OK)
        return HANDLE_INVALID;
    return d;
}

status_t comp_start(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    handle_t srv = HANDLE_INVALID;
    if (!wl_srv || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, svcs[COMPOSITOR].path, &data, &size) != OK ||
        jam_handle_duplicate(wl_srv, RIGHT_SAME, &srv) != OK) {
        printf("init: no %s (or no channel for it): no compositor\n", svcs[COMPOSITOR].path);
        svcs[COMPOSITOR].given_up = true;
        return OK;
    }
    struct spawn_handle x[] = {
        { SR_RESOURCE, services_root_with(RIGHTS_BASIC | RIGHT_ROOT_SCREEN) },
        { SR_USER + 0, srv },
    };
    return svc_start1(COMPOSITOR, x, 2);
}

void comp_init(bool on)
{
    if (!on || jam_channel_create(&wl_cli, &wl_srv) != OK) {
        wl_cli = wl_srv = HANDLE_INVALID;
        svcs[COMPOSITOR].given_up = true;   /* not on this boot */
        return;
    }
    printf("init: the compositor draws the screen: each terminal is a window\n");
}
