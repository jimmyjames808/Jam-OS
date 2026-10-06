/* console: the text terminal on the screen.
 *
 * Owns the boot framebuffer once it has started (framebuffer_take: a WC
 * VMO of it; the kernel stops drawing its log and draws again if this
 * process dies). Serves the `console` protocol (abi/idl/console.idl) to
 * programs and the `input` protocol on each channel connect_input hands
 * out (a HID driver, the serial source). The parts are listed in
 * console.h.
 *
 * Arguments: "quiet" (init, when the boot splash plays first without a
 * compositor): draw nothing until the splash has borrowed the screen and
 * given it back (screen_quiet), at most QUIET_MAX. "nolog" (init, on
 * every boot but a `verbose` one's first terminal: the quiet boot): the
 * kernel log stays off the screen, except while a client asks for it
 * (console.show_log: the shell, while a command whose output is the log
 * runs); the full-screen console (no compositor) still shows the few
 * lines notices.c makes of it. The log is still all in the kernel's
 * ring, on the serial port, in /data/logs and over netlog: `log` and
 * `dmesg` show it. "selftest" (alone; `run console selftest` from the
 * shell): the notices' checks (selftest.c), then exit. "term=<n>" (init,
 * in window mode): this is terminal n (1 to 9; 1 if none is given): its
 * window's title; a terminal other than the first keeps the log off its
 * screen but on request (as "nolog"). A terminal in a window makes no
 * notices: the system's news is the desktop's (init's, devmgr's and
 * netstack's notices through /svc/notify). "font=smooth|bitmap" (init, in window mode,
 * from the settings' terminal.font): the window's cells in JetBrains Mono
 * (smooth, the default) or the 8x16 bitmap; console.set_font changes it
 * later. The full-screen console always draws the bitmap.
 *
 * Startup handles:
 *   SR_RESOURCE     the root resource with RIGHT_ROOT_KLOG (klog_open),
 *                   RIGHT_ROOT_SCREEN (framebuffer_take),
 *                   RIGHT_ROOT_SERIAL_OUT (serial_write) and
 *                   RIGHT_ROOT_REBOOT (reboot, on Ctrl+Alt+Del, if init
 *                   doesn't answer)
 *   SR_USER + n     server ends of `console` channels (n = 0..7): init's;
 *                   clients share one by duplicating the client end
 *   SR_USER + 8     init's control channel (abi/idl/initctl.idl), which
 *                   answers this holder only `reboot` (Ctrl+Alt+Del) and
 *                   `terminal` (Super+Enter: another terminal)
 *   SR_USER + 9     init's table of the log writers whose lines make
 *                   notices (<logwriters.h>), read-only
 *   SR_USER + 10    optional: the client end of /svc/wayland's shared
 *                   channel (WAYLAND_ROLE). With it the console runs in
 *                   window mode (window.c): it draws into a window of the
 *                   compositor's and never takes the framebuffer; without
 *                   it, as below, on the framebuffer
 *
 * The screen: a grid of cells (8x16 on the full screen; a window's are
 * cells.h's). Committed lines live in a scrollback ring; the line the
 * programs are writing (the "current line", where the cursor is) is the
 * bottom row of the full screen, and in a window the row after the text
 * so far, from the top until the window is full (view.c). Kernel log lines (a klog reader,
 * here) and notices are committed ABOVE the current line, so a log line
 * never breaks up the prompt the shell is editing. Colours: kernel lines
 * grey, lines a process logged through debug_write ("[name] ...") green,
 * notices yellow, program output white (ESC [ ... m changes it; ESC [ 1 m
 * is also bold in a window's smooth font). Shift+PageUp/PageDown (or
 * PageUp/PageDown from a serial terminal) scroll back; any other key goes
 * back to the bottom.
 *
 * Who may do what: clients.c (the client levels) and keys.c (the keys).
 * Drawing, and lending the screen to a program: screen.c. Full-screen text
 * programs can use the alternate screen: text.c. */
#include <logwriters.h>
#include "console.h"

#define QUIET_MAX (5 * NS_PER_S)   /* the splash borrows the screen as it starts */
/* What a starting console draws of the log already in the kernel's ring
 * (4 MiB): its last part. The rest is still read, for the notices' state
 * (which mounts there are), but drawing it would only fill a scrollback
 * that keeps SCROLLBACK lines, after a restart in a long boot. */
#define CATCH_UP_DRAWN (256u << 10)
#define KLOG_LINES_PER_CALL 256    /* klog_lines gives at most this many marks a call */

handle_t root, port;

/* ---- the kernel log ------------------------------------------------------------- */

static handle_t klog;              /* our kernel log reader */
static uint64_t klog_pos;          /* the log position read up to */
static char klog_buf[KLOG_BUF];    /* what one klog_read returns */
static char partial[1024];         /* the line being gathered, without its newline */
static size_t npartial;            /* its length */
static uint64_t partial_at;        /* the log position of partial[0] */
static enum log_writer partial_by; /* who wrote it (the mark at partial_at) */
static bool mid_line;              /* the next text may start inside a line (a gap, a cut) */
/* The marks of the processes' lines starting in the text klog_read last
 * gave (a line is at least a 15-byte stamp and a newline), the next to
 * look at, and the stretch of the log they are complete for: a line in
 * [marks_known, marks_upto) without a mark is the kernel's; outside it,
 * not known. */
static struct klog_line marks[KLOG_BUF / 16];
static size_t nmarks, mark_next;
static uint64_t marks_known, marks_upto;
static const struct log_writers *writers;   /* init's table, mapped (NULL: none) */
static bool log_off;               /* "nolog": the log is off the screen but on request */
static bool notices;               /* ... and its notices are ours to show (the full screen's) */
static bool catching_up;           /* reading the log from before we started */
static bool log_whole = true;       /* the log read so far starts at the boot's first line */
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

/* The marks of the processes' lines starting in [first, first + n) of the
 * log. */
static void fetch_marks(uint64_t first, int64_t n)
{
    nmarks = mark_next = 0;
    marks_known = UINT64_MAX;
    marks_upto = 0;   /* nothing known until a call has answered */
    uint64_t pos = first;
    while (nmarks < sizeof(marks) / sizeof(marks[0])) {
        uint64_t want = sizeof(marks) / sizeof(marks[0]) - nmarks, known;
        if (want > KLOG_LINES_PER_CALL)
            want = KLOG_LINES_PER_CALL;
        int64_t got = jam_klog_lines(klog, pos, marks + nmarks, want, &known);
        if (got < 0)
            return;
        nmarks += (size_t)got;
        marks_known = known;
        if ((uint64_t)got < want) {
            marks_upto = UINT64_MAX;   /* every mark there is */
            return;
        }
        pos = marks[nmarks - 1].pos + 1;
        marks_upto = pos;   /* complete up to the last one given */
        if (pos >= first + (uint64_t)n)
            return;   /* past the text */
    }
}

/* Who wrote the line that starts at pos: its mark, against init's table. */
static enum log_writer writer_at(uint64_t pos)
{
    while (mark_next < nmarks && marks[mark_next].pos < pos)
        mark_next++;
    if (mark_next == nmarks || marks[mark_next].pos != pos)
        return pos >= marks_known && pos < marks_upto ? W_KERNEL : W_OTHER;
    uint64_t w = marks[mark_next].writer;
    if (!writers || w == KLOG_WRITER_UNKNOWN)
        return W_OTHER;
    return w == __atomic_load_n(&writers->init, __ATOMIC_ACQUIRE)     ? W_INIT
           : w == __atomic_load_n(&writers->devmgr, __ATOMIC_ACQUIRE) ? W_DEVMGR
           : w == __atomic_load_n(&writers->logd, __ATOMIC_ACQUIRE)   ? W_LOGD
                                                                       : W_OTHER;
}

/* One whole line of the log (a line longer than `partial` in pieces). */
static void klog_line(const char *s, size_t n)
{
    size_t nl;
    const char *name = line_name(s, n, &nl);
    bool shown = (!log_off || clients_show_line(name, nl)) &&
                 (!catching_up || partial_at >= draw_from);
    if (shown)
        kernel_line(s, n);
    drawn_lines += shown && catching_up;
    if (notices)
        notice_take(s, n, !shown && !catching_up, partial_by);
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
            mid_line = c != '\n';   /* the rest of a line cut in pieces */
            if (c == '\n')
                continue;
        }
        if (!npartial) {
            partial_at = at + (uint64_t)i;
            /* Not a line's start: no mark could say whose it is. */
            partial_by = mid_line ? W_OTHER : writer_at(partial_at);
            mid_line = false;
        }
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
        if (first != klog_pos) {
            log_whole = log_whole && !catching_up;   /* the boot's first lines are gone */
            npartial = 0;
            mid_line = true;   /* the oldest text kept may start inside a line */
        }
        if (first != klog_pos && klog_pos) {
            char gap[64];
            int m = snprintf(gap, sizeof(gap), "[console: %lu bytes of kernel log missed]",
                             (unsigned long)(first - klog_pos));
            npartial = 0;
            if (!log_off || clients_show_all())
                kernel_line(gap, (size_t)m);
        }
        klog_pos = first + (uint64_t)n;
        if (notices)
            fetch_marks(first, n);   /* only the notices need them */
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
    case K_SOURCE:
        if (i < MAX_SOURCES)
            source_event(i);
        break;
    case K_ALT:
        alt_owner_event(i);
        break;
    case K_LEASE:
        lease_ended();
        break;
    case K_INIT:
        init_event();
        break;
    case K_WL:
        window_event();
        break;
    }
}

/* The screen: the window (window mode, with /svc/wayland's channel from
 * init) or the framebuffer; the text model at its size. false: out of
 * memory. *screen: something shows the text. */
static bool start_screen(bool *screen)
{
    handle_t wayland = startup_handle(SR_USER + WAYLAND_ROLE);
    if (wayland) {
        notices = false;   /* the desktop's notices say the news */
        cols = WIN_COLS;   /* until the output is known (window.c regrids) */
        rows = WIN_ROWS;
        *screen = window_init(wayland, term_no);
        return text_init() && paint_regrid();
    }
    *screen = screen_init();
    return text_init() && screen_alloc();
}

/* Read the kernel log written before we started: drawn (its last part),
 * and the notices' state from all of it. */
static void read_boot_log(void)
{
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
    draw_from = end > CATCH_UP_DRAWN ? end - CATCH_UP_DRAWN : 0;
    catching_up = true;
    klog_event();   /* the boot log so far: no news in it */
    catching_up = false;
    printf("console: the kernel log so far: drew %lu lines of the last %lu KiB (of %lu "
           "KiB)%s\n", (unsigned long)drawn_lines, (unsigned long)((end - draw_from) >> 10),
           (unsigned long)(end >> 10), draw_from ? "; `dmesg` shows the rest" : "");
    notice_settle(log_whole);
}

/* The loop: returns only when our window was closed (0) or the port
 * failed (1). */
static int serve(void)
{
    uint64_t last = 0;
    while (!closing) {
        /* A client with requests left over: take what else is queued (a
         * key, another client) without sleeping, then give it another round. */
        uint64_t deadline = clients_pending() ? 0 : dirty ? last + RENDER_NS : DEADLINE_NEVER;
        uint64_t more[4] = { reboot_deadline(), notices ? notice_deadline() : DEADLINE_NEVER,
                             window_deadline(), paste_deadline() };
        for (unsigned i = 0; i < 4; i++)
            deadline = more[i] < deadline ? more[i] : deadline;
        struct port_packet pkt;
        status_t st = jam_port_wait(port, deadline, &pkt);
        if (st == OK) {
            port_event(&pkt);
        } else if (st != ERR_TIMED_OUT) {
            printf("console: port_wait: %s\n", status_str(st));
            return 1;
        }
        if (window_mode && st == ERR_TIMED_OUT)
            window_event();   /* its own work: a key repeat, a reconnect's try */
        clients_serve_pending();
        paste_pump();   /* a paste's keys, as fast as the focus reads them */
        reboot_due();
        if (notices)
            notice_tick(clients_show_all());
        uint64_t t = now();
        if (dirty && t >= last + RENDER_NS) {
            render();
            last = t;
        }
    }
    return 0;
}

/* The arguments (the file's header says what each does). */
static void take_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "quiet"))
            screen_quiet(now() + QUIET_MAX);
        else if (!strcmp(argv[i], "nolog"))
            log_off = true;
        else if (!strncmp(argv[i], "term=", 5) && argv[i][5] >= '1' && argv[i][5] <= '9' &&
                 !argv[i][6])
            term_no = (unsigned)(argv[i][5] - '0');
        else if (!strncmp(argv[i], "font=", 5) && term_font_parse(argv[i] + 5) >= 0)
            font_bitmap = term_font_parse(argv[i] + 5) == 1;
    }
    if (term_no > 1)
        log_off = true;   /* the log, on a `verbose` boot, is the first terminal's */
    notices = log_off && term_no == 1;   /* and not in a window: start_screen */
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "selftest"))
        return console_selftest();
    root = startup_handle(SR_RESOURCE);
    uint64_t waddr = 0;
    if (startup_handle(CONSOLE_WRITERS_ROLE) &&
        jam_vmar_map(startup_handle(SR_SELF_VMAR), startup_handle(CONSOLE_WRITERS_ROLE), 0,
                     PAGE_SIZE, VMAR_READ, &waddr) == OK)
        writers = (const struct log_writers *)(uintptr_t)waddr;
    take_args(argc, argv);
    status_t st = jam_port_create(&port);
    if (st != OK)
        return 1;
    clients_init();
    init_watch();
    bool screen = false;
    if (!start_screen(&screen)) {
        printf("console: out of memory\n");
        return 1;
    }
    read_boot_log();
    printf("console: %ux%u cells%s, %u client channel(s)%s\n", cols, rows,
           screen ? "" : " (no screen)", client_count(),
           !log_off ? "" : notices ? "; the kernel log off the screen (notices only)"
                                   : "; the kernel log off the screen");
    render();
    return serve();
}
