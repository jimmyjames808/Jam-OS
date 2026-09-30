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
 * the console and serial terminals both understand.
 *
 * Exits 2 when the console goes away (init restarts the console, then the
 * shell with the new console's channel). */
#include <idl/console.h>
#include "sh_core.h"

#define LINE_MAX  240
#define HIST      32
#define PROMPT    "\033[93mjam>\033[0m "
#define PROMPT_W  5

static handle_t con, keys;

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
/* The console's current line is one screen row and the redraw goes back
 * with \r: a line that wraps can't be redrawn (each redraw would commit
 * another copy of its first row). So a line fits the row (main). */
static unsigned line_max = LINE_MAX;

struct edit {
    char     line[LINE_MAX + 1];   /* the line being edited */
    unsigned len, pos;             /* its length; the cursor */
    unsigned back;                 /* 0 = the new line, n = the n-th newest in the history */
    char     saved[LINE_MAX + 1];  /* the new line while browsing the history */
};

static void redraw(const struct edit *e)
{
    echo("\r" PROMPT "%.*s\033[K", (int)e->len, e->line);
    if (e->pos < e->len)
        echo("\033[%uD", e->len - e->pos);
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
    e->len = e->pos = (unsigned)strlen(src);
    memcpy(e->line, src, e->len);
    return true;
}

/* A printable character at the cursor; true if the line needs a redraw. */
static bool insert(struct edit *e, char c)
{
    if (e->len >= line_max)
        return false;
    memmove(e->line + e->pos + 1, e->line + e->pos, e->len - e->pos);
    e->line[e->pos++] = c;
    e->len++;
    if (e->pos < e->len)
        return true;
    sh_console_write(&c, 1);   /* typing at the end: just echo */
    return false;
}

/* One editing key (not Enter); true if the line needs a redraw. */
static bool edit_key(struct edit *e, const struct input_key_event *ev)
{
    uint32_t cp = ev->codepoint;
    uint16_t u = ev->usage;
    if (sh_is_ctrl(ev, 'c')) {
        echo("^C\r\n");
        e->len = e->pos = e->back = 0;
        echo(PROMPT);
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
        if (e->pos) {
            e->pos--;
            echo("\033[D");
        }
        return false;
    }
    if (u == U_RIGHT) {
        if (e->pos < e->len) {
            e->pos++;
            echo("\033[C");
        }
        return false;
    }
    if (u == U_UP || u == U_DOWN)
        return browse(e, u == U_UP);
    if (u == U_TAB || (!u && cp == '\t'))
        return sh_complete(e->line, &e->len, &e->pos, line_max);
    if (cp >= 0x20 && cp < 0x7f && !(ev->mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT)))
        return insert(e, (char)cp);
    return false;
}

/* Read one line into buf (NUL-terminated). */
static void read_line(char *buf)
{
    struct edit e = { .len = 0 };
    echo(PROMPT);
    sh_flush();
    for (;;) {
        struct input_key_event ev;
        if (!sh_typeahead(&ev))   /* first what was typed while the last command ran */
            sh_get_key(&ev, DEADLINE_NEVER);
        uint16_t u = ev.usage;
        if (u == U_ENTER || u == U_KP_ENTER ||
            (!u && (ev.codepoint == '\n' || ev.codepoint == '\r'))) {
            e.line[e.len] = '\0';
            memcpy(buf, e.line, e.len + 1);
            echo("\r\n");
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
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
    uint16_t cols = 0, rows = 0;
    if (console_size(con, &cols, &rows) == OK && cols > PROMPT_W + 1 &&
        cols - PROMPT_W - 1 < LINE_MAX)
        line_max = cols - PROMPT_W - 1;
    sh_init();
    echo("\n\033[1mJam OS shell.\033[0m Type \033[1mhelp\033[0m for the commands.\n");
    for (;;) {
        char line[LINE_MAX + 1];
        read_line(line);
        remember(line);
        sh_line(line);
        sh_flush();
    }
}
