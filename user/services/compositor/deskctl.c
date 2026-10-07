/* The desktop's plumbing (track D2b, docs/G1-PLAN.md "As built: D2b"):
 * what desk.c's hooks (comp.h) reach through the channels init gives the
 * compositor, each as narrow as its feature:
 *
 *   notices      compctl's notify, notify_wait and withdraw (ctl.c checks
 *                the channel's level: NOTIFY or ADMIN), here: a card per
 *                notice (notify.c), whose button presses go back to the
 *                channel that posted it (notify_wait, answered later, or
 *                kept for the next one). A channel that closes takes its
 *                cards with buttons with it: nobody is left to act on them.
 *                The compositor itself never acts on a button: a Reboot
 *                button is the poster's (init's update notice), who holds
 *                the power to reboot.
 *   volume       SR_USER + 6: the mixer's desktop channel (audioctl.idl):
 *                `desk` (the master volume, what plays, the output's name)
 *                and `set_master` only: no stream, no sound card.
 *   network      SR_USER + 7: netstack's read-only control channel
 *                (netctl.idl): `summary` (link, address, speed, the
 *                card, bytes in and out); the rates are worked out here
 *                from two summaries a second or so apart.
 *
 * The loop never waits on the mixer or netstack (main.c): each request is
 * written with a transaction id of ours and answered on the port
 * (SEAT_KEY_DESK + n); the hooks return what the last answer said and ask
 * again if it is older than ASK_AGAIN (an answer refreshes an open
 * popover). One request of each kind is out at a time; one unanswered
 * for ASK_WAIT is given up on (its answer, if it comes, is dropped by its
 * id). A channel whose peer is gone (init gave up on the service) is
 * closed: the popover then shows what a desktop with nothing behind it
 * does. The volume popover's percent is the master volume on a dB scale:
 * 100% is 0 dB, each percent below 0.6 dB less (1% -59.4 dB), 0% silence. */
#include <idl/audioctl.h>
#include <idl/compctl.h>
#include <idl/netctl.h>
#include "desk.h"
#include "seat.h"

#define MIXER_ROLE   6                    /* SR_USER + this: the mixer's desktop channel */
#define NET_ROLE     7                    /* SR_USER + this: netstack's read-only channel */
#define KEY_MIXER    (SEAT_KEY_DESK + 0)
#define KEY_NET      (SEAT_KEY_DESK + 1)
#define ASK_AGAIN    (500 * NS_PER_MS)    /* an answer this fresh is good enough */
#define ASK_WAIT     (2 * NS_PER_S)       /* an answer this late is given up on */
#define RATE_SPAN    (3 * NS_PER_S)       /* two summaries further apart give no rate */
#define TXID_DESK    0xd5c00000u          /* | a count: audioctl.desk */
#define TXID_MASTER  0xd5d00000u          /* | a count: audioctl.set_master */
#define TXID_NET     0xd5e00000u          /* | a count: netctl.summary */
#define TXID_COUNT   0xffffu
#define PRESSES_MAX  4u                   /* presses kept for a channel not waiting */
#define OWNED_MAX    16u                  /* notices remembered with who posted them */
#define BUTTON_CARDS 3u                   /* cards with buttons a channel may have up */
#define SILENT_CB    (-960)               /* the mixer's silence */
#define CB_PER_PCT   6                    /* centibels a percent below 100% */

/* ---- notices -------------------------------------------------------------------------------- */

/* Each compctl channel's (by ctl.c's slot): its wait and the presses it
 * hasn't taken. */
struct waiter {
    uint32_t gen;                  /* the channel these are for (0: none yet) */
    bool waiting;
    struct idl_txn txn;
    struct { uint32_t id; uint8_t button; } q[PRESSES_MAX];
    unsigned nq;
};
static struct waiter waiters[CTL_ALL];

/* A notice on the screen and who posted it (id 0: a free entry). */
static struct {
    uint32_t id, gen;
    uint8_t slot;
    bool buttons;
} owned[OWNED_MAX];

static struct waiter *waiter(unsigned slot, uint32_t gen)
{
    struct waiter *w = &waiters[slot];
    if (w->gen != gen)
        *w = (struct waiter){ .gen = gen };   /* the slot holds another channel now */
    return w;
}

/* s (cap bytes) ends with a NUL and holds no control character; with
 * need, not empty either. */
static bool text_ok(const uint8_t *s, size_t cap, bool need)
{
    size_t n = strnlen((const char *)s, cap);
    if (n == cap || (need && !n))
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] == 0x7f)
            return false;
    return true;
}

static unsigned button_cards(unsigned slot, uint32_t gen)
{
    unsigned n = 0;
    for (unsigned i = 0; i < OWNED_MAX; i++)
        if (owned[i].id && owned[i].slot == slot && owned[i].gen == gen && owned[i].buttons &&
            notify_live(owned[i].id))
            n++;
    return n;
}

/* Remember id as slot's: in a free entry, or one whose card has gone. */
static void own(uint32_t id, unsigned slot, uint32_t gen, bool buttons)
{
    unsigned at = 0;
    for (unsigned i = 0; i < OWNED_MAX; i++)
        if (!owned[i].id || !notify_live(owned[i].id)) {
            at = i;
            break;
        }
    owned[at].id = id;
    owned[at].gen = gen;
    owned[at].slot = (uint8_t)slot;
    owned[at].buttons = buttons;
}

status_t deskctl_notify(unsigned slot, uint32_t gen, const uint8_t title[64],
                        const uint8_t body[96], uint8_t icon, uint8_t tint,
                        const uint8_t buttons[72], uint32_t *out_id)
{
    static const uint32_t tints[3] = { LOOK_JAM_BLACKCURRANT, LOOK_JAM_RASPBERRY,
                                       LOOK_JAM_APRICOT };
    if (!text_ok(title, 64, true) || !text_ok(body, 96, false) || tint > 2 ||
        (icon && !(icon >= 'A' && icon <= 'Z') && !(icon >= '0' && icon <= '9')))
        return ERR_INVALID_ARGS;
    struct notify_spec n = {
        .title = (const char *)title, .body = (const char *)body, .letter = (char)icon,
        .colour = tints[tint],
    };
    for (unsigned b = 0; b < NOTIFY_BUTTONS_MAX; b++) {
        const uint8_t *label = buttons + 24 * b;
        if (!label[0])
            break;
        if (!text_ok(label, 24, true))
            return ERR_INVALID_ARGS;
        n.buttons[n.nbuttons++] = (const char *)label;
    }
    if (n.nbuttons && button_cards(slot, gen) >= BUTTON_CARDS)
        return ERR_NO_RESOURCES;
    if (!desk_on())
        return ERR_NOT_SUPPORTED;
    uint32_t id = notify_post(&n);
    if (!id)
        return ERR_NOT_SUPPORTED;
    (void)waiter(slot, gen);
    own(id, slot, gen, n.nbuttons != 0);
    printf("compositor: notice %u: %s%s%s\n", id, n.title, n.body[0] ? ": " : "", n.body);
    *out_id = id;
    return OK;
}

status_t deskctl_notify_wait(unsigned slot, uint32_t gen, struct idl_txn txn, uint32_t *out_id,
                             uint8_t *out_button)
{
    struct waiter *w = waiter(slot, gen);
    if (w->waiting)
        return ERR_BAD_STATE;
    if (w->nq) {
        *out_id = w->q[0].id;
        *out_button = w->q[0].button;
        memmove(&w->q[0], &w->q[1], (w->nq - 1) * sizeof(w->q[0]));
        w->nq--;
        return OK;
    }
    w->waiting = true;
    w->txn = txn;
    return IDL_LATER;
}

status_t deskctl_withdraw(unsigned slot, uint32_t gen, uint32_t id)
{
    for (unsigned i = 0; i < OWNED_MAX; i++)
        if (id && owned[i].id == id && owned[i].slot == slot && owned[i].gen == gen &&
            notify_live(id)) {
            owned[i].id = 0;
            notify_withdraw(id);
            return OK;
        }
    return ERR_NOT_FOUND;
}

void deskctl_closed(unsigned slot, uint32_t gen)
{
    for (unsigned i = 0; i < OWNED_MAX; i++)
        if (owned[i].id && owned[i].slot == slot && owned[i].gen == gen) {
            if (owned[i].buttons)
                notify_withdraw(owned[i].id);   /* nobody is left to act on its buttons */
            owned[i].id = 0;
        }
    waiters[slot] = (struct waiter){ 0 };
}

/* desk.c's hook: a card's button was pressed (the card goes). */
void ctl_notify_answered(uint32_t id, uint32_t button)
{
    unsigned i = 0;
    while (i < OWNED_MAX && (!id || owned[i].id != id))
        i++;
    if (i == OWNED_MAX)
        return;   /* its poster is gone */
    owned[i].id = 0;
    struct waiter *w = &waiters[owned[i].slot];
    if (w->gen != owned[i].gen)
        return;   /* its channel closed meanwhile */
    printf("compositor: notice %u: button %u pressed\n", id, button);
    if (w->waiting) {
        w->waiting = false;
        /* A failed write: its caller is gone, and the channel goes with it. */
        (void)compctl_reply_notify_wait(w->txn, OK, id, (uint8_t)button);
        return;
    }
    if (w->nq == PRESSES_MAX) {
        printf("compositor: notice %u's press dropped: its poster takes none\n", w->q[0].id);
        memmove(&w->q[0], &w->q[1], (PRESSES_MAX - 1) * sizeof(w->q[0]));
        w->nq--;
    }
    w->q[w->nq].id = id;
    w->q[w->nq++].button = (uint8_t)button;
}

/* ---- a request out, its answer in ------------------------------------------------------------ */

/* One kind of request on one channel. */
struct ask {
    uint32_t txid;                 /* the one out (0: none) */
    uint64_t sent;                 /* when */
    uint64_t answered;             /* when the last answer came (0: never) */
    uint32_t count;
};

/* May a new request go: none out (or the one out given up on), and the
 * last answer not fresh. */
static bool may_ask(struct ask *a, uint64_t t, bool fresh_ok)
{
    if (a->txid && t < a->sent + ASK_WAIT)
        return false;
    a->txid = 0;
    return fresh_ok || !a->answered || t >= a->answered + ASK_AGAIN;
}

static uint32_t next_txid(struct ask *a, uint32_t base)
{
    a->count = (a->count + 1) & TXID_COUNT;
    return base | a->count;
}

/* ch's peer is gone: no more answers on it. */
static void drop(handle_t *ch, uint64_t key, const char *what)
{
    (void)jam_port_unbind(comp.port, *ch, key);
    jam_handle_close(*ch);
    *ch = HANDLE_INVALID;
    printf("compositor: the %s's channel is gone: the desktop can't show it\n", what);
}

/* ---- the volume: the mixer's desktop channel -------------------------------------------------- */

static struct {
    handle_t ch;                   /* 0: none */
    struct ask desk, master;
    bool known;                    /* an answer came */
    int32_t master_cb;
    bool playing;
    char title[64], output[48];
    bool want;                     /* a set_master to send once the one out is answered */
    int32_t want_cb;
} vol;

static uint32_t pct_of(int32_t cb)
{
    return cb <= -100 * CB_PER_PCT ? 0 : (uint32_t)(100 + cb / CB_PER_PCT);
}

static int32_t cb_of(uint32_t pct)
{
    return pct == 0 ? SILENT_CB : -(int32_t)(100 - (pct > 100 ? 100 : pct)) * CB_PER_PCT;
}

static void ask_volume(bool now_anyway)
{
    uint64_t t = now();
    if (!vol.ch || !may_ask(&vol.desk, t, now_anyway))
        return;
    uint32_t txid = next_txid(&vol.desk, TXID_DESK);
    if (audioctl_desk_send(vol.ch, txid) == OK) {
        vol.desk.txid = txid;
        vol.desk.sent = t;
    }
}

static void send_master(int32_t cb)
{
    uint64_t t = now();
    if (!may_ask(&vol.master, t, true)) {
        vol.want = true;   /* the latest value goes when the one out is answered */
        vol.want_cb = cb;
        return;
    }
    vol.want = false;
    uint32_t txid = next_txid(&vol.master, TXID_MASTER);
    if (audioctl_set_master_send(vol.ch, txid, cb) == OK) {
        vol.master.txid = txid;
        vol.master.sent = t;
    }
}

static void volume_event(void)
{
    for (unsigned n = 0; vol.ch && n < 16; n++) {   /* bounded: two kinds out at most */
        uint8_t rep[AUDIOCTL_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(vol.ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK && st != ERR_INTERNAL) {
            drop(&vol.ch, KEY_MIXER, "mixer");
            vol.known = false;
            return;
        }
        if (m.txid && m.txid == vol.desk.txid) {
            uint8_t playing = 0, title[64], output[48];
            int32_t master = 0;
            st = audioctl_desk_result(rep, &m, &master, &playing, title, output);
            vol.desk.txid = 0;
            vol.desk.answered = now();
            if (st != OK)
                continue;
            vol.known = true;
            if (!vol.master.txid && !vol.want)
                vol.master_cb = master;   /* else ours is newer */
            vol.playing = playing == 1;
            snprintf(vol.title, sizeof(vol.title), "%.*s", 63, (const char *)title);
            snprintf(vol.output, sizeof(vol.output), "%.*s", 47, (const char *)output);
            if (pop.kind == POP_VOLUME && !pop.dragging)
                pop_refresh(now());
        } else if (m.txid && m.txid == vol.master.txid) {
            int32_t set = 0;
            st = audioctl_set_master_result(rep, &m, &set);
            vol.master.txid = 0;
            vol.master.answered = now();
            if (st != OK)
                printf("compositor: the mixer didn't take the volume (%s)\n", status_str(st));
            if (vol.want)
                send_master(vol.want_cb);
        } else {
            idl_msg_drop(&m);   /* given up on, or not ours */
        }
    }
}

bool ctl_volume(uint32_t *percent)
{
    ask_volume(false);
    if (!vol.known)
        return false;
    *percent = pct_of(vol.master_cb);
    return true;
}

void ctl_set_volume(uint32_t percent)
{
    if (!vol.ch)
        return;
    vol.master_cb = cb_of(percent);
    send_master(vol.master_cb);
}

bool ctl_audio_output(char *buf, size_t n)
{
    ask_volume(false);
    if (!vol.known || !vol.output[0])
        return false;
    snprintf(buf, n, "%s", vol.output);
    return true;
}

bool ctl_now_playing(char *buf, size_t n)
{
    ask_volume(false);
    if (!vol.known || !vol.playing || !vol.title[0])
        return false;
    snprintf(buf, n, "%s", vol.title);
    return true;
}

/* ---- the network: netstack's read-only channel ------------------------------------------------- */

static struct {
    handle_t ch;                   /* 0: none */
    struct ask sum;
    bool known;
    struct desk_net now;
    struct desk_net said;          /* what the log was last told */
    uint64_t rx, tx, at;           /* the last summary's counts, and when it came */
} net;

static void ask_net(bool now_anyway)
{
    uint64_t t = now();
    if (!net.ch || !may_ask(&net.sum, t, now_anyway))
        return;
    uint32_t txid = next_txid(&net.sum, TXID_NET);
    if (netctl_summary_send(net.ch, txid) == OK) {
        net.sum.txid = txid;
        net.sum.sent = t;
    }
}

static uint64_t rate(uint64_t now_count, uint64_t then_count, uint64_t dt)
{
    if (now_count < then_count || !dt)
        return 0;   /* netstack started again: its counts did too */
    return (now_count - then_count) * NS_PER_S / dt;
}

/* An answer to our summary into net.now. */
static void take_summary(uint8_t link, uint32_t addr, uint32_t speed, uint64_t rx,
                         uint64_t tx, const uint8_t chip[16])
{
    uint64_t t = now(), dt = t - net.at;
    struct desk_net *d = &net.now;
    bool fresh = net.at && dt <= RATE_SPAN;
    d->up = link == 1 && addr != 0;
    if (addr)
        snprintf(d->address, sizeof(d->address), "%u.%u.%u.%u", addr >> 24,
                 (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff);
    else
        d->address[0] = '\0';
    snprintf(d->nic, sizeof(d->nic), "%.*s", 15, (const char *)chip);
    d->mbps = link == 1 ? speed : 0;
    if (!net.known || d->up != net.said.up || strcmp(d->address, net.said.address) ||
        strcmp(d->nic, net.said.nic) || d->mbps != net.said.mbps) {
        printf("compositor: network: %s, address %s, %s, %u Mb/s\n",
               d->up ? "connected" : "not connected", d->address[0] ? d->address : "none",
               d->nic[0] ? d->nic : "no card", d->mbps);
        net.said = *d;   /* said once a change: the rates are not news */
    }
    d->rx_bps = fresh ? rate(rx, net.rx, dt) : 0;
    d->tx_bps = fresh ? rate(tx, net.tx, dt) : 0;
    net.rx = rx;
    net.tx = tx;
    net.at = t;
    net.known = true;
}

static void net_event(void)
{
    for (unsigned n = 0; net.ch && n < 16; n++) {
        uint8_t rep[NETCTL_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(net.ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK && st != ERR_INTERNAL) {
            drop(&net.ch, KEY_NET, "network");
            net.known = false;
            return;
        }
        if (!m.txid || m.txid != net.sum.txid) {
            idl_msg_drop(&m);
            continue;
        }
        uint8_t link = 0, chip[16];
        uint32_t addr = 0, mask = 0, speed = 0;
        uint64_t rx = 0, tx = 0;
        st = netctl_summary_result(rep, &m, &link, &addr, &mask, &speed, &rx, &tx, chip);
        net.sum.txid = 0;
        net.sum.answered = now();
        if (st != OK)
            continue;
        take_summary(link, addr, speed, rx, tx, chip);
        if (pop.kind == POP_NETWORK)
            pop_refresh(now());
    }
}

bool ctl_network(struct desk_net *out)
{
    ask_net(false);
    if (!net.known)
        return false;
    *out = net.now;
    return true;
}

/* ---- the loop's side --------------------------------------------------------------------------- */

static handle_t watch(unsigned role, uint64_t key)
{
    handle_t ch = startup_handle(SR_USER + role);
    if (ch && jam_port_bind(comp.port, ch, key, SIG_READABLE | SIG_PEER_CLOSED,
                            PORT_BIND_PERSISTENT) != OK)
        return HANDLE_INVALID;
    return ch;
}

void deskctl_init(void)
{
    vol.ch = watch(MIXER_ROLE, KEY_MIXER);
    net.ch = watch(NET_ROLE, KEY_NET);
    ask_volume(true);   /* so the popovers open with something to show */
    ask_net(true);
}

void deskctl_packet(uint64_t key)
{
    if (key == KEY_MIXER)
        volume_event();
    else if (key == KEY_NET)
        net_event();
}

uint64_t deskctl_deadline(void)
{
    return DEADLINE_NEVER;   /* nothing is due: a request given up on is asked again on demand */
}
