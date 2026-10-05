/* compctl's channels (abi/idl/compctl.idl) and the compositor's requests
 * to init (seat.h).
 *
 * The compositor starts with init's ADMIN channel (SR_USER + 2; a test
 * may start it without one). new_client makes narrower ones (INPUT: for
 * devmgr and serialin, connect_input only), at most CTL_MAX at once, each
 * with its level fixed when it is made. Every channel is served by the
 * loop with a budget, as an input source is; one whose peer closes is
 * dropped.
 *
 * The layout: init starts the compositor with the saved one (`layout=`)
 * and may send it again (set_layout: /data came after the start); it
 * keeps a layout_wait waiting on its channel, which is answered when the
 * layout is no longer the one init knows (the user's Super+T: wm.c calls
 * ctl_layout_changed), and init saves it.
 *
 * init's control channel (SR_USER + 3) answers us `reboot` and `terminal`
 * only. Each request is written without waiting for the answer, and the
 * channel is read whenever it is readable (init_event), so the answers
 * never pile up.
 *   - Ctrl+Alt+Del: initctl.reboot, as the console did (init syncs /data
 *     and kexecs; it answers only if that failed). From then on the screen
 *     is blank and every key and mouse report is dropped, while the loop
 *     keeps serving the sources so their drivers never wait on it while
 *     init stops them. If init says it failed, is missing, or is still at
 *     it after REBOOT_WAIT, the compositor resets the machine itself if
 *     its root resource allows it (RIGHT_ROOT_REBOOT); if not, it says so
 *     and takes keys again.
 *   - Super+Enter: initctl.terminal, another terminal window (one ask at
 *     a time; a refusal is said in the log). */
#include <idl/compctl.h>
#include <idl/initctl.h>
#include "seat.h"

#define CTL_MAX      8u                    /* compctl channels at once */
#define CTL_ROLE     2                     /* SR_USER + this: init's ADMIN channel */
#define INITCTL_ROLE 3                     /* SR_USER + this: init's control channel */
#define REBOOT_WAIT  (60 * NS_PER_S)       /* init's kexec: sync (2 s) + drivers stopped (30 s) */
#define REBOOT_TXID  0x0cad0002u           /* our reboot request's transaction id */
#define TERM_TXID    0x7e570002u           /* our terminal request's */

enum { L_ADMIN, L_INPUT, L_LAST = L_INPUT };

struct ctl {
    handle_t ch;          /* our end; HANDLE_INVALID (0): a free slot */
    uint8_t  level;       /* L_* */
    bool     ready;       /* a port packet came */
    bool     more;        /* its budget ran out with requests left */
    bool     waiting;     /* a layout_wait is kept in wait, for the layout to leave wait_for */
    uint8_t  wait_for;    /* ... the layout its caller knows */
    struct idl_txn wait;
};

static struct ctl ctls[CTL_MAX];
static bool rebooting;            /* Ctrl+Alt+Del asked for it: input is dropped */
static uint64_t reboot_at;        /* when the compositor resets the machine itself */
static bool watching;             /* init's control channel is bound to our port */
static bool term_asked;           /* a terminal request waits for init's answer */

bool ctl_rebooting(void)
{
    return rebooting;
}

/* ch (ours) as a compctl channel of level, served from now on. */
static status_t ctl_add(handle_t ch, uint8_t level)
{
    unsigned i = 0;
    while (i < CTL_MAX && ctls[i].ch != HANDLE_INVALID)
        i++;
    if (i == CTL_MAX)
        return ERR_NO_RESOURCES;
    status_t st = jam_port_bind(comp.port, ch, SEAT_KEY_CTL + i, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st == OK)
        ctls[i] = (struct ctl){ .ch = ch, .level = level, .ready = true };
    return st;
}

status_t ctl_init(void)
{
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    /* Without it Ctrl+Alt+Del resets the machine itself; Super+Enter does nothing. */
    watching = init != HANDLE_INVALID &&
               jam_port_bind(comp.port, init, SEAT_KEY_INIT, SIG_READABLE | SIG_PEER_CLOSED,
                             PORT_BIND_PERSISTENT) == OK;
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
    (void)ctx;   /* every level may */
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
    layout_now(scene.layout);
    return OK;
}

static status_t op_layout_wait(void *ctx, struct idl_txn txn, uint8_t layout, uint8_t *out_now)
{
    struct ctl *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;
    if (c->waiting)
        return ERR_BAD_STATE;
    if (layout != scene.layout) {
        *out_now = (uint8_t)scene.layout;
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

static const struct compctl_ops ctl_ops = {
    .connect_input = op_connect_input, .blank = op_blank, .new_client = op_new_client,
    .stats = op_stats, .set_layout = op_set_layout, .layout_wait = op_layout_wait,
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
            (void)jam_port_unbind(comp.port, c->ch, SEAT_KEY_CTL + (unsigned)(c - ctls));
            jam_handle_close(c->ch);
            *c = (struct ctl){ 0 };   /* a layout_wait kept goes with it */
            return;
        }
    }
    c->more = true;
}

static void init_event(void);
static void reboot_due(void);

void ctl_packet(uint64_t key)
{
    if (key == SEAT_KEY_INIT) {
        init_event();
        return;
    }
    uint64_t i = key - SEAT_KEY_CTL;
    if (key >= SEAT_KEY_CTL && i < CTL_MAX && ctls[i].ch != HANDLE_INVALID)
        ctls[i].ready = true;
}

void ctl_serve(void)
{
    for (unsigned i = 0; i < CTL_MAX; i++)
        if (ctls[i].ch != HANDLE_INVALID && (ctls[i].ready || ctls[i].more))
            serve_ctl(&ctls[i]);
    reboot_due();
}

bool ctl_more(void)
{
    for (unsigned i = 0; i < CTL_MAX; i++)
        if (ctls[i].more)
            return true;
    return false;
}

uint64_t ctl_deadline(void)
{
    return rebooting ? reboot_at : DEADLINE_NEVER;
}

/* ---- init's channel: Ctrl+Alt+Del and Super+Enter ----------------------------------- */

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

void ctl_terminal(void)
{
    if (rebooting || term_asked)
        return;   /* one at a time: a second Super+Enter before the answer is the same ask */
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    status_t st = watching ? initctl_terminal_send(init, TERM_TXID) : ERR_NOT_FOUND;
    if (st == OK)
        term_asked = true;
    else
        printf("compositor: Super+Enter: no new terminal (%s)\n", status_str(st));
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
            watching = term_asked = false;
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
            if (st != OK)
                printf("compositor: Super+Enter: no new terminal (%s)\n", status_str(st));
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
