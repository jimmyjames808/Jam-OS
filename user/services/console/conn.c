/* console: the connection back end (console.h, struct backend; <termconn.h>).
 *
 * A terminal whose screen and keyboard are a connection: bin/console with
 * the argument "conn". There is no text model, no window, no COM1 and no
 * escape interpreter here: the programs' output goes out as bytes on the
 * master channel, for a terminal at the other end (an SSH client's) to
 * show, and what it types comes in on the same channel (connin.c). The
 * size comes on a control channel (`conn_ctl`).
 *
 * Output. console.write's bytes are queued in a ring (OUT_RING) as they
 * were written, a bare "\n" as "\r\n", and sent in messages of at most
 * CONN_OUT_MSG_MAX bytes by flush. When the master's queue is full the
 * ring fills, and while less than OUT_ROOM_MIN is free the console serves
 * no client's request (congested): the writers wait, as a program writing
 * into a pipe does, and nothing is dropped. The kernel log's lines are the
 * exception: a line that doesn't fit is dropped and counted, and a notice
 * says so once there is room, because the log is not worth stopping the
 * clients for. A request can overrun the room only if kernel lines took it
 * since the check; its tail is then dropped the same way.
 *
 * The owner closing the master hangs the terminal up (closing: main ends
 * with 0, the clients' channels close); so does a write finding it closed.
 * The control channel closing only ends the size changes. */
#include <idl/conn_ctl.h>
#include <termconn.h>
#include <utf8.h>
#include "console.h"

#define OUT_RING     (64u << 10)        /* output waiting for the master; a power of two */
#define OUT_ROOM_MIN 4096u              /* free room to serve a request: a write is at most
                                         * 2048 bytes, each possibly sent as CR LF */
#define OUT_MSGS     16                 /* messages one turn sends, at most */
#define RETRY_NS     (5 * NS_PER_MS)    /* the master's queue is full: try again after this */
#define CTL_MSGS     16                 /* control requests one turn serves, at most */
#define DEF_COLS     80
#define DEF_ROWS     24

_Static_assert(CONN_COLS_MAX == MAX_COLS && CONN_ROWS_MAX == MAX_ROWS,
               "a connection console reports the window console's limits");
_Static_assert((OUT_RING & (OUT_RING - 1)) == 0, "the ring's index is masked");

static handle_t master, ctl;
static uint16_t term_cols = DEF_COLS, term_rows = DEF_ROWS;   /* the remote terminal's size */

static uint8_t *out;               /* the ring, OUT_RING bytes */
static uint32_t out_head, out_len; /* its first byte, and how many are queued */
static uint64_t out_retry_at;      /* the master was full: the next try (0: none) */
static uint32_t out_dropped;       /* bytes dropped since the last notice */
static bool out_cr;                /* the last byte queued as text was CR: no CR before an LF */
static bool ctl_more;              /* the control channel had more than a turn's requests */

/* ---- the queue ----------------------------------------------------------------- */

static void out_put(uint8_t b)
{
    if (out_len == OUT_RING) {
        out_dropped++;
        return;
    }
    out[(out_head + out_len++) & (OUT_RING - 1)] = b;
}

/* Program text: a bare LF is sent as CR LF. */
static void out_text(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n' && !out_cr)
            out_put('\r');
        out_put(p[i]);
        out_cr = p[i] == '\r';
    }
}

static bool conn_congested(void)
{
    return OUT_RING - out_len < OUT_ROOM_MIN;
}

/* ESC [ ? 2004 h and l, in the output: the writing client asks for
 * bracketed paste or stops asking (connin.c types the markers only to a
 * client that did). The window console's term.c reads these too. */
static void scan_modes(struct client *c, const uint8_t *p, uint32_t n)
{
    static const char mode[] = "\033[?2004";
    static struct client *scanning;   /* whose bytes the match so far is */
    static unsigned at;               /* bytes of `mode` matched */
    if (c != scanning) {
        scanning = c;
        at = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (at == sizeof(mode) - 1) {
            if (p[i] == 'h' || p[i] == 'l')
                c->bracketed = p[i] == 'h';
            at = 0;
            if (p[i] != 0x1b)
                continue;
        }
        at = p[i] == (uint8_t)mode[at] ? at + 1 : p[i] == 0x1b ? 1 : 0;
    }
}

static void conn_write(struct client *c, const uint8_t *text, uint32_t length)
{
    scan_modes(c, text, length);
    out_text(text, length);
}

/* A kernel log line as text, one line, with any control character a '?'
 * (the log is checked by the kernel; the remote terminal is not ours to
 * trust with an escape sequence a process logged). */
static void conn_log_line(const char *s, size_t n)
{
    if (OUT_RING - out_len < n + 2) {
        out_dropped += (uint32_t)n;
        return;
    }
    for (size_t i = 0; i < n;) {
        uint8_t b = (uint8_t)s[i];
        uint32_t cp;
        int k = b < 0x80 ? 1 : utf8_seq((const uint8_t *)s + i, n - i, &cp);
        if (b < 0x80) {
            out_put(b == '\t' ? ' ' : b < 0x20 || b == 0x7f ? '?' : b);
        } else if (k > 0 && !utf8_is_control(cp)) {
            for (int j = 0; j < k; j++)
                out_put((uint8_t)s[i + (size_t)j]);
        } else {
            out_put('?');
            k = k > 0 ? k : -k;
        }
        i += (size_t)k;
    }
    out_put('\r');
    out_put('\n');
    out_cr = false;
}

static void conn_clear(void)
{
    static const uint8_t home_clear[] = "\033[2J\033[H";
    for (unsigned i = 0; i < sizeof(home_clear) - 1; i++)
        out_put(home_clear[i]);
}

static void conn_size(uint16_t *c, uint16_t *r)
{
    *c = term_cols;
    *r = term_rows;
}

/* ---- sending ------------------------------------------------------------------- */

/* The terminal is gone: the console ends (main), its clients see it. */
static void hangup(const char *why)
{
    if (!closing)
        printf("console: connection: hung up (%s)\n", why);
    closing = true;
}

/* The notice that kernel lines were dropped, once there is room for it. */
static void dropped_notice(void)
{
    char msg[64];
    if (!out_dropped || OUT_RING - out_len < sizeof(msg))
        return;
    int n = snprintf(msg, sizeof(msg), "\r\n[console: %u bytes of output dropped]\r\n",
                     (unsigned)out_dropped);
    out_dropped = 0;
    for (int i = 0; i < n; i++)
        out_put((uint8_t)msg[i]);
    out_cr = false;
}

/* Send what is queued: whole messages while the master takes them. A full
 * queue (or no memory for the message) is tried again after RETRY_NS. */
static void out_flush(void)
{
    if (closing || (out_retry_at && now() < out_retry_at))
        return;
    out_retry_at = 0;
    dropped_notice();
    for (unsigned k = 0; out_len && k < OUT_MSGS; k++) {
        uint32_t n = out_len < CONN_OUT_MSG_MAX ? out_len : CONN_OUT_MSG_MAX;
        if (n > OUT_RING - out_head)
            n = OUT_RING - out_head;   /* up to the ring's end: one message is contiguous */
        status_t st = jam_channel_write(master, out + out_head, n, NULL, 0);
        if (st == ERR_PEER_CLOSED) {
            hangup("the master was closed");
            return;
        }
        if (st != OK) {
            out_retry_at = now() + RETRY_NS;
            return;
        }
        out_head = (out_head + n) & (OUT_RING - 1);
        out_len -= n;
    }
}

/* ---- the control channel -------------------------------------------------------- */

static status_t op_resize(void *ctx, uint16_t c, uint16_t r)
{
    (void)ctx;
    if (!c || !r)
        return ERR_INVALID_ARGS;
    term_cols = c < CONN_COLS_MAX ? c : CONN_COLS_MAX;
    term_rows = r < CONN_ROWS_MAX ? r : CONN_ROWS_MAX;
    return OK;
}

static const struct conn_ctl_ops ctl_ops = { .resize = op_resize };

static void ctl_take(void)
{
    ctl_more = false;
    for (unsigned k = 0; ctl && k < CTL_MSGS; k++) {
        status_t st = conn_ctl_serve_one(ctl, &ctl_ops, NULL);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK) {   /* its owner let it go: the size stays as it is */
            jam_port_unbind(port, ctl, KEY(K_CTL, 0));
            jam_handle_close(ctl);
            ctl = HANDLE_INVALID;
            return;
        }
    }
    ctl_more = ctl != HANDLE_INVALID;
}

/* ---- the back end --------------------------------------------------------------- */

void conn_arg(const char *arg)
{
    unsigned long v = 0;
    if (strncmp(arg, "cols=", 5) && strncmp(arg, "rows=", 5))
        return;
    const char *p = arg + 5;
    if (!*p || *p == '0' || strlen(p) > 4)
        return;
    for (; *p; p++) {
        if (*p < '0' || *p > '9')
            return;
        v = v * 10 + (unsigned long)(*p - '0');
    }
    if (arg[0] == 'c')
        term_cols = v < CONN_COLS_MAX ? (uint16_t)v : CONN_COLS_MAX;
    else
        term_rows = v < CONN_ROWS_MAX ? (uint16_t)v : CONN_ROWS_MAX;
}

static bool conn_init(void)
{
    master = startup_handle(CONN_MASTER_ROLE);
    ctl = startup_handle(CONN_CTL_ROLE);
    if (!master) {
        printf("console: connection: no master channel (startup role %u)\n", CONN_MASTER_ROLE);
        return false;
    }
    out = malloc(OUT_RING);
    if (!out || !conn_in_init(master)) {
        printf("console: out of memory\n");
        return false;
    }
    status_t st = jam_port_bind(port, master, KEY(K_MASTER, 0), SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st != OK) {
        printf("console: connection: port_bind: %s\n", status_str(st));
        return false;
    }
    if (ctl && jam_port_bind(port, ctl, KEY(K_CTL, 0), SIG_READABLE | SIG_PEER_CLOSED,
                             PORT_BIND_PERSISTENT) != OK) {
        jam_handle_close(ctl);   /* no way to read it: the size stays */
        ctl = HANDLE_INVALID;
    }
    return true;
}

static void conn_banner(unsigned clients, bool log_off)
{
    (void)log_off;   /* a connection console shows the log only when a client asks */
    printf("console: connection: %ux%u cells, %u client channel(s)%s\n", term_cols, term_rows,
           clients, ctl ? "" : " (no control channel)");
}

static void conn_event(uint32_t kind, uint32_t i)
{
    (void)i;
    if (kind == K_MASTER) {
        status_t st = conn_in_take();
        if (st != OK)
            hangup(st == ERR_PEER_CLOSED ? "the master was closed" : status_str(st));
    } else if (kind == K_CTL) {
        ctl_take();
    }
}

static uint64_t conn_deadline(void)
{
    uint64_t d = conn_in_deadline();
    if (ctl_more)
        return 0;
    if (out_len)
        d = out_retry_at ? (out_retry_at < d ? out_retry_at : d) : 0;
    return d;
}

static void conn_flush(void)
{
    status_t st = conn_in_idle();
    if (st != OK)
        hangup(st == ERR_PEER_CLOSED ? "the master was closed" : status_str(st));
    if (ctl_more)
        ctl_take();
    out_flush();
    if (!conn_congested())
        clients_resume();   /* requests that waited for room */
}

const struct backend conn_backend = {
    .init = conn_init, .banner = conn_banner, .write = conn_write, .log_line = conn_log_line,
    .size = conn_size, .clear = conn_clear, .event = conn_event, .deadline = conn_deadline,
    .flush = conn_flush, .congested = conn_congested,
};
