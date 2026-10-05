/* seat.h: what the seat's files share (comp.h has what other tracks use).
 *
 *   seat.c      wl_seat (capabilities, name, get_keyboard, get_pointer),
 *               each client's seat objects, the seat's part of a turn;
 *   keyboard.c  wl_keyboard: the keymap VMO, keys held, modifiers and
 *               locks, key events to the focused window's client, serial
 *               text typed as key presses;
 *   pointer.c   wl_pointer: the position, focus and the implicit grab,
 *               buttons and their serials, the wheel, set_cursor, grabs of
 *               the compositor's own;
 *   focus.c     the keyboard focus and its rules, the keys no client sees,
 *               the window manager's hooks' defaults;
 *   sources.c   input sources: the `input` protocol, a budget each;
 *   ctl.c       compctl's channels (levels, connect_input, blank,
 *               new_client, stats, the layout) and the requests to init
 *               (Ctrl+Alt+Del's reboot, Super+Enter's terminal);
 *   testwin.c   the `testwin` test power's windows.
 *
 * Pointers into the scene (a window with the keyboard or pointer focus)
 * are dropped in seat_window_gone, which scene.c calls before a window is
 * unmapped or freed, so none is ever left dangling. Nothing is ever sent
 * to a client whose connection is dead (client_alive): a client being torn
 * down still has its windows for a moment. */
#pragma once

#include <termkeys.h>
#include "comp.h"

#define SEAT_NAME     "seat0"
#define SEAT_OBJS_MAX 8u          /* per client: wl_seat, wl_keyboard and wl_pointer objects, each */
#define SEAT_KEY_CTL  (COMP_KEY_SEAT + 0x000u)   /* + compctl channel slot */
#define SEAT_KEY_SRC  (COMP_KEY_SEAT + 0x100u)   /* + input source slot */
#define SEAT_KEY_INIT (COMP_KEY_SEAT + 0x200u)   /* init's answers (Ctrl+Alt+Del, Super+Enter) */
#define SEAT_BUDGET   64u         /* requests of one source or compctl channel per turn */
#define SOURCES_MAX   16u         /* input sources at once */
#define KEYS_HELD_MAX 32u         /* keys held down at once, over every keyboard */

/* One wl_seat, wl_keyboard or wl_pointer object of a client's: the data
 * libjwl's map holds for it. */
enum seat_kind { SEAT_SEAT, SEAT_KEYBOARD, SEAT_POINTER, SEAT_KINDS };
struct seat_res {
    struct seat_res *next;         /* the client's objects of this kind */
    struct comp_client *client;
    uint32_t id;
    uint32_t version;
    enum seat_kind kind;
};

/* What the seat keeps per client (struct comp_client's seat points here). */
struct seat_client {
    struct seat_res *res[SEAT_KINDS];   /* its objects, by kind */
    uint32_t nres[SEAT_KINDS];
    bool had_window;               /* a window of its was mapped once: later ones don't take focus */
    bool motion_owed;              /* pointer motion held back while it was behind (pointer.c) */
    uint32_t enter_serial;         /* the last wl_pointer.enter's serial it got: set_cursor's */
    /* Its cursor (set_cursor): shown while the pointer is over its window. */
    bool cursor_set;               /* it asked: else the default arrow */
    struct comp_surface *cursor;   /* the surface, or NULL: no cursor at all */
    int32_t hot_x, hot_y;
};

/* seat.c */
struct seat_client *seat_of(struct comp_client *cl);       /* never NULL */
bool client_alive(const struct comp_client *cl);           /* its connection still works */
/* Does s's id still name s on a live connection (for an event naming it)? */
bool surface_live(const struct comp_surface *s);
extern uint64_t seat_keys, seat_reserved;                  /* key presses taken; kept by us */
/* A new object of kind on cl's list, entered as id's data: OK, or a
 * protocol error posted (the cap, no memory). */
status_t seat_res_add(struct comp_client *cl, enum seat_kind kind, uint32_t id, uint32_t version,
                      struct seat_res **out);
/* r off its client's list and freed (its id is libjwl's business). */
void     seat_res_free(struct seat_res *r);

/* keyboard.c */
status_t keyboard_init(void);
status_t keyboard_create(struct comp_client *cl, uint32_t id, uint32_t version);
/* A key from source src (input.key): usage, INPUT_KEY_*, hid's modifier
 * byte. */
void     keyboard_key(unsigned src, uint16_t usage, uint8_t state, uint8_t mods);
/* A terminal's bytes from source src (input.text) as the key presses that
 * type them. */
void     keyboard_text(unsigned src, struct termkeys *t, const uint8_t *bytes, unsigned n);
/* Source src went: the keys and modifiers it held are released. */
void     keyboard_source_gone(unsigned src);
/* The focus left w's client's keyboards / arrived on w (enter with the keys held). */
void     keyboard_leave(struct comp_window *w);
void     keyboard_enter(struct comp_window *w);

/* pointer.c */
void     pointer_init_position(void);
status_t pointer_create(struct comp_client *cl, uint32_t id, uint32_t version);
/* A mouse report from a source (input.mouse). */
void     pointer_report(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons);
void     pointer_turn(void);
void     pointer_window_gone(struct comp_window *w);
void     pointer_client_gone(struct comp_client *cl);

/* focus.c */
/* Is this key press one no client sees? If so it was acted on. */
bool     focus_reserved_key(uint16_t usage, uint8_t mods);
/* A button press on w's surface: the keyboard focus to it, and the window
 * manager told. */
void     focus_click(struct comp_window *w);
void     focus_window_mapped(struct comp_window *w);
void     focus_window_gone(struct comp_window *w);

/* sources.c */
status_t sources_connect(handle_t *out);
void     sources_packet(unsigned slot);
void     sources_serve(void);
bool     sources_more(void);
unsigned sources_count(void);

/* ctl.c */
status_t ctl_init(void);
void     ctl_packet(uint64_t key);
void     ctl_serve(void);
bool     ctl_more(void);
uint64_t ctl_deadline(void);
/* Ctrl+Alt+Del: ask init to reboot. */
void     ctl_reboot(void);
/* Super+Enter: ask init for another terminal. */
void     ctl_terminal(void);
/* Asked for: every key and mouse report is dropped from now on. */
bool     ctl_rebooting(void);
