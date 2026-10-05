/* init's shell mode: the terminal, a console and the shell on it (shell.c
 * supervises them; services.c starts the other services).
 *
 *   console   bin/console: root with CONSOLE_ROOT (klog, the screen,
 *             serial output; reboot on Ctrl+Alt+Del), the server end of a
 *             console channel (SR_USER + 0) and a control channel of init's
 *             that answers only `reboot` (SR_USER + 8, ctl.c: Ctrl+Alt+Del
 *             goes through init, which syncs /data first), and the log
 *             writers' table read-only (CONSOLE_WRITERS_ROLE, writers.c);
 *             init keeps the client end. With the splash (the argument
 *             "quiet") it draws nothing until the splash has borrowed the
 *             screen and given it back; on a boot with the splash every
 *             console also gets "nolog": the kernel log off the screen
 *             but for its notices
 *   shell     bin/shell: a SHELL-level console channel (SR_CONSOLE:
 *             console.new_client; no connect_input), root with SHELL_ROOT,
 *             RES_PCI with RIGHTS_BASIC (SR_USER + 1), a channel from
 *             init (SR_USER + 2) and init's whole namespace as it is
 *             (SR_NS, followed: every mount, /data's etc included, and
 *             every service: devmgr's channels, init's control channel
 *             /svc/init, made anew for each shell (ctl.c: kill, sync,
 *             reboot, mount), the mixer's, the music player's, logd's,
 *             netstack's /svc/net).
 *             On the boot after a panic the first shell waits for logd's
 *             answer (lastboot.c) and finds its one line queued on the
 *             SR_USER + 2 channel (INIT_SHELL_NOTE) when it starts
 * Neither gets RIGHT_ROOT_MAP or RIGHT_ROOT_SLICE: they can't reach
 * hardware beyond the calls made for them.
 *
 * A new console gets a new channel, so serialin and the shell (whose
 * channel then closes) exit and come back connected to it, and devmgr
 * gets the new channel (DEVMGR_SET_CONSOLE): its HID drivers, which end
 * when their console goes, come back connected to it. */
#include <devmgr.h>
#include <idl/console.h>
#include <logwriters.h>
#include <os.h>
#include "init.h"

/* The root's powers (<jam/abi.h> RIGHT_ROOT_*; neither can map or slice):
 * the console reads the log, draws on the screen, mirrors it to the
 * serial port's output and reboots on Ctrl+Alt+Del if init doesn't
 * answer; the shell reads the log, the system's figures, the clock, the
 * kernel's debug commands, a reboot when init doesn't answer, and
 * programs from /data (VMEX). */
#define CONSOLE_ROOT (RIGHT_ROOT_KLOG | RIGHT_ROOT_SCREEN | RIGHT_ROOT_SERIAL_OUT | \
                      RIGHT_ROOT_REBOOT)
#define SHELL_ROOT   (RIGHT_ROOT_KLOG | RIGHT_ROOT_SYSINFO | RIGHT_ROOT_CLOCK | RIGHT_ROOT_DEBUG | \
                      RIGHT_ROOT_REBOOT | RIGHT_ROOT_VMEX)

static handle_t port;
static handle_t cons;        /* the console client end (0: none) */
static handle_t to_shell;    /* init's end of the shell's SR_USER + 2 channel */
static bool quiet_console;   /* the next console starts quiet (the splash's first one) */
static bool nolog_console;   /* every console keeps the log off the screen (a splash boot) */
/* An argument for the first shell started ("soak=3": run the soak test), or NULL. */
static const char *first_arg;

handle_t shell_console(void)
{
    return cons;
}

bool services_console_up(void)
{
    return cons != HANDLE_INVALID;
}

/* devmgr takes a new console (after the console restarted): its HID
 * drivers come back connected to it. */
static void tell_devmgr(void)
{
    handle_t c = HANDLE_INVALID;
    handle_t devmgr = shell_devmgr();
    if (!devmgr || jam_handle_duplicate(cons, RIGHT_SAME, &c) != OK)
        return;
    struct devmgr_req q = { 0, DEVMGR_SET_CONSOLE, 0, 0, 0 };
    struct devmgr_rep r;
    uint32_t n = 0, got = 0;
    struct channel_call_args a = {
        .h = devmgr, .wn = sizeof(q), .wbytes = (uint64_t)(uintptr_t)&q,
        .wh = (uint64_t)(uintptr_t)&c, .whn = 1, .rcap = sizeof(r),
        .rbytes = (uint64_t)(uintptr_t)&r, .ractual = (uint64_t)(uintptr_t)&n,
        .rhactual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + 5 * NS_PER_S,
    };
    status_t st = jam_channel_call(&a);   /* c goes with the request either way */
    if (st != OK || n < DEVMGR_REP_HDR || r.status != OK)
        printf("init: devmgr didn't take the new console (%s)\n",
               status_str(st != OK ? st : r.status));
}

status_t terms_start_console(void)
{
    handle_t a, b, ctl = HANDLE_INVALID;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    /* Without it the console still reboots, without the sync. */
    if (ctl_new(CTL_CONSOLE, port, KEY_CTL + CTL_CONSOLE, &ctl) != OK)
        ctl = HANDLE_INVALID;
    struct spawn_handle x[4] = {
        { SR_RESOURCE, services_root_with(RIGHTS_BASIC | CONSOLE_ROOT) },
        { SR_USER + 0, b },
    };
    unsigned nx = 2;
    handle_t writers = writers_for_console();   /* without it no process makes notices */
    if (writers)
        x[nx++] = (struct spawn_handle){ CONSOLE_WRITERS_ROLE, writers };
    if (ctl)
        x[nx++] = (struct spawn_handle){ SR_USER + 8, ctl };
    const char *argv[3] = { svcs[CONSOLE].path };
    int argc = 1;
    if (nolog_console)
        argv[argc++] = "nolog";
    if (quiet_console)
        argv[argc++] = "quiet";
    st = svc_start(CONSOLE, argc, argv, x, nx);
    quiet_console = false;   /* a restarted console draws at once */
    if (st != OK) {
        jam_handle_close(a);
        return st;
    }
    if (cons)
        jam_handle_close(cons);
    cons = a;
    tell_devmgr();   /* a restart: devmgr reconnects its HID drivers */
    return OK;
}

/* The line the boot's first shell prints after a panic (lastboot.c), queued
 * on its init channel before it starts. */
static void queue_banner(handle_t to)
{
    const char *b = lastboot_banner();
    size_t n = strlen(b);
    if (!to || !n)
        return;
    struct { uint32_t kind; char text[INIT_SHELL_NOTE_MAX]; } m = { INIT_SHELL_NOTE, { 0 } };
    if (n > sizeof(m.text))
        n = sizeof(m.text);
    memcpy(m.text, b, n);
    if (jam_channel_write(to, &m, (uint32_t)(sizeof(m.kind) + n), NULL, 0) != OK)
        printf("init: the shell can't be given the last boot's line\n");
}

status_t terms_start_shell(void)
{
    handle_t c = HANDLE_INVALID, pci = HANDLE_INVALID, p2 = HANDLE_INVALID;
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID, ctl = HANDLE_INVALID;
    /* A SHELL-level console channel: no input sources of its own. */
    status_t st = console_new_client_until(cons, now() + 5 * NS_PER_S, 1, &c);
    if (st != OK)
        return st;
    if (jam_resource_create(shell_root(), RES_PCI, 0, 0, &pci) == OK &&
        jam_handle_replace(pci, RIGHTS_BASIC, &p2) != OK)
        p2 = HANDLE_INVALID;
    if (jam_channel_create(&mine, &theirs) != OK)
        mine = theirs = HANDLE_INVALID;
    queue_banner(mine);
    /* Its control channel of init's: /svc/init, published before the
     * shell is given our namespace (a new one each time: the old one's
     * holders see ERR_PEER_CLOSED). */
    if (ctl_new(CTL_SHELL, port, KEY_CTL + CTL_SHELL, &ctl) == OK) {
        services_publish(SVC_INIT, ctl, false);
        jam_handle_close(ctl);
    }
    struct spawn_handle x[] = {
        { SR_CONSOLE, c },
        { SR_RESOURCE, services_root_with(RIGHTS_BASIC | SHELL_ROOT) },
        { SR_USER + 1, p2 },
        { SR_USER + 2, theirs },
    };
    /* Leave out the ones we don't have. */
    struct spawn_handle y[4];
    unsigned n = 0;
    for (unsigned k = 0; k < 4; k++)
        if (x[k].h)
            y[n++] = x[k];
    /* The boot's first shell gets the boot word's command (shell_first_arg);
     * one init restarts later is an ordinary shell. */
    const char *argv[] = { svcs[SHELL].path, first_arg };
    st = svc_start(SHELL, first_arg ? 2 : 1, argv, y, n);
    first_arg = NULL;
    if (st != OK) {
        if (mine)
            jam_handle_close(mine);
        return st;
    }
    if (to_shell)
        jam_handle_close(to_shell);
    to_shell = mine;
    return OK;
}

void terms_closed(unsigned i)
{
    if (i == CONSOLE && cons) {
        jam_handle_close(cons);   /* the shell and serialin see PEER_CLOSED */
        cons = HANDLE_INVALID;
    }
    if (i == SHELL && to_shell) {
        jam_handle_close(to_shell);
        to_shell = HANDLE_INVALID;
    }
}

void terms_init(handle_t loop_port, bool splash, const char *shell_arg)
{
    port = loop_port;
    first_arg = shell_arg;
    quiet_console = nolog_console = splash;
}
