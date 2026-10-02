/* serve: the shares (serve.h has the states). A share's file comes on its
 * `give` channel as two handles; its transfer buffer is mapped here
 * read-only (no call to the file's service: the size is the shell's), and
 * the file is read with file.idl's read sent without waiting, one read at a
 * time, for the client at the head of the share's queue. */
#include <fs_idl.h>
#include "serve.h"

#define BUF_MAX (1u << 20)   /* a bigger transfer buffer than this is a broken service */

static void answer(struct share *sh, status_t st)
{
    struct serve_answer a = { .status = st, .port = sh->port };
    if (sh->give) {
        (void)jam_channel_write(sh->give, &a, sizeof(a), NULL, 0);   /* gone: nobody asks */
        (void)netwait_remove(serve_w, sh->give_id);
        jam_handle_close(sh->give);
    }
    sh->give = HANDLE_INVALID;
    sh->give_id = 0;
}

/* Back to S_FREE: whatever it holds goes (no client is left on it). */
static void share_free(struct share *sh)
{
    if (sh->give)
        answer(sh, ERR_CANCELED);
    if (sh->file_id)
        (void)netwait_remove(serve_w, sh->file_id);
    if (sh->lst_id)
        (void)netwait_remove(serve_w, sh->lst_id);
    if (sh->state == S_SERVING)
        net_listener_close(&sh->lst);
    if (sh->file.ch)
        file_close(&sh->file);   /* unmaps the buffer, closes both handles */
    memset(sh, 0, sizeof(*sh));
}

status_t share_giving(unsigned i, uint16_t port, const char *name, handle_t *out_give)
{
    struct share *sh = &shares[i];
    handle_t theirs;
    memset(sh, 0, sizeof(*sh));
    status_t st = jam_channel_create(&sh->give, &theirs);
    struct netwait_handle h = { sh->give, SIG_READABLE, 0, SIG_PEER_CLOSED };
    if (st == OK)
        st = netwait_add_handle(serve_w, &h, NETWAIT_READ, serve_key(T_GIVE, i), &sh->give_id);
    if (st != OK) {
        if (sh->give) {
            jam_handle_close(sh->give);
            jam_handle_close(theirs);
        }
        memset(sh, 0, sizeof(*sh));
        return st;
    }
    sh->state = S_GIVING;
    sh->port = port;
    memcpy(sh->name, name, strnlen(name, SERVE_NAME_MAX - 1));
    sh->type = http_content_type(sh->name);
    sh->give_until = now() + SERVE_GIVE_WAIT;
    *out_give = theirs;
    return OK;
}

/* The file's transfer buffer, mapped read-only into sh->file. */
static status_t map_file(struct share *sh, handle_t ch, handle_t buf, uint64_t size)
{
    uint64_t len = 0, addr = 0;
    sh->file = (struct jfile){ .ch = ch, .buf_vmo = buf, .flags = FS_READ, .size = size };
    status_t st = jam_vmo_get_size(buf, &len);
    if (st == OK && (!len || len > BUF_MAX || (len & (PAGE_SIZE - 1))))
        st = ERR_INVALID_ARGS;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), buf, 0, len, VMAR_READ, &addr);
    if (st != OK)
        return st;
    sh->file.buf = (uint8_t *)(uintptr_t)addr;
    sh->file.buf_size = (uint32_t)len;
    sh->size = size;
    return OK;
}

/* The message on give: the file, checked. */
static status_t take_file(struct share *sh)
{
    struct serve_give g;
    handle_t hs[2] = { 0 };
    uint32_t n = 0, nh = 0;
    struct channel_read_args a = {
        .h = sh->give, .bytes_cap = sizeof(g), .bytes = (uint64_t)(uintptr_t)&g,
        .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = 2, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_SHOULD_WAIT)
        return st;
    if (st == OK && (n != sizeof(g) || nh != 2 || g.magic != SERVE_GIVE_MAGIC || g.reserved))
        st = ERR_INVALID_ARGS;
    if (st == OK)
        return map_file(sh, hs[0], hs[1], g.size);   /* the handles are sh->file's now */
    if (st != ERR_PEER_CLOSED)
        for (uint32_t k = 0; k < nh && k < 2; k++)
            jam_handle_close(hs[k]);
    return st;
}

void share_give_ready(unsigned i)
{
    struct share *sh = &shares[i];
    if (sh->state != S_GIVING)
        return;
    status_t st = take_file(sh);
    if (st == ERR_SHOULD_WAIT)
        return;
    if (st == OK)
        st = listen_ask(i);
    if (st != OK) {
        answer(sh, st);
        share_free(sh);
        return;
    }
    sh->state = S_LISTENING;   /* the give channel stays for the answer */
}

uint64_t shares_expire(uint64_t until)
{
    uint64_t t = now();
    for (unsigned i = 0; i < SERVE_SHARES; i++) {
        struct share *sh = &shares[i];
        if (sh->state != S_GIVING)
            continue;
        if (t >= sh->give_until)
            share_free(sh);   /* the shell never sent the file */
        else if (sh->give_until < until)
            until = sh->give_until;
    }
    return until;
}

void share_listened(unsigned i)
{
    struct share *sh = &shares[i];
    status_t st = sh->listen_st;
    struct netwait_handle h;
    if (st == OK) {
        sh->state = S_SERVING;   /* lst is ours from here (share_free closes it) */
        sh->port = sh->lst.port;
        net_listener_waitable(&sh->lst, &h);
        st = netwait_add_handle(serve_w, &h, NETWAIT_READ, serve_key(T_LISTENER, i), &sh->lst_id);
    }
    /* The file's channel joins the set only now: its end (a disk gone)
     * is reported at every wait, and only a serving share acts on it. */
    struct netwait_handle f = { sh->file.ch, SIG_READABLE, 0, SIG_PEER_CLOSED };
    if (st == OK)
        st = netwait_add_handle(serve_w, &f, NETWAIT_READ, serve_key(T_FILE, i), &sh->file_id);
    if (st == OK)
        st = net_tcp_accept_send(&sh->lst);
    answer(sh, st);
    if (st != OK) {
        share_free(sh);
        return;
    }
    serve_log("serve: serving %s (%lu bytes, %s) on port %u\n", sh->name,
              (unsigned long)sh->size, sh->type, sh->port);
}

void share_stop(unsigned i, const char *why)
{
    struct share *sh = &shares[i];
    if (sh->state != S_SERVING)
        return;
    for (unsigned k = 0; k < SERVE_CLIENTS; k++)
        if (clients[k].used && clients[k].sh == sh)
            client_end(&clients[k], "the file stopped being served");
    serve_log("serve: port %u (%s): %s after %lu requests, %lu bytes\n", sh->port, sh->name, why,
              (unsigned long)sh->requests, (unsigned long)sh->bytes);
    share_free(sh);
}

void share_accept(unsigned i)
{
    struct share *sh = &shares[i];
    struct net_sock s;
    uint32_t peer = 0;
    uint16_t port = 0;
    if (sh->state != S_SERVING)
        return;
    status_t st = net_tcp_accept_take(&sh->lst, &s, &peer, &port);
    if (st == ERR_SHOULD_WAIT)
        return;
    status_t next = net_tcp_accept_send(&sh->lst);
    /* client_new takes s only when it says true. */
    if (st == OK && (sh->clients >= SERVE_PER_SHARE || !client_new(sh, &s, peer, port))) {
        serve_log("serve: port %u: too many clients: %u.%u.%u.%u:%u turned away\n", sh->port,
                  peer >> 24, (peer >> 16) & 0xff, (peer >> 8) & 0xff, peer & 0xff, port);
        net_close(&s);   /* its request unread: a reset */
    } else if (st != OK) {
        serve_log("serve: port %u: accept: %s\n", sh->port, status_str(st));
    }
    if (next != OK || st == ERR_PEER_CLOSED)
        share_stop(i, "its listener failed");
}

/* The next read for the client at the queue's head, if none is out. */
static void next_read(struct share *sh)
{
    while (!sh->reading && sh->queued) {   /* each turn sends one read, or fails a client */
        struct client *c = sh->queue[0];
        memmove(sh->queue, sh->queue + 1, --sh->queued * sizeof(sh->queue[0]));
        uint64_t len = c->left < CHUNK ? c->left : CHUNK;
        if (len > sh->file.buf_size)
            len = sh->file.buf_size;
        sh->txid = idl_txid_next(&sh->last_txid);
        status_t st = file_read_send(sh->file.ch, sh->txid, c->at, (uint32_t)len);
        if (st != OK) {
            client_chunk(c, 0, st);
            continue;
        }
        sh->reading = true;
        sh->reader = c;
    }
}

void share_want(struct share *sh, struct client *c)
{
    if (sh->queued < SERVE_PER_SHARE)   /* a client is queued once: its share's clients fit */
        sh->queue[sh->queued++] = c;
    c->waiting = true;
    next_read(sh);
}

void share_forget(struct share *sh, struct client *c)
{
    for (unsigned k = 0; k < sh->queued; k++) {
        if (sh->queue[k] != c)
            continue;
        memmove(sh->queue + k, sh->queue + k + 1, (sh->queued - k - 1) * sizeof(sh->queue[0]));
        sh->queued--;
        break;
    }
    if (sh->reader == c)
        sh->reader = NULL;   /* its read's answer is thrown away */
}

void share_file_ready(unsigned i)
{
    struct share *sh = &shares[i];
    _Alignas(8) uint8_t rep[FILE_REP_MAX];
    struct idl_msg m;
    if (sh->state != S_SERVING)
        return;
    for (;;) {   /* each turn takes one reply off the file channel */
        status_t st = idl_reply_read(sh->file.ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if ((st == OK || st == ERR_INTERNAL) && (!sh->reading || m.txid != sh->txid)) {
            idl_msg_drop(&m);   /* not the read that is out */
            continue;
        }
        if (st != OK && st != ERR_INTERNAL) {
            share_stop(i, "its file's disk went away");
            return;
        }
        uint32_t actual = 0;
        st = file_read_result(rep, &m, &actual);
        if (st == OK && actual > sh->file.buf_size)
            st = ERR_INTERNAL;
        struct client *c = sh->reader;
        sh->reading = false;
        sh->reader = NULL;
        if (c && st == OK)
            memcpy(c->chunk, sh->file.buf, actual < CHUNK ? actual : CHUNK);
        if (c)
            client_chunk(c, actual < CHUNK ? actual : CHUNK, st);
        next_read(sh);
    }
}
