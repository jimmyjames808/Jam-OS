/* console: its clients, and the console protocol (console.h).
 *
 * Each client channel has a level (console.idl new_client): ADMIN (init's,
 * and the copy devmgr gets), SHELL (the shell's: no connect_input), PROGRAM
 * (what the shell hands a program it runs: write, size, clear, open_keys,
 * lend_screen; not blank, not show_log). A client can only make channels
 * of a lower level than its own. keys.c has what the levels mean for the
 * keys.
 *
 * Program output (console.write) is also written to COM1 as it is (the
 * kernel log goes there by itself), so a serial terminal, and the QEMU
 * tests, see the same session. */
#include "console.h"

static handle_t clients[MAX_CLIENTS];
static struct client client_info[MAX_CLIENTS];

static status_t op_write(void *ctx, uint16_t length, const uint8_t text[2048])
{
    (void)ctx;
    if (length > 2048)
        return ERR_INVALID_ARGS;
    /* Kernel lines logged before this write go above it: e.g. a ktest's
     * output before the shell's summary line. */
    klog_event();
    bool was_alt = alt_on;
    for (unsigned i = 0; i < length; i++)
        out_char(text[i]);
    if (!was_alt || !alt_on)   /* the alternate screen isn't mirrored to COM1 */
        (void)jam_serial_write(root, text, length);   /* the mirror is best effort */
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

/* Watch client channel h (slot i) on the port; a failure is logged. */
static status_t bind_client(unsigned i, handle_t h)
{
    status_t st = jam_port_bind(port, h, KEY(K_CLIENT, i), SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st != OK)
        printf("console: client %u: port_bind: %s; channel refused\n", i, status_str(st));
    return st;
}

static status_t op_new_client(void *ctx, uint8_t level, handle_t *out)
{
    const struct client *c = ctx;
    if (level > L_PROGRAM)
        return ERR_INVALID_ARGS;
    if (level <= c->level)
        return ERR_ACCESS_DENIED;
    unsigned i;
    for (i = START_CLIENTS; i < MAX_CLIENTS && clients[i]; i++)
        ;
    if (i == MAX_CLIENTS)
        return ERR_NO_RESOURCES;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    if ((st = bind_client(i, mine)) != OK) {   /* refused: the caller gets the error */
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    clients[i] = mine;
    client_info[i] = (struct client){ .level = level };
    *out = theirs;
    return OK;
}

static status_t op_blank(void *ctx, uint8_t on)
{
    const struct client *c = ctx;
    if (c->level == L_PROGRAM)
        return ERR_ACCESS_DENIED;
    if (on > 1)
        return ERR_INVALID_ARGS;
    screen_blank(on);
    return OK;
}

/* A client asks for the kernel log on the screen, or stops asking. */
static status_t op_show_log(void *ctx, uint8_t on, const uint8_t only[32])
{
    struct client *c = ctx;
    if (c->level == L_PROGRAM)
        return ERR_ACCESS_DENIED;
    if (on > 1 || only[LOG_ONLY_MAX - 1])
        return ERR_INVALID_ARGS;
    klog_event();   /* what was logged before the change goes by the old rule */
    c->show_log = on == 1;
    memcpy(c->log_only, only, LOG_ONLY_MAX);
    return OK;
}

static const struct console_ops console_ops = {
    op_write, op_size, op_clear, op_open_keys, op_connect_input, op_lend_screen, op_new_client,
    op_blank, op_show_log,
};

void clients_init(void)
{
    for (unsigned i = 0; i < START_CLIENTS; i++) {
        client_info[i].level = L_ADMIN;
        handle_t h = startup_handle(SR_USER + i);
        /* Unbound, nothing would ever serve it: close it instead, so its
         * client sees ERR_PEER_CLOSED rather than waiting forever. */
        if (h && bind_client(i, h) != OK)
            jam_handle_close(h);
        else
            clients[i] = h;
    }
}

unsigned client_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        n += clients[i] != HANDLE_INVALID;
    return n;
}

/* Clients whose last round ended on the budget, with requests maybe left:
 * bit i is clients[i]. Their channels stay readable, so the PERSISTENT
 * binding (it fires on an edge) won't fire again: the main loop comes back
 * for them. */
static uint32_t pending;
_Static_assert(MAX_CLIENTS <= 32, "pending is a 32-bit mask");

void client_event(unsigned i)
{
    if (i >= MAX_CLIENTS)
        return;
    pending &= ~(1u << i);
    if (!clients[i])
        return;
    uint64_t t0 = now();
    status_t st = OK;
    for (unsigned n = 0; n < CLIENT_BUDGET && now() - t0 < CLIENT_BUDGET_NS; n++)
        if ((st = console_serve_one(clients[i], &console_ops, &client_info[i])) != OK)
            break;
    if (st == OK) {
        pending |= 1u << i;   /* the budget ran out first */
        return;
    }
    if (st != ERR_SHOULD_WAIT) {   /* ERR_PEER_CLOSED: that client end is gone */
        jam_port_unbind(port, clients[i], KEY(K_CLIENT, i));
        jam_handle_close(clients[i]);
        clients[i] = HANDLE_INVALID;
        client_info[i].show_log = false;   /* gone: it asks no more */
    }
}

bool clients_show_line(const char *name, size_t n)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        const struct client *c = &client_info[i];
        if (!clients[i] || !c->show_log)
            continue;
        if (!c->log_only[0] || !n || (strlen(c->log_only) == n && !memcmp(c->log_only, name, n)))
            return true;
    }
    return false;
}

bool clients_show_all(void)
{
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] && client_info[i].show_log && !client_info[i].log_only[0])
            return true;
    return false;
}

bool clients_pending(void)
{
    return pending != 0;
}

void clients_serve_pending(void)
{
    uint32_t p = pending;   /* one round each, even for those that stay pending */
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        if (p & 1u << i)
            client_event(i);
}
