/* console: the text terminal on the screen.
 *
 * Owns the boot framebuffer once it has started (framebuffer_take: a WC
 * VMO of it; the kernel stops drawing its log and draws again if this
 * process dies). Serves the `console` protocol (abi/idl/console.idl) to
 * programs and the `input` protocol on each channel connect_input hands
 * out (a HID driver, the serial source). The parts are listed in
 * console.h.
 *
 * Startup handles:
 *   SR_RESOURCE     the root resource with RIGHT_READ (klog_open),
 *                   RIGHT_WRITE (framebuffer_take, serial_write) and
 *                   RIGHT_MANAGE (reboot, on Ctrl+Alt+Del)
 *   SR_USER + n     server ends of `console` channels (n = 0..7): init's;
 *                   clients share one by duplicating the client end
 *
 * The screen: a grid of 8x16 cells. Committed lines live in a scrollback
 * ring; the line the programs are writing (the "current line", where the
 * cursor is) is always the bottom row. Kernel log lines (a klog reader,
 * here) are committed ABOVE the current line, so a log line never breaks
 * up the prompt the shell is editing. Colours: kernel lines grey, lines a
 * process logged through debug_write ("[name] ...") green, program output
 * white (ESC [ ... m changes it). Shift+PageUp/PageDown (or PageUp/PageDown
 * from a serial terminal) scroll back; any other key goes back to the
 * bottom.
 *
 * Who may do what: clients.c (the client levels) and keys.c (the keys).
 * Drawing, and lending the screen to a program: screen.c. Full-screen text
 * programs can use the alternate screen: text.c. */
#include "console.h"

handle_t root, port;

/* ---- the kernel log ------------------------------------------------------------- */

static handle_t klog;
static uint64_t klog_pos;
static char klog_buf[KLOG_BUF];
static char partial[1024];
static size_t npartial;

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
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    root = startup_handle(SR_RESOURCE);
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
        jam_port_bind(port, klog, KEY(K_KLOG, 0), SIG_READABLE, PORT_BIND_PERSISTENT);
        klog_event();   /* the boot log so far */
    } else {
        printf("console: no kernel log (%s)\n", status_str(st));
    }
    printf("console: %ux%u cells%s, %u client channel(s)\n", cols, rows,
           screen ? "" : " (no screen)", client_count());
    render();

    uint64_t last = 0;
    for (;;) {
        uint64_t deadline = dirty ? last + RENDER_NS : DEADLINE_NEVER;
        struct port_packet pkt;
        st = jam_port_wait(port, deadline, &pkt);
        if (st == OK) {
            port_event(&pkt);
        } else if (st != ERR_TIMED_OUT) {
            printf("console: port_wait: %s\n", status_str(st));
            return 1;
        }
        uint64_t t = now();
        if (dirty && t >= last + RENDER_NS) {
            render();
            last = t;
        }
    }
}
