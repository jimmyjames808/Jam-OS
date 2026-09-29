/* console: its clients, and the console protocol (console.h).
 *
 * Each client channel has a level (console.idl new_client): ADMIN (init's,
 * and the copy devmgr gets), SHELL (the shell's: no connect_input), PROGRAM
 * (what the shell hands a program it runs: write, size, clear, open_keys,
 * lend_screen). A client can only make channels of a lower level than its
 * own. keys.c has what the levels mean for the keys.
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
        jam_serial_write(root, text, length);
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
    st = jam_port_bind(port, mine, KEY(K_CLIENT, i), SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    clients[i] = mine;
    client_info[i].level = level;
    *out = theirs;
    return OK;
}

static const struct console_ops console_ops = {
    op_write, op_size, op_clear, op_open_keys, op_connect_input, op_lend_screen, op_new_client,
};

void clients_init(void)
{
    for (unsigned i = 0; i < START_CLIENTS; i++) {
        client_info[i].level = L_ADMIN;
        clients[i] = startup_handle(SR_USER + i);
        if (clients[i])
            jam_port_bind(port, clients[i], KEY(K_CLIENT, i), SIG_READABLE | SIG_PEER_CLOSED,
                          PORT_BIND_PERSISTENT);
    }
}

unsigned client_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_CLIENTS; i++)
        n += clients[i] != HANDLE_INVALID;
    return n;
}

void client_event(unsigned i)
{
    if (i >= MAX_CLIENTS || !clients[i])
        return;
    status_t st;
    while ((st = console_serve_one(clients[i], &console_ops, &client_info[i])) == OK)
        ;
    if (st != ERR_SHOULD_WAIT) {   /* ERR_PEER_CLOSED: that client end is gone */
        jam_port_unbind(port, clients[i], KEY(K_CLIENT, i));
        jam_handle_close(clients[i]);
        clients[i] = HANDLE_INVALID;
    }
}
