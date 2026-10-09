/* Input sources (seat.h): HID drivers and serialin, each on a channel
 * whose end compctl.add_source gave us, speaking the `input` protocol
 * (abi/idl/input.idl): events both ways, nothing answered.
 *
 * The loop serves the sources first in each turn (seat_serve), at most
 * SEAT_BUDGET requests of each, so a source sending flat out delays the
 * others and the clients by at most that; what is left waits for the next
 * turn (`more`: the loop doesn't sleep). A source whose channel closes
 * (its driver ended, its device was unplugged) lets go of the keys it
 * held. At most SOURCES_MAX at once.
 *
 * What each event becomes: key (its HID usage, state and modifier byte:
 * clients decode keys with the keymap) to keyboard.c; mouse to
 * pointer.c; text (a terminal's bytes) typed as keys, decoded with a
 * terminal state of the source's own; ready logged with the time since
 * the kernel started, and "input ready" once the first keyboard and the
 * first mouse both are. A keyboard's ready is answered with its lights
 * (input.leds: the locks are the compositor's, review W7), and every
 * keyboard is sent them again whenever a lock key changes one
 * (sources_leds). An event that breaks the format is dropped (the
 * generated server's rule). */
#include <idl/input.h>
#include "seat.h"

struct source {
    handle_t ch;             /* our end of its `input` channel; HANDLE_INVALID (0): a free slot */
    unsigned slot;
    bool ready;              /* a port packet came: it may have requests */
    bool more;               /* its budget ran out with requests left */
    bool keyboard;           /* it said it is a keyboard (ready): it gets the lights */
    struct termkeys term;    /* its terminal text's decoding state */
};

static struct source sources[SOURCES_MAX];
static unsigned nsources;
static uint64_t first_ready[3];   /* when the first keyboard and mouse were ready (0: not yet) */

static void op_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods)
{
    const struct source *s = ctx;
    if (state != INPUT_KEY_UP && state != INPUT_KEY_DOWN)
        return;   /* not a state a source sends */
    if (ctl_rebooting())
        return;   /* the machine is going: nobody reads keys any more */
    keyboard_key(s->slot, usage, state, mods);
}

static void op_mouse(void *ctx, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    (void)ctx;
    if (!ctl_rebooting())
        pointer_report(dx, dy, wheel, buttons);
}

static void op_text(void *ctx, const uint8_t *bytes, uint32_t n)
{
    struct source *s = ctx;
    if (!ctl_rebooting())
        keyboard_text(s->slot, &s->term, bytes, n);
}

static void op_ready(void *ctx, uint8_t kind, uint16_t vendor, uint16_t product)
{
    struct source *s = ctx;
    if (kind != INPUT_READY_KEYBOARD && kind != INPUT_READY_MOUSE)
        return;
    if (kind == INPUT_READY_KEYBOARD) {
        s->keyboard = true;
        (void)input_leds(s->ch, keyboard_leds());   /* a full queue: the next change says it */
    }
    uint64_t t = now();
    bool first = !first_ready[kind];
    if (first)
        first_ready[kind] = t;
    printf("compositor: %s %04x:%04x ready %lu.%03lu s after the kernel started%s\n",
           kind == INPUT_READY_KEYBOARD ? "keyboard" : "mouse", vendor, product,
           (unsigned long)(t / NS_PER_S), (unsigned long)(t % NS_PER_S / NS_PER_MS),
           !first ? "" : kind == INPUT_READY_KEYBOARD ? " (the first keyboard)"
                                                      : " (the first mouse)");
    /* The boot log's one line for "input works" (tools/usb-early-test.sh
     * times it), as the console said it before the compositor took the
     * input. */
    uint64_t k = first_ready[INPUT_READY_KEYBOARD], m = first_ready[INPUT_READY_MOUSE];
    if (first && k && m) {
        uint64_t both = k > m ? k : m;
        printf("compositor: input ready: the first keyboard and mouse %lu.%03lu s after the "
               "kernel started\n", (unsigned long)(both / NS_PER_S),
               (unsigned long)(both % NS_PER_S / NS_PER_MS));
    }
}

static const struct input_ops input_ops = {
    .key = op_key, .mouse = op_mouse, .text = op_text, .ready = op_ready,
};

void sources_leds(uint8_t leds)
{
    for (unsigned i = 0; i < SOURCES_MAX; i++)
        if (sources[i].ch != HANDLE_INVALID && sources[i].keyboard)
            (void)input_leds(sources[i].ch, leds);   /* a full queue: its keyboard isn't reading */
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

status_t sources_add(handle_t mine)
{
    unsigned i = 0;
    while (i < SOURCES_MAX && sources[i].ch != HANDLE_INVALID)
        i++;
    if (i == SOURCES_MAX) {
        jam_handle_close(mine);
        return ERR_NO_RESOURCES;
    }
    status_t st = jam_port_bind(comp.port, mine, SEAT_KEY_SRC + i, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        return st;
    }
    sources[i] = (struct source){ .ch = mine, .slot = i, .ready = true };
    nsources++;
    /* What it sent already goes before anything later on the channel that
     * added it (a test's stats call, init's next request). */
    serve_source(&sources[i]);
    return OK;
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
