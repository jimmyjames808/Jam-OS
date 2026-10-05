/* Input sources (seat.h): HID drivers and serialin, each on a channel
 * compctl.connect_input made, speaking the `input` protocol
 * (abi/idl/input.idl) unchanged, as they did to the console.
 *
 * The loop serves the sources first in each turn (seat_serve), at most
 * SEAT_BUDGET requests of each, so a source sending flat out delays the
 * others and the clients by at most that; what is left waits for the next
 * turn (`more`: the loop doesn't sleep). A source whose channel closes
 * (its driver ended, its device was unplugged) lets go of the keys it
 * held. At most SOURCES_MAX at once.
 *
 * What each request becomes: key (its HID usage, state and modifier byte;
 * hid's character is not used: clients decode keys with the keymap) to
 * keyboard.c; mouse to pointer.c; text (a terminal's bytes) typed as
 * keys, decoded with a terminal state of the source's own; ready logged
 * with the time since the kernel started, as the console did. */
#include <idl/input.h>
#include "seat.h"

struct source {
    handle_t ch;             /* our end of its `input` channel; HANDLE_INVALID (0): a free slot */
    unsigned slot;
    bool ready;              /* a port packet came: it may have requests */
    bool more;               /* its budget ran out with requests left */
    struct termkeys term;    /* its terminal text's decoding state */
};

static struct source sources[SOURCES_MAX];
static unsigned nsources;
static uint64_t first_ready[3];   /* when the first keyboard and mouse were ready (0: not yet) */

static status_t op_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    const struct source *s = ctx;
    (void)cp;   /* hid's US character: clients decode keys themselves */
    if (state > INPUT_KEY_REPEAT)
        return ERR_INVALID_ARGS;
    if (ctl_rebooting())
        return OK;   /* the machine is going: nobody reads keys any more */
    keyboard_key(s->slot, usage, state, mods);
    return OK;
}

static status_t op_mouse(void *ctx, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    (void)ctx;
    if (!ctl_rebooting())
        pointer_report(dx, dy, wheel, buttons);
    return OK;
}

static status_t op_text(void *ctx, uint16_t length, const uint8_t bytes[64])
{
    struct source *s = ctx;
    if (length > 64)
        return ERR_INVALID_ARGS;
    if (!ctl_rebooting())
        keyboard_text(s->slot, &s->term, bytes, length);
    return OK;
}

static status_t op_ready(void *ctx, uint8_t kind, uint16_t vendor, uint16_t product)
{
    (void)ctx;
    if (kind != INPUT_READY_KEYBOARD && kind != INPUT_READY_MOUSE)
        return ERR_INVALID_ARGS;
    uint64_t t = now();
    bool first = !first_ready[kind];
    if (first)
        first_ready[kind] = t;
    printf("compositor: %s %04x:%04x ready %lu.%03lu s after the kernel started%s\n",
           kind == INPUT_READY_KEYBOARD ? "keyboard" : "mouse", vendor, product,
           (unsigned long)(t / NS_PER_S), (unsigned long)(t % NS_PER_S / NS_PER_MS),
           !first ? "" : kind == INPUT_READY_KEYBOARD ? " (the first keyboard)"
                                                      : " (the first mouse)");
    return OK;
}

static const struct input_ops input_ops = { op_key, op_mouse, op_text, op_ready };

status_t sources_connect(handle_t *out)
{
    unsigned i = 0;
    while (i < SOURCES_MAX && sources[i].ch != HANDLE_INVALID)
        i++;
    if (i == SOURCES_MAX)
        return ERR_NO_RESOURCES;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_port_bind(comp.port, mine, SEAT_KEY_SRC + i, SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    sources[i] = (struct source){ .ch = mine, .slot = i, .ready = true };
    nsources++;
    *out = theirs;
    return OK;
}

static void source_gone(struct source *s, status_t why)
{
    (void)jam_port_unbind(comp.port, s->ch, SEAT_KEY_SRC + s->slot);
    jam_handle_close(s->ch);
    s->ch = HANDLE_INVALID;
    s->ready = s->more = false;
    nsources--;
    keyboard_source_gone(s->slot);
    printf("compositor: input source %u went away (%s)\n", s->slot, status_str(why));
}

void sources_packet(unsigned slot)
{
    if (slot < SOURCES_MAX && sources[slot].ch != HANDLE_INVALID)
        sources[slot].ready = true;
}

static void serve_source(struct source *s)
{
    s->more = false;
    for (unsigned n = 0; n < SEAT_BUDGET; n++) {
        status_t st = input_serve_one(s->ch, &input_ops, s);
        if (st == ERR_SHOULD_WAIT) {
            s->ready = false;
            return;
        }
        if (st != OK) {
            source_gone(s, st);   /* ERR_PEER_CLOSED: the source ended */
            return;
        }
    }
    s->more = true;
}

void sources_serve(void)
{
    for (unsigned i = 0; i < SOURCES_MAX; i++) {
        struct source *s = &sources[i];
        if (s->ch != HANDLE_INVALID && (s->ready || s->more))
            serve_source(s);
    }
}

bool sources_more(void)
{
    for (unsigned i = 0; i < SOURCES_MAX; i++)
        if (sources[i].more)
            return true;
    return false;
}

unsigned sources_count(void)
{
    return nsources;
}
