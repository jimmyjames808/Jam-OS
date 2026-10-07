/* compctl's channels (abi/idl/compctl.idl) and the compositor's requests
 * to init (seat.h).
 *
 * The compositor starts with init's ADMIN channel (SR_USER + 2; a test
 * may start it without one). new_client makes narrower ones (INPUT: for
 * devmgr and serialin, connect_input only; NOTIFY: the notices only), at
 * most CTL_MAX ADMIN and INPUT ones at once and NOTIFY_CHANNELS NOTIFY
 * ones, each with its level fixed when it is made. /svc/notify's shared
 * channel (SR_USER + 5, init's) makes a NOTIFY channel for each svc.connect
 * on it. Every channel is served by the loop with a budget, as an input
 * source is; one whose peer closes is dropped (and its notices with
 * buttons go: deskctl.c has the notices).
 *
 * The layout: init starts the compositor with the saved one (`layout=`)
 * and may send it again (set_layout: /data came after the start); it
 * keeps a layout_wait waiting on its channel, which is answered when the
 * layout is no longer the one init knows (the user's Super+T: wm.c calls
 * ctl_layout_changed), and init saves it.
 *
 * init's control channel (SR_USER + 3) answers us `reboot`, `terminal`
 * and `launch` only. Each request is written without waiting for the
 * answer, and the channel is read whenever it is readable (init_event), so
 * the answers never pile up.
 *   - Ctrl+Alt+Del: initctl.reboot, as the console did (init syncs /data
 *     and kexecs; it answers only if that failed). From then on the screen
 *     is blank and every key and mouse report is dropped, while the loop
 *     keeps serving the sources so their drivers never wait on it while
 *     init stops them. If init says it failed, is missing, or is still at
 *     it after REBOOT_WAIT, the compositor resets the machine itself if
 *     its root resource allows it (RIGHT_ROOT_REBOOT); if not, it says so
 *     and takes keys again.
 *   - Super+Enter: initctl.terminal, another terminal window (one ask at
 *     a time; a refusal is said in the log, and when TERMINALS_MAX are
 *     open, ERR_NO_RESOURCES, on the screen too: desk_terminals_full's
 *     "No more terminals" notice, one while it is up); the search box's
 *     "Run ... in a terminal" the same with its command
 *     (ctl_run_in_terminal).
 *   - the search box's apps (ctl_launch): "terminal" is initctl.terminal,
 *     any other name initctl.launch, which init does only for its fixed
 *     list of desktop apps (<deskapps.h>): the compositor can't start
 *     anything else. The cursor is busy from the ask until the app's first
 *     window maps (desk.c), matched by its title: init answers a terminal's
 *     number ("Terminal 2"), and an app's window has the app's name
 *     ("Jamjar"); a refusal ends the busy cursor at once. */
#include <idl/compctl.h>
#include <idl/initctl.h>
#include <idl/svc.h>
#include "desk.h"
#include "seat.h"

#define CTL_ROLE     2                     /* SR_USER + this: init's ADMIN channel */
#define INITCTL_ROLE 3                     /* SR_USER + this: init's control channel */
#define NOTIFY_ROLE  5                     /* SR_USER + this: /svc/notify's shared channel */
#define REBOOT_WAIT  (60 * NS_PER_S)       /* init's kexec: sync (2 s) + drivers stopped (30 s) */
#define REBOOT_TXID  0x0cad0002u           /* our reboot request's transaction id */
#define TERM_TXID    0x7e570002u           /* our terminal request's */
#define LAUNCH_TXID  0x1a0c0002u           /* our launch request's */

enum { L_ADMIN, L_INPUT, L_NOTIFY, L_LAST = L_NOTIFY };

struct ctl {
    handle_t ch;          /* our end; HANDLE_INVALID (0): a free slot */
    uint32_t gen;         /* which channel the slot holds (deskctl.c's notices) */
    uint8_t  level;       /* L_* */
    bool     ready;       /* a port packet came */
    bool     more;        /* its budget ran out with requests left */
    bool     waiting;     /* a layout_wait is kept in wait, for the layout to leave wait_for */
    uint8_t  wait_for;    /* ... the layout its caller knows */
    struct idl_txn wait;
};

static struct ctl ctls[CTL_ALL];   /* [0, CTL_MAX): ADMIN and INPUT; then NOTIFY */
static uint32_t next_gen;
static handle_t note_svc;         /* /svc/notify's shared channel, our end (0: none) */
static bool note_ready;           /* ... a port packet came */
static bool rebooting;            /* Ctrl+Alt+Del asked for it: input is dropped */
static uint64_t reboot_at;        /* when the compositor resets the machine itself */
static bool watching;             /* init's control channel is bound to our port */
static bool term_asked;           /* a terminal request waits for init's answer */
static bool term_busy;            /* ... and the cursor is busy until its window shows */
static bool launch_asked;         /* a launch request waits for init's answer */
static char launch_app[16];       /* ... for this app */

bool ctl_rebooting(void)
{
    return rebooting;
}

/* ch (ours) as a compctl channel of level, served from now on: a slot of
 * its level's pool. */
static status_t ctl_add(handle_t ch, uint8_t level)
{
    unsigned i = level == L_NOTIFY ? CTL_MAX : 0, end = level == L_NOTIFY ? CTL_ALL : CTL_MAX;
    while (i < end && ctls[i].ch != HANDLE_INVALID)
        i++;
    if (i == end)
        return ERR_NO_RESOURCES;
    status_t st = jam_port_bind(comp.port, ch, SEAT_KEY_CTL + i, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st == OK)
        ctls[i] = (struct ctl){ .ch = ch, .gen = ++next_gen, .level = level, .ready = true };
    return st;
}

status_t ctl_init(void)
{
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    /* Without it Ctrl+Alt+Del resets the machine itself; Super+Enter does nothing. */
    watching = init != HANDLE_INVALID &&
               jam_port_bind(comp.port, init, SEAT_KEY_INIT, SIG_READABLE | SIG_PEER_CLOSED,
                             PORT_BIND_PERSISTENT) == OK;
    /* Without it no service posts notices (they still say them in the log). */
    note_svc = startup_handle(SR_USER + NOTIFY_ROLE);
    if (note_svc && jam_port_bind(comp.port, note_svc, SEAT_KEY_NOTE,
                                  SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT) != OK)
        note_svc = HANDLE_INVALID;
    note_ready = note_svc != HANDLE_INVALID;   /* connects may be queued from before us */
    deskctl_init();
    handle_t ch = startup_handle(SR_USER + CTL_ROLE);
    return ch == HANDLE_INVALID ? OK : ctl_add(ch, L_ADMIN);
}

/* ---- the methods -------------------------------------------------------------------- */

/* The screen blank (the background only) or drawn again: all of it to
 * compose. */
static void blank(bool on)
{
    comp.blanked = on;
    scene_damage(box_make(0, 0, scene.width, scene.height));
}

static status_t op_connect_input(void *ctx, handle_t *out)
{
    const struct ctl *c = ctx;
    if (c->level != L_ADMIN && c->level != L_INPUT)
        return ERR_ACCESS_DENIED;
    return sources_connect(out);
}

static status_t op_blank(void *ctx, uint8_t on)
{
    const struct ctl *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;
    if (on > 1)
        return ERR_INVALID_ARGS;
    if (comp.blanked != (on == 1))
        blank(on == 1);
    return OK;
}

static status_t op_new_client(void *ctx, uint8_t level, handle_t *out)
{
    const struct ctl *c = ctx;
    if (level > L_LAST)
        return ERR_INVALID_ARGS;
    if (c->level != L_ADMIN || level <= c->level)
        return ERR_ACCESS_DENIED;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = ctl_add(mine, level);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    *out = theirs;
    return OK;
}

/* The live clients, their surfaces. */
static void count_clients(uint32_t *clients, uint32_t *surfaces)
{
    *clients = *surfaces = 0;
    for (unsigned i = 0; i < COMP_CLIENTS_MAX; i++) {
        const struct comp_client *cl = conn_client_at(i);
        if (cl) {
            (*clients)++;
            *surfaces += cl->nsurfaces;
        }
    }
}

static status_t op_stats(void *ctx, uint64_t *paints, uint64_t *painted_px,
                         uint64_t *last_paint_ns, uint64_t *worst_paint_ns, uint32_t *clients,
                         uint32_t *surfaces, uint32_t *windows, uint64_t *connected,
                         uint64_t *refused, uint64_t *gone_closed, uint64_t *gone_protocol,
                         uint64_t *gone_slow, uint32_t *nsources, uint64_t *keys,
                         uint64_t *reserved_keys)
{
    const struct ctl *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;
    const struct comp_stats *s = &comp.stats;
    *paints = s->paints;
    *painted_px = s->painted_px;
    *last_paint_ns = s->last_paint_ns;
    *worst_paint_ns = s->worst_paint_ns;
    count_clients(clients, surfaces);
    *windows = scene.nwindows;
    *connected = s->clients;
    *refused = s->refused;
    *gone_closed = s->gone[COMP_GONE_CLOSED];
    *gone_protocol = s->gone[COMP_GONE_PROTOCOL];
    *gone_slow = s->gone[COMP_GONE_SLOW];
    *nsources = sources_count();
    *keys = seat_keys;
    *reserved_keys = seat_reserved;
    return OK;
}

/* The layout is l now: every layout_wait kept for another one is answered. */
static void layout_now(enum comp_layout l)
{
    for (unsigned i = 0; i < CTL_MAX; i++) {
        struct ctl *c = &ctls[i];
        if (c->ch == HANDLE_INVALID || !c->waiting || c->wait_for == l)
            continue;
        c->waiting = false;
        /* A failed write: its caller is gone, and the channel goes with it. */
        (void)compctl_reply_layout_wait(c->wait, OK, (uint8_t)l);
    }
}

static status_t op_set_layout(void *ctx, uint8_t layout)
{
    const struct ctl *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;
    if (layout != COMP_FLOATING && layout != COMP_TILING)
        return ERR_INVALID_ARGS;
    wm_set_layout((enum comp_layout)layout);
    layout_now(screens_default());
    return OK;
}

static status_t op_layout_wait(void *ctx, struct idl_txn txn, uint8_t layout, uint8_t *out_now)
{
    struct ctl *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;
    if (c->waiting)
        return ERR_BAD_STATE;
    if (layout != screens_default()) {   /* (the screens' choice, not the one shown: a
                                          * restored screen of another layout isn't a switch) */
        *out_now = (uint8_t)screens_default();
        return OK;
    }
    c->waiting = true;
    c->wait_for = layout;
    c->wait = txn;
    return IDL_LATER;
}

/* The user switched the layout (wm.c's Super+T): init hears it. */
void ctl_layout_changed(enum comp_layout layout)
{
    layout_now(layout);
}

/* ---- the notices (deskctl.c): NOTIFY and ADMIN ------------------------------------------ */

static bool may_notify(const struct ctl *c)
{
    return c->level == L_NOTIFY || c->level == L_ADMIN;
}

static unsigned slot_of(const struct ctl *c)
{
    return (unsigned)(c - ctls);
}

static status_t op_notify(void *ctx, const uint8_t title[64], const uint8_t body[96],
                          uint8_t icon, uint8_t tint, const uint8_t buttons[72], uint32_t *out_id)
{
    const struct ctl *c = ctx;
    if (!may_notify(c))
        return ERR_ACCESS_DENIED;
    return deskctl_notify(slot_of(c), c->gen, title, body, icon, tint, buttons, out_id);
}

static status_t op_notify_wait(void *ctx, struct idl_txn txn, uint32_t *out_id,
                               uint8_t *out_button)
{
    const struct ctl *c = ctx;
    if (!may_notify(c))
        return ERR_ACCESS_DENIED;
    return deskctl_notify_wait(slot_of(c), c->gen, txn, out_id, out_button);
}

static status_t op_withdraw(void *ctx, uint32_t id)
{
    const struct ctl *c = ctx;
    if (!may_notify(c))
        return ERR_ACCESS_DENIED;
    return deskctl_withdraw(slot_of(c), c->gen, id);
}

static const struct compctl_ops ctl_ops = {
    .connect_input = op_connect_input, .blank = op_blank, .new_client = op_new_client,
    .stats = op_stats, .set_layout = op_set_layout, .layout_wait = op_layout_wait,
    .notify = op_notify, .notify_wait = op_notify_wait, .withdraw = op_withdraw,
};

/* ---- serving ------------------------------------------------------------------------ */

static void serve_ctl(struct ctl *c)
{
    c->more = false;
    for (unsigned n = 0; n < SEAT_BUDGET; n++) {
        status_t st = compctl_serve_one(c->ch, &ctl_ops, c);
        if (st == ERR_SHOULD_WAIT) {
            c->ready = false;
            return;
        }
        if (st != OK) {   /* ERR_PEER_CLOSED: whoever held it is gone */
            (void)jam_port_unbind(comp.port, c->ch, SEAT_KEY_CTL + slot_of(c));
            jam_handle_close(c->ch);
            deskctl_closed(slot_of(c), c->gen);   /* its cards with buttons, its presses */
            *c = (struct ctl){ 0 };   /* a layout_wait kept goes with it */
            return;
        }
    }
    c->more = true;
}

static void init_event(void);
static void reboot_due(void);

/* /svc/notify's svc.connect: a new NOTIFY channel. */
static status_t note_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = ctl_add(mine, L_NOTIFY);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    *out = theirs;
    return OK;
}

/* The shared channel speaks only svc.connect. */
static uint32_t svc_only(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                         uint32_t *rhn)
{
    static const struct svc_ops none;
    return svc_dispatch(&none, ctx, req, n, rep, rhs, rhn);
}

static void serve_note_svc(void)
{
    note_ready = false;
    for (unsigned n = 0; n < SEAT_BUDGET; n++) {
        status_t st = svc_serve_request(note_svc, svc_only, note_connect, NULL);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK) {   /* init is gone: no more notices from services */
            (void)jam_port_unbind(comp.port, note_svc, SEAT_KEY_NOTE);
            jam_handle_close(note_svc);
            note_svc = HANDLE_INVALID;
            return;
        }
    }
    note_ready = true;   /* its budget is spent: more may be queued */
}

void ctl_packet(uint64_t key)
{
    if (key == SEAT_KEY_INIT) {
        init_event();
        return;
    }
    if (key == SEAT_KEY_NOTE) {
        note_ready = note_svc != HANDLE_INVALID;
        return;
    }
    if (key >= SEAT_KEY_DESK) {
        deskctl_packet(key);
        return;
    }
    uint64_t i = key - SEAT_KEY_CTL;
    if (key >= SEAT_KEY_CTL && i < CTL_ALL && ctls[i].ch != HANDLE_INVALID)
        ctls[i].ready = true;
}

void ctl_serve(void)
{
    for (unsigned i = 0; i < CTL_ALL; i++)
        if (ctls[i].ch != HANDLE_INVALID && (ctls[i].ready || ctls[i].more))
            serve_ctl(&ctls[i]);
    if (note_ready)
        serve_note_svc();
    reboot_due();
}

bool ctl_more(void)
{
    for (unsigned i = 0; i < CTL_ALL; i++)
        if (ctls[i].more)
            return true;
    return note_ready;
}

uint64_t ctl_deadline(void)
{
    uint64_t d = deskctl_deadline();
    return rebooting && reboot_at < d ? reboot_at : d;
}

/* ---- init's channel: Ctrl+Alt+Del, Super+Enter and the search box -------------------- */

/* init couldn't: reset the machine ourselves, or give up and take keys again. */
static void reset_now(void)
{
    status_t st = jam_reboot(startup_handle(SR_RESOURCE));
    printf("compositor: reboot: %s\n", status_str(st));
    rebooting = false;   /* still here: the machine goes on */
    blank(false);
}

void ctl_reboot(void)
{
    if (rebooting)
        return;
    rebooting = true;
    reboot_at = now() + REBOOT_WAIT;
    printf("compositor: Ctrl+Alt+Del: rebooting\n");
    blank(true);   /* nothing drawn until the next boot's splash */
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    status_t st = watching ? initctl_reboot_send(init, REBOOT_TXID) : ERR_NOT_FOUND;
    if (st != OK) {
        printf("compositor: init: %s\n", status_str(st));
        reset_now();
    }
}

void ctl_terminal_run(const char *cmd, bool busy)
{
    if (rebooting || term_asked) {   /* one at a time: a second ask before the answer is lost */
        if (busy)
            desk_launch_failed();
        return;
    }
    uint8_t line[128] = { 0 };
    snprintf((char *)line, sizeof(line), "%s", cmd);
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    status_t st = watching ? initctl_terminal_send(init, TERM_TXID, line) : ERR_NOT_FOUND;
    if (st == OK) {
        term_asked = true;
        term_busy = busy;
        return;
    }
    printf("compositor: no new terminal (%s)\n", status_str(st));
    if (busy)
        desk_launch_failed();
}

void ctl_terminal(void)
{
    ctl_terminal_run("", false);
}

/* desk.c's hooks: the search box's rows. */
void ctl_launch(const char *app)
{
    if (!strcmp(app, "terminal")) {   /* a terminal is init's `terminal`, not an app */
        ctl_terminal_run("", true);
        return;
    }
    status_t st = ERR_BAD_STATE;   /* one at a time */
    if (!rebooting && !launch_asked) {
        uint8_t name[16] = { 0 };
        snprintf((char *)name, sizeof(name), "%s", app);
        snprintf(launch_app, sizeof(launch_app), "%s", app);
        handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
        st = watching ? initctl_launch_send(init, LAUNCH_TXID, name) : ERR_NOT_FOUND;
        launch_asked = st == OK;
    }
    if (st != OK) {
        printf("compositor: launch %s: %s\n", app, status_str(st));
        desk_launch_failed();
    }
}

void ctl_run_in_terminal(const char *cmd)
{
    ctl_terminal_run(cmd, true);
}

/* init's channel is readable: its answers, by their transaction ids. */
static void init_event(void)
{
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    for (;;) {   /* bounded: init answers only what we asked, two at most */
        uint8_t rep[64];
        struct idl_msg m;
        status_t st = idl_reply_read(init, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK && st != ERR_INTERNAL) {   /* init's end is gone */
            (void)jam_port_unbind(comp.port, init, SEAT_KEY_INIT);
            if (term_busy || launch_asked)
                desk_launch_failed();
            watching = term_asked = term_busy = launch_asked = false;
            if (rebooting) {
                printf("compositor: init: %s\n", status_str(st));
                reset_now();
            }
            return;
        }
        if (m.txid == REBOOT_TXID && rebooting) {
            printf("compositor: init: %s\n", status_str(initctl_reboot_result(rep, &m)));
            reset_now();
        } else if (m.txid == TERM_TXID && term_asked) {
            uint8_t number = 0;
            st = initctl_terminal_result(rep, &m, &number);
            term_asked = false;
            if (st == ERR_NO_RESOURCES)
                (void)desk_terminals_full();   /* all open: the "No more terminals" notice */
            else if (st != OK)
                printf("compositor: no new terminal (%s)\n", status_str(st));
            if (term_busy && st == OK) {   /* its window: "Terminal <n>", the first's
                                            * "Terminal" (console/window.c) */
                char title[16];
                if (number > 1)
                    snprintf(title, sizeof(title), "Terminal %u", (unsigned)number);
                else
                    snprintf(title, sizeof(title), "Terminal");
                desk_launch_awaits(title);
            } else if (term_busy) {
                desk_launch_failed();
            }
            term_busy = false;
        } else if (m.txid == LAUNCH_TXID && launch_asked) {
            uint64_t koid = 0;
            st = initctl_launch_result(rep, &m, &koid);
            launch_asked = false;
            if (st == OK) {
                printf("compositor: launched %s (process %lu)\n", launch_app,
                       (unsigned long)koid);
            } else {
                printf("compositor: launch %s: init says %s\n", launch_app, status_str(st));
                desk_launch_failed();
            }
        } else {
            idl_msg_drop(&m);   /* not an answer we wait for */
        }
    }
}

static void reboot_due(void)
{
    if (!rebooting || now() < reboot_at)
        return;
    printf("compositor: init did not reboot in %lu s\n", (unsigned long)(REBOOT_WAIT / NS_PER_S));
    reset_now();
}
