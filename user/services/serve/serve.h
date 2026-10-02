/* serve: the file server's insides (bin/serve; main.c says what it is).
 *
 * One thread serves everything from one wait set (<netwait.h>): the
 * control channels (init's shared one and each opener's), each file's
 * `give` channel while it waits for the file, each file's listener and
 * `file` channel, and every client's connection. A second thread, the
 * listen worker (listen.c), does the one thing that waits on another
 * process: asking netstack for a listener; it serves nobody, and hands
 * the listener back through the share's slot.
 *
 * A share is one file on one port: its state goes FREE -> GIVING (the
 * shell's `give` channel is open, waiting for the file) -> LISTENING (the
 * worker asks netstack for the listener) -> SERVING, and back to FREE on
 * `stop` or a failure. A client is one connection: HEAD (reading the
 * request's head), SEND (the response's head, then the file's bytes),
 * CLOSING (our end sent, waiting for the client's). The file is read
 * without waiting (file.idl's read sent, its answer taken from the `file`
 * channel when it comes), one read at a time per file, into its 64 KiB
 * transfer buffer and from there into the waiting client's chunk; the
 * clients that want their next chunk queue for it.
 *
 * Every client has a deadline: its head within HEAD_WAIT, no progress
 * (bytes read or written) for IDLE_WAIT, the next request on a kept
 * connection within KEEP_WAIT; past it the connection is closed. */
#pragma once

#include <http.h>
#include <net.h>
#include <netwait.h>
#include <os.h>
#include <serve.h>

#define HEAD_MAX   8192u                 /* a request's head, at most (else 431) */
#define CHUNK      (64u * 1024)          /* file bytes a client holds to send */
#define TX_RING    (64u * 1024)          /* a client's tx ring */
#define RX_RING    4096u                 /* ... and rx (requests are small) */
#define BACKLOG    4u                    /* connections waiting to be accepted, a file */
#define HEAD_WAIT  (10 * NS_PER_S)
#define IDLE_WAIT  (30 * NS_PER_S)
#define KEEP_WAIT  (15 * NS_PER_S)
#define CLOSE_WAIT (5 * NS_PER_S)        /* for the client's end after ours */
#define CTL_MAX    8u                    /* control channels from svc.connect at once */

/* What an entry in the wait set is: its tag, in the entry's user pointer
 * (the low bits; the slot above them). */
enum tag { T_CTL, T_GIVE, T_LISTENER, T_FILE, T_CLIENT, T_SHARED };

enum share_state { S_FREE, S_GIVING, S_LISTENING, S_SERVING };

struct client;

struct share {
    enum share_state state;
    uint16_t   port;             /* asked for, then the listener's */
    char       name[SERVE_NAME_MAX];
    const char *type;            /* its Content-Type (http_content_type) */
    handle_t   give;             /* GIVING: our end of the shell's channel */
    uint32_t   give_id;
    uint64_t   give_until;       /* GIVING: the file must come by then */
    struct jfile file;           /* LISTENING, SERVING: the file (its buffer mapped read-only) */
    uint32_t   file_id;
    uint64_t   size;
    struct net_listener lst;     /* SERVING */
    uint32_t   lst_id;
    /* the listen worker's answer (listen.c): done is stored last (release) */
    status_t   listen_st;
    bool       done;
    /* reading the file: one read at a time, for the client at the queue's head */
    struct client *queue[SERVE_PER_SHARE];
    unsigned   queued;
    bool       reading;          /* a read is out; its answer comes on the file channel */
    uint32_t   txid, last_txid;
    struct client *reader;       /* the client it is for (NULL: it went meanwhile) */
    /* counts */
    unsigned   clients;          /* connected now */
    uint64_t   requests, bytes;
};

enum client_state { C_HEAD, C_SEND, C_CLOSING };

struct client {
    bool       used;
    enum client_state state;
    struct share *sh;
    struct net_sock s;
    uint32_t   id;               /* its wait set entry */
    uint32_t   peer;
    uint16_t   peer_port;
    uint64_t   deadline;         /* absolute: see the header */
    uint8_t    head[HEAD_MAX];   /* the request's head (and what came after it) */
    size_t     have;
    /* the response */
    struct http_request req;
    unsigned   status;
    bool       close_after;      /* close once it is sent */
    char       out[640];         /* its head */
    size_t     out_len, out_sent;
    uint64_t   at, left;         /* the file's bytes still to send: from `at`, `left` of them */
    uint8_t   *chunk;            /* CHUNK bytes: the file's next bytes (malloc'd) */
    size_t     chunk_len, chunk_sent;
    bool       waiting;          /* queued for the file, or its read is out */
    uint64_t   sent;             /* body bytes sent for this request */
};

/* main.c */
extern struct netwait *serve_w;
extern struct share shares[SERVE_SHARES];
/* The wait set's user pointer for a tag and a slot, and back. */
void    *serve_key(enum tag t, unsigned slot);
/* A line in the log, at most LOG_BURST in LOG_WINDOW (the rest counted, said later). */
void     serve_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* listen.c */
status_t listen_init(void);
/* Ask the worker for share i's listener; the answer: shares[i].done. */
status_t listen_ask(unsigned i);

/* share.c */
/* Share slot i (free) for port and name, waiting for its file: *out_give is
 * the end the shell gets. */
status_t share_giving(unsigned i, uint16_t port, const char *name, handle_t *out_give);
/* Its give channel is readable (the file came) or closed. */
void     share_give_ready(unsigned i);
/* Shares whose file didn't come in time freed; the earliest of `until` and
 * the deadlines of those still waiting. */
uint64_t shares_expire(uint64_t until);
/* A share's listener came (or didn't): serve it, or free it. */
void     share_listened(unsigned i);
/* Stop share i: its listener, its file and its clients go. */
void     share_stop(unsigned i, const char *why);
/* Its listener's answer is in: take the connection. */
void     share_accept(unsigned i);
/* Its file channel has the answer to a read. */
void     share_file_ready(unsigned i);
/* Client c wants its next chunk: queue it for the file. */
void     share_want(struct share *sh, struct client *c);
/* Client c goes: off the share's queue. */
void     share_forget(struct share *sh, struct client *c);

/* client.c */
extern struct client clients[SERVE_CLIENTS];
/* A new connection for sh (one of SERVE_CLIENTS slots): false if none is free. */
bool     client_new(struct share *sh, struct net_sock *s, uint32_t peer, uint16_t port);
/* Its entry was reported ready. */
void     client_ready(struct client *c, const struct netwait_ready *r);
/* Its chunk was filled (n bytes; 0: the file ended early) or the read failed. */
void     client_chunk(struct client *c, size_t n, status_t st);
/* Close it now (why: for the log; NULL: quietly). */
void     client_end(struct client *c, const char *why);
/* Every client past its deadline closed; the earliest deadline left. */
uint64_t clients_expire(uint64_t t);
