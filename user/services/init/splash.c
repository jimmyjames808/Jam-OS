/* init: the boot splash's side of shell mode (<splash.h>).
 *
 * On a plain boot (the kernel passes init the word "splash") bin/splash
 * is started right after the console, before everything else, and the
 * shell waits for it: the splash plays the boot animation while the other
 * services start, says SPLASH_PLAYED when it is over, and holds its last
 * frame; init then starts the shell, and when the shell says it is ready
 * (initctl.shell_ready, ctl.c) init sends SPLASH_GO and the splash hands
 * the screen back to the console. The splash runs once: it is never
 * started again, and its end (however it ends) counts as "played". */
#include <splash.h>
#include "init.h"

static handle_t ch;          /* our end of the splash's channel (0: none) */
static handle_t ch_port;     /* the port it is bound on */
static uint64_t ch_key;
static bool played = true;   /* the shell may start (no splash, or it is over) */

void splash_expect(void)
{
    played = false;
}

bool splash_played(void)
{
    return played;
}

status_t splash_channel(handle_t port, uint64_t key, handle_t *theirs)
{
    handle_t mine, other;
    status_t st = jam_channel_create(&mine, &other);
    if (st != OK)
        return st;
    st = jam_port_bind(port, mine, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(other);
        return st;
    }
    ch = mine;
    ch_port = port;
    ch_key = key;
    *theirs = other;
    return OK;
}

static void close_channel(void)
{
    if (!ch)
        return;
    jam_port_unbind(ch_port, ch, ch_key);
    jam_handle_close(ch);
    ch = HANDLE_INVALID;
}

void splash_event(void)
{
    uint32_t msg = 0, n = 0;
    struct channel_read_args a = {
        .h = ch, .bytes_cap = sizeof(msg), .bytes = (uint64_t)(uintptr_t)&msg,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    status_t st = ch ? jam_channel_read(&a) : ERR_BAD_STATE;
    if (st == OK && n == sizeof(msg) && msg == SPLASH_PLAYED) {
        printf("init: the splash has played: starting the shell\n");
        played = true;
    } else if (st == ERR_PEER_CLOSED) {
        played = true;
        close_channel();
    }
}

void splash_ended(void)
{
    played = true;
    close_channel();
}

void splash_shell_ready(void)
{
    if (!ch)
        return;
    uint32_t msg = SPLASH_GO;
    (void)jam_channel_write(ch, &msg, sizeof(msg), NULL, 0);   /* it is gone: nothing to tell */
    close_channel();
}
