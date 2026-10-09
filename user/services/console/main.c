/* console: a terminal: a window on the compositor's desktop.
 *
 * Serves the `console` protocol (abi/idl/console.idl) to programs, and
 * draws their text, with the kernel log's lines when they are asked for,
 * into a window of the compositor's (window.c), copying it to COM1. The
 * compositor owns the screen and the input: the keys come to the window
 * (M12's Q14: the full-screen console that took the framebuffer and the
 * input sources under the boot word `nocomp` is gone). Without a
 * compositor (none in bootfs) the text goes to COM1 alone. This file is
 * the front end: the kernel log, startup and the event loop around the
 * back end (console.h's struct backend; here, the window's: winback.c).
 * The parts are listed in console.h.
 *
 * Arguments: "nolog" (init, on every boot but a `verbose` one's first
 * terminal: the quiet boot): the kernel log stays off the screen, except
 * while a client asks for it (console.show_log: the shell, while a
 * command whose output is the log runs). The log is still all in the
 * kernel's ring, on the serial port, in /data/logs and over netlog: `log`
 * and `dmesg` show it. "term=<n>" (init): this is terminal n (1 to
 * TERMINALS_MAX, 16; 1 if none is given): its window's title; a terminal
 * other than the first keeps the log off its screen but on request (as
 * "nolog"). A terminal makes no notices: the system's news is the
 * desktop's (init's, devmgr's and netstack's notices through
 * /svc/notify). "font=smooth|bitmap" (init, from the settings'
 * terminal.font): the window's cells in JetBrains Mono (smooth, the
 * default) or the 8x16 bitmap; console.set_font changes it later.
 *
 * "conn": a connection console (conn.c, <termconn.h>): no screen and no
 * COM1, the programs' output goes out as bytes on a master channel and the
 * keys come in on it; "cols=<n>" and "rows=<n>" (its owner's, from the
 * remote terminal) are its size at the start. Its kernel log stays off
 * unless a client asks (console.show_log), and starts at the log's end.
 *
 * Startup handles:
 *   SR_RESOURCE     the root resource with RIGHT_ROOT_KLOG (klog_open) and
 *                   RIGHT_ROOT_SERIAL_OUT (serial_write); a connection
 *                   console's has RIGHT_ROOT_KLOG alone, or none is given
 *   SR_USER + n     server ends of `console` channels (n = 0..7): init's;
 *                   clients share one by duplicating the client end
 *   SR_USER + 10    the client end of /svc/wayland's shared channel
 *                   (WAYLAND_ROLE): the window (window.c). Without it the
 *                   console has no screen: COM1 only
 *   SR_USER + 11, 12  a connection console's master and control channels
 *                   (CONN_MASTER_ROLE, CONN_CTL_ROLE)
 *
 * The text: a grid of cells (cells.h's). Committed lines live in a
 * scrollback ring; the line the programs are writing (the "current line",
 * where the cursor is) is the row after the text so far, from the top
 * until the window is full (view.c). Kernel log lines (a klog reader,
 * here) are committed ABOVE the current line, so a log line never breaks
 * up the prompt the shell is editing. Colours: kernel lines grey, lines a
 * process logged through debug_write ("[name] ...") green, program output
 * white (ESC [ ... m changes it; ESC [ 1 m is also bold in the smooth
 * font). Shift+PageUp/PageDown scroll back; any other key goes back to the
 * bottom.
 *
 * Who may do what: clients.c (the client levels) and keys.c (the keys).
 * Drawing: screen.c and winpaint.c. Full-screen text programs can use the
 * alternate screen: text.c. */
#include <deskapps.h>
#include "console.h"

/* What a starting console draws of the log already in the kernel's ring
 * (4 MiB): its last part. Drawing the rest would only fill a scrollback
 * that keeps SCROLLBACK lines, after a restart in a long boot. */
#define CATCH_UP_DRAWN (256u << 10)

handle_t root, port;
bool closing;
const struct backend *back = &win_backend;

/* ---- the kernel log ------------------------------------------------------------- */

static handle_t klog;              /* our kernel log reader */
static uint64_t klog_pos;          /* the log position read up to */
static char klog_buf[KLOG_BUF];    /* what one klog_read returns */
static char partial[1024];         /* the line being gathered, without its newline */
static size_t npartial;            /* its length */
static uint64_t partial_at;        /* the log position of partial[0] */
static bool log_off;               /* "nolog": the log is off the screen but on request */
static bool catching_up;           /* reading the log from before we started */
static uint64_t draw_from;         /* the catch-up draws no line that starts before this */
static uint64_t drawn_lines;       /* lines the catch-up drew */

/* The process name of a log line ("[    1.234567] [name] ..."), or n 0
 * for the kernel's own. */
static const char *line_name(const char *s, size_t n, size_t *len)
{
    *len = 0;
    size_t i = 0;
    while (i < n && i < 20 && s[i] != ']')
        i++;
    if (i + 3 >= n || s[0] != '[' || s[i] != ']' || s[i + 1] != ' ' || s[i + 2] != '[')
        return NULL;
    const char *name = s + i + 3;
    for (size_t k = 0; k < 40 && name + k < s + n; k++)
        if (name[k] == ']') {
            *len = k;
            return name;
        }
    return NULL;
}

/* One whole line of the log (a line longer than `partial` in pieces). */
static void klog_line(const char *s, size_t n)
{
    size_t nl;
    const char *name = line_name(s, n, &nl);
    bool shown = (!log_off || clients_show_line(name, nl)) &&
                 (!catching_up || partial_at >= draw_from);
    if (shown)
        back->log_line(s, n);
    drawn_lines += shown && catching_up;
}

/* n bytes of the kernel log from position `at`: each whole line to
 * klog_line. */
static void klog_take(const char *p, int64_t n, uint64_t at)
{
    for (int64_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n' || npartial == sizeof(partial)) {
            klog_line(partial, npartial);
            npartial = 0;
            if (c == '\n')
                continue;
        }
        if (!npartial)
            partial_at = at + (uint64_t)i;
        partial[npartial++] = c;
    }
}

void klog_event(void)
{
    for (;;) {
        uint64_t first = 0;
        int64_t n = jam_klog_read(klog, klog_pos, klog_buf, sizeof(klog_buf), &first);
        if (n <= 0)
            return;
        if (first != klog_pos)
            npartial = 0;   /* the oldest text kept may start inside a line */
        if (first != klog_pos && klog_pos) {
            char gap[64];
            int m = snprintf(gap, sizeof(gap), "[console: %lu bytes of kernel log missed]",
                             (unsigned long)(first - klog_pos));
            npartial = 0;
            if (!log_off || clients_show_all())
                back->log_line(gap, (size_t)m);
        }
        klog_pos = first + (uint64_t)n;
        klog_take(klog_buf, n, first);
    }
}

/* ---- main ------------------------------------------------------------------------ */

static void port_event(const struct port_packet *pkt)
{
    uint32_t kind = (uint32_t)(pkt->key >> 32), i = (uint32_t)pkt->key;
    switch (kind) {
    case K_KLOG:
        klog_event();
        break;
    case K_CLIENT:
        client_event(i);
        break;
    default:
        back->event(kind, i);   /* the back end's own (K_ALT, K_WL, ...) */
        break;
    }
}

/* Read the kernel log written before we started: its last part drawn. */
static void read_boot_log(void)
{
    if (!root)
        return;   /* a connection console with no kernel log to show */
    status_t st = jam_klog_open(root, &klog);
    if (st != OK) {
        printf("console: no kernel log (%s)\n", status_str(st));
        return;
    }
    /* Unbound, new kernel lines show only when a program writes (each
     * write pulls them in first): say so, it's worth knowing. */
    st = jam_port_bind(port, klog, KEY(K_KLOG, 0), SIG_READABLE, PORT_BIND_PERSISTENT);
    if (st != OK)
        printf("console: kernel log: port_bind: %s; its lines show only with program "
               "output\n", status_str(st));
    /* Past the end: 0 bytes and the end. If that fails, end stays 0 and
     * everything is drawn, as a console did before it looked. */
    uint64_t end = 0;
    char c;
    jam_klog_read(klog, UINT64_MAX, &c, 1, &end);
    if (!back->replay_log) {
        klog_pos = end;   /* only the lines logged from now on */
        return;
    }
    draw_from = end > CATCH_UP_DRAWN ? end - CATCH_UP_DRAWN : 0;
    catching_up = true;
    klog_event();   /* the boot log so far */
    catching_up = false;
    printf("console: the kernel log so far: drew %lu lines of the last %lu KiB (of %lu "
           "KiB)%s\n", (unsigned long)drawn_lines, (unsigned long)((end - draw_from) >> 10),
           (unsigned long)(end >> 10), draw_from ? "; `dmesg` shows the rest" : "");
}

/* The loop: returns only when the terminal is gone (0) or the port
 * failed (1). */
static int serve(void)
{
    while (!closing) {
        /* A client with requests left over: take what else is queued (a
         * key, another client) without sleeping, then give it another round. */
        uint64_t deadline = clients_pending() ? 0 : DEADLINE_NEVER;
        uint64_t more[2] = { back->deadline(), paste_deadline() };
        for (unsigned i = 0; i < 2; i++)
            deadline = more[i] < deadline ? more[i] : deadline;
        struct port_packet pkt;
        status_t st = jam_port_wait(port, deadline, &pkt);
        if (st == OK) {
            port_event(&pkt);
        } else if (st != ERR_TIMED_OUT) {
            printf("console: port_wait: %s\n", status_str(st));
            return 1;
        }
        if (st == ERR_TIMED_OUT && back->tick)
            back->tick();
        clients_serve_pending();
        paste_pump();   /* a paste's keys, as fast as the focus reads them */
        back->flush();
    }
    return 0;
}

/* The arguments (the file's header says what each does). */
static void take_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "nolog"))
            log_off = true;
        else if (!strcmp(argv[i], "conn"))
            back = &conn_backend;
        else if (!strncmp(argv[i], "term=", 5) && terminal_number(argv[i] + 5))
            term_no = terminal_number(argv[i] + 5);   /* 1 to TERMINALS_MAX */
        else if (!strncmp(argv[i], "font=", 5) && term_font_parse(argv[i] + 5) >= 0)
            font_bitmap = term_font_parse(argv[i] + 5) == 1;
        else
            conn_arg(argv[i]);   /* cols=, rows= */
    }
    if (term_no > 1 || back == &conn_backend)
        log_off = true;   /* the log, on a `verbose` boot, is the first terminal's */
}

int main(int argc, char **argv)
{
    root = startup_handle(SR_RESOURCE);
    take_args(argc, argv);
    status_t st = jam_port_create(&port);
    if (st != OK)
        return 1;
    clients_init();
    if (!back->init())
        return 1;
    read_boot_log();
    back->banner(client_count(), log_off);
    if (back->ready)
        back->ready();
    return serve();
}
