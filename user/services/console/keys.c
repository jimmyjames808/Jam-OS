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
#include "console.h"

/* ---- keys: the focus stack of open_keys channels ------------------------------ */

handle_t focus[MAX_FOCUS];
static uint8_t focus_level[MAX_FOCUS];   /* the level of the client that opened it */
static uint32_t focus_want[MAX_FOCUS];   /* INPUT_WANT_*: what it asked for besides keys */
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

static void key_event(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp, bool terminal)
{
    /* Ctrl+Alt+Del (a keyboard's: usage 0x4c with CTRL and ALT) reboots. */
    if (usage == 0x4c && state == INPUT_KEY_DOWN && (mods & INPUT_MOD_CTRL) &&
        (mods & INPUT_MOD_ALT)) {
        printf("console: Ctrl+Alt+Del: rebooting\n");
        status_t st = jam_reboot(root);
        printf("console: reboot: %s\n", status_str(st));
        return;
    }
    /* Scrollback: Shift+PageUp/Down on a keyboard, PageUp/Down on a terminal. */
    bool page = usage == 0x4b || usage == 0x4e;
    if (page && !alt_on && (terminal || (mods & INPUT_MOD_SHIFT))) {
        if (state == INPUT_KEY_UP)
            return;
        uint32_t step = rows / 2 ? rows / 2 : 1;
        uint64_t max = committed < SCROLLBACK ? committed : SCROLLBACK;
        max = max > rows ? max - rows + 1 : 0;
        if (usage == 0x4b)
            view_back = view_back + step < max ? view_back + step : (uint32_t)max;
        else
            view_back = view_back > step ? view_back - step : 0;
        dirty = true;
        return;
    }
    if (state != INPUT_KEY_UP && view_back) {
        view_back = 0;
        dirty = true;
    }
    struct input_key_event ev = { usage, state, mods, cp };
    send_key(&ev);
}

status_t op_open_keys(void *ctx, handle_t *out)
{
    const struct client *c = ctx;
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
    handle_t ch;        /* our end of its `input` channel; 0: a free slot */
    int      esc;       /* terminal escape parser */
    char     params[8]; /* the escape's parameter bytes so far */
    unsigned np;        /* how many */
    bool     last_cr;   /* the last text byte was '\r' (a '\n' after it is dropped) */
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

static status_t op_mouse(void *ctx, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    (void)ctx;
    struct input_mouse_event ev = {
        .kind = INPUT_EVENT_MOUSE, .dx = dx, .dy = dy, .wheel = wheel, .buttons = buttons,
    };
    if (mouse_to_focus(&ev))
        return OK;
    /* Nobody wants the mouse: the wheel scrolls back, unless the screen is
     * lent out (the text isn't on it to be scrolled). */
    if (screen_lent())
        return OK;
    if (wheel > 0)
        key_event(0x4b, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0, false);
    else if (wheel < 0)
        key_event(0x4e, INPUT_KEY_DOWN, INPUT_MOD_LSHIFT, 0, false);
    return OK;
}

static void term_key(uint16_t usage, uint32_t cp)
{
    key_event(usage, INPUT_KEY_DOWN, 0, cp, true);
}

/* ESC [ <params> <final> or ESC O <final> from a terminal. */
static void term_escape(struct source *s, char final)
{
    s->params[s->np < sizeof(s->params) ? s->np : sizeof(s->params) - 1] = '\0';
    switch (final) {
    case 'A': term_key(0x52, 0); return;   /* up */
    case 'B': term_key(0x51, 0); return;   /* down */
    case 'C': term_key(0x4f, 0); return;   /* right */
    case 'D': term_key(0x50, 0); return;   /* left */
    case 'H': term_key(0x4a, 0); return;   /* home */
    case 'F': term_key(0x4d, 0); return;   /* end */
    case '~': {
        int n = 0;
        for (unsigned i = 0; i < s->np && s->params[i] >= '0' && s->params[i] <= '9'; i++)
            n = n * 10 + s->params[i] - '0';
        switch (n) {
        case 1: case 7: term_key(0x4a, 0); return;
        case 4: case 8: term_key(0x4d, 0); return;
        case 3: term_key(0x4c, 0); return;      /* delete */
        case 5: term_key(0x4b, 0); return;      /* page up */
        case 6: term_key(0x4e, 0); return;      /* page down */
        }
        return;
    }
    }
}

/* Byte b of source s's text while an escape sequence may be open: true
 * if the sequence took it. A byte that ends a lone ESC sends the ESC key
 * and is then an ordinary byte (false). */
static bool escape_byte(struct source *s, uint8_t b)
{
    if (s->esc == 1) {   /* after ESC */
        if (b == '[' || b == 'O') {
            s->esc = 2;
            s->np = 0;
            return true;
        }
        s->esc = 0;
        term_key(0x29, 0x1b);   /* a lone ESC */
        return false;
    }
    if (s->esc == 2) {
        if ((b >= '0' && b <= '9') || b == ';') {
            if (s->np < sizeof(s->params) - 1)
                s->params[s->np++] = (char)b;
            return true;
        }
        s->esc = 0;
        term_escape(s, (char)b);
        return true;
    }
    return false;
}

/* An ordinary byte of source s's text as a key. */
static void text_byte(struct source *s, uint8_t b)
{
    bool cr = false;
    if (b == 0x1b)
        s->esc = 1;
    else if (b == '\r' || (b == '\n' && !s->last_cr))
        term_key(0x28, '\n'), cr = b == '\r';
    else if (b == '\n')
        ;   /* the LF of a CR LF */
    else if (b == 0x7f || b == 0x08)
        term_key(0x2a, 0x08);
    else if (b == '\t')
        term_key(0x2b, '\t');
    else
        term_key(0, b);   /* printable, or a control character (Ctrl+C = 3) */
    s->last_cr = cr;
}

static status_t op_text(void *ctx, uint16_t length, const uint8_t bytes[64])
{
    struct source *s = ctx;
    if (length > 64)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < length; i++)
        if (!escape_byte(s, bytes[i]))
            text_byte(s, bytes[i]);
    return OK;
}

static const struct input_ops input_ops = { op_key, op_mouse, op_text };

status_t op_connect_input(void *ctx, handle_t *out)
{
    const struct client *c = ctx;
    if (c->level != L_ADMIN)
        return ERR_ACCESS_DENIED;   /* input sources are devmgr's and init's */
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
