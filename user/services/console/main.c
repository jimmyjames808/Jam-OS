/* console: the text terminal on the screen.
 *
 * Owns the boot framebuffer once it has started (framebuffer_take: a WC
 * VMO of it; the kernel stops drawing its log and draws again if this
 * process dies). Serves the `console` protocol (abi/idl/console.idl) to
 * programs and the `input` protocol on each channel connect_input hands
 * out (a HID driver, the serial source). The parts are listed in
 * console.h.
 *
 * Arguments: "quiet" (init, when the boot splash plays first): draw
 * nothing until the splash has borrowed the screen and given it back
 * (screen_quiet), at most QUIET_MAX. "nolog" (init, on a plain boot with
 * the splash: every console it starts then): the kernel log stays off the
 * screen, but for the few lines notices.c makes of it, except while a
 * client asks for it (console.show_log: the shell, while a command whose
 * output is the log runs). The log is still all in the kernel's ring, on
 * the serial port and in /data/logs: `log` and `dmesg` show it.
 * "selftest" (alone; `run console selftest` from the shell): the notices'
 * checks (selftest.c), then exit.
 *
 * Startup handles:
 *   SR_RESOURCE     the root resource with RIGHT_ROOT_KLOG (klog_open),
 *                   RIGHT_WRITE (framebuffer_take, serial_write) and
 *                   RIGHT_ROOT_REBOOT (reboot, on Ctrl+Alt+Del, if init
 *                   doesn't answer)
 *   SR_USER + n     server ends of `console` channels (n = 0..7): init's;
 *                   clients share one by duplicating the client end
 *   SR_USER + 8     init's control channel (abi/idl/initctl.idl), which
 *                   answers this holder only `reboot`: Ctrl+Alt+Del
 *
 * The screen: a grid of 8x16 cells. Committed lines live in a scrollback
 * ring; the line the programs are writing (the "current line", where the
 * cursor is) is always the bottom row. Kernel log lines (a klog reader,
 * here) and notices are committed ABOVE the current line, so a log line
 * never breaks up the prompt the shell is editing. Colours: kernel lines
 * grey, lines a process logged through debug_write ("[name] ...") green,
 * notices yellow, program output white (ESC [ ... m changes it). Shift+PageUp/PageDown (or PageUp/PageDown
 * from a serial terminal) scroll back; any other key goes back to the
 * bottom.
 *
 * Who may do what: clients.c (the client levels) and keys.c (the keys).
 * Drawing, and lending the screen to a program: screen.c. Full-screen text
 * programs can use the alternate screen: text.c. */
#include "console.h"

#define QUIET_MAX (5 * NS_PER_S)   /* the splash borrows the screen as it starts */
/* What a starting console draws of the log already in the kernel's ring
 * (4 MiB): its last part. The rest is still read, for the notices' state
 * (which mounts there are), but drawing it would only fill a scrollback
 * that keeps SCROLLBACK lines, after a restart in a long boot. */
#define CATCH_UP_DRAWN (256u << 10)

handle_t root, port;

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
        kernel_line(s, n);
    drawn_lines += shown && catching_up;
    if (log_off)
        notice_take(s, n, !shown && !catching_up);
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
        if (first != klog_pos && klog_pos) {
            char gap[64];
            int m = snprintf(gap, sizeof(gap), "[console: %lu bytes of kernel log missed]",
                             (unsigned long)(first - klog_pos));
            npartial = 0;
            if (!log_off || clients_show_all())
                kernel_line(gap, (size_t)m);
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
    case K_REBOOT:
        reboot_event();
        break;
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "selftest"))
        return console_selftest();
    root = startup_handle(SR_RESOURCE);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "quiet"))
            screen_quiet(now() + QUIET_MAX);
        else if (!strcmp(argv[i], "nolog"))
            log_off = true;
    }
    status_t st = jam_port_create(&port);
    if (st != OK)
        return 1;
    clients_init();
    bool screen = screen_init();
    if (!text_init() || !screen_alloc()) {
        printf("console: out of memory\n");
        return 1;
    }
    st = jam_klog_open(root, &klog);
    if (st == OK) {
        /* Unbound, new kernel lines show only when a program writes (each
         * write pulls them in first): say so, it's worth knowing. */
        st = jam_port_bind(port, klog, KEY(K_KLOG, 0), SIG_READABLE, PORT_BIND_PERSISTENT);
        if (st != OK)
            printf("console: kernel log: port_bind: %s; its lines show only with program "
                   "output\n", status_str(st));
        uint64_t end = 0;
        char c;
        jam_klog_read(klog, UINT64_MAX, &c, 1, &end);   /* past the end: 0 bytes, the end */
        draw_from = end > CATCH_UP_DRAWN ? end - CATCH_UP_DRAWN : 0;
        catching_up = true;
        klog_event();   /* the boot log so far: no news in it */
        catching_up = false;
        printf("console: the kernel log so far: drew %lu lines of the last %lu KiB (of %lu "
               "KiB)\n", (unsigned long)drawn_lines, (unsigned long)((end - draw_from) >> 10),
               (unsigned long)(end >> 10));
        notice_settle();
    } else {
        printf("console: no kernel log (%s)\n", status_str(st));
    }
    printf("console: %ux%u cells%s, %u client channel(s)%s\n", cols, rows,
           screen ? "" : " (no screen)", client_count(),
           log_off ? "; the kernel log off the screen (notices only)" : "");
    render();

    uint64_t last = 0;
    for (;;) {
        /* A client with requests left over: take what else is queued (a
         * key, another client) without sleeping, then give it another round. */
        uint64_t deadline = clients_pending() ? 0 : dirty ? last + RENDER_NS : DEADLINE_NEVER;
        if (reboot_deadline() < deadline)
            deadline = reboot_deadline();
        uint64_t nd = log_off ? notice_deadline() : DEADLINE_NEVER;
        if (nd < deadline)
            deadline = nd;
        struct port_packet pkt;
        st = jam_port_wait(port, deadline, &pkt);
        if (st == OK) {
            port_event(&pkt);
        } else if (st != ERR_TIMED_OUT) {
            printf("console: port_wait: %s\n", status_str(st));
            return 1;
        }
        clients_serve_pending();
        reboot_due();
        if (log_off)
            notice_tick(clients_show_all());
        uint64_t t = now();
        if (dirty && t >= last + RENDER_NS) {
            render();
            last = t;
        }
    }
}
