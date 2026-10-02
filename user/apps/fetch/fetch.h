/* fetch: a file from a web server over plain HTTP/1.1 (bin/fetch, the
 * helper of the shell's `fetch`). The shell checks the URL, opens where
 * the body goes and starts this with only that, its list (the network and
 * the resolver: `svc net`, `svc dns`) and two channels of its own, so a
 * hostile server reaches nothing else of the shell's. Its handles:
 *   SR_USER + 0, 1  the file to write (file_give / file_adopt), or
 *   SR_USER + 3     a channel the body goes down in messages of at most
 *                   FETCH_MSG bytes (the shell puts them in its pipe)
 *   SR_USER + 2     the shell's stop channel: a byte (or its close) on it
 *                   is Ctrl+C: it stops at once, closing the connection
 *   SR_STDOUT       its lines (progress, the end), the shell's own
 * argv: fetch <url> file|pipe.
 *
 * Files: main.c the handles, the output and the lines; conn.c the
 * connection (the name, the connect, reads and writes, each with its
 * deadline and the stop channel watched, all through one wait set); get.c
 * one request and its answer (the head, the body by Content-Length,
 * chunks or the connection's end, the redirects).
 *
 * Exit: 0 the whole body; 1 it failed (said why); 130 stopped. */
#pragma once

#include <http.h>
#include <net.h>
#include <netwait.h>
#include <os.h>

#define FETCH_MSG       4096u                /* bytes of a message on the body channel */
#define FETCH_REDIRECTS 3u                   /* redirects followed, at most */
#define CONNECT_WAIT    (10 * NS_PER_S)      /* a name, then a connection, each */
#define HEAD_WAIT       (20 * NS_PER_S)      /* the whole head, from the connection */
#define IDLE_WAIT       (30 * NS_PER_S)      /* a body that sends nothing this long has stalled */
#define SAY_EVERY       (2 * NS_PER_S)       /* a progress line */
#define EXIT_STOPPED    130

/* The connection and its waiting. */
struct conn {
    struct net_sock s;
    bool            open;      /* s holds a socket */
    struct netwait *w;
    uint32_t        sock_id;   /* s in w (0: not in it) */
    uint32_t        stop_id;   /* the stop channel in w */
    uint32_t        peer;      /* the address it went to */
};

/* conn.c */
/* The wait set, with the stop channel in it. */
status_t conn_init(struct conn *c);
/* host's address (a dotted one as it is, else the resolver's first),
 * waiting at most CONNECT_WAIT. ERR_CANCELED: stopped. */
status_t conn_resolve(struct conn *c, const char *host, uint32_t *addr);
/* A connection to addr:port, OPEN, waiting at most CONNECT_WAIT. */
status_t conn_open(struct conn *c, uint32_t addr, uint16_t port);
/* All n bytes into the connection, by the deadline. */
status_t conn_write(struct conn *c, const void *data, size_t n, uint64_t deadline);
/* Some bytes (at most cap) by the deadline: *got > 0, or 0 at the peer's
 * end. The connection's error once it closed with one. */
status_t conn_read(struct conn *c, void *buf, size_t cap, uint64_t deadline, size_t *got);
/* Close the connection (the wait set stays). */
void     conn_close(struct conn *c);
/* The stop channel has a byte or closed (Ctrl+C). */
bool     conn_stopped(void);

/* get.c */
/* The body of url (redirects followed) through fetch_out; *final: the URL
 * it came from. Says what went wrong itself; ERR_CANCELED when stopped. */
status_t get_url(struct conn *c, const struct http_url *url, struct http_url *final);

/* main.c: where the body goes. */
status_t fetch_out(const uint8_t *data, size_t n);
/* The answer's head is in: its length (-1: unknown) and type, said once. */
void     fetch_started(int64_t length, unsigned status);
