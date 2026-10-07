/* console: keys, the focus stack of open_keys channels, and the input
 * sources (console.h).
 *
 * The keys go to the newest open_keys channel (the focus); keys typed
 * before anyone listened wait for the first. Only ADMIN clients connect
 * input sources, so Ctrl+Alt+Del and every key come from devmgr's drivers
 * or init's serial source. A focus a PROGRAM opened can't hold the keys
 * hostage: Ctrl+C goes to it and to the SHELL/ADMIN focus below it (the
 * shell that ran it kills it).
 *
 * The mouse goes to the focus too, but only to one that asked: a client
 * writes a struct input_want on its key channel, and mouse reports then
 * arrive there as struct input_mouse_event, a message of another size
 * than a key (<jam/abi.h> has the wire format). A client that never asked
 * never gets one, so the shell's line editor reads nothing but keys. With
 * no such focus the wheel scrolls the console back while the console has
 * the screen, and the rest of the report is dropped. */
#include <deskapps.h>
#include <idl/initctl.h>
#include <termkeys.h>
#include "console.h"

#define INITCTL_ROLE 8                  /* SR_USER + this: init's control channel */
/* init's reboot: a kexec reads the kernel and boot image from /esp, syncs
 * (2 s at most) and stops every driver (30 s at most) before the jump. */
#define REBOOT_WAIT  (60 * NS_PER_S)

/* ---- keys: the focus stack of open_keys channels ------------------------------ */

handle_t focus[MAX_FOCUS];
static uint8_t focus_level[MAX_FOCUS];   /* the level of the client that opened it */
static uint32_t focus_want[MAX_FOCUS];   /* INPUT_WANT_*: what it asked for besides keys */
struct client *focus_client[MAX_FOCUS];
unsigned nfocus;
static struct input_key_event pending[PENDING_KEYS];
static unsigned npending;

/* A focus channel whose client is gone is noticed when a key is sent to it
 * (ERR_PEER_CLOSED): it is dropped and the one below gets the key. */
void focus_drop(unsigned i)
{
    if (focus[i] == alt_owner)
        alt_leave();
    jam_handle_close(focus[i]);
    for (unsigned j = i; j + 1 < nfocus; j++) {
        focus[j] = focus[j + 1];
        focus_level[j] = focus_level[j + 1];
        focus_want[j] = focus_want[j + 1];
        focus_client[j] = focus_client[j + 1];
    }
    nfocus--;
}

static bool is_ctrl_c(const struct input_key_event *ev)
{
    return ev->state != INPUT_KEY_UP &&
           (ev->codepoint == 3 ||
            ((ev->mods & INPUT_MOD_CTRL) && (ev->usage == 0x06 || ev->codepoint == 'c')));
}

/* The key to the newest focus with a level <= max (dropping the ones whose
 * client is gone on the way). *at: its index; false if none took it. */
static bool send_below(const struct input_key_event *ev, unsigned from, uint8_t max, unsigned *at)
{
    for (unsigned i = from; i-- > 0;) {
        if (focus_level[i] > max)
            continue;
        status_t st = jam_channel_write(focus[i], ev, sizeof(*ev), NULL, 0);
        if (st == ERR_PEER_CLOSED) {
            focus_drop(i);
            continue;
        }
        *at = i;
        return st == OK;   /* else full (the client isn't reading): dropped */
    }
    return false;
}

static void send_key(const struct input_key_event *ev)
{
    unsigned at = 0;
    if (!send_below(ev, nfocus, L_PROGRAM, &at) && !nfocus) {
        if (npending < PENDING_KEYS)
            pending[npending++] = *ev;
        return;
    }
    /* A program's focus: Ctrl+C reaches the shell below it too. */
    if (nfocus && at < nfocus && focus_level[at] == L_PROGRAM && is_ctrl_c(ev))
        send_below(ev, at, L_SHELL, &at);
}

/* ---- init: Ctrl+Alt+Del and another terminal ------------------------------------ */

/* Ctrl+Alt+Del (a keyboard's: usage 0x4c with CTRL and ALT) reboots:
 * through init, which syncs /data first and kexecs into the kernel on the
 * stick (initctl.reboot answers only if that failed). The request is
 * written without waiting for its answer, and from then on every key and
 * mouse report is dropped at once: the console's loop goes on serving its
 * input sources, so the keyboard drivers never wait on it while init
 * stops them. Without init, if init's answer says it failed, or if init
 * is still at it after REBOOT_WAIT, the console resets the machine
 * itself.
 *
 * Super+Enter asks init for another terminal (initctl.terminal) the same
 * way, without waiting; only a refusal is said, on this terminal (the new
 * one is its own news). Both answers come on init's channel, told apart
 * by their transaction ids (init_event). */
static bool rebooting;              /* asked for: keys are dropped from now on */
static uint64_t reboot_at;          /* when the console resets the machine itself */
static bool watching;               /* init's channel is bound to our port */
static bool term_asked;             /* a terminal request is unanswered */
#define REBOOT_TXID 0x0cad0001u     /* the reboot request's transaction id */
#define TERM_TXID   0x7e570001u     /* the terminal request's */

static handle_t init_channel(void)
{
    return startup_handle(SR_USER + INITCTL_ROLE);
}

static void reboot_now(void)
{
    status_t st = jam_reboot(root);
    printf("console: reboot: %s\n", status_str(st));
}

void init_watch(void)
{
    handle_t init = init_channel();
    if (!init)
        return;
    status_t st = jam_port_bind(port, init, KEY(K_INIT, 0), SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    watching = st == OK;
    if (!watching)   /* Ctrl+Alt+Del then resets the machine itself */
        printf("console: init's channel: port_bind: %s\n", status_str(st));
}

static void ctrl_alt_del(void)
{
    if (rebooting)
        return;
    rebooting = true;
    reboot_at = now() + REBOOT_WAIT;
    printf("console: Ctrl+Alt+Del: rebooting\n");
    screen_blank(true);   /* nothing drawn until the next boot's splash */
    handle_t init = init_channel();
    status_t st = init && watching ? initctl_reboot_send(init, REBOOT_TXID) : ERR_NOT_FOUND;
    if (st != OK) {
        printf("console: init: %s\n", status_str(st));
        reboot_now();
    }
}

/* A terminal request refused, or one that never went: said here. */
static void terminal_said(status_t st)
{
    char line[96];
    int n;
    if (st == ERR_NOT_SUPPORTED)
        n = snprintf(line, sizeof(line), "[another terminal needs the compositor: this boot "
                                         "has none]");
    else if (st == ERR_NO_RESOURCES)
        n = snprintf(line, sizeof(line), "[no more terminals: %u is the most; close one to open "
                                         "another]", TERMINALS_MAX);
    else
        n = snprintf(line, sizeof(line), "[no new terminal: %s]", status_str(st));
    notice_out(line, (size_t)n);
    dirty = true;
}

void terminal_ask(void)
{
    if (rebooting || term_asked)
        return;   /* one at a time: a second Super+Enter before the answer is the same ask */
    handle_t init = init_channel();
    static const uint8_t none[128];   /* a plain terminal: no command */
    status_t st = init && watching ? initctl_terminal_send(init, TERM_TXID, none)
                                   : ERR_NOT_FOUND;
    if (st == OK)
        term_asked = true;
    else
        terminal_said(st);
}

void init_event(void)
{
    handle_t init = init_channel();
    for (;;) {   /* bounded: init answers only what we asked, two at most */
        uint8_t rep[64];
        struct idl_msg m;
        status_t st = idl_reply_read(init, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK && st != ERR_INTERNAL) {   /* init's end is gone */
            (void)jam_port_unbind(port, init, KEY(K_INIT, 0));
            watching = false;
            if (rebooting) {
                printf("console: init: %s\n", status_str(st));
                reboot_now();
            }
            return;
        }
        if (m.txid == REBOOT_TXID && rebooting) {
            printf("console: init: %s\n", status_str(initctl_reboot_result(rep, &m)));
            reboot_now();
        } else if (m.txid == TERM_TXID && term_asked) {
            uint8_t number = 0;
            st = initctl_terminal_result(rep, &m, &number);
            term_asked = false;
            if (st != OK)
                terminal_said(st);
        } else {
            idl_msg_drop(&m);   /* not an answer we wait for */
        }
    }
}

uint64_t reboot_deadline(void)
{
    return rebooting ? reboot_at : DEADLINE_NEVER;
}

void reboot_due(void)
{
    if (!rebooting || now() < reboot_at)
        return;
    printf("console: init did not reboot in %lu s\n", (unsigned long)(REBOOT_WAIT / NS_PER_S));
    reboot_at = DEADLINE_NEVER;
    reboot_now();
}

/* ---- keys to the focus ----------------------------------------------------------- */

void key_event(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp, bool terminal)
{
    if (rebooting)
        return;   /* the machine is going: nobody reads keys any more */
    struct input_key_event ev = { usage, state, mods, cp };
    if (asks_terminal(&ev)) {
        terminal_ask();
        return;
    }
    if (usage == 0x4c && state == INPUT_KEY_DOWN && (mods & INPUT_MOD_CTRL) &&
        (mods & INPUT_MOD_ALT)) {
        ctrl_alt_del();
        return;
    }
    /* Scrollback: Shift+PageUp/Down on a keyboard, PageUp/Down on a terminal. */
    bool page = usage == 0x4b || usage == 0x4e;
    if (page && !alt_on && (terminal || (mods & INPUT_MOD_SHIFT))) {
        if (state == INPUT_KEY_UP)
            return;
        uint32_t step = rows / 2 ? rows / 2 : 1;
        struct view v = view_now();
        uint32_t max = view_back_max(&v);
        if (usage == 0x4b)
            view_back = view_back + step < max ? view_back + step : max;
        else
            view_back = view_back > step ? view_back - step : 0;
        dirty = true;
        return;
    }
    if (state != INPUT_KEY_UP && view_back) {
        view_back = 0;
        dirty = true;
    }
    send_key(&ev);
}

status_t op_open_keys(void *ctx, handle_t *out)
{
    struct client *c = ctx;
    if (nfocus == MAX_FOCUS) {
        if (c->level == L_PROGRAM)
            return ERR_NO_RESOURCES;   /* a program can't push the shell's focus out */
        focus_drop(0);   /* the oldest loses its place */
    }
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    focus_level[nfocus] = c->level;
    focus_want[nfocus] = 0;
    focus_client[nfocus] = c;
    focus[nfocus++] = mine;
    *out = theirs;
    /* Keys typed before anyone listened. */
    unsigned n = npending;
    npending = 0;
    for (unsigned i = 0; i < n; i++)
        jam_channel_write(mine, &pending[i], sizeof(pending[i]), NULL, 0);
    return OK;
}

/* ---- input sources ------------------------------------------------------------- */

struct source {
    handle_t        ch;     /* our end of its `input` channel; 0: a free slot */
    struct termkeys term;   /* its terminal text's decoding state */
};
static struct source sources[MAX_SOURCES];

static status_t op_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    (void)ctx;
    key_event(usage, state, mods, cp, false);
    return OK;
}

/* A round of what focus i wrote on its key channel: each struct input_want
 * replaces what it wants. At most WANT_BUDGET of them, so a client writing
 * flat out can't hold up the mouse. false: its client is gone, or wrote
 * something the wire format doesn't allow, and the focus must be dropped. */
#define WANT_BUDGET 8
static bool focus_requests(unsigned i)
{
    for (unsigned n = 0; n < WANT_BUDGET; n++) {
        struct input_want w;
        uint32_t got = 0;
        struct channel_read_args a = {
            .h = focus[i], .bytes_cap = sizeof(w), .bytes = (uint64_t)(uintptr_t)&w,
            .actual_bytes = (uint64_t)(uintptr_t)&got,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT)
            return true;
        if (st != OK || got != sizeof(w) || (w.events & ~INPUT_WANT_MOUSE) || w.reserved)
            return false;
        focus_want[i] = w.events;
    }
    return true;
}

/* A mouse report to the focus, if it asked for the mouse; false if nobody
 * took it. Only the newest focus counts: the mouse belongs to whoever has
 * the keys. */
static bool mouse_to_focus(const struct input_mouse_event *ev)
{
    while (nfocus) {
        unsigned i = nfocus - 1;
        if (!focus_requests(i)) {
            focus_drop(i);
            continue;
        }
        if (!(focus_want[i] & INPUT_WANT_MOUSE))
            return false;
        status_t st = jam_channel_write(focus[i], ev, sizeof(*ev), NULL, 0);
        if (st == ERR_PEER_CLOSED) {
            focus_drop(i);
            continue;
        }
        return true;   /* sent, or its queue is full (it isn't reading): dropped */
    }
    return false;
}

bool focus_wants_mouse(void)
{
    while (nfocus) {
        unsigned i = nfocus - 1;
        if (focus_requests(i))
            return (focus_want[i] & INPUT_WANT_MOUSE) != 0;
        focus_drop(i);
    }
    return false;
}

void mouse_event(const struct input_mouse_event *ev)
{
    if (rebooting)
        return;
    if (mouse_to_focus(ev))
        return;
    /* Nobody wants the mouse: the wheel scrolls back, unless the screen is
     * lent out (the text isn't on it to be scrolled). */
    if (screen_lent())
        return;
    if (ev->wheel > 0)
        key_event(0x4b, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0, false);
    else if (ev->wheel < 0)
        key_event(0x4e, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0, false);
}

static status_t op_mouse(void *ctx, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    (void)ctx;
    struct input_mouse_event ev = {
        .kind = INPUT_EVENT_MOUSE, .dx = dx, .dy = dy, .wheel = wheel, .buttons = buttons,
    };
    mouse_event(&ev);
    return OK;
}

/* A key a terminal typed (<termkeys.h>). */
static void term_key(uint16_t usage, uint32_t cp)
{
    key_event(usage, INPUT_KEY_DOWN, 0, cp, true);
}

static status_t op_text(void *ctx, uint16_t length, const uint8_t bytes[64])
{
    struct source *s = ctx;
    if (length > 64)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < length; i++) {
        struct termkey k[2];
        unsigned n = termkeys_byte(&s->term, bytes[i], k);
        for (unsigned j = 0; j < n; j++)
            term_key(k[j].usage, k[j].cp);
    }
    return OK;
}

/* When the first keyboard and the first mouse became usable (ns since the
 * kernel started; 0: none yet), for the boot log. */
static uint64_t first_ready[3];

static status_t op_ready(void *ctx, uint8_t kind, uint16_t vendor, uint16_t product)
{
    (void)ctx;
    if (kind != INPUT_READY_KEYBOARD && kind != INPUT_READY_MOUSE)
        return ERR_INVALID_ARGS;
    uint64_t t = now();
    bool first = !first_ready[kind];
    if (first)
        first_ready[kind] = t;
    printf("console: %s %04x:%04x ready %lu.%03lu s after the kernel started%s\n",
           kind == INPUT_READY_KEYBOARD ? "keyboard" : "mouse", vendor, product,
           (unsigned long)(t / NS_PER_S), (unsigned long)(t % NS_PER_S / NS_PER_MS),
           first ? (kind == INPUT_READY_KEYBOARD ? " (the first keyboard)" : " (the first mouse)")
                 : "");
    uint64_t k = first_ready[INPUT_READY_KEYBOARD], m = first_ready[INPUT_READY_MOUSE];
    if (first && k && m) {
        uint64_t both = k > m ? k : m;
        printf("console: input ready: the first keyboard and mouse %lu.%03lu s after the kernel "
               "started\n", (unsigned long)(both / NS_PER_S),
               (unsigned long)(both % NS_PER_S / NS_PER_MS));
    }
    return OK;
}

static const struct input_ops input_ops = { op_key, op_mouse, op_text, op_ready };

status_t op_connect_input(void *ctx, handle_t *out)
{
    const struct client *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;   /* input sources are devmgr's and init's */
    if (window_mode)
        return ERR_NOT_SUPPORTED;   /* the input is the compositor's: our keys come to our window */
    unsigned i;
    for (i = 0; i < MAX_SOURCES && sources[i].ch; i++)
        ;
    if (i == MAX_SOURCES)
        return ERR_NO_RESOURCES;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_port_bind(port, mine, KEY(K_SOURCE, i), SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    sources[i] = (struct source){ .ch = mine };
    *out = theirs;
    return OK;
}

void source_event(unsigned i)
{
    struct source *s = &sources[i];
    if (!s->ch)
        return;
    status_t st;
    while ((st = input_serve_one(s->ch, &input_ops, s)) == OK)
        ;
    if (st != ERR_SHOULD_WAIT) {   /* ERR_PEER_CLOSED: the source is gone */
        jam_port_unbind(port, s->ch, KEY(K_SOURCE, i));
        jam_handle_close(s->ch);
        s->ch = HANDLE_INVALID;
        printf("console: input source %u went away\n", i);
    }
}
