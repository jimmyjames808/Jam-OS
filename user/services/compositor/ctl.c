/* compctl's channels (abi/idl/compctl.idl) and Ctrl+Alt+Del's request to
 * init (seat.h).
 *
 * The compositor starts with init's ADMIN channel (SR_USER + 2; a test
 * may start it without one). new_client makes narrower ones (INPUT: for
 * devmgr and serialin, connect_input only), at most CTL_MAX at once, each
 * with its level fixed when it is made. Every channel is served by the
 * loop with a budget, as an input source is; one whose peer closes is
 * dropped.
 *
 * Ctrl+Alt+Del: the compositor asks init to reboot, as the console did:
 * initctl.reboot written on init's control channel (SR_USER + 3, reboot
 * only) without waiting for the answer (init syncs /data and kexecs; it
 * answers only if that failed). From then on the screen is blank and every
 * key and mouse report is dropped, while the loop keeps serving the
 * sources so their drivers never wait on it while init stops them. If init
 * says it failed, is missing, or is still at it after REBOOT_WAIT, the
 * compositor resets the machine itself if its root resource allows it
 * (RIGHT_ROOT_REBOOT); if not, it says so and takes keys again. */
#include <idl/compctl.h>
#include <idl/initctl.h>
#include "seat.h"

#define CTL_MAX      8u                    /* compctl channels at once */
#define CTL_ROLE     2                     /* SR_USER + this: init's ADMIN channel */
#define INITCTL_ROLE 3                     /* SR_USER + this: init's control channel */
#define REBOOT_WAIT  (60 * NS_PER_S)       /* init's kexec: sync (2 s) + drivers stopped (30 s) */
#define REBOOT_TXID  0x0cad0002u           /* our reboot request's transaction id */

enum { L_ADMIN, L_INPUT, L_LAST = L_INPUT };

struct ctl {
    handle_t ch;          /* our end; HANDLE_INVALID (0): a free slot */
    uint8_t  level;       /* L_* */
    bool     ready;       /* a port packet came */
    bool     more;        /* its budget ran out with requests left */
};

static struct ctl ctls[CTL_MAX];
static bool rebooting;            /* Ctrl+Alt+Del asked for it: input is dropped */
static uint64_t reboot_at;        /* when the compositor resets the machine itself */

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

static const struct compctl_ops ctl_ops = {
    .connect_input = op_connect_input, .blank = op_blank, .new_client = op_new_client,
    .stats = op_stats,
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
            *c = (struct ctl){ 0 };
            return;
        }
    }
    c->more = true;
}

static void reboot_answer(void);
static void reboot_due(void);

void ctl_packet(uint64_t key)
{
    if (key == SEAT_KEY_INIT) {
        reboot_answer();
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

/* ---- Ctrl+Alt+Del ------------------------------------------------------------------- */

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
    struct initctl_reboot_req q = { .txid = REBOOT_TXID, .ordinal = INITCTL_REBOOT };
    status_t st = init ? jam_channel_write(init, &q, sizeof(q), NULL, 0) : ERR_NOT_FOUND;
    if (st == OK)
        st = jam_port_bind(comp.port, init, SEAT_KEY_INIT, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_ONCE);
    if (st != OK) {
        printf("compositor: init: %s\n", status_str(st));
        reset_now();
    }
}

static void reboot_answer(void)
{
    handle_t init = startup_handle(SR_USER + INITCTL_ROLE);
    struct initctl_reboot_rep r = { 0 };
    uint32_t n = 0;
    struct channel_read_args a = {
        .h = init, .bytes_cap = sizeof(r), .bytes = (uint64_t)(uintptr_t)&r,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    status_t st = jam_channel_read(&a);
    if (st == OK && (n != sizeof(r) || r.txid != REBOOT_TXID)) {
        /* Not the answer: wait on. */
        (void)jam_port_bind(comp.port, init, SEAT_KEY_INIT, SIG_READABLE | SIG_PEER_CLOSED,
                            PORT_BIND_ONCE);
        return;
    }
    if (!rebooting)
        return;
    printf("compositor: init: %s\n", status_str(st == OK ? r.status : st));
    reset_now();
}

static void reboot_due(void)
{
    if (!rebooting || now() < reboot_at)
        return;
    printf("compositor: init did not reboot in %lu s\n", (unsigned long)(REBOOT_WAIT / NS_PER_S));
    reset_now();
}
