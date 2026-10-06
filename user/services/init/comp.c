/* init's shell mode: the compositor (docs/G1-PLAN.md), which draws the
 * screen on every plain boot; with the boot word `nocomp` (the boot
 * menu's "Jam OS (no compositor)") it isn't started, and the console owns
 * the screen and the input as it always did.
 *
 *   compositor  bin/compositor, first (before the console), with
 *               root with RIGHT_ROOT_SCREEN (framebuffer_take) and
 *                 RIGHT_ROOT_REBOOT (Ctrl+Alt+Del's reset when init
 *                 can't reboot), none of the console's other powers;
 *               SR_USER + 0  the server end of /svc/wayland's shared
 *                 channel, on which each opener's svc.connect makes its
 *                 own connection. init makes that channel once and keeps
 *                 both ends, so a restarted compositor serves the same
 *                 channel and its clients connect again through it. init
 *                 publishes the client end as /svc/wayland: the shell
 *                 hands it to the programs whose list asks (`svc
 *                 wayland`), the splash's namespace has it, and each
 *                 console gets a duplicate (terms.c: window mode);
 *               SR_USER + 2  the server end of a compctl channel (ADMIN,
 *                 abi/idl/compctl.idl), a new one each start; init keeps
 *                 the client end (admin) and makes the INPUT channels from
 *                 it: one for devmgr's HID drivers (services.c, and
 *                 DEVMGR_SET_CONSOLE after a restart: comp_restarted), and
 *                 serialin's source;
 *               SR_USER + 3  a control channel of init's that answers it
 *                 only `reboot` (Ctrl+Alt+Del), `terminal` (Super+Enter,
 *                 the search box's "Run ... in a terminal") and `launch`
 *                 (the search box's apps: <deskapps.h>'s alone, apps.c),
 *                 ctl.c's CTL_COMPOSITOR;
 *               SR_USER + 5  the server end of /svc/notify's shared
 *                 channel, made once and kept (both ends) as /svc/wayland's:
 *                 its svc.connect makes a compctl NOTIFY channel (post a
 *                 notice, hear its buttons; nothing else). init publishes the
 *                 client end as /svc/notify (the shell's `notify`) and gives
 *                 devmgr and netstack a duplicate each (sticks; the network);
 *               SR_USER + 6  a duplicate of the mixer's desktop channel's
 *                 client end (services.c: audioctl's desk and set_master
 *                 only, the volume popover);
 *               SR_USER + 7  a duplicate of netstack's read-only control
 *                 channel's client end (net.c: netctl's info, stats, device
 *                 and summary only, the network popover);
 *               arguments `layout=floating|tiling` (/data/etc/settings'
 *                 display.layout; tiling, the default, without /data,
 *                 which comes after the first start: comp_settings then
 *                 sends the saved one, compctl.set_layout), `hz=<n>` (display.hz,
 *                 if it is set: how often it paints at most) and, at its
 *                 first start on a boot that plays the splash, `splash`:
 *                 it shows only the splash's background until the
 *                 splash's window maps (5 s at most), so the desktop never
 *                 shows before the splash. A restart later never waits.
 *               Like the console it is started again however often it ends
 *               (shell.c, never_given_up): the terminals have no screen
 *               without it.
 *
 * The layout: init keeps a compctl.layout_wait waiting on its channel,
 * for the layout it knows; the compositor answers it when the user
 * switches (Super+T), init saves the new one as display.layout and waits
 * again. A switch before /data is there is saved when /data comes (and the
 * settings' layout is then not applied over it).
 *
 * init's own notices (comp_notice: update.c's "Update written") go on its
 * ADMIN channel, without waiting; a compctl.notify_wait is kept waiting
 * there once one has buttons. A press of the Reboot button of a notice
 * posted with `reboot` is init's to act on: it reboots as initctl.reboot
 * does. The compositor only says which button was pressed: it never
 * reboots for a notice, and no other poster can make init reboot. A new
 * compositor has none of the old one's notices. */
#include <idl/compctl.h>
#include <settings.h>
#include "init.h"

#define CALL_WAIT     NS_PER_S                /* a compctl call: the compositor never blocks */
#define LAYOUT_KEY    "display.layout"        /* the compositor's WM_LAYOUT_SETTING */
#define HZ_KEY        "display.hz"
#define HZ_MAX        1000u                   /* the compositor's limit */
#define LAYOUT_TXID   0x1a70000u              /* our layout_wait's transaction id */
#define NOTE_TXID     0x2b80000u              /* | a count: a notify of ours */
#define NOTE_WAIT     0x2b90000u              /* our notify_wait's */
#define NOTE_COUNT    0xffffu
#define LAYOUT_TILING 1u                      /* compctl's layouts: 0 floating, 1 tiling */

static handle_t wl_srv, wl_cli;   /* /svc/wayland's shared channel (0: no compositor) */
static handle_t admin;            /* the running compositor's compctl channel, ADMIN (0: none) */
static handle_t port;             /* the loop's: admin's answers come as KEY_COMP */
static uint8_t layout = LAYOUT_TILING;   /* the layout as init knows it (compctl's numbering):
                                         * tiling until the settings say otherwise */
static bool unsaved;              /* the user switched it before /data was there */
static bool started_once;         /* a compositor has started this boot */
static handle_t note_srv, note_cli;   /* /svc/notify's shared channel (0: no compositor) */
static uint32_t note_count;       /* our notices' transaction ids */
static uint32_t reboot_txid;      /* the last notice with a Reboot button, until answered */
static uint32_t reboot_id;        /* ... its id (0: none up) */
static bool note_waiting;         /* our notify_wait is out */

bool comp_on(void)
{
    return wl_cli != HANDLE_INVALID;
}

bool comp_up(void)
{
    return admin != HANDLE_INVALID;
}

handle_t comp_wayland(void)
{
    handle_t d = HANDLE_INVALID;
    if (!wl_cli || jam_handle_duplicate(wl_cli, RIGHT_SAME, &d) != OK)
        return HANDLE_INVALID;
    return d;
}

status_t comp_input_channel(handle_t *out)
{
    return admin ? compctl_new_client_until(admin, now() + CALL_WAIT, 1, out) : ERR_BAD_STATE;
}

status_t comp_source(handle_t *out)
{
    return admin ? compctl_connect_input_until(admin, now() + CALL_WAIT, out) : ERR_BAD_STATE;
}

void comp_blank(bool on)
{
    if (!admin)
        return;
    status_t st = compctl_blank_until(admin, now() + CALL_WAIT, on ? 1 : 0);
    if (st != OK)
        printf("init: the compositor didn't %s the screen (%s)\n", on ? "blank" : "unblank",
               status_str(st));
}

static const char *layout_name(uint8_t l)
{
    return l == LAYOUT_TILING ? "tiling" : "floating";
}

/* display.layout from the settings into *out: false if there is none (or no /data). */
static bool saved_layout(uint8_t *out)
{
    char v[SETTINGS_VALUE_MAX];
    if (settings_get(SETTINGS_FILE, LAYOUT_KEY, v, sizeof(v)) != OK)
        return false;
    if (strcmp(v, "floating") && strcmp(v, "tiling")) {
        printf("init: settings: %s = %s is not floating or tiling\n", LAYOUT_KEY, v);
        return false;
    }
    *out = strcmp(v, "tiling") ? 0 : LAYOUT_TILING;
    return true;
}

/* display.hz as "hz=<n>" into buf; false if it isn't set or isn't 1 to HZ_MAX. */
static bool saved_hz(char *buf, size_t cap)
{
    char v[SETTINGS_VALUE_MAX];
    if (settings_get(SETTINGS_FILE, HZ_KEY, v, sizeof(v)) != OK)
        return false;
    unsigned hz = 0;
    const char *p = v;
    while (*p >= '0' && *p <= '9' && hz <= HZ_MAX)
        hz = hz * 10 + (unsigned)(*p++ - '0');
    if (*p || !hz || hz > HZ_MAX) {
        printf("init: settings: %s = %s is not 1 to %u\n", HZ_KEY, v, HZ_MAX);
        return false;
    }
    snprintf(buf, cap, "hz=%u", hz);
    return true;
}

/* Wait for the user's next switch: a layout_wait for the layout we know,
 * its answer on the port. */
static void wait_layout(void)
{
    status_t st = compctl_layout_wait_send(admin, LAYOUT_TXID, layout);
    if (st != OK)
        printf("init: the compositor's layout switches are not saved (%s)\n", status_str(st));
}

/* The layout we know into the settings, or kept for /data (unsaved). */
static void write_layout(void)
{
    status_t st = settings_set(SETTINGS_FILE, LAYOUT_KEY, layout_name(layout));
    unsaved = st != OK;
    printf("init: the window layout is %s%s%s\n", layout_name(layout),
           unsaved ? ": not saved yet, " : " (saved)", unsaved ? status_str(st) : "");
}

/* The layout is l now (the user's switch). */
static void save_layout(uint8_t l)
{
    if (l == layout)
        return;   /* ours, echoed (comp_settings' set_layout) */
    layout = l;
    write_layout();
}

handle_t comp_notify_client(void)
{
    handle_t d = HANDLE_INVALID;
    if (!note_cli || jam_handle_duplicate(note_cli, RIGHT_SAME, &d) != OK)
        return HANDLE_INVALID;
    return d;
}

void comp_notice(const char *title, const char *body, char icon, uint8_t tint,
                 const char *const *buttons, bool reboot)
{
    if (!admin)
        return;   /* no compositor (or none yet): the log has it */
    uint8_t t[64] = { 0 }, b[96] = { 0 }, btn[72] = { 0 };
    snprintf((char *)t, sizeof(t), "%s", title);
    snprintf((char *)b, sizeof(b), "%s", body ? body : "");
    for (unsigned i = 0; buttons && i < 3 && buttons[i]; i++)
        snprintf((char *)btn + 24 * i, 24, "%s", buttons[i]);
    note_count = (note_count + 1) & NOTE_COUNT;
    uint32_t txid = NOTE_TXID | note_count;
    status_t st = compctl_notify_send(admin, txid, t, b, (uint8_t)icon, tint, btn);
    if (st != OK) {
        printf("init: the notice \"%s\" isn't shown (%s)\n", title, status_str(st));
        return;
    }
    if (reboot)
        reboot_txid = txid;
    if (btn[0] && !note_waiting)
        note_waiting = compctl_notify_wait_send(admin, NOTE_WAIT) == OK;
}

/* A press of one of our notices' buttons: the Reboot one reboots. */
static void note_pressed(uint32_t id, uint8_t button)
{
    if (!id || id != reboot_id || button != 0)
        return;   /* Later, or a notice gone */
    reboot_id = 0;
    printf("init: the notice's Reboot was pressed: rebooting\n");
    (void)init_reboot_kexec();   /* comes back only if it failed, having said why */
    printf("init: rebooting through the firmware instead\n");
    (void)init_reboot_firmware();
}

/* An answer of the compositor's to a notice call of ours. */
static void note_answer(const uint8_t *rep, struct idl_msg *m)
{
    if (m->txid == NOTE_WAIT) {
        uint32_t id = 0;
        uint8_t button = 0;
        status_t st = compctl_notify_wait_result(rep, m, &id, &button);
        note_waiting = false;
        if (st == OK)
            note_waiting = compctl_notify_wait_send(admin, NOTE_WAIT) == OK;
        else
            printf("init: the compositor's notify_wait: %s\n", status_str(st));
        if (st == OK)
            note_pressed(id, button);
        return;
    }
    uint32_t id = 0;
    status_t st = compctl_notify_result(rep, m, &id);
    if (st != OK)
        printf("init: a notice isn't shown (%s)\n", status_str(st));
    else if (m->txid == reboot_txid)
        reboot_id = id;
}

void comp_event(void)
{
    for (unsigned n = 0; admin && n < 8; n++) {   /* bounded: a few calls of ours wait */
        uint8_t rep[COMPCTL_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(admin, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK && st != ERR_INTERNAL) {   /* the compositor is gone: comp_closed */
            (void)jam_port_unbind(port, admin, KEY_COMP);
            return;
        }
        if (m.txid == NOTE_WAIT || (m.txid & ~NOTE_COUNT) == NOTE_TXID) {
            note_answer(rep, &m);
            continue;
        }
        if (m.txid != LAYOUT_TXID) {
            idl_msg_drop(&m);   /* a late answer to a call that gave up */
            continue;
        }
        uint8_t now_l = 0;
        st = compctl_layout_wait_result(rep, &m, &now_l);
        if (st == OK && now_l <= LAYOUT_TILING)
            save_layout(now_l);
        else
            printf("init: the compositor's layout_wait: %s\n", status_str(st));
        if (st == OK)
            wait_layout();
    }
}

void comp_settings(void)
{
    if (!comp_on())
        return;
    uint8_t l;
    if (unsaved) {   /* the user's switch, made before /data: it wins */
        write_layout();
        return;
    }
    if (!saved_layout(&l) || l == layout)
        return;
    layout = l;
    status_t st = admin ? compctl_set_layout_until(admin, now() + CALL_WAIT, l) : OK;
    printf("init: the window layout: %s, as the settings say%s%s\n", layout_name(l),
           st == OK ? "" : ": not applied, ", st == OK ? "" : status_str(st));
}

/* A compositor's handles beyond the root and /svc/wayland (x has room for
 * 5): its ADMIN channel (*mine: init's end), init's control channel, and
 * the desktop's: /svc/notify's server end, the mixer's desktop channel and
 * netstack's read-only one (each left out if there is none). */
static status_t comp_handles(struct spawn_handle *x, unsigned *n, handle_t *mine)
{
    handle_t theirs, ctl, h;
    status_t st = jam_channel_create(mine, &theirs);
    if (st != OK)
        return st;
    x[(*n)++] = (struct spawn_handle){ SR_USER + 2, theirs };
    /* Without it Ctrl+Alt+Del resets the machine without the sync. */
    if (ctl_new(CTL_COMPOSITOR, port, KEY_CTL + CTL_COMPOSITOR, &ctl) == OK)
        x[(*n)++] = (struct spawn_handle){ SR_USER + 3, ctl };
    /* Without these the desktop shows no notices, volume or network. */
    if (note_srv && jam_handle_duplicate(note_srv, RIGHT_SAME, &h) == OK)
        x[(*n)++] = (struct spawn_handle){ SR_USER + 5, h };
    if ((h = services_audio_desk()) != HANDLE_INVALID)
        x[(*n)++] = (struct spawn_handle){ SR_USER + 6, h };
    if ((h = net_info_channel()) != HANDLE_INVALID)
        x[(*n)++] = (struct spawn_handle){ SR_USER + 7, h };
    return OK;
}

status_t comp_start(void)
{
    handle_t srv = HANDLE_INVALID, mine = HANDLE_INVALID;
    if (jam_handle_duplicate(wl_srv, RIGHT_SAME, &srv) != OK)
        return ERR_NO_RESOURCES;
    struct spawn_handle x[7] = {
        { SR_RESOURCE, services_root_with(RIGHTS_BASIC | RIGHT_ROOT_SCREEN | RIGHT_ROOT_REBOOT) },
        { SR_USER + 0, srv },
    };
    unsigned nx = 2;
    status_t st = comp_handles(x, &nx, &mine);
    if (st != OK) {
        jam_handle_close(x[0].h);
        jam_handle_close(srv);
        return st;
    }
    char lw[24], hz[16];
    snprintf(lw, sizeof(lw), "layout=%s", layout_name(layout));
    const char *argv[4] = { svcs[COMPOSITOR].path, lw };
    unsigned argc = 2;
    if (saved_hz(hz, sizeof(hz)))
        argv[argc++] = hz;
    if (!started_once && !splash_played())
        argv[argc++] = "splash";   /* the splash is coming: nothing before it */
    st = svc_start(COMPOSITOR, (int)argc, argv, x, nx);
    if (st != OK) {
        jam_handle_close(mine);
        return st;
    }
    admin = mine;
    note_waiting = false;   /* the old compositor's notices went with it */
    reboot_id = reboot_txid = 0;
    if (jam_port_bind(port, admin, KEY_COMP, SIG_READABLE | SIG_PEER_CLOSED,
                      PORT_BIND_PERSISTENT) == OK)
        wait_layout();
    if (started_once)
        comp_restarted();   /* devmgr's HID drivers connect to this one */
    started_once = true;
    return OK;
}

void comp_closed(void)
{
    if (!admin)
        return;
    (void)jam_port_unbind(port, admin, KEY_COMP);
    jam_handle_close(admin);   /* its INPUT channels went with it */
    admin = HANDLE_INVALID;
}

void comp_init(handle_t loop_port, bool on)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    port = loop_port;
    if (on && (bootfs_default(&fs) != OK ||
               bootfs_lookup(fs, svcs[COMPOSITOR].path, &data, &size) != OK)) {
        printf("init: no %s in bootfs: the console draws the screen\n", svcs[COMPOSITOR].path);
        on = false;
    }
    if (!on || jam_channel_create(&wl_cli, &wl_srv) != OK) {
        wl_cli = wl_srv = HANDLE_INVALID;
        svcs[COMPOSITOR].given_up = true;   /* not on this boot */
        if (on)
            printf("init: no channel for /svc/wayland: the console draws the screen\n");
        return;
    }
    services_publish(SVC_WAYLAND, wl_cli, true);   /* a connection per opener (svc.connect) */
    if (jam_channel_create(&note_cli, &note_srv) != OK)
        note_cli = note_srv = HANDLE_INVALID;   /* no notices: the log has them */
    else
        services_publish(SVC_NOTIFY, note_cli, true);   /* a NOTIFY channel per opener */
    printf("init: the compositor draws the screen: each terminal is a window\n");
}
