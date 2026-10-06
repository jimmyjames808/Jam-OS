/* console: the window mode (console.h): the terminal as a Wayland client
 * of the compositor, through libjwl (<jwl_client.h>).
 *
 * init hands a console the client end of /svc/wayland (WAYLAND_ROLE) when
 * a compositor owns the screen; without one the console takes the
 * framebuffer as it always did (screen.c). In window mode:
 *   - the text model, the kernel log and its notices, the console
 *     protocol and the copy to COM1 are as ever; only the drawing moves:
 *     winpaint.c draws the cell grid into the window's buffers;
 *   - the window opens centred (the compositor places it) at a size that
 *     leaves the desktop around it (term_grid); it is resizable, and every
 *     size the compositor gives it re-grids the text (text_regrid): lines
 *     keep their cells, cut or padded at the new width, as a terminal
 *     emulator does; nothing is re-wrapped;
 *   - the cells are JetBrains Mono's (cells.h), or the 8x16 bitmap's with
 *     the argument "font=bitmap" (init's, from terminal.font) or after
 *     console.set_font asks for it, WIN_PAD pixels in from each edge; the
 *     text fills the window from the top (view.c);
 *   - keys come from wl_keyboard while the window has the focus, into the
 *     same focus stack as a keyboard driver's (keys.c), so Ctrl+C reaches
 *     the shell below a program as before; Super+Enter asks init for
 *     another terminal instead (keys.c, terminal_ask); the pointer's
 *     movement, buttons and wheel become the mouse reports a program that
 *     asked for the mouse gets, or the wheel scrolls back;
 *   - the close box: the first terminal stays (it is the system's: init
 *     restarts it whatever happens, so closing it would only bring it
 *     back) and says so; any other terminal's console ends with code 0,
 *     which tells init to close that terminal and end its shell;
 *   - when the compositor dies, libjwl connects again by itself and makes
 *     the window again; the grid is then drawn again in full.
 *
 * Connecting never blocks the loop: the svc protocol's connect is a call
 * to the compositor, which waits while a restarted compositor comes up,
 * so a thread of its own (the connector) makes it and hands the new
 * channel back over a private channel; libjwl's connect function only
 * asks and collects (ERR_SHOULD_WAIT until a channel has come). The
 * shared /svc/wayland channel is one endpoint every holder shares, so
 * its replies must be the kernel's to route: a call does that, a
 * request sent without waiting would let another holder read its reply. */
#include <idl/svc.h>
#include <jwl_client.h>
#include "console.h"

#define CONNECT_WAIT (5 * NS_PER_S)   /* svc.connect: a restarted compositor comes up */
#define CONNECTOR_STACK (16u << 10)

bool window_mode;
unsigned term_no = 1;
bool closing;
bool font_bitmap;
struct cell_look look = { GW, GH, 0, NULL, NULL };

static struct jwl_client *wl;
static struct jwl_window *win;
static bool no_windows;            /* the compositor has no xdg_wm_base: until it is new */
static struct pointer_track ptr;   /* where the pointer was, and the buttons held */
static uint32_t shape_asked;       /* the pointer's shape asked of the compositor (0: none) */
static int32_t win_w, win_h;       /* the size the compositor gave the window (0: none yet) */

/* ---- the connector thread ------------------------------------------------------- */

static handle_t wl_svc;            /* /svc/wayland's shared channel, client end */
static handle_t ask_mine;          /* our end of the connector's channel */
static handle_t ask_theirs;        /* the connector's end */
static bool asking;                /* a request is with the connector, unanswered */
static uint8_t connector_stack[CONNECTOR_STACK] __attribute__((aligned(16)));

/* What the connector writes back (with the channel, when st is OK). */
struct connect_answer {
    int32_t  st;
    uint32_t reserved;
};

/* Each byte on its channel asks for one connection; each answer carries
 * it. It ends when the loop's end closes (never: it lives as long as the
 * process). */
static void connector(void *arg)
{
    (void)arg;
    for (;;) {
        signals_t seen = 0;
        if (jam_object_wait_one(ask_theirs, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                                &seen) != OK || !(seen & SIG_READABLE))
            return;
        uint8_t go;
        uint32_t n = 0, nh = 0;
        if (drv_channel_read(ask_theirs, &go, 1, &n, NULL, 0, &nh) != OK)
            return;
        handle_t ch = HANDLE_INVALID;
        struct connect_answer a = { svc_connect_until(wl_svc, now() + CONNECT_WAIT, &ch), 0 };
        if (jam_channel_write(ask_theirs, &a, sizeof(a), a.st == OK ? &ch : NULL,
                              a.st == OK ? 1 : 0) != OK && a.st == OK)
            jam_handle_close(ch);
    }
}

/* libjwl's connect function: the connector's answer if one has come,
 * else ask (once) and ERR_SHOULD_WAIT. */
static status_t wl_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    struct connect_answer a;
    handle_t h = HANDLE_INVALID;
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ask_mine, &a, sizeof(a), &n, &h, 1, &nh);
    if (st == OK) {
        asking = false;
        if (n == sizeof(a) && a.st == OK && nh == 1) {
            *out = h;
            return OK;
        }
        if (nh)
            jam_handle_close(h);
        return n == sizeof(a) && a.st != OK ? a.st : ERR_INTERNAL;
    }
    if (st != ERR_SHOULD_WAIT)
        return st;
    if (!asking) {
        uint8_t go = 1;
        st = jam_channel_write(ask_mine, &go, 1, NULL, 0);
        if (st != OK)
            return st;
        asking = true;
    }
    return ERR_SHOULD_WAIT;
}

/* ---- the window ------------------------------------------------------------------ */

/* The grid becomes c x r: the text (text_regrid) and the window's copies
 * of it (winpaint.c). false: no memory for it; it stays as it was. */
static bool regrid(uint32_t c, uint32_t r)
{
    if (c == cols && r == rows)
        return true;
    if (!text_regrid(c, r) || !paint_regrid()) {
        printf("console: no memory for a %ux%u grid: it stays %ux%u\n", c, r, cols, rows);
        return false;
    }
    printf("console: the terminal is %ux%u cells\n", cols, rows);
    return true;
}

/* Open the window once the client is ready and knows the output. */
static void open_window(void)
{
    if (win || no_windows || jwl_client_status(wl) != OK)
        return;
    const struct jwl_client_info *in = jwl_client_info(wl);
    uint32_t c, r;
    term_grid(&look, in->output_width, in->output_height, &c, &r);
    (void)regrid(c, r);   /* else at the size it has */
    char title[24];
    if (term_no > 1)
        snprintf(title, sizeof(title), "Terminal %u", term_no);
    else
        snprintf(title, sizeof(title), "Terminal");
    struct jwl_window_config cfg = {
        .title = title, .app_id = "jamos.terminal", .resizable = true,
    };
    window_size(&look, cols, rows, &cfg.width, &cfg.height);
    status_t st = jwl_window_create(wl, &cfg, &win);
    if (st == OK) {
        printf("console: window mode: terminal %u opens a %ux%u-cell window (%dx%d pixels) on a "
               "%dx%d output\n", term_no, cols, rows, cfg.width, cfg.height, in->output_width,
               in->output_height);
        return;
    }
    win = NULL;
    no_windows = true;   /* tried again with the next connection, not at every event */
    printf("console: window mode: no window (%s)%s\n", status_str(st),
           st == ERR_NOT_SUPPORTED ? ": the compositor offers no xdg_wm_base; the text goes to "
                                     "the serial port only until it does" : "");
}

/* The compositor's size for the window (resized, tiled, maximised), or
 * only its states (the focus): a new size re-grids (and has new buffers,
 * which winpaint.c draws in full); the first configure after a reconnect
 * draws everything again. */
static void configured(const struct jwl_event *ev)
{
    uint32_t c, r;
    win_w = ev->configure.width;
    win_h = ev->configure.height;
    grid_of_size(&look, win_w, win_h, &c, &r);
    (void)regrid(c, r);
    if (ev->configure.rebuilt)
        paint_forget();
    dirty = true;
}

static void close_asked(void)
{
    if (term_no == 1) {
        static const char m[] = "[the first terminal stays open: close the others, or type "
                                "exit in them]";
        notice_out(m, sizeof(m) - 1);
        dirty = true;
        return;
    }
    printf("console: terminal %u: its window was closed\n", term_no);
    closing = true;   /* main ends with 0: init closes the terminal */
}

static void key(const struct jwl_event *ev)
{
    struct input_key_event k;
    if (!key_of_wayland(ev->key.code, ev->key.state, ev->key.mods, ev->key.cp, &k))
        return;
    key_event(k.usage, k.state, k.mods, k.codepoint, false);
}

/* The text bar while the pointer is over the text, the arrow over the
 * padding (wp-cursor-shape-v1, through libjwl: sent at each enter too). */
static void pointer_shape(const struct jwl_event *ev)
{
    if (ev->type == JWL_EV_POINTER_LEAVE) {
        shape_asked = 0;   /* the next enter decides again */
        return;
    }
    if (ev->type != JWL_EV_POINTER_ENTER && ev->type != JWL_EV_POINTER_MOTION)
        return;
    uint32_t want = pointer_shape_at(&look, cols, rows, ev->pointer.x >> 8, ev->pointer.y >> 8);
    if (want != shape_asked && jwl_client_set_cursor(wl, want) == OK)
        shape_asked = want;
}

static void take(const struct jwl_event *ev)
{
    struct input_mouse_event m;
    switch (ev->type) {
    case JWL_EV_CONFIGURE:
        configured(ev);
        break;
    case JWL_EV_CLOSE:
        close_asked();
        break;
    case JWL_EV_KEY:
        key(ev);
        break;
    case JWL_EV_POINTER_ENTER:
    case JWL_EV_POINTER_LEAVE:
    case JWL_EV_POINTER_MOTION:
    case JWL_EV_POINTER_BUTTON:
    case JWL_EV_POINTER_AXIS:
        pointer_shape(ev);
        if (mouse_of_wayland(&ptr, ev, &m))
            mouse_event(&m);
        break;
    case JWL_EV_RECONNECTED:
        no_windows = false;   /* a new compositor: it may have windows now */
        shape_asked = 0;
        paint_forget();
        dirty = true;
        break;
    case JWL_EV_DEAD:
        printf("console: window mode: the compositor ended our connection for good (%s): the "
               "text goes to the serial port only\n", status_str(ev->conn.why));
        break;
    }
}

/* ---- the console's side ------------------------------------------------------------ */

void window_set_font(bool bitmap)
{
    font_bitmap = bitmap;
    if (!window_mode || bitmap == !look.reg)
        return;   /* the full screen's is the bitmap always; or no change */
    /* The console draws on its one thread, between events: nothing is
     * drawing with the fonts closed here. */
    if (bitmap)
        cell_look_close(&look);
    else if (!cell_look_open(&look))
        printf("console: no memory for the smooth font: the terminal keeps the 8x16 one\n");
    printf("console: terminal %u: the %s font, %dx%d-pixel cells\n", term_no,
           look.reg ? "smooth (JetBrains Mono)" : "8x16 bitmap", look.w, look.h);
    if (win_w > 0) {   /* the window's size stays; the grid in it changes */
        uint32_t c, r;
        grid_of_size(&look, win_w, win_h, &c, &r);
        (void)regrid(c, r);
    }
    paint_forget();
    dirty = true;
}

bool window_init(handle_t svc, unsigned term)
{
    window_mode = true;
    wl_svc = svc;
    term_no = term ? term : 1;
    if (!font_bitmap)   /* the look starts as the bitmap's */
        window_set_font(false);
    handle_t thread;
    status_t st = jam_channel_create(&ask_mine, &ask_theirs);
    if (st == OK)
        st = jam_port_bind(port, ask_mine, KEY(K_WL, 0), SIG_READABLE, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = thread_spawn("connector", connector, NULL, connector_stack, sizeof(connector_stack),
                          &thread);
    struct jwl_client_config cfg = { .connect = wl_connect, .name = "console" };
    if (st == OK)
        st = jwl_client_create(&cfg, &wl);
    if (st == OK)
        st = jwl_client_bind_port(wl, port, KEY(K_WL, 0));
    if (st != OK) {
        printf("console: window mode: can't start (%s): the text goes to the serial port only\n",
               status_str(st));
        return false;
    }
    printf("console: window mode (terminal %u): the compositor draws the screen\n", term_no);
    return true;
}

void window_event(void)
{
    if (!wl)
        return;
    (void)jwl_client_dispatch(wl);   /* a dead client: its JWL_EV_DEAD says so */
    open_window();
    struct jwl_event ev;
    while (jwl_client_next_event(wl, &ev) == OK)
        take(&ev);
}

uint64_t window_deadline(void)
{
    return wl ? jwl_client_deadline(wl) : DEADLINE_NEVER;
}

struct jwl_window *window_now(void)
{
    return wl && win && jwl_client_status(wl) == OK && jwl_window_configured(win) ? win : NULL;
}
