/* console: the text model (committed lines in a scrollback ring, the
 * current line) and the alternate screen's grid (console.h). */
#include "console.h"

/* ---- the text model -------------------------------------------------------- */

uint32_t cols = 80, rows = 25;
static struct cell *sb;     /* SCROLLBACK lines of cols cells */
uint64_t committed;
struct cell cur[MAX_COLS];
uint32_t cur_x;
uint8_t out_attr = A_OUT;
uint8_t out_style;
uint32_t view_back;
uint64_t top_line;
bool dirty = true;

struct cell *line(uint64_t i)
{
    return &sb[(i % SCROLLBACK) * cols];
}

void blank(struct cell *c, uint32_t n, uint8_t attr)
{
    for (uint32_t i = 0; i < n; i++)
        c[i] = (struct cell){ ' ', attr, 0 };
}

struct view view_now(void)
{
    return (struct view){ committed, top_line, rows, window_mode };
}

static void commit(const struct cell *c)
{
    struct view v = view_now();
    int64_t base = view_base(&v);
    struct cell *l = line(committed++);
    for (uint32_t i = 0; i < cols; i++)
        l[i] = c[i];
    if (view_back) {   /* keep the view where it was, if the screen scrolled */
        v = view_now();
        uint32_t max = view_back_max(&v);
        view_back += (uint32_t)(view_base(&v) - base);
        view_back = view_back < max ? view_back : max;
    }
    dirty = true;
}

uint16_t cell_glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp < 0x7f)
        return (uint16_t)cp;
    if (cp >= FONT_LATIN_FIRST && cp < FONT_LATIN_FIRST + FONT_LATIN_N)
        return (uint16_t)(G_LATIN + cp - FONT_LATIN_FIRST);
    if (cp >= 0x2500 && cp < 0x2500 + G_BOX_N)
        return (uint16_t)(G_BOX + cp - 0x2500);   /* box drawing, block elements */
    return '?';
}

/* The next character of s[*i..n) as a cell glyph, *i moved past it: a
 * control character or a bad piece of UTF-8 (<utf8.h>) is one '?', a tab
 * a space. The log's text is checked by the kernel already; the console
 * checks again rather than trust it. */
static uint16_t next_glyph(const char *s, size_t n, size_t *i)
{
    uint8_t b = (uint8_t)s[*i];
    if (b < 0x80) {
        (*i)++;
        return b == '\t' ? ' ' : b < 0x20 || b == 0x7f ? '?' : b;
    }
    uint32_t cp = 0;
    int k = utf8_seq((const uint8_t *)s + *i, n - *i, &cp);
    *i += (size_t)(k > 0 ? k : -k);
    return k > 0 && !utf8_is_control(cp) ? cell_glyph(cp) : '?';
}

/* s as lines above the current one, wrapped at cols: its first `stamp`
 * bytes in A_STAMP, the rest in attr. */
static void log_text(const char *s, size_t n, size_t stamp, uint8_t attr)
{
    struct cell l[MAX_COLS];
    uint32_t x = 0;
    blank(l, cols, A_KERNEL);
    for (size_t i = 0; i < n;) {
        if (s[i] == '\r') {
            i++;
            continue;
        }
        bool in_stamp = i < stamp;
        uint16_t g = next_glyph(s, n, &i);
        if (x == cols) {
            commit(l);
            blank(l, cols, A_KERNEL);
            x = 0;
        }
        l[x++] = (struct cell){ g, in_stamp ? A_STAMP : attr, 0 };
    }
    commit(l);
}

void notice_out(const char *s, size_t n)
{
    log_text(s, n, 0, A_NOTICE);
}

void kernel_line(const char *s, size_t n)
{
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
    log_text(s, n, stamp, text);
}

/* Everything allocated (and touched) up front: the console's memory use
 * doesn't move while it runs (a ktest from the shell measures the kernel's
 * free pages around each test). */
bool text_init(void)
{
    sb = malloc((size_t)SCROLLBACK * cols * sizeof(struct cell));
    alt = malloc((size_t)rows * cols * sizeof(struct cell));
    if (!sb || !alt)
        return false;
    for (uint64_t i = 0; i < SCROLLBACK; i++)
        blank(line(i), cols, A_OUT);
    blank(alt, rows * cols, A_OUT);
    blank(cur, cols, A_OUT);
    return true;
}

/* Line i of a scrollback of SCROLLBACK lines n cells wide, at base. */
static struct cell *line_in(struct cell *base, uint64_t i, uint32_t n)
{
    return &base[(i % SCROLLBACK) * n];
}

/* n cells of from into to (m cells): the first min(n, m), the rest blank. */
static void copy_cells(struct cell *to, uint32_t m, const struct cell *from, uint32_t n)
{
    uint32_t k = n < m ? n : m;
    memcpy(to, from, (size_t)k * sizeof(struct cell));
    if (k < m)
        blank(to + k, m - k, A_OUT);
}

bool text_regrid(uint32_t c, uint32_t r)
{
    if (!c || !r || c > MAX_COLS || r > MAX_ROWS)
        return false;
    struct cell *nsb = malloc((size_t)SCROLLBACK * c * sizeof(struct cell));
    struct cell *nalt = malloc((size_t)r * c * sizeof(struct cell));
    if (!nsb || !nalt) {
        free(nsb);
        free(nalt);
        return false;
    }
    for (uint64_t i = 0; i < SCROLLBACK; i++)
        copy_cells(line_in(nsb, i, c), c, line_in(sb, i, cols), cols);
    /* The alternate screen keeps its top left; its program draws again
     * at its next frame (or never: it asked for the size once). */
    for (uint32_t y = 0; y < r; y++) {
        if (y < rows)
            copy_cells(nalt + (size_t)y * c, c, alt + (size_t)y * cols, cols);
        else
            blank(nalt + (size_t)y * c, c, A_OUT);
    }
    if (c > cols)
        blank(cur + cols, c - cols, A_OUT);
    free(sb);
    free(alt);
    sb = nsb;
    alt = nalt;
    cols = c;
    rows = r;
    cur_x = cur_x < cols ? cur_x : cols - 1;
    alt_x = alt_x < cols ? alt_x : cols - 1;
    alt_y = alt_y < rows ? alt_y : rows - 1;
    struct view v = view_now();   /* the current line stays in view (view_base) */
    uint32_t max = view_back_max(&v);
    view_back = view_back < max ? view_back : max;
    dirty = true;
    return true;
}

void new_line(void)
{
    commit(cur);
    blank(cur, cols, A_OUT);
    cur_x = 0;
}

void clear_screen(void)
{
    struct cell l[MAX_COLS];
    blank(l, cols, A_OUT);
    commit(cur);
    if (window_mode)
        top_line = committed;   /* the screen starts again, at the top */
    else
        for (uint32_t i = 0; i + 1 < rows; i++)
            commit(l);
    blank(cur, cols, A_OUT);
    cur_x = 0;
    view_back = 0;
}

/* ---- the alternate screen (full-screen programs) ------------------------------
 *
 * ESC [ ? 1049 h switches to a grid of its own (rows x cols, blank), and
 * ESC [ ? 1049 l back to the text screen, which is redrawn as it was. On
 * the alternate screen ESC [ <row> ; <col> H moves the cursor anywhere,
 * ESC [ 2 J, K, A, B, C and D work as usual, ESC [ ? 25 l / h hide and
 * show the cursor, and ESC [ ? 2026 h / l bracket a frame (synchronized
 * output: nothing is drawn in between, for at most 250 ms). Its output is
 * NOT mirrored to COM1 (a full frame is ~100 KB: seconds at 115200 baud).
 * Kernel log lines keep going into the scrollback meanwhile.
 *
 * The alternate screen belongs to the client whose key channel had focus
 * when it was entered (so: open_keys first). When that channel closes (the
 * program exits, crashes or is killed), the console leaves the alternate
 * screen by itself. */

struct cell *alt;
bool alt_on, alt_cursor, alt_sync;
uint64_t alt_sync_since;
uint32_t alt_x, alt_y;
handle_t alt_owner;
static uint32_t alt_gen;    /* tells a stale owner packet from a live one */

void alt_newline(void)
{
    alt_x = 0;
    if (++alt_y < rows)
        return;
    alt_y = rows - 1;
    memmove(alt, alt + cols, (size_t)(rows - 1) * cols * sizeof(struct cell));
    blank(alt + (size_t)(rows - 1) * cols, cols, A_OUT);
}

/* Entered for the focused key channel. */
void alt_enter(void)
{
    if (alt_on)
        return;
    alt_on = true;
    alt_cursor = true;
    alt_sync = false;
    alt_x = alt_y = 0;
    blank(alt, rows * cols, A_OUT);
    alt_gen++;
    alt_owner = nfocus ? focus[nfocus - 1] : HANDLE_INVALID;
    if (alt_owner &&
        jam_port_bind(port, alt_owner, KEY(K_ALT, alt_gen), SIG_PEER_CLOSED, PORT_BIND_ONCE) != OK)
        alt_owner = HANDLE_INVALID;
    dirty = true;
}

void alt_leave(void)
{
    if (!alt_on)
        return;
    if (alt_owner)
        jam_port_unbind(port, alt_owner, KEY(K_ALT, alt_gen));   /* may have fired already */
    alt_owner = HANDLE_INVALID;
    alt_on = false;
    alt_sync = false;
    sgr(0);
    dirty = true;
}

/* The alternate screen's owner closed its key channel: drop it from the
 * focus stack, which leaves the alternate screen. */
void alt_owner_event(uint32_t gen)
{
    if (!alt_on || gen != alt_gen || !alt_owner)
        return;   /* stale: left (and maybe entered again) since */
    for (unsigned i = 0; i < nfocus; i++)
        if (focus[i] == alt_owner) {
            focus_drop(i);
            return;
        }
    alt_leave();
}
