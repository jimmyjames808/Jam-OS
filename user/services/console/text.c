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
uint32_t view_back;
bool dirty = true;

struct cell *line(uint64_t i)
{
    return &sb[(i % SCROLLBACK) * cols];
}

void blank(struct cell *c, uint32_t n, uint8_t attr)
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

void kernel_line(const char *s, size_t n)
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
