/* shell: the command line. This file is the console side: output, keys,
 * the line editor and its history; sh_line() (sh_exec.c) runs each line,
 * and sh.h says where everything else is.
 *
 * Talks to the console (abi/idl/console.idl): writes through
 * console.write, and reads keys from the channel console.open_keys gives
 * (struct input_key_event; keys from a serial terminal come with usage 0
 * and the character, or with the usage of the special key). It does its
 * own line editing: left/right/home/end, backspace/delete, Ctrl+A/E/U,
 * Ctrl+C (cancel the line, or kill what `run` started), Ctrl+L (clear),
 * up/down for the history. The line is redrawn with \r and ESC [ K, which
 * the console and serial terminals both understand; a line longer than the
 * row scrolls sideways in it (scroll). It asks the terminal
 * for bracketed paste (ESC [ ? 2004 h) and takes pasted text onto the line
 * without running it, line breaks as spaces (sh_paste.c).
 *
 * The prompt is `jam:<cwd>> ` ("jam:/data/music> "): "jam" and the current
 * directory each in a colour of the console's 16 (term.c, cellpaint.c's
 * palette), the nearest to the logo's jams: "jam" yellow (0xccaa33) for
 * apricot (#ef9f27), the directory bright blue (0x6699ff) for blackcurrant
 * (#7f77dd); ':' and '>' plain. A directory too long for half the line
 * shows its end, after "...". The test scripts wait for it as {prompt}
 * (tools/serial-feed.py), whatever the directory.
 *
 * Exits 2 when the console goes away (init restarts the console, then the
 * shell with the new console's channel). */
#include <idl/console.h>
#include <idl/initctl.h>
#include <wants.h>
#include "sh_core.h"
#include "sh_paste.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("mount * rw\n"
          "svc audio\n"
          "svc audioctl\n"
          "svc music\n"
          "svc devmgr\n"
          "svc net\n"
          "svc dns\n");

#define LINE_MAX  240
#define HIST      32
#define C_JAM     "\033[33m"   /* "jam": yellow, the palette's nearest to apricot */
#define C_DIR     "\033[94m"   /* the directory: bright blue, its nearest to blackcurrant */
#define C_PLAIN   "\033[0m"
#define DIR_MIN   12            /* the directory's room in the prompt: half the row, at least this */
#define VIEW_MIN  8             /* the line's room in a narrow row: the directory shrinks for it */
#define C_MARK    "\033[30;47m" /* the markers at a cut edge: black on grey, unlike typed text */
#define RESIZE_POLL_NS (250 * NS_PER_MS) /* a line being edited: how often the width is asked */
#define PASTE_GAP_NS (2 * NS_PER_S)   /* a bracketed paste's keys: the longest wait for the next */
#define MARK_GAP_NS  (100 * NS_PER_MS) /* after an Escape: the longest wait for a marker's next key */

static handle_t con, keys;
unsigned sh_term_no;   /* init's "term=<n>": this shell's terminal closes with `exit` */

/* ---- output ------------------------------------------------------------------- */

static uint8_t obuf[2048];
static uint32_t on;

void sh_flush(void)
{
    if (!on)
        return;
    status_t st = console_write(con, (uint16_t)on, obuf);
    on = 0;
    if (st == ERR_PEER_CLOSED)
        jam_process_exit(2);
}

void sh_console_write(const char *s, size_t n)
{
    while (n) {
        uint32_t k = sizeof(obuf) - on < n ? sizeof(obuf) - on : (uint32_t)n;
        memcpy(obuf + on, s, k);
        on += k;
        s += k;
        n -= k;
        if (on == sizeof(obuf))
            sh_flush();
    }
}

/* The line editor's own output: always the screen. */
static void echo(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void echo(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    if (n > 0)
        sh_console_write(buf, (size_t)n);
}

handle_t sh_console(void)
{
    return con;
}

void sh_show_log(bool on, const char *only)
{
    static unsigned depth;   /* pairs open */
    if (!on && !depth)
        return;   /* no pair open: nothing to end */
    depth += on ? 1 : -1u;
    if (depth != (on ? 1u : 0u))
        return;   /* nested: only the outermost pair asks */
    sh_flush();   /* what the command wrote so far goes before the change */
    /* A shell on a PROGRAM channel (`run shell`) may not ask: its own
     * shell asks for it, since it runs it. */
    uint8_t name[32] = { 0 };
    if (on && only)
        memcpy(name, only, strnlen(only, sizeof(name) - 1));
    (void)console_show_log(con, on, name);
}

/* ---- keys ---------------------------------------------------------------------- */

enum {
    U_ENTER = 0x28, U_ESC = 0x29, U_BACKSPACE = 0x2a, U_TAB = 0x2b, U_RIGHT = 0x4f,
    U_LEFT = 0x50, U_DOWN = 0x51, U_UP = 0x52, U_HOME = 0x4a, U_END = 0x4d, U_DELETE = 0x4c,
    U_KP_ENTER = 0x58,
};

bool sh_get_key(struct input_key_event *ev, uint64_t deadline)
{
    for (;;) {
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = keys, .bytes_cap = sizeof(*ev), .bytes = (uint64_t)(uintptr_t)ev,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            if (n == sizeof(*ev) && ev->state != INPUT_KEY_UP)
                return true;
            continue;
        }
        if (st == ERR_PEER_CLOSED)
            jam_process_exit(2);
        if (st != ERR_SHOULD_WAIT)
            jam_process_exit(3);
        signals_t seen;
        st = jam_object_wait_one(keys, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            return false;
        if (st != OK)
            jam_process_exit(3);
    }
}

bool sh_is_ctrl(const struct input_key_event *ev, char letter)
{
    /* A terminal sends the control character; a keyboard, the letter with Ctrl. */
    if (ev->codepoint == (uint32_t)(letter - 'a' + 1))
        return true;
    return (ev->mods & INPUT_MOD_CTRL) &&
           (ev->codepoint == (uint32_t)letter || ev->usage == 4 + (letter - 'a'));
}

/* ---- the line editor --------------------------------------------------------- */

static char hist[HIST][LINE_MAX + 1];
static unsigned nhist;   /* entries ever added */

/* The console's current line is one screen row, and the console can't go
 * back up into the rows above it (a row is committed to the scrollback
 * when the text goes past its end): so a line that wrapped could not be
 * redrawn (each redraw would commit another copy of its first row). The
 * line editor keeps the line on the one row instead: a line longer than
 * the room after the prompt scrolls sideways to keep the cursor in view,
 * with a marker (C_MARK) where text is cut off: '<' at the left, '>' at
 * the right. Enter (and Ctrl+C) writes the whole line out once more, which
 * the console wraps, so the scrollback keeps all of it. Nothing is ever
 * written in the row's last column: the cursor sits there at the end of a
 * line that fills the room, and an echo there would wrap. */
static uint16_t cols;    /* the console's columns (0: not known: no scrolling) */
static unsigned room;    /* the row's cells after the prompt, less the last column */

/* The prompt for the next line (set_prompt): its bytes, colours and all. */
static char prompt[SH_PATH_MAX + 48];

/* The prompt for the current directory, and the room left for the line. */
static void set_prompt(void)
{
    const char *dir = sh_cwd();
    size_t n = strlen(dir), keep = !cols ? n : cols / 2 > DIR_MIN ? cols / 2u : DIR_MIN;
    if (cols && cols > 6 + 1 + VIEW_MIN + 4 && keep > cols - 6u - 1u - VIEW_MIN)
        keep = cols - 6u - 1u - VIEW_MIN;   /* a narrow row: the line keeps VIEW_MIN cells */
    const char *cut = n > keep ? "..." : "";
    if (n > keep)
        dir += n - (keep - 3);   /* its end, after "..." */
    snprintf(prompt, sizeof(prompt), C_JAM "jam" C_PLAIN ":" C_DIR "%s%s" C_PLAIN "> ", cut,
             dir);
    unsigned width = 6 + (unsigned)strlen(cut) + (unsigned)strlen(dir);   /* jam: > and the space */
    /* At least 3 cells, so a character shows between the two markers
     * (scroll's loop needs one); a row narrower than that wraps anyway. */
    room = !cols ? LINE_MAX : cols > width + 3 ? cols - width - 1u : 3;
}

/* The console's width again (a terminal window is resized at any time),
 * and with it the prompt and the room. True if the row must be redrawn:
 * the width changed with text on the line (line), or the prompt changed
 * (its directory gets half the row). An empty line under the same prompt
 * stays as it is (a redraw would only repeat the prompt). */
static bool resized(bool line)
{
    uint16_t c = 0, r = 0;
    if (console_size(con, &c, &r) != OK || c == cols)
        return false;
    char was[sizeof(prompt)];
    memcpy(was, prompt, sizeof(prompt));
    cols = c;
    set_prompt();
    return line || strcmp(was, prompt) != 0;
}

struct edit {
    char     line[LINE_MAX + 1];   /* the line being edited */
    unsigned len, pos;             /* its length; the cursor */
    unsigned first;                /* the first character shown (> 0: scrolled, '<' shown) */
    unsigned back;                 /* 0 = the new line, n = the n-th newest in the history */
    char     saved[LINE_MAX + 1];  /* the new line while browsing the history */
};

/* What the row shows of a line from its character `first` on. */
struct shown {
    unsigned left, right;   /* the '<' and '>' markers: 0 or 1 each */
    unsigned chars;         /* the characters between them */
};

static struct shown shown_at(const struct edit *e)
{
    struct shown v = { .left = e->first > 0 };
    unsigned cells = room - v.left;
    v.right = e->len - e->first > cells;
    v.chars = v.right ? cells - 1 : e->len - e->first;
    return v;
}

/* The cursor is in view: on a shown character, or just past the last one
 * when nothing is cut off at the right. */
static bool in_view(const struct edit *e)
{
    struct shown v = shown_at(e);
    return e->pos >= e->first &&
           (e->pos < e->first + v.chars || (!v.right && e->pos == e->len));
}

/* Scroll so the cursor is in view. A line that fits shows whole; one that
 * doesn't fills the room (no blank cells at the right while text is cut
 * off at the left). The cursor leaving the view jumps it to show a third
 * of the room past the cursor, so moving along a line shows what comes;
 * typing at the end keeps the cursor at the right edge (the end can't go
 * further). True if e->first changed (the row must be redrawn). */
static bool scroll(struct edit *e)
{
    unsigned was = e->first, third = room / 3;
    if (e->len <= room) {
        e->first = 0;
        return e->first != was;
    }
    if (e->pos < e->first) {
        e->first = e->pos > third ? e->pos - third : 0;
    } else if (!in_view(e)) {
        /* the cursor's cell (after '<'): a third of the room and '>' after it */
        unsigned cell = room > third + 3 ? room - 2 - third : 1;
        e->first = e->pos + 1 > cell ? e->pos + 1 - cell : 0;
    }
    if (e->first > e->len - (room - 1))
        e->first = e->len - (room - 1);   /* the end and the cursor's cell fill the room */
    while (!in_view(e))
        e->first++;   /* first <= pos here, and first == pos is in view: it ends */
    return e->first != was;
}

static void redraw(struct edit *e)
{
    (void)scroll(e);
    struct shown v = shown_at(e);
    echo("\r%s%s%.*s%s\033[K", prompt, v.left ? C_MARK "<" C_PLAIN : "", (int)v.chars,
         e->line + e->first, v.right ? C_MARK ">" C_PLAIN : "");
    unsigned end = v.left + v.chars + v.right, at = v.left + e->pos - e->first;
    if (at < end)
        echo("\033[%uD", end - at);
}

/* The line is done (Enter, or Ctrl+C with "^C"): a line cut off on the row
 * is written out whole, for the console to wrap, so all of it stays in the
 * scrollback; then tail and the next row. */
static void end_line(const struct edit *e, const char *tail)
{
    if (e->len > room)
        echo("\r%s%.*s\033[K", prompt, (int)e->len, e->line);
    echo("%s\r\n", tail);
}

/* Up or down in the history; true if the line changed. */
static bool browse(struct edit *e, bool up)
{
    unsigned have = nhist < HIST ? nhist : HIST;
    unsigned nb = up ? (e->back < have ? e->back + 1 : e->back) : (e->back ? e->back - 1 : 0);
    if (nb == e->back)
        return false;
    if (e->back == 0) {
        memcpy(e->saved, e->line, e->len);
        e->saved[e->len] = '\0';
    }
    e->back = nb;
    const char *src = e->back ? hist[(nhist - e->back) % HIST] : e->saved;
    size_t n = strlen(src);
    e->len = e->pos = n < LINE_MAX ? (unsigned)n : LINE_MAX;
    memcpy(e->line, src, e->len);
    return true;
}

/* A printable character at the cursor; true if the line needs a redraw. */
static bool insert(struct edit *e, char c)
{
    if (e->len >= LINE_MAX)
        return false;
    memmove(e->line + e->pos + 1, e->line + e->pos, e->len - e->pos);
    e->line[e->pos++] = c;
    e->len++;
    if (e->pos < e->len || e->len > room)
        return true;   /* in the middle, or past the room: the row scrolls */
    sh_console_write(&c, 1);   /* typing at the end of a line that fits: just echo */
    return false;
}

/* One editing key (not Enter); true if the line needs a redraw. */
static bool edit_key(struct edit *e, const struct input_key_event *ev)
{
    uint32_t cp = ev->codepoint;
    uint16_t u = ev->usage;
    if (sh_is_ctrl(ev, 'c')) {
        end_line(e, "^C");
        e->len = e->pos = e->back = 0;
        echo("%s", prompt);
        return false;
    }
    if (sh_is_ctrl(ev, 'l')) {
        console_clear(con);
        return true;
    }
    if (sh_is_ctrl(ev, 'a') || u == U_HOME) {
        e->pos = 0;
        return true;
    }
    if (sh_is_ctrl(ev, 'e') || u == U_END) {
        e->pos = e->len;
        return true;
    }
    if (sh_is_ctrl(ev, 'u')) {
        memmove(e->line, e->line + e->pos, e->len - e->pos);
        e->len -= e->pos;
        e->pos = 0;
        return true;
    }
    if (u == U_BACKSPACE || (!u && (cp == 8 || cp == 0x7f))) {
        if (!e->pos)
            return false;
        memmove(e->line + e->pos - 1, e->line + e->pos, e->len - e->pos);
        e->pos--;
        e->len--;
        return true;
    }
    if (u == U_DELETE) {
        if (e->pos >= e->len)
            return false;
        memmove(e->line + e->pos, e->line + e->pos + 1, e->len - e->pos - 1);
        e->len--;
        return true;
    }
    if (u == U_LEFT) {
        if (!e->pos)
            return false;
        e->pos--;
        if (scroll(e))
            return true;
        echo("\033[D");
        return false;
    }
    if (u == U_RIGHT) {
        if (e->pos >= e->len)
            return false;
        e->pos++;
        if (scroll(e))
            return true;
        echo("\033[C");
        return false;
    }
    if (u == U_UP || u == U_DOWN)
        return browse(e, u == U_UP);
    if (u == U_TAB || (!u && cp == '\t'))
        return sh_complete(e->line, &e->len, &e->pos, LINE_MAX);
    if (cp >= 0x20 && cp < 0x7f && !(ev->mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT)))
        return insert(e, (char)cp);
    return false;
}

/* The next key for the line editor: first what was typed while the last
 * command ran. While programs run in the background, their output is
 * shown as it comes (over the line, which is drawn again below it). While
 * a line is being edited, a resize of the terminal redraws it within
 * RESIZE_POLL_NS (the console doesn't say: the shell asks); an empty line
 * waits without waking. */
static void next_key(struct input_key_event *ev, struct edit *e)
{
    if (sh_typeahead(ev))
        return;
    for (;;) {
        bool jobs = sh_jobs_running();
        if (!jobs && !e->len) {
            sh_get_key(ev, DEADLINE_NEVER);
            return;
        }
        if (sh_get_key(ev, now() + (jobs ? SH_JOBS_POLL_NS : RESIZE_POLL_NS)))
            return;
        bool again = jobs && sh_jobs_poll(true);
        if (e->len && resized(true))
            again = true;
        if (again)
            redraw(e);
        sh_flush();
    }
}

/* ---- bracketed paste (sh_paste.c) ------------------------------------------------ */

/* Keys typed after an Escape that were not a paste marker, to be taken as
 * typed (the line editor's next keys). */
static struct input_key_event replay[SH_PASTE_MARK_LEN];
static unsigned nreplay, ireplay;

/* k[0] is an Escape: the keys after it, as far as a marker goes (each
 * within gap). Returns how many of k are filled, *kind what they are. */
static unsigned marker_keys(struct input_key_event *k, uint64_t gap, enum sh_paste_mark *kind)
{
    unsigned n = 1;
    *kind = sh_paste_marker(k, 1);
    while (*kind == SH_PASTE_MORE && n < SH_PASTE_MARK_LEN && sh_get_key(&k[n], now() + gap))
        *kind = sh_paste_marker(k, ++n);
    if (*kind == SH_PASTE_MORE)
        *kind = SH_PASTE_NO;   /* the keys stopped part way */
    return n;
}

/* A pasted key onto the line at the cursor (no echo: the line is redrawn). */
static void paste_insert(struct edit *e, const struct input_key_event *k)
{
    char c = sh_paste_byte(k);
    if (!c || e->len >= LINE_MAX)
        return;   /* dropped: not a character the line holds, or the line is full */
    memmove(e->line + e->pos + 1, e->line + e->pos, e->len - e->pos);
    e->line[e->pos++] = c;
    e->len++;
}

/* Pasted text, up to its end marker (or a pause of PASTE_GAP_NS: what came
 * stays), onto the line. Bounded by what the console pastes. */
static void take_paste(struct edit *e)
{
    struct input_key_event k[SH_PASTE_MARK_LEN];
    while (sh_get_key(&k[0], now() + PASTE_GAP_NS)) {
        enum sh_paste_mark kind = SH_PASTE_NO;
        unsigned n = sh_paste_is_esc(&k[0]) ? marker_keys(k, PASTE_GAP_NS, &kind) : 1;
        if (kind == SH_PASTE_END)
            return;
        for (unsigned i = 0; i < n; i++)
            paste_insert(e, &k[i]);
    }
}

/* An Escape came: a paste's start (taken now: true), or keys to take as
 * typed, which replay hands out next. */
static bool paste_or_keys(struct edit *e, const struct input_key_event *esc)
{
    struct input_key_event k[SH_PASTE_MARK_LEN];
    k[0] = *esc;
    enum sh_paste_mark kind;
    unsigned n = marker_keys(k, MARK_GAP_NS, &kind);
    if (kind == SH_PASTE_BEGIN) {
        take_paste(e);
        return true;
    }
    memcpy(replay, k + 1, (n - 1) * sizeof(k[0]));   /* the Escape itself does nothing */
    nreplay = n - 1;
    ireplay = 0;
    return false;
}

/* Read one line into buf (NUL-terminated). */
static void read_line(char *buf)
{
    static bool asked;
    struct edit e = { .len = 0 };
    sh_jobs_report();   /* the background programs that ended */
    set_prompt();       /* the directory may have changed */
    (void)resized(false);   /* and the width, while the last command ran */
    if (!asked)   /* once: the console keeps it for this shell's channel (term.c) */
        echo("\033[?2004h");
    asked = true;
    echo("%s", prompt);
    sh_flush();
    for (;;) {
        struct input_key_event ev;
        if (ireplay < nreplay)
            ev = replay[ireplay++];
        else
            next_key(&ev, &e);
        if (resized(e.len > 0)) {   /* before the key: its echo assumes the row's width */
            redraw(&e);
            sh_flush();
        }
        if (sh_paste_is_esc(&ev) && ireplay >= nreplay) {
            if (paste_or_keys(&e, &ev))
                redraw(&e);
            sh_flush();
            continue;
        }
        uint16_t u = ev.usage;
        if (u == U_ENTER || u == U_KP_ENTER ||
            (!u && (ev.codepoint == '\n' || ev.codepoint == '\r'))) {
            e.line[e.len] = '\0';
            memcpy(buf, e.line, e.len + 1);
            end_line(&e, "");
            sh_flush();
            return;
        }
        if (edit_key(&e, &ev))
            redraw(&e);
        sh_flush();
    }
}

static void remember(const char *line)
{
    if (!line[0])
        return;
    if (nhist && !strcmp(hist[(nhist - 1) % HIST], line))
        return;
    size_t n = strnlen(line, LINE_MAX);
    memcpy(hist[nhist % HIST], line, n);
    hist[nhist % HIST][n] = '\0';
    nhist++;
}

unsigned sh_history_count(void)
{
    return nhist;
}

const char *sh_history_at(unsigned i)
{
    return i < nhist && nhist - i <= HIST ? hist[i % HIST] : NULL;
}

/* The boot word soak=<minutes> (init passes it to the boot's first shell):
 * wait for the stick's /data, so the file load has something to write to,
 * then run the soak test as if typed, halting on the first failure. */
static void boot_soak(const char *minutes)
{
    char line[48];
    echo("soak (boot word): waiting up to 20 s for /data\n");
    sh_flush();
    for (int i = 0; i < 100 && fs_statfs("/data", NULL, NULL, NULL, NULL) != OK; i++)
        jam_nanosleep(now() + 200 * NS_PER_MS);
    snprintf(line, sizeof(line), "soak %s halt", minutes);
    set_prompt();
    echo("%s%s\n", prompt, line);
    sh_line(line);
    sh_flush();
}

/* "run=<line>" (init's, after term=: initctl.terminal's command, the
 * compositor's "Run ... in a terminal"): the line run as if typed at the
 * first prompt, and remembered; then the prompt as ever. */
static void run_first(const char *line)
{
    char copy[LINE_MAX + 1];
    snprintf(copy, sizeof(copy), "%s", line);
    set_prompt();
    echo("%s%s\n", prompt, copy);
    remember(copy);
    sh_line(copy);
    sh_flush();
}

/* init's arguments, in any order: "term=<n>" (with a compositor: terminal
 * n's shell, which `exit` closes), "soak=<minutes>" (the boot word's soak
 * test) and "run=<line>" (initctl.terminal's command). */
static const char *soak_arg, *run_arg;

static void take_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "term=", 5) && a[5] >= '1' && a[5] <= '9' && !a[6])
            sh_term_no = (unsigned)(a[5] - '0');
        else if (!strncmp(a, "soak=", 5))
            soak_arg = a + 5;
        else if (!strncmp(a, "run=", 4) && a[4])
            run_arg = a + 4;
    }
}

int main(int argc, char **argv)
{
    take_args(argc, argv);
    con = startup_handle(SR_CONSOLE);
    if (!con) {
        printf("shell: no console channel\n");
        return 1;
    }
    status_t st = console_open_keys(con, &keys);
    if (st != OK) {
        printf("shell: console.open_keys: %s\n", status_str(st));
        return st == ERR_PEER_CLOSED ? 2 : 1;
    }
    uint16_t rows = 0;
    if (console_size(con, &cols, &rows) != OK)
        cols = 0;
    sh_init();
    echo("\033[1mJam OS shell.\033[0m Type \033[1mhelp\033[0m for the commands.\n");
    const char *note = sh_boot_note();   /* after a panic: what happened to that boot */
    if (note[0])
        echo("\033[93m%s\033[0m\n", note);
    sh_flush();
    /* Up: the boot splash, if one holds the screen, gives it back now.
     * Without init's channel (a shell run from a shell) there is none. */
    if (sh_initctl())
        (void)initctl_shell_ready_until(sh_initctl(), now() + 2 * NS_PER_S);
    if (soak_arg)
        boot_soak(soak_arg);
    if (run_arg)
        run_first(run_arg);
    for (;;) {
        char line[LINE_MAX + 1];
        read_line(line);
        remember(line);
        sh_line(line);
        sh_flush();
    }
}
