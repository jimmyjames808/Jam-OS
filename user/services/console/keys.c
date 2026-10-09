/* console: keys, and the focus stack of open_keys channels (console.h).
 *
 * The keys come to the terminal's window from the compositor (wlinput.c
 * turns them into the console's key events) and go to the newest
 * open_keys channel (the focus); keys typed before anyone listened wait
 * for the first. Ctrl+Alt+Del and Super+Enter are the compositor's: they
 * never reach a window. A focus a PROGRAM opened can't hold the keys
 * hostage: Ctrl+C goes to it and to the SHELL/ADMIN focus below it (the
 * shell that ran it kills it).
 *
 * The mouse goes to the focus too, but only to one that asked: a client
 * writes a struct input_want on its key channel, and mouse reports then
 * arrive there as struct input_mouse_event, a message of another size
 * than a key (<jam/abi.h> has the wire format). A client that never asked
 * never gets one, so the shell's line editor reads nothing but keys. With
 * no such focus the wheel scrolls the console back, and the rest of the
 * report is dropped. */
#include "console.h"

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

/* ---- keys to the focus ----------------------------------------------------------- */

void key_event(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    struct input_key_event ev = { usage, state, mods, cp };
    /* Scrollback: Shift+PageUp/Down. */
    bool page = usage == 0x4b || usage == 0x4e;
    if (page && !alt_on && (mods & INPUT_MOD_SHIFT)) {
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
            .h = focus[i], .rd_cap = sizeof(w), .rd_bytes = (uint64_t)(uintptr_t)&w,
            .rd_actual = (uint64_t)(uintptr_t)&got,
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
    if (mouse_to_focus(ev))
        return;
    /* Nobody wants the mouse: the wheel scrolls back. */
    if (ev->wheel > 0)
        key_event(0x4b, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0);
    else if (ev->wheel < 0)
        key_event(0x4e, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0);
}
