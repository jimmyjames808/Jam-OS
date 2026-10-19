/* console: keys, and the focus stack of open_keys channels and tty clients
 * (console.h).
 *
 * The keys come from the back end (the window's, where wlinput.c turns the
 * compositor's keys into the console's key events; the connection's, which
 * decodes the bytes typed at the remote terminal) and go to the newest
 * open_keys channel or open_tty client (the focus); keys typed before
 * anyone listened wait for the first. A tty client gets them as bytes,
 * through the terminal's line discipline (tty.c). Ctrl+Alt+Del and
 * Super+Enter are the compositor's: they never reach a window.
 *
 * Nothing a program opens can hold the keys hostage:
 *   - a focus a PROGRAM opened with open_keys gets Ctrl+C, and so does the
 *     SHELL or ADMIN focus below it (the shell that ran it kills it);
 *   - a tty client with ISIG on: its Ctrl+C goes to the shell's keys below
 *     it in its place (tty.c); with ISIG off it is the byte 3, the
 *     program's (raw mode: an editor's key);
 *   - in a window, Ctrl+Alt+C is Ctrl+C to the newest SHELL or ADMIN key
 *     channel whatever has the focus, so the shell can always end what it
 *     runs (over a connection the remote terminal has its own: OpenSSH's
 *     "~.").
 *
 * A key channel speaks the `keys` protocol (abi/idl/console.idl: its key
 * events, review W14); the console never reads what a client writes on
 * it. */
#include "console.h"

#define U_C 0x06   /* HID usage: the C key */

/* ---- the focus stack ---------------------------------------------------------- */

handle_t focus[MAX_FOCUS];
static uint8_t focus_level[MAX_FOCUS];   /* the level of the client that opened it */
struct client *focus_client[MAX_FOCUS];
int8_t focus_tty[MAX_FOCUS];
unsigned nfocus;
static struct input_key_event pending[PENDING_KEYS];
static unsigned npending;

/* A focus channel whose client is gone is noticed when a key is sent to it
 * (ERR_PEER_CLOSED), or, for a tty, when its channel closes (tty.c): it is
 * dropped and the one below gets the keys. */
void focus_drop(unsigned i)
{
    int tty = focus_tty[i];
    if (back->focus_dropped)
        back->focus_dropped(focus[i]);
    if (tty >= 0)
        ttys_release((unsigned)tty);   /* it closes the channel */
    else
        jam_handle_close(focus[i]);
    for (unsigned j = i; j + 1 < nfocus; j++) {
        focus[j] = focus[j + 1];
        focus_level[j] = focus_level[j + 1];
        focus_client[j] = focus_client[j + 1];
        focus_tty[j] = focus_tty[j + 1];
    }
    nfocus--;
    ttys_refocus();   /* what the terminal holds goes to the new top, from the loop */
}

int focus_top_tty(void)
{
    return nfocus ? focus_tty[nfocus - 1] : -1;
}

static bool is_ctrl_c(const struct input_key_event *ev)
{
    return ev->state != INPUT_KEY_UP &&
           (ev->codepoint == 3 ||
            ((ev->mods & INPUT_MOD_CTRL) && (ev->usage == U_C || ev->codepoint == 'c')));
}

/* Ctrl+Alt+C: the chord no program gets (a window's: a connection sends
 * Alt as ESC). */
static bool is_shell_chord(const struct input_key_event *ev)
{
    return (ev->mods & INPUT_MOD_CTRL) && (ev->mods & INPUT_MOD_ALT) &&
           (ev->usage == U_C || ev->codepoint == 'c' || ev->codepoint == 3);
}

/* The last send_below met a full channel (the key was dropped), and the
 * last focus_key's key did, or found no focus and PENDING_KEYS waiting
 * for one: focus_key_try tells its caller. */
static bool last_full, key_refused;

/* The key to the newest key channel below `from` with a level <= max
 * (dropping the ones whose client is gone on the way; ttys are passed
 * over). *at: its index; false if none took it. */
static bool send_below(const struct input_key_event *ev, unsigned from, uint8_t max, unsigned *at)
{
    last_full = false;
    for (unsigned i = from; i-- > 0;) {
        if (focus_level[i] > max || focus_tty[i] >= 0)
            continue;
        status_t st = keys_key(focus[i], ev->usage, ev->state, ev->mods, ev->codepoint);
        if (st == ERR_PEER_CLOSED) {
            focus_drop(i);
            continue;
        }
        *at = i;
        last_full = st == ERR_SHOULD_WAIT;
        return st == OK;   /* else full (the client isn't reading): dropped */
    }
    return false;
}

bool focus_to_shell(const struct input_key_event *ev, unsigned below)
{
    unsigned at;
    return send_below(ev, below < nfocus ? below : nfocus, L_SHELL, &at);
}

static void send_key(const struct input_key_event *ev)
{
    unsigned at = 0;
    bool took = send_below(ev, nfocus, L_PROGRAM, &at);
    key_refused = last_full;
    if (!took && !nfocus) {
        if (npending < PENDING_KEYS)
            pending[npending++] = *ev;
        else
            key_refused = true;   /* no room: dropped, or kept by focus_key_try's caller */
        return;
    }
    /* A program's focus: Ctrl+C reaches the shell below it too. */
    if (nfocus && at < nfocus && focus_level[at] == L_PROGRAM && is_ctrl_c(ev))
        send_below(ev, at, L_SHELL, &at);
}

/* ---- keys to the focus ----------------------------------------------------------- */

void focus_key(const struct input_key_event *ev)
{
    if (is_shell_chord(ev)) {
        if (ev->state == INPUT_KEY_UP)
            return;
        struct input_key_event c = { U_C, INPUT_KEY_DOWN, INPUT_MOD_LCTRL, 0x03 };
        (void)focus_to_shell(&c, nfocus);   /* no shell: nobody to tell */
        return;
    }
    int tty = focus_top_tty();
    if (tty >= 0)
        ttys_key((unsigned)tty, ev);
    else
        send_key(ev);
}

bool focus_key_try(const struct input_key_event *ev)
{
    key_refused = false;
    focus_key(ev);
    return !key_refused;
}

status_t focus_push(handle_t h, struct client *c, int tty)
{
    if (nfocus == MAX_FOCUS) {
        if (c->level == L_PROGRAM)
            return ERR_NO_RESOURCES;   /* a program can't push the shell's focus out */
        focus_drop(0);   /* the oldest loses its place */
    }
    focus_level[nfocus] = c->level;
    focus_client[nfocus] = c;
    focus_tty[nfocus] = (int8_t)tty;
    focus[nfocus++] = h;
    /* Keys typed before anyone listened. */
    unsigned n = npending;
    npending = 0;
    for (unsigned i = 0; i < n; i++)
        focus_key(&pending[i]);
    return OK;
}

status_t op_open_keys(void *ctx, handle_t *out)
{
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    if ((st = focus_push(mine, ctx, -1)) != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    *out = theirs;
    return OK;
}
