/* init: the boot splash's side of shell mode (<splash.h>).
 *
 * On a plain boot (the kernel passes init the word "splash") bin/splash
 * is started right after the console, before everything else, and the
 * shell waits for it: the splash plays the boot animation while the other
 * services start, says `played` when it is over, and holds its last
 * frame; init then starts the shell, and when the shell says it is ready
 * (initctl.shell_ready, ctl.c) init sends `go` and the splash hands the
 * screen back to the console. The splash runs once: it is never
 * started again, and its end (however it ends) counts as "played".
 *
 * No key skips it, so a splash that hangs would keep the shell away for
 * good: the shell waits for it at most SPLASH_DEADLINE from its start
 * (the video's 8.5 s, its wait of up to 2 s for the sound, and a
 * margin), then init kills it, which gives the screen back to the
 * console, and starts the shell anyway (shell.c, splash_overdue). */
#include <idl/splashch.h>
#include <splash.h>
#include "init.h"

#define SPLASH_DEADLINE (20 * NS_PER_S)   /* from its start: then the shell starts anyway */
#define ROUND           8                 /* messages read before the loop gets a turn */

static handle_t ch;          /* our end of the splash's channel (0: none) */
static handle_t ch_port;     /* the port it is bound on */
static uint64_t ch_key;
static bool played = true;   /* the shell may start (no splash, or it is over) */
static uint64_t started;     /* uptime ns: it was started (or expected, until it is) */

void splash_expect(void)
{
    played = false;
    started = now();   /* a splash that never starts can't keep the shell away either */
}

uint64_t splash_deadline(void)
{
    return played ? DEADLINE_NEVER : started + SPLASH_DEADLINE;
}

void splash_overdue(void)
{
    if (played)
        return;
    init_say("init: the splash didn't finish in %lu s: starting the shell",
             (unsigned long)(SPLASH_DEADLINE / NS_PER_S));
    played = true;
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
    started = now();
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

/* splashch.played: the animation is over. */
static void on_played(void *ctx)
{
    (void)ctx;
    if (!played)
        printf("init: the splash has played: starting the shell\n");
    played = true;
}

static const struct splashch_ops ops = { .played = on_played };

void splash_event(void)
{
    status_t st = OK;
    for (unsigned n = 0; ch && st == OK && n < ROUND; n++)
        st = splashch_serve_one(ch, &ops, NULL);   /* a message that breaks the format means nothing */
    if (st == ERR_PEER_CLOSED) {
        played = true;
        close_channel();
    } else if (ch && st == OK) {   /* more may be queued: the binding fires on a new edge */
        struct port_packet pkt = { .key = ch_key, .type = PORT_PACKET_USER };
        (void)jam_port_queue(ch_port, &pkt);
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
    (void)splashch_go(ch);   /* it is gone: nothing to tell */
    close_channel();
}
