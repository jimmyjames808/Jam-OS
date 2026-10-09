/* console: the connection back end's input (console.h, "connin.c").
 *
 * What the remote terminal typed arrives on the master channel as byte
 * messages. <termkeys.h> turns the bytes into keys, which go to the focus
 * (focus_key) as the keys of a serial terminal do: a printable character
 * or a control character as usage 0 and the byte itself, Enter, Backspace,
 * Tab, Escape and the cursor keys as their usage.
 *
 * Two things termkeys leaves to us:
 *   - A paste. A terminal that was told ESC [ ? 2004 h wraps what it
 *     pastes in ESC [ 200 ~ and ESC [ 201 ~, which termkeys drops as keys
 *     it doesn't know. They are kept here and typed as the window
 *     console's paste does (paste.c): the Escape key and the characters,
 *     to a client that asked for bracketed paste (the one with the focus),
 *     else nothing, so a program that didn't ask never sees them.
 *   - A lone Escape key. termkeys keeps an ESC until the next byte shows
 *     what follows it. A terminal sends the sequence of a key in one
 *     write, so an ESC that has waited ESC_GAP_NS alone is the Escape key.
 *
 * A key the focus can't take (its channel is full: the client isn't
 * reading) is dropped, as in the window; so a paste of more than the
 * channel's 1024 keys in one burst loses its tail. */
#include <termconn.h>
#include <termkeys.h>
#include "console.h"

#define IN_MSGS    8                    /* messages one turn takes from the master, at most */
#define IN_NS      (20 * NS_PER_MS)     /* ... or this long */
#define ESC_GAP_NS (50 * NS_PER_MS)     /* a lone ESC this old is the Escape key */
#define U_ESC      0x29                 /* HID usage: Escape */

static handle_t master;
static uint8_t *inbuf;                  /* one message: CHANNEL_MAX_BYTES */
static struct termkeys tk;              /* <termkeys.h>'s state between bytes */
static const char marks[2][7] = { "\033[200~", "\033[201~" };
static uint8_t held[6];                 /* bytes that may be the start of a marker */
static unsigned nheld;
static bool more;                       /* the turn's budget ended with messages left */
static uint64_t esc_at;                 /* when a lone ESC becomes the Escape key (0: none) */

bool conn_in_init(handle_t m)
{
    master = m;
    inbuf = malloc(CHANNEL_MAX_BYTES);
    return inbuf != NULL;
}

static void type(uint16_t usage, uint32_t cp)
{
    struct input_key_event ev = { usage, INPUT_KEY_DOWN, 0, cp };
    focus_key(&ev);
}

/* A byte through termkeys (the paste markers are dealt with before). */
static void to_keys(uint8_t b)
{
    struct termkey k[2];
    unsigned got = termkeys_byte(&tk, b, k);
    for (unsigned i = 0; i < got; i++)
        type(k[i].usage, k[i].cp);
}

/* A paste marker, for the client that has the focus if it asked for it. */
static void marker(const char *m)
{
    const struct client *c = nfocus ? focus_client[nfocus - 1] : NULL;
    if (!c || !c->bracketed)
        return;
    for (unsigned i = 0; i < sizeof(marks[0]) - 1; i++)
        type(m[i] == 0x1b ? U_ESC : 0, (uint8_t)m[i]);
}

/* The marker held[0..n) is the start of, if any. */
static const char *marker_of(unsigned n)
{
    for (unsigned m = 0; m < 2; m++)
        if (!memcmp(held, marks[m], n))
            return marks[m];
    return NULL;
}

static void release_held(void)
{
    unsigned n = nheld;
    nheld = 0;
    for (unsigned i = 0; i < n; i++)
        to_keys(held[i]);
}

static void in_byte(uint8_t b)
{
    held[nheld] = b;
    const char *m = marker_of(nheld + 1);
    if (m) {
        if (++nheld == sizeof(marks[0]) - 1) {
            nheld = 0;
            marker(m);
        }
        return;
    }
    release_held();
    if (b == 0x1b) {   /* not the marker we held, but maybe the start of the other */
        held[0] = b;
        nheld = 1;
        return;
    }
    to_keys(b);
}

/* Something is waiting for more bytes (a lone ESC, or the start of a
 * marker): the Escape key if nothing comes. */
static void arm_escape(void)
{
    esc_at = nheld || tk.esc == 1 ? now() + ESC_GAP_NS : 0;
}

status_t conn_in_take(void)
{
    uint64_t t0 = now();
    more = false;
    for (unsigned k = 0; k < IN_MSGS; k++) {
        if (k && now() - t0 > IN_NS) {
            more = true;
            break;
        }
        handle_t hs[CHANNEL_MAX_HANDLES];
        uint32_t n = 0, nh = 0;
        status_t st = drv_channel_read(master, inbuf, CHANNEL_MAX_BYTES, &n, hs,
                                       CHANNEL_MAX_HANDLES, &nh);
        if (st == ERR_SHOULD_WAIT)
            break;
        if (st != OK)
            return st;
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);   /* the master carries bytes only */
        for (uint32_t i = 0; i < n; i++)
            in_byte(inbuf[i]);
        more = k == IN_MSGS - 1;
    }
    arm_escape();
    return OK;
}

uint64_t conn_in_deadline(void)
{
    return more ? 0 : esc_at ? esc_at : DEADLINE_NEVER;
}

status_t conn_in_idle(void)
{
    if (more)
        return conn_in_take();
    if (esc_at && now() >= esc_at) {
        release_held();
        if (tk.esc == 1) {   /* an ESC alone: the Escape key */
            tk.esc = 0;
            type(U_ESC, 0x1b);
        }
        esc_at = 0;
    }
    return OK;
}
