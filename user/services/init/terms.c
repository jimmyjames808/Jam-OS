/* init's shell mode: the terminals, a console and a shell each (shell.c
 * supervises them; services.c starts the other services).
 *
 * The first terminal is the system's: its console and its shell start
 * with the boot and are started again whatever happens, however often
 * (shell.c, never_given_up). With a compositor (comp.c) there can be more,
 * up to TERM_MAX in all: initctl.terminal (the shell's `term`, a
 * console's Super+Enter) opens one, a console in a window of its own with
 * a shell of its own. Each is supervised as the first is: its console or
 * shell ending by a crash or a kill is started again with the same
 * backoff; but one that ends too often is given up on, and an extra
 * terminal closes for good when its window is closed (its console ends
 * with code 0) or its shell ends with `exit` (code 0): the other one is
 * then ended too and the terminal's place is free. A shell that ends
 * because its console did (codes 2 and 3: its console channel closed) is
 * no `exit`.
 *
 *   console   bin/console: root with CONSOLE_ROOT (klog, the screen,
 *             serial output; reboot on Ctrl+Alt+Del), the server end of a
 *             console channel (SR_USER + 0) and a control channel of init's
 *             of its own that answers only `reboot` and `terminal`
 *             (SR_USER + 8, ctl.c: Ctrl+Alt+Del goes through init, which
 *             syncs /data first; with a compositor the compositor keeps
 *             Ctrl+Alt+Del and Super+Enter, which never reach it), and the
 *             log writers' table read-only (CONSOLE_WRITERS_ROLE,
 *             writers.c); init keeps the client end. With the splash (the
 *             argument "quiet") the first draws nothing until the splash
 *             has borrowed the screen and given it back; on a boot with
 *             the splash every console also gets "nolog": the kernel log
 *             off the screen but for its notices. With a compositor each
 *             also gets /svc/wayland's client end (SR_USER + 10: window
 *             mode, the screen is the compositor's, so no RIGHT_ROOT_SCREEN)
 *             and "term=<n>", its terminal's number (an extra terminal's
 *             console shows the log only on request and makes no notices),
 *             and "font=bitmap" when the settings' terminal.font says so
 *             (terms_settings: read when /data comes, which is after the
 *             first console starts, so it is then sent to each running
 *             console as console.set_font)
 *   shell     bin/shell: a SHELL-level console channel (SR_CONSOLE:
 *             console.new_client; no connect_input), root with SHELL_ROOT,
 *             RES_PCI with RIGHTS_BASIC (SR_USER + 1), and init's whole
 *             namespace as it is (SR_NS, followed: every mount, /data's
 *             etc included, and every service: devmgr's channels, init's
 *             control channel /svc/init (ctl.c: kill, sync, reboot,
 *             mount, terminal), the mixer's, the music player's, logd's,
 *             netstack's /svc/net). The first terminal's shell also gets a
 *             channel from init (SR_USER + 2), and /svc/init is made anew
 *             each time it starts (the old one's holders see
 *             ERR_PEER_CLOSED, and the other shells are sent the new one);
 *             on the boot after a panic it waits for logd's answer
 *             (lastboot.c) and finds its one line queued on that channel
 *             (INIT_SHELL_NOTE) when it starts. An extra terminal's shell
 *             gets "term=<n>" (`exit` closes its terminal), and the first
 *             time it starts "run=<line>" when initctl.terminal asked for
 *             a command (the compositor's "Run ... in a terminal": the
 *             shell runs it as if typed at its first prompt; a restarted
 *             shell isn't given it again)
 * Neither gets RIGHT_ROOT_MAP or RIGHT_ROOT_SLICE: they can't reach
 * hardware beyond the calls made for them.
 *
 * A new console gets a new channel, so the shell on it (whose channel
 * then closes) exits and comes back connected to it. Under `nocomp` the
 * first console is also the input's hub: it takes serialin with it, and
 * devmgr gets the new channel (DEVMGR_SET_CONSOLE): its HID drivers,
 * which end when their console goes, come back connected to it. With a
 * compositor the input is the compositor's (comp.c), and a console's
 * keys come to its window. */
#include <devmgr.h>
#include <idl/console.h>
#include <logwriters.h>
#include <os.h>
#include <settings.h>
#include "init.h"

#define FONT_KEY "terminal.font"   /* smooth or bitmap: the terminal windows' font */

/* The root's powers (<jam/abi.h> RIGHT_ROOT_*; neither can map or slice):
 * the console reads the log, draws on the screen (not with a
 * compositor: the screen is its), mirrors it to the serial port's output
 * and reboots on Ctrl+Alt+Del if init doesn't answer; the shell reads the
 * log, the system's figures, the clock, the kernel's debug commands, a
 * reboot when init doesn't answer, and programs from /data (VMEX). */
#define CONSOLE_ROOT (RIGHT_ROOT_KLOG | RIGHT_ROOT_SCREEN | RIGHT_ROOT_SERIAL_OUT | \
                      RIGHT_ROOT_REBOOT)
#define SHELL_ROOT   (RIGHT_ROOT_KLOG | RIGHT_ROOT_SYSINFO | RIGHT_ROOT_CLOCK | RIGHT_ROOT_DEBUG | \
                      RIGHT_ROOT_REBOOT | RIGHT_ROOT_VMEX)
#define WAYLAND_ROLE 10   /* the console's SR_USER + this: /svc/wayland */

enum { TERM_CLOSED, TERM_OPEN, TERM_CLOSING };

struct term {
    uint8_t  state;      /* TERM_*: the first is always OPEN */
    handle_t cons;       /* its console's client end (0: none running) */
    char     cmd[TERM_CMD_MAX];   /* its shell's first line, until it has started ("": none) */
};
static struct term terms[TERM_MAX];

static handle_t port;
static handle_t to_shell;    /* init's end of the first shell's SR_USER + 2 channel */
static bool quiet_console;   /* the next first console starts quiet (the splash's) */
static bool nolog_console;   /* every console keeps the log off the screen (a splash boot) */
/* An argument for the first shell started ("soak=3": run the soak test), or NULL. */
static const char *first_arg;
/* The settings' terminal.font is bitmap (read when /data comes,
 * terms_settings; smooth until then). */
static bool font_bitmap;

int term_of(unsigned i)
{
    if (i == CONSOLE || i == SHELL)
        return 0;
    return i >= TERMS && i < NSVC ? (int)((i - TERMS) / 2 + 1) : -1;
}

static bool is_console(unsigned i)
{
    return i == CONSOLE || (i >= TERMS && i < NSVC && (i - TERMS) % 2 == 0);
}

handle_t shell_console(void)
{
    return terms[0].cons;
}

bool services_console_up(void)
{
    return terms[0].cons != HANDLE_INVALID;
}

bool terms_console_up(unsigned i)
{
    int k = term_of(i);
    return k >= 0 && terms[k].cons != HANDLE_INVALID;
}

bool terms_extra_open(void)
{
    for (unsigned k = 1; k < TERM_MAX; k++)
        if (terms[k].state != TERM_CLOSED)
            return true;
    return false;
}

/* devmgr takes a new console (after the console restarted, under
 * `nocomp`: with a compositor the keys are its): its HID drivers come
 * back connected to it. */
static void tell_devmgr(void)
{
    handle_t c = HANDLE_INVALID;
    if (shell_devmgr() && !comp_on() &&
        jam_handle_duplicate(terms[0].cons, RIGHT_SAME, &c) == OK)
        services_devmgr_input(c);
}

/* Terminal k's console's handles (x has room for 5), how many. */
static unsigned console_handles(unsigned k, handle_t b, struct spawn_handle *x)
{
    handle_t ctl = HANDLE_INVALID, wayland = comp_wayland();
    /* Without it the console still reboots, without the sync. */
    if (ctl_new(CTL_CONSOLE + k, port, KEY_CTL + CTL_CONSOLE + k, &ctl) != OK)
        ctl = HANDLE_INVALID;
    rights_t rights = RIGHTS_BASIC | CONSOLE_ROOT;
    if (wayland)
        rights &= ~RIGHT_ROOT_SCREEN;   /* the compositor's */
    unsigned nx = 0;
    x[nx++] = (struct spawn_handle){ SR_RESOURCE, services_root_with(rights) };
    x[nx++] = (struct spawn_handle){ SR_USER + 0, b };
    handle_t writers = k ? HANDLE_INVALID : writers_for_console();   /* the first's notices */
    if (writers)
        x[nx++] = (struct spawn_handle){ CONSOLE_WRITERS_ROLE, writers };
    if (ctl)
        x[nx++] = (struct spawn_handle){ SR_USER + 8, ctl };
    if (wayland)
        x[nx++] = (struct spawn_handle){ SR_USER + WAYLAND_ROLE, wayland };
    return nx;
}

/* Terminal k's console: its client end kept, devmgr told (the first's). */
static status_t start_console(unsigned k)
{
    handle_t a, b;
    status_t st = jam_channel_create(&a, &b);
    if (st != OK)
        return st;
    struct spawn_handle x[5];
    unsigned nx = console_handles(k, b, x);
    char term[8];
    snprintf(term, sizeof(term), "term=%u", k + 1);
    const char *argv[5] = { svcs[TERM_CONSOLE(k)].path };
    int argc = 1;
    if (nolog_console)
        argv[argc++] = "nolog";
    if (quiet_console && !k && !comp_on())
        argv[argc++] = "quiet";
    if (comp_on())
        argv[argc++] = term;
    if (comp_on() && font_bitmap)
        argv[argc++] = "font=bitmap";
    st = svc_start(TERM_CONSOLE(k), argc, argv, x, nx);
    if (!k)
        quiet_console = false;   /* a restarted console draws at once */
    if (st != OK) {
        jam_handle_close(a);
        return st;
    }
    if (terms[k].cons)
        jam_handle_close(terms[k].cons);
    terms[k].cons = a;
    if (!k)
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

/* The first shell's own: its init channel (*mine: init's end; *theirs:
 * its) with the last boot's line, and a new /svc/init. */
static void first_shell_extras(handle_t *mine, handle_t *theirs)
{
    if (jam_channel_create(mine, theirs) != OK)
        *mine = *theirs = HANDLE_INVALID;
    queue_banner(*mine);
    /* Its control channel of init's: /svc/init, published before the
     * shell is given our namespace (a new one each time: the old one's
     * holders see ERR_PEER_CLOSED). The other terminals' shells follow
     * the namespace: they are sent the new one. */
    handle_t ctl;
    if (ctl_new(CTL_SHELL, port, KEY_CTL + CTL_SHELL, &ctl) == OK) {
        services_publish(SVC_INIT, ctl, false);
        jam_handle_close(ctl);
        if (terms_extra_open())
            tell_mounts();
    }
}

/* Terminal k's shell, on its console. */
static status_t start_shell(unsigned k)
{
    handle_t c = HANDLE_INVALID, pci = HANDLE_INVALID, p2 = HANDLE_INVALID;
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    /* A SHELL-level console channel: no input sources of its own. */
    status_t st = console_new_client_until(terms[k].cons, now() + 5 * NS_PER_S, 1, &c);
    if (st != OK)
        return st;
    if (jam_resource_create(shell_root(), RES_PCI, 0, 0, &pci) == OK &&
        jam_handle_replace(pci, RIGHTS_BASIC, &p2) != OK)
        p2 = HANDLE_INVALID;
    if (!k)
        first_shell_extras(&mine, &theirs);
    struct spawn_handle x[] = {
        { SR_CONSOLE, c },
        { SR_RESOURCE, services_root_with(RIGHTS_BASIC | SHELL_ROOT) },
        { SR_USER + 1, p2 },
        { SR_USER + 2, theirs },
    };
    /* Leave out the ones we don't have. */
    struct spawn_handle y[4];
    unsigned n = 0;
    for (unsigned j = 0; j < 4; j++)
        if (x[j].h)
            y[n++] = x[j];
    /* The boot's first shell gets the boot word's command (shell_first_arg);
     * one init restarts later is an ordinary shell. */
    char term[8], run[4 + TERM_CMD_MAX];
    snprintf(term, sizeof(term), "term=%u", k + 1);
    snprintf(run, sizeof(run), "run=%s", terms[k].cmd);
    const char *argv[] = { svcs[TERM_SHELL(k)].path, k ? term : first_arg, run };
    st = svc_start(TERM_SHELL(k), !argv[1] ? 1 : k && terms[k].cmd[0] ? 3 : 2, argv, y, n);
    if (!k)
        first_arg = NULL;
    if (st == OK)
        terms[k].cmd[0] = '\0';   /* once: a restarted shell is an ordinary one */
    if (st != OK) {
        if (mine)
            jam_handle_close(mine);
        return st;
    }
    if (!k) {
        if (to_shell)
            jam_handle_close(to_shell);
        to_shell = mine;
    }
    return OK;
}

status_t terms_start(unsigned i)
{
    int k = term_of(i);
    if (k < 0)
        return ERR_INVALID_ARGS;
    return is_console(i) ? start_console((unsigned)k) : start_shell((unsigned)k);
}

void terms_closed(unsigned i)
{
    int k = term_of(i);
    if (k < 0)
        return;
    if (is_console(i) && terms[k].cons) {
        jam_handle_close(terms[k].cons);   /* the shell (and serialin) see PEER_CLOSED */
        terms[k].cons = HANDLE_INVALID;
    }
    if (i == SHELL && to_shell) {
        jam_handle_close(to_shell);
        to_shell = HANDLE_INVALID;
    }
}

/* Extra terminal k closes: both stay stopped, and whichever still runs is
 * ended (its end comes to the loop: terms_ended frees the place). */
static void close_term(unsigned k)
{
    unsigned both[2] = { TERM_CONSOLE(k), TERM_SHELL(k) };
    terms[k].state = TERM_CLOSING;
    bool any = false;
    for (unsigned j = 0; j < 2; j++) {
        struct svc *s = &svcs[both[j]];
        s->given_up = true;
        if (s->running) {
            jam_job_kill(s->job);
            any = true;
        }
    }
    if (!any) {
        terms[k].state = TERM_CLOSED;
        printf("init: terminal %u is closed\n", k + 1);
    }
}

bool terms_ended(unsigned i, bool killed, int64_t code)
{
    int k = term_of(i);
    if (k <= 0)
        return false;   /* the first terminal: never closes */
    if (terms[k].state == TERM_CLOSING) {
        close_term((unsigned)k);   /* closed once neither runs */
        return true;
    }
    if (killed || code != 0)
        return false;   /* a crash or a kill: started again, as the first is */
    printf("init: terminal %u: %s\n", k + 1,
           is_console(i) ? "its window was closed" : "its shell ended with exit");
    close_term((unsigned)k);
    return true;
}

void terms_given_up(unsigned i)
{
    int k = term_of(i);
    if (k <= 0 || terms[k].state != TERM_OPEN)
        return;
    printf("init: terminal %u closes: its %s ended too often\n", k + 1,
           is_console(i) ? "console" : "shell");
    close_term((unsigned)k);
}

status_t terms_open(const char *cmd, uint8_t *number)
{
    if (!comp_on())
        return ERR_NOT_SUPPORTED;   /* one screen, one terminal */
    unsigned k = 1;
    while (k < TERM_MAX && terms[k].state != TERM_CLOSED)
        k++;
    if (k == TERM_MAX)
        return ERR_NO_RESOURCES;
    terms[k].state = TERM_OPEN;
    snprintf(terms[k].cmd, sizeof(terms[k].cmd), "%s", cmd);
    unsigned both[2] = { TERM_CONSOLE(k), TERM_SHELL(k) };
    for (unsigned j = 0; j < 2; j++) {
        struct svc *s = &svcs[both[j]];
        s->given_up = false;
        s->ends = 0;
        s->backoff = 0;
        s->next_try = 0;
        s->kill_at = 0;
    }
    if (cmd[0])
        printf("init: terminal %u opens to run \"%s\"\n", k + 1, cmd);
    else
        printf("init: terminal %u opens\n", k + 1);
    *number = (uint8_t)(k + 1);
    return OK;
}

bool terms_named(const char *name, unsigned *i)
{
    bool shell = !strncmp(name, "shell-", 6);
    if (!shell && strncmp(name, "console-", 8))
        return false;
    const char *n = name + (shell ? 6 : 8);
    if (n[0] < '2' || n[0] > '0' + TERM_MAX || n[1])
        return false;
    unsigned k = (unsigned)(n[0] - '1');
    *i = shell ? TERM_SHELL(k) : TERM_CONSOLE(k);
    return true;
}

void terms_settings(void)
{
    char v[SETTINGS_VALUE_MAX];
    bool bitmap = false;
    if (settings_get(SETTINGS_FILE, FONT_KEY, v, sizeof(v)) == OK) {
        if (!strcmp(v, "bitmap"))
            bitmap = true;
        else if (strcmp(v, "smooth"))
            printf("init: settings: %s = %s is not smooth or bitmap: smooth\n", FONT_KEY, v);
    }
    if (bitmap == font_bitmap)
        return;
    font_bitmap = bitmap;   /* for the consoles started from now on (start_console) */
    if (!comp_on())
        return;   /* the full-screen console draws the bitmap always */
    for (unsigned k = 0; k < TERM_MAX; k++) {
        if (!terms[k].cons)
            continue;
        status_t st = console_set_font_until(terms[k].cons, now() + NS_PER_S, bitmap ? 1 : 0);
        if (st != OK)
            printf("init: terminal %u: its font not changed (%s)\n", k + 1, status_str(st));
    }
    printf("init: the terminals' font: %s, as the settings say\n", bitmap ? "bitmap" : "smooth");
}

void terms_init(handle_t loop_port, bool splash, const char *shell_arg)
{
    port = loop_port;
    first_arg = shell_arg;
    quiet_console = nolog_console = splash;
    terms[0].state = TERM_OPEN;
    for (unsigned k = 1; k < TERM_MAX; k++) {
        svcs[TERM_CONSOLE(k)] = (struct svc){ .path = "bin/console", .given_up = true };
        svcs[TERM_SHELL(k)] = (struct svc){ .path = "bin/shell", .given_up = true };
    }
}
