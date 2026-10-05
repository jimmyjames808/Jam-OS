/* wl_keyboard (seat.h): the keymap, the keys held, the modifiers, and key
 * events to the client whose window has the keyboard focus, and only to
 * it.
 *
 * Keys arrive as HID usages (the `input` protocol) and leave as Linux
 * evdev codes (<keymap.h>), the numbers Wayland's keymaps are written
 * for. The keymap is the US layout's XKB text (keymap_us), in one VMO
 * made at start whose pages stay (VMO_KEEP_PAGES, so a client mapping it
 * can't be cut short), handed to each wl_keyboard read-only: read and map,
 * no write, no resize, no duplicate. Its first line names the layout, for
 * our own clients' tables.
 *
 * Keys held: each source's presses, with the source, so a keyboard that is
 * unplugged lets go of what it held. A press a client never sees (a
 * reserved key, focus.c) is remembered too, so its release is dropped as
 * well. hid's own REPEAT events are dropped: Wayland clients repeat keys
 * themselves, from repeat_info (hid's own rate: 30 a second after 500 ms).
 *
 * Modifiers: Shift, Ctrl, Alt and Super from the modifier bytes the
 * sources send (any source's counts), Caps Lock and Num Lock as locked
 * modifiers that follow the lock keys seen (Num Lock starts on, as hid's
 * does), in XKB's bits for the generated keymap (KEYMAP_MOD_*), sent as
 * wl_keyboard.modifiers whenever they change and after every enter.
 *
 * Keys typed while no window has the keys are kept, at most EARLY_MAX key
 * events with the modifiers each was typed with, and handed to the next
 * window that takes the keys, after its enter, if they are at most
 * EARLY_KEEP old by then: the shell's prompt is up a few ms before its
 * terminal's window is mapped (at boot, after the console or the
 * compositor restarted), and what is typed at it must not be lost, as the
 * console kept keys for the first to listen. Older ones go nowhere.
 *
 * Terminal text (serialin, a QEMU test): each byte or escape sequence
 * (<termkeys.h>) becomes the presses that type it on the US layout: Shift
 * or Ctrl pressed around the key as needed, so a client sees exactly what
 * a keyboard would have sent. */
#include <jwl/wayland.h>
#include <keymap.h>
#include "seat.h"

#define REPEAT_RATE   30    /* keys a second: hid's */
#define REPEAT_DELAY  500   /* ms before the first repeat: hid's */
#define U_CAPS_LOCK   0x39
#define U_NUM_LOCK    0x53
#define U_LCTRL       0xe0
#define U_LSHIFT      0xe1

static handle_t keymap_vmo = HANDLE_INVALID;   /* the XKB text, kept pages */

/* A key held down. */
struct held {
    uint8_t usage;     /* HID usage */
    uint8_t src;       /* the source that pressed it */
    bool    hidden;    /* a key no client sees: its release is dropped too */
    uint8_t reserved;
    uint32_t code;     /* evdev code (0 for a hidden key with none) */
};
static struct held held[KEYS_HELD_MAX];
static unsigned nheld;
static uint8_t src_mods[SOURCES_MAX];          /* each source's last modifier byte */
static uint32_t locked = KEYMAP_MOD_NUM;       /* KEYMAP_MOD_CAPS / _NUM on */
static uint32_t sent_depressed, sent_locked;   /* what the focused client was last told */

/* A key event typed while no window had the keys, the modifiers then, and when. */
#define EARLY_MAX  128
#define EARLY_KEEP (5 * NS_PER_S)
struct early {
    uint32_t code, state;            /* evdev code, WL_KEYBOARD_KEY_STATE_* */
    uint32_t depressed, locked;      /* KEYMAP_MOD_* */
    uint64_t at;                     /* uptime ns */
};
static struct early early[EARLY_MAX];
static unsigned nearly;              /* kept; more are dropped */

status_t keyboard_init(void)
{
    uint64_t size = (keymap_us.xkb_size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    status_t st = jam_vmo_create(size, VMO_KEEP_PAGES, HANDLE_INVALID, &keymap_vmo);
    if (st == OK)
        st = jam_vmo_write(keymap_vmo, 0, keymap_us.xkb, keymap_us.xkb_size);
    return st;
}

static uint32_t depressed(void)
{
    uint8_t all = 0;
    for (unsigned i = 0; i < SOURCES_MAX; i++)
        all |= src_mods[i];
    return keymap_mods_of_hid(all);
}

/* The client with the keyboard focus, if it can be told anything. */
static struct comp_window *focus_target(void)
{
    struct comp_window *w = seat_focused();
    return w && surface_live(w->surface) ? w : NULL;
}

static void send_modifiers(struct comp_client *cl, uint32_t serial)
{
    struct seat_client *sc = seat_of(cl);
    for (struct seat_res *r = sc->res[SEAT_KEYBOARD]; r; r = r->next)
        (void)jwl_wl_keyboard_send_modifiers(cl->conn, r->id, serial, sent_depressed, 0,
                                             sent_locked, 0);   /* a dead conn: torn down */
}

/* The modifiers to the focused client if they changed. */
static void modifiers_changed(void)
{
    uint32_t d = depressed();
    if (d == sent_depressed && locked == sent_locked)
        return;
    sent_depressed = d;
    sent_locked = locked;
    struct comp_window *w = focus_target();
    if (w)
        send_modifiers(w->surface->client, comp_serial());
}

/* The evdev codes of the keys clients see held, each once, for
 * wl_keyboard.enter. */
static unsigned held_codes(uint32_t *out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < nheld; i++) {
        bool dup = false;
        for (unsigned j = 0; j < n; j++)
            dup |= out[j] == held[i].code;
        if (!held[i].hidden && held[i].code && !dup)
            out[n++] = held[i].code;
    }
    return n;
}

static void send_enter(struct comp_window *w, const struct seat_res *only)
{
    struct comp_client *cl = w->surface->client;
    uint32_t codes[KEYS_HELD_MAX];
    unsigned n = held_codes(codes);
    uint32_t serial = comp_serial();
    sent_depressed = depressed();
    sent_locked = locked;
    for (struct seat_res *r = seat_of(cl)->res[SEAT_KEYBOARD]; r; r = r->next)
        if (!only || r == only)
            (void)jwl_wl_keyboard_send_enter(cl->conn, r->id, serial, w->surface->id, codes,
                                             n * sizeof(codes[0]));
    serial = comp_serial();
    for (struct seat_res *r = seat_of(cl)->res[SEAT_KEYBOARD]; r; r = r->next)
        if (!only || r == only)
            (void)jwl_wl_keyboard_send_modifiers(cl->conn, r->id, serial, sent_depressed, 0,
                                                 sent_locked, 0);
}

/* The keys kept while no window had them, to w's client (just entered),
 * each after the modifiers it was typed with, those no older than
 * EARLY_KEEP; then the modifiers as they are. */
static void send_early(struct comp_window *w)
{
    struct comp_client *cl = w->surface->client;
    uint64_t t0 = now();
    for (unsigned i = 0; i < nearly; i++) {
        const struct early *e = &early[i];
        if (t0 - e->at > EARLY_KEEP)
            continue;
        if (e->depressed != sent_depressed || e->locked != sent_locked) {
            sent_depressed = e->depressed;
            sent_locked = e->locked;
            send_modifiers(cl, comp_serial());
        }
        uint32_t serial = comp_serial(), t = comp_ms(now());
        for (struct seat_res *r = seat_of(cl)->res[SEAT_KEYBOARD]; r; r = r->next)
            (void)jwl_wl_keyboard_send_key(cl->conn, r->id, serial, t, e->code, e->state);
    }
    nearly = 0;
    if (depressed() != sent_depressed || locked != sent_locked) {
        sent_depressed = depressed();
        sent_locked = locked;
        send_modifiers(cl, comp_serial());
    }
}

void keyboard_enter(struct comp_window *w)
{
    if (!surface_live(w->surface))
        return;
    send_enter(w, NULL);
    send_early(w);
}

void keyboard_leave(struct comp_window *w)
{
    struct comp_client *cl = w->surface->client;
    if (!surface_live(w->surface))
        return;   /* gone with its surface: the client knows */
    uint32_t serial = comp_serial();
    for (struct seat_res *r = seat_of(cl)->res[SEAT_KEYBOARD]; r; r = r->next)
        (void)jwl_wl_keyboard_send_leave(cl->conn, r->id, serial, w->surface->id);
}

status_t keyboard_create(struct comp_client *cl, uint32_t id, uint32_t version)
{
    struct seat_res *r;
    status_t st = seat_res_add(cl, SEAT_KEYBOARD, id, version, &r);
    if (st != OK)
        return st;
    handle_t map;
    st = jam_handle_duplicate(keymap_vmo, RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER, &map);
    if (st != OK)
        return comp_no_memory(cl, id, "no handle for the keymap");
    st = jwl_wl_keyboard_send_keymap(cl->conn, id, JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, map,
                                     keymap_us.xkb_size);
    if (st == OK && version >= JWL_WL_KEYBOARD_EV_REPEAT_INFO_SINCE)
        st = jwl_wl_keyboard_send_repeat_info(cl->conn, id, REPEAT_RATE, REPEAT_DELAY);
    struct comp_window *w = focus_target();
    if (st == OK && w && w->surface->client == cl)
        send_enter(w, r);
    return st;
}

/* ---- keys ------------------------------------------------------------------------- */

static void send_key(uint32_t code, uint32_t state)
{
    struct comp_window *w = focus_target();
    if (!w) {   /* kept for the next window, the old ones forgotten first */
        uint64_t t = now();
        unsigned keep = 0;
        for (unsigned i = 0; i < nearly; i++)
            if (t - early[i].at <= EARLY_KEEP)
                early[keep++] = early[i];
        nearly = keep;
        if (nearly < EARLY_MAX)
            early[nearly++] = (struct early){ code, state, depressed(), locked, t };
    }
    if (!w)
        return;
    struct comp_client *cl = w->surface->client;
    uint32_t serial = comp_serial(), t = comp_ms(now());
    for (struct seat_res *r = seat_of(cl)->res[SEAT_KEYBOARD]; r; r = r->next)
        (void)jwl_wl_keyboard_send_key(cl->conn, r->id, serial, t, code, state);
}

static int find_held(unsigned src, uint16_t usage)
{
    for (unsigned i = 0; i < nheld; i++)
        if (held[i].src == src && held[i].usage == usage)
            return (int)i;
    return -1;
}

/* Does another keyboard hold a key clients see with this code? (The key
 * is pressed once for them, and released once, when the last lets go.) */
static bool code_held(uint32_t code)
{
    for (unsigned i = 0; i < nheld; i++)
        if (!held[i].hidden && held[i].code == code)
            return true;
    return false;
}

static void press(unsigned src, uint16_t usage, uint8_t mods)
{
    seat_keys++;
    if (find_held(src, usage) >= 0 || nheld == KEYS_HELD_MAX || usage >= KEYMAP_HID_USAGES)
        return;   /* pressed twice (a broken source), or too many keys at once */
    uint32_t code = keymap_evdev_of_hid(usage);
    bool hidden = focus_reserved_key(usage, mods);
    if (hidden)
        seat_reserved++;
    if (usage == U_CAPS_LOCK)
        locked ^= KEYMAP_MOD_CAPS;
    if (usage == U_NUM_LOCK)
        locked ^= KEYMAP_MOD_NUM;
    bool shown = !hidden && code && !code_held(code);
    held[nheld++] = (struct held){ .usage = (uint8_t)usage, .src = (uint8_t)src,
                                   .hidden = hidden, .code = code };
    if (shown)
        send_key(code, JWL_WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void release_at(unsigned i)
{
    struct held k = held[i];
    held[i] = held[--nheld];
    if (!k.hidden && k.code && !code_held(k.code))
        send_key(k.code, JWL_WL_KEYBOARD_KEY_STATE_RELEASED);
}

void keyboard_key(unsigned src, uint16_t usage, uint8_t state, uint8_t mods)
{
    if (src >= SOURCES_MAX || state == INPUT_KEY_REPEAT)
        return;   /* clients repeat keys themselves */
    if (state == INPUT_KEY_DOWN)
        press(src, usage, mods);
    else if (state == INPUT_KEY_UP) {
        int i = find_held(src, usage);
        if (i >= 0)
            release_at((unsigned)i);
    }
    src_mods[src] = mods;
    modifiers_changed();
}

void keyboard_source_gone(unsigned src)
{
    if (src >= SOURCES_MAX)
        return;
    for (unsigned i = nheld; i-- > 0;)
        if (held[i].src == src)
            release_at(i);
    src_mods[src] = 0;
    modifiers_changed();
}

/* ---- terminal text ---------------------------------------------------------------- */

/* The usage and modifier byte that type k; false if no key does. */
static bool typing(const struct termkey *k, uint16_t *usage, uint8_t *mods)
{
    *mods = 0;
    if (k->usage) {
        *usage = k->usage;
        return true;
    }
    uint32_t cp = k->cp, code, kmods;
    if (cp >= 1 && cp <= 26) {   /* a control character: Ctrl and its letter */
        cp = 'a' + cp - 1;
        *mods = INPUT_MOD_LCTRL;
    }
    if (!keymap_typing(&keymap_us, cp, &code, &kmods))
        return false;
    bool shift = (kmods & KEYMAP_MOD_SHIFT) != 0;
    if (keymap_us.keys[code].type == KEYMAP_ALPHABETIC && (locked & KEYMAP_MOD_CAPS))
        shift = !shift;   /* Caps Lock is on: Shift gives the other case */
    if (shift)
        *mods |= INPUT_MOD_LSHIFT;
    *usage = (uint16_t)keymap_hid_of_evdev(code);
    return *usage != 0;
}

/* One key typed by source src: its modifiers down, the key down and up,
 * the modifiers up, each event with the modifier byte of the moment. */
static void type_key(unsigned src, const struct termkey *k)
{
    uint16_t usage;
    uint8_t mods, now_mods = 0;
    if (!typing(k, &usage, &mods))
        return;   /* nothing on the layout types it */
    if (mods & INPUT_MOD_LCTRL)
        keyboard_key(src, U_LCTRL, INPUT_KEY_DOWN, now_mods |= INPUT_MOD_LCTRL);
    if (mods & INPUT_MOD_LSHIFT)
        keyboard_key(src, U_LSHIFT, INPUT_KEY_DOWN, now_mods |= INPUT_MOD_LSHIFT);
    keyboard_key(src, usage, INPUT_KEY_DOWN, now_mods);
    keyboard_key(src, usage, INPUT_KEY_UP, now_mods);
    if (mods & INPUT_MOD_LSHIFT)
        keyboard_key(src, U_LSHIFT, INPUT_KEY_UP, now_mods &= (uint8_t)~INPUT_MOD_LSHIFT);
    if (mods & INPUT_MOD_LCTRL)
        keyboard_key(src, U_LCTRL, INPUT_KEY_UP, now_mods &= (uint8_t)~INPUT_MOD_LCTRL);
}

void keyboard_text(unsigned src, struct termkeys *t, const uint8_t *bytes, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        struct termkey k[2];
        unsigned got = termkeys_byte(t, bytes[i], k);
        for (unsigned j = 0; j < got; j++)
            type_key(src, &k[j]);
    }
}
