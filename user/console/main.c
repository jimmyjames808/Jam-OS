/* console: the text terminal on the screen (M7 Track C).
 *
 * Owns the boot framebuffer once it has started (framebuffer_take: a WC
 * VMO of it; the kernel stops drawing its log and draws again if this
 * process dies). Serves the `console` protocol (abi/idl/console.idl) to
 * programs and the `input` protocol on each channel connect_input hands
 * out (a HID driver, the serial source).
 *
 * Startup handles:
 *   SR_RESOURCE     the root resource with RIGHT_READ (klog_open) and
 *                   RIGHT_WRITE (framebuffer_take, serial_write)
 *   SR_USER + n     server ends of `console` channels (n = 0..7): init's;
 *                   clients share one by duplicating the client end
 *
 * The screen: a grid of 8x16 cells. Committed lines live in a scrollback
 * ring; the line the programs are writing (the "current line", where the
 * cursor is) is always the bottom row. Kernel log lines (a klog reader)
 * are committed ABOVE the current line, so a log line never breaks up the
 * prompt the shell is editing. Colours: kernel lines grey, lines a process
 * logged through debug_write ("[name] ...") green, program output white
 * (ESC [ ... m changes it). Shift+PageUp/PageDown (or PageUp/PageDown from
 * a serial terminal) scroll back; any other key goes back to the bottom.
 *
 * Drawing only ever writes to the framebuffer (reading WC memory back is
 * very slow): a shadow grid remembers what each screen cell shows, and a
 * render pass (at most every 16 ms) draws the cells that changed.
 *
 * Program output (console.write) is also written to COM1 as it is (the
 * kernel log goes there by itself), so a serial terminal, and the QEMU
 * tests, see the same session. */
#include <os.h>
#include <idl/console.h>
#include <idl/input.h>

#define GW 8
#define GH 16
#define MAX_COLS   480
#define MAX_ROWS   180
#define SCROLLBACK 4000         /* committed lines kept */
#define MAX_CLIENTS 8
#define MAX_SOURCES 16
#define MAX_FOCUS   8
#define PENDING_KEYS 128
#define RENDER_NS  16000000ull
#define KLOG_BUF   16384

extern const uint8_t font_8x16[128][16];

/* Palette indices. */
enum { C_BLACK, C_RED, C_GREEN, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN, C_GREY,
       C_DARK, C_BRED, C_BGREEN, C_BYELLOW, C_BBLUE, C_BMAGENTA, C_BCYAN, C_WHITE };
static const uint32_t rgb[16] = {
    0x101018, 0xcc4444, 0x44aa44, 0xccaa33, 0x4466cc, 0xaa44aa, 0x44aaaa, 0xb0b0b0,
    0x707070, 0xff6666, 0x66dd66, 0xffdd55, 0x6699ff, 0xdd77dd, 0x66dddd, 0xf0f0f0,
};
#define ATTR(fg, bg) ((uint8_t)((fg) | (bg) << 4))
#define A_KERNEL  ATTR(C_GREY, C_BLACK)
#define A_STAMP   ATTR(C_DARK, C_BLACK)
#define A_PROC    ATTR(C_BGREEN, C_BLACK)
#define A_OUT     ATTR(C_WHITE, C_BLACK)

struct cell {
    uint8_t ch, attr;
};

/* ---- the text model -------------------------------------------------------- */

static uint32_t cols = 80, rows = 25;
static struct cell *sb;            /* SCROLLBACK lines of cols cells */
static uint64_t committed;         /* lines ever committed; line i at i % SCROLLBACK */
static struct cell cur[MAX_COLS];  /* the current line */
static uint32_t cur_x;             /* the cursor */
static uint8_t out_attr = A_OUT;   /* program output colour (ESC [ m) */
static uint32_t view_back;         /* lines scrolled back (0: at the bottom) */
static bool dirty = true;

static struct cell *line(uint64_t i)
{
    return &sb[(i % SCROLLBACK) * cols];
}

static void blank(struct cell *c, uint32_t n, uint8_t attr)
{
    for (uint32_t i = 0; i < n; i++)
        c[i] = (struct cell){ ' ', attr };
}

static void commit(const struct cell *c)
{
    struct cell *l = line(committed++);
    for (uint32_t i = 0; i < cols; i++)
        l[i] = c[i];
    if (view_back && view_back < SCROLLBACK - rows)
        view_back++;   /* keep the view where it was */
    dirty = true;
}

/* A kernel log line (without its newline), wrapped at cols, above the
 * current line. */
static void kernel_line(const char *s, size_t n)
{
    struct cell l[MAX_COLS];
    /* "[    1.234567] " then the text; "[name] " after the stamp: a process. */
    size_t stamp = 0;
    if (n > 2 && s[0] == '[') {
        for (size_t i = 1; i < n && i < 20; i++)
            if (s[i] == ']') {
                stamp = i + 1;
                break;
            }
    }
    uint8_t text = A_KERNEL;
    if (stamp + 1 < n && s[stamp] == ' ' && s[stamp + 1] == '[')
        for (size_t i = stamp + 2; i < n && i < stamp + 40; i++)
            if (s[i] == ']') {
                text = A_PROC;
                break;
            }
    uint32_t x = 0;
    blank(l, cols, A_KERNEL);
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch == '\r')
            continue;
        if (ch == '\t')
            ch = ' ';
        if (ch < 0x20 || ch > 0x7e)
            ch = '?';
        if (x == cols) {
            commit(l);
            blank(l, cols, A_KERNEL);
            x = 0;
        }
        l[x++] = (struct cell){ ch, i < stamp ? A_STAMP : text };
    }
    commit(l);
}

/* ---- program output: a small terminal on the current line ------------------ */

enum { ES_NONE, ES_ESC, ES_CSI };
static int esc_state;
static char esc_buf[16];
static uint32_t esc_len;

static void new_line(void)
{
    commit(cur);
    blank(cur, cols, A_OUT);
    cur_x = 0;
}

static void clear_screen(void)
{
    struct cell l[MAX_COLS];
    blank(l, cols, A_OUT);
    commit(cur);
    for (uint32_t i = 0; i + 1 < rows; i++)
        commit(l);
    blank(cur, cols, A_OUT);
    cur_x = 0;
    view_back = 0;
}

static void sgr(uint32_t p)
{
    uint8_t fg = out_attr & 15, bg = out_attr >> 4;
    if (p == 0) {
        fg = C_WHITE;
        bg = C_BLACK;
    } else if (p == 1) {
        fg |= 8;
    } else if (p >= 30 && p <= 37) {
        fg = (uint8_t)(p - 30) | (fg & 8);
    } else if (p == 39) {
        fg = C_WHITE;
    } else if (p >= 40 && p <= 47) {
        bg = (uint8_t)(p - 40);
    } else if (p == 49) {
        bg = C_BLACK;
    } else if (p >= 90 && p <= 97) {
        fg = (uint8_t)(p - 90 + 8);
    }
    out_attr = ATTR(fg, bg);
}

static void csi(char final)
{
    uint32_t params[4] = { 0 }, np = 0;
    bool any = false;
    for (uint32_t i = 0; i < esc_len && np < 4; i++) {
        char c = esc_buf[i];
        if (c >= '0' && c <= '9') {
            params[np] = params[np] * 10 + (uint32_t)(c - '0');
            any = true;
        } else if (c == ';') {
            np++;
        }
    }
    if (any || esc_len)
        np++;
    uint32_t n = params[0] ? params[0] : 1;
    switch (final) {
    case 'm':
        if (np == 0)
            sgr(0);
        for (uint32_t i = 0; i < np; i++)
            sgr(params[i]);
        break;
    case 'K':   /* erase to the end of the line */
        blank(cur + cur_x, cols - cur_x, out_attr);
        break;
    case 'C':
        cur_x = cur_x + n < cols ? cur_x + n : cols - 1;
        break;
    case 'D':
        cur_x = cur_x > n ? cur_x - n : 0;
        break;
    case 'J':
        if (params[0] == 2)
            clear_screen();
        break;
    case 'H':
        cur_x = 0;
        break;
    }
}

static void out_char(uint8_t ch)
{
    if (esc_state == ES_ESC) {
        esc_state = ch == '[' ? ES_CSI : ES_NONE;
        esc_len = 0;
        return;
    }
    if (esc_state == ES_CSI) {
        if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?') {
            if (esc_len < sizeof(esc_buf))
                esc_buf[esc_len++] = (char)ch;
        } else {
            csi((char)ch);
            esc_state = ES_NONE;
        }
        return;
    }
    switch (ch) {
    case 0x1b:
        esc_state = ES_ESC;
        return;
    case '\n':
        new_line();
        return;
    case '\r':
        cur_x = 0;
        return;
    case '\b':
        if (cur_x)
            cur_x--;
        return;
    case '\t':
        do
            out_char(' ');
        while (cur_x % 8);
        return;
    }
    if (ch < 0x20)
        return;
    if (ch > 0x7e)
        ch = '?';   /* UTF-8 continuation bytes and the like: the font is ASCII */
    if (cur_x >= cols)
        new_line();
    cur[cur_x++] = (struct cell){ ch, out_attr };
}

/* ---- the framebuffer ------------------------------------------------------- */

static volatile uint32_t *fbp;
static struct fb_info fbi;
static uint32_t native[16];
static struct cell *shadow;        /* rows * cols: what each screen cell shows */
static uint8_t *shadow_cursor;     /* rows * cols: drawn inverted */

static void draw_cell(uint32_t x, uint32_t y, struct cell c, bool inverse)
{
    uint32_t fg = native[c.attr & 15], bg = native[c.attr >> 4];
    if (inverse) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    const uint8_t *g = font_8x16[c.ch & 0x7f];
    volatile uint32_t *row = fbp + (uint64_t)y * GH * (fbi.pitch / 4) + x * GW;
    for (int i = 0; i < GH; i++, row += fbi.pitch / 4) {
        uint8_t bits = g[i];
        for (int j = 0; j < GW; j++)
            row[j] = (bits & (0x80 >> j)) ? fg : bg;
    }
}

static void render(void)
{
    dirty = false;
    if (!fbp)
        return;
    struct cell empty = { ' ', A_OUT };
    uint64_t first;   /* the committed line on screen row 0 */
    uint32_t shown = view_back ? rows : rows - 1;
    uint64_t end = committed > view_back ? committed - view_back : 0;
    first = end > shown ? end - shown : 0;
    uint64_t oldest = committed > SCROLLBACK ? committed - SCROLLBACK : 0;
    for (uint32_t y = 0; y < rows; y++) {
        const struct cell *l = NULL;
        bool cursor_row = false;
        uint64_t i = first + y;
        if (!view_back && y == rows - 1) {
            l = cur;
            cursor_row = true;
        } else if (i < end && i >= oldest) {
            l = line(i);
        }
        for (uint32_t x = 0; x < cols; x++) {
            struct cell c = l ? l[x] : empty;
            uint8_t inv = cursor_row && x == cur_x;
            struct cell *s = &shadow[y * cols + x];
            if (s->ch != c.ch || s->attr != c.attr || shadow_cursor[y * cols + x] != inv) {
                draw_cell(x, y, c, inv);
                *s = c;
                shadow_cursor[y * cols + x] = inv;
            }
        }
    }
}

static bool screen_init(handle_t root)
{
    handle_t vmo, owner;
    status_t st = jam_framebuffer_take(root, &fbi, &vmo, &owner);
    if (st != OK) {
        printf("console: no screen (%s): serial only\n", status_str(st));
        return false;
    }
    uint64_t addr = 0;
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, fbi.size, VMAR_READ | VMAR_WRITE,
                      &addr);
    jam_handle_close(vmo);   /* the mapping keeps it; `owner` stays open for good */
    if (st != OK) {
        printf("console: can't map the framebuffer (%s)\n", status_str(st));
        jam_handle_close(owner);
        return false;
    }
    fbp = (volatile uint32_t *)(uintptr_t)addr;
    for (int i = 0; i < 16; i++)
        native[i] = ((rgb[i] >> 16 & 0xff) << fbi.red_shift) |
                    ((rgb[i] >> 8 & 0xff) << fbi.green_shift) | ((rgb[i] & 0xff) << fbi.blue_shift);
    return true;
}

/* ---- keys: the focus stack of open_keys channels ------------------------------ */

static handle_t focus[MAX_FOCUS];
static unsigned nfocus;
static struct input_key_event pending[PENDING_KEYS];
static unsigned npending;
static handle_t port;

enum { K_KLOG = 1, K_CLIENT, K_SOURCE };
#define KEY(kind, i) ((uint64_t)(kind) << 32 | (i))

/* A focus channel whose client is gone is noticed when a key is sent to it
 * (ERR_PEER_CLOSED): it is dropped and the one below gets the key. */
static void focus_drop(unsigned i)
{
    jam_handle_close(focus[i]);
    for (unsigned j = i; j + 1 < nfocus; j++)
        focus[j] = focus[j + 1];
    nfocus--;
}

static void send_key(const struct input_key_event *ev)
{
    while (nfocus) {
        status_t st = jam_channel_write(focus[nfocus - 1], ev, sizeof(*ev), NULL, 0);
        if (st == OK)
            return;
        if (st != ERR_PEER_CLOSED)
            return;   /* full (the client isn't reading): dropped */
        focus_drop(nfocus - 1);
    }
    if (npending < PENDING_KEYS)
        pending[npending++] = *ev;
}

static void key_event(uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp, bool terminal)
{
    /* Scrollback: Shift+PageUp/Down on a keyboard, PageUp/Down on a terminal. */
    bool page = usage == 0x4b || usage == 0x4e;
    if (page && (terminal || (mods & INPUT_MOD_SHIFT))) {
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

static status_t op_open_keys(void *ctx, handle_t *out)
{
    (void)ctx;
    if (nfocus == MAX_FOCUS)
        focus_drop(0);   /* the oldest loses its place */
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
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
    handle_t ch;
    int      esc;       /* terminal escape parser */
    char     params[8];
    unsigned np;
    bool     last_cr;
};
static struct source sources[MAX_SOURCES];

static status_t op_key(void *ctx, uint16_t usage, uint8_t state, uint8_t mods, uint32_t cp)
{
    (void)ctx;
    key_event(usage, state, mods, cp, false);
    return OK;
}

static status_t op_mouse(void *ctx, int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    (void)ctx;
    (void)dx;
    (void)dy;
    (void)buttons;
    /* M7: the wheel scrolls back; nothing else uses the mouse yet. */
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
        case 3: term_key(0x4c, 0x7f); return;   /* delete */
        case 5: term_key(0x4b, 0); return;      /* page up */
        case 6: term_key(0x4e, 0); return;      /* page down */
        }
        return;
    }
    }
}

static status_t op_text(void *ctx, uint16_t length, const uint8_t bytes[64])
{
    struct source *s = ctx;
    if (length > 64)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < length; i++) {
        uint8_t b = bytes[i];
        if (s->esc == 1) {   /* after ESC */
            if (b == '[' || b == 'O') {
                s->esc = 2;
                s->np = 0;
                continue;
            }
            s->esc = 0;
            term_key(0x29, 0x1b);   /* a lone ESC */
        } else if (s->esc == 2) {
            if ((b >= '0' && b <= '9') || b == ';') {
                if (s->np < sizeof(s->params) - 1)
                    s->params[s->np++] = (char)b;
                continue;
            }
            s->esc = 0;
            term_escape(s, (char)b);
            continue;
        }
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
    return OK;
}

static const struct input_ops input_ops = { op_key, op_mouse, op_text };

static status_t op_connect_input(void *ctx, handle_t *out)
{
    (void)ctx;
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

static void source_event(unsigned i)
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

/* ---- console clients ------------------------------------------------------------ */

static handle_t clients[MAX_CLIENTS];
static handle_t root;
static void klog_event(void);

static status_t op_write(void *ctx, uint16_t length, const uint8_t text[2048])
{
    (void)ctx;
    if (length > 2048)
        return ERR_INVALID_ARGS;
    /* Kernel lines logged before this write go above it: e.g. a ktest's
     * output before the shell's summary line. */
    klog_event();
    for (unsigned i = 0; i < length; i++)
        out_char(text[i]);
    jam_serial_write(root, text, length);
    dirty = true;
    return OK;
}

static status_t op_size(void *ctx, uint16_t *c, uint16_t *r)
{
    (void)ctx;
    *c = (uint16_t)cols;
    *r = (uint16_t)rows;
    return OK;
}

static status_t op_clear(void *ctx)
{
    (void)ctx;
    clear_screen();
    jam_serial_write(root, "\033[2J\033[H", 7);
    return OK;
}

static const struct console_ops console_ops = {
    op_write, op_size, op_clear, op_open_keys, op_connect_input,
};

static void client_event(unsigned i)
{
    status_t st;
    while ((st = console_serve_one(clients[i], &console_ops, NULL)) == OK)
        ;
    if (st != ERR_SHOULD_WAIT) {   /* ERR_PEER_CLOSED: that client end is gone */
        jam_port_unbind(port, clients[i], KEY(K_CLIENT, i));
        jam_handle_close(clients[i]);
        clients[i] = HANDLE_INVALID;
    }
}

/* ---- the kernel log ------------------------------------------------------------- */

static handle_t klog;
static uint64_t klog_pos;
static char klog_buf[KLOG_BUF];
static char partial[1024];
static size_t npartial;

static void klog_event(void)
{
    for (;;) {
        uint64_t first = 0;
        int64_t n = jam_klog_read(klog, klog_pos, klog_buf, sizeof(klog_buf), &first);
        if (n <= 0)
            return;
        if (first != klog_pos && klog_pos) {
            char gap[64];
            int m = snprintf(gap, sizeof(gap), "[console: %lu bytes of kernel log missed]",
                             (unsigned long)(first - klog_pos));
            npartial = 0;
            kernel_line(gap, (size_t)m);
        }
        klog_pos = first + (uint64_t)n;
        for (int64_t i = 0; i < n; i++) {
            char c = klog_buf[i];
            if (c == '\n' || npartial == sizeof(partial)) {
                kernel_line(partial, npartial);
                npartial = 0;
                if (c == '\n')
                    continue;
            }
            partial[npartial++] = c;
        }
    }
}

/* ---- main ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    root = startup_handle(SR_RESOURCE);
    status_t st = jam_port_create(&port);
    if (st != OK)
        return 1;
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        clients[i] = startup_handle(SR_USER + i);
        if (clients[i])
            jam_port_bind(port, clients[i], KEY(K_CLIENT, i), SIG_READABLE | SIG_PEER_CLOSED,
                          PORT_BIND_PERSISTENT);
    }

    bool screen = screen_init(root);
    if (screen) {
        cols = fbi.width / GW;
        rows = fbi.height / GH;
        if (cols > MAX_COLS)
            cols = MAX_COLS;
        if (rows > MAX_ROWS)
            rows = MAX_ROWS;
    }
    /* Everything allocated (and touched) up front: the console's memory
     * use doesn't move while it runs (a ktest from the shell measures the
     * kernel's free pages around each test). */
    sb = malloc((size_t)SCROLLBACK * cols * sizeof(struct cell));
    shadow = malloc((size_t)rows * cols * sizeof(struct cell));
    shadow_cursor = malloc((size_t)rows * cols);
    if (!sb || !shadow || !shadow_cursor) {
        printf("console: out of memory\n");
        return 1;
    }
    for (uint64_t i = 0; i < SCROLLBACK; i++)
        blank(line(i), cols, A_OUT);
    memset(shadow, 0, (size_t)rows * cols * sizeof(struct cell));   /* ch 0: redraw all */
    memset(shadow_cursor, 0, (size_t)rows * cols);
    blank(cur, cols, A_OUT);

    st = jam_klog_open(root, &klog);
    if (st == OK) {
        jam_port_bind(port, klog, KEY(K_KLOG, 0), SIG_READABLE, PORT_BIND_PERSISTENT);
        klog_event();   /* the boot log so far */
    } else {
        printf("console: no kernel log (%s)\n", status_str(st));
    }
    printf("console: %ux%u cells%s, %u client channel(s)\n", cols, rows,
           screen ? "" : " (no screen)", MAX_CLIENTS);
    render();

    uint64_t last = 0;
    for (;;) {
        uint64_t deadline = dirty ? last + RENDER_NS : DEADLINE_NEVER;
        struct port_packet pkt;
        st = jam_port_wait(port, deadline, &pkt);
        if (st == OK) {
            uint32_t kind = (uint32_t)(pkt.key >> 32), i = (uint32_t)pkt.key;
            switch (kind) {
            case K_KLOG:
                klog_event();
                break;
            case K_CLIENT:
                if (i < MAX_CLIENTS && clients[i])
                    client_event(i);
                break;
            case K_SOURCE:
                if (i < MAX_SOURCES)
                    source_event(i);
                break;
            }
        } else if (st != ERR_TIMED_OUT) {
            printf("console: port_wait: %s\n", status_str(st));
            return 1;
        }
        uint64_t now = (uint64_t)jam_clock_get();
        if (dirty && now >= last + RENDER_NS) {
            render();
            last = now;
        }
    }
}
