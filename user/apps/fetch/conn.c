/* fetch: the connection. Every wait (the resolver's answer, the
 * handshake, room to write, bytes to read) is one netwait_wait on a set
 * that also holds the shell's stop channel, so Ctrl+C ends any of them at
 * once, and each has a deadline, so a server that stops answering can't
 * hold fetch forever. */
#include <dns.h>
#include <idl/dns.h>
#include <ipv4.h>
#include "fetch.h"

#define ROLE_STOP (SR_USER + 2)
#define TAG_STOP  ((void *)1)
#define TAG_SOCK  ((void *)2)
#define TAG_DNS   ((void *)3)
#define DNS_MS    10000u   /* the resolver's own limit for our name */

bool conn_stopped(void)
{
    signals_t seen = 0;
    handle_t stop = startup_handle(ROLE_STOP);
    return stop && jam_object_wait_one(stop, SIG_READABLE | SIG_PEER_CLOSED, 0, &seen) == OK &&
           (seen & (SIG_READABLE | SIG_PEER_CLOSED));
}

status_t conn_init(struct conn *c)
{
    memset(c, 0, sizeof(*c));
    status_t st = netwait_create(4, &c->w);
    handle_t stop = startup_handle(ROLE_STOP);
    if (st == OK && stop) {
        struct netwait_handle h = { stop, SIG_READABLE, 0, SIG_PEER_CLOSED };
        st = netwait_add_handle(c->w, &h, NETWAIT_READ, TAG_STOP, &c->stop_id);
    }
    return st;
}

/* Wait for the entry tagged `tag` (or the stop channel) until the
 * deadline: OK with its readiness in *ready; ERR_CANCELED: stopped;
 * ERR_TIMED_OUT; the entry's error when it has one and nothing to read
 * (a connection refused or reset, netstack gone). */
static status_t wait_for(struct conn *c, void *tag, uint64_t deadline, uint32_t *ready)
{
    for (;;) {   /* each turn waits, to the deadline */
        struct netwait_ready r[4];
        uint32_t n = 0;
        status_t st = netwait_wait(c->w, deadline, r, 4, &n);
        if (st != OK)
            return st;
        *ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (r[i].user == TAG_STOP)
                return ERR_CANCELED;
            if (r[i].user != tag)
                continue;
            *ready |= r[i].ready;
            if ((r[i].ready & NETWAIT_ERROR) && !(r[i].ready & NETWAIT_READ))
                return r[i].error != OK ? r[i].error : ERR_PEER_CLOSED;
        }
        if (*ready)
            return OK;
    }
}

/* The resolver's answer to txid on ch, by the deadline. */
static status_t dns_answer(struct conn *c, handle_t ch, uint32_t txid, uint64_t deadline,
                           uint32_t *addr)
{
    for (;;) {   /* each turn reads a reply, or waits for one */
        _Alignas(8) uint8_t rep[DNS_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if ((st == OK || st == ERR_INTERNAL) && m.txid == txid) {
            uint8_t n = 0;
            uint32_t a[DNS_ADDRS_MAX] = { 0 }, ttl = 0;
            if (st == OK)
                st = dns_resolve_result(rep, &m, &n, &a[0], &a[1], &a[2], &a[3], &ttl);
            if (st == OK && (!n || n > DNS_ADDRS_MAX))
                st = ERR_INTERNAL;
            if (st == OK)
                *addr = a[0];
            return st;
        }
        if (st == OK || st == ERR_INTERNAL) {
            idl_msg_drop(&m);   /* not ours */
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return st;
        uint32_t ready;
        if ((st = wait_for(c, TAG_DNS, deadline, &ready)) != OK)
            return st;
    }
}

status_t conn_resolve(struct conn *c, const char *host, uint32_t *addr)
{
    const char *end = NULL;
    if (ipv4_parse(host, addr, &end) && !*end)
        return OK;   /* an address needs no resolver */
    uint8_t field[DNS_TEXT_MAX] = { 0 };
    size_t len = strnlen(host, DNS_TEXT_MAX);
    if (len == DNS_TEXT_MAX)
        return ERR_INVALID_ARGS;
    memcpy(field, host, len);
    handle_t ch;
    uint32_t id = 0, last = 0, txid = idl_txid_next(&last);
    status_t st = svc_open(SVC_DNS, &ch);
    if (st != OK)
        return st == ERR_NOT_FOUND ? ERR_PEER_CLOSED : st;
    struct netwait_handle h = { ch, SIG_READABLE, 0, SIG_PEER_CLOSED };
    st = netwait_add_handle(c->w, &h, NETWAIT_READ, TAG_DNS, &id);
    if (st == OK)
        st = dns_resolve_send(ch, txid, field, DNS_MS);
    if (st == OK)
        st = dns_answer(c, ch, txid, now() + CONNECT_WAIT, addr);
    if (id)
        (void)netwait_remove(c->w, id);   /* ours: can't fail */
    jam_handle_close(ch);
    return st;
}

status_t conn_open(struct conn *c, uint32_t addr, uint16_t port)
{
    uint64_t deadline = now() + CONNECT_WAIT;
    status_t st = net_wait_up(net_svc(), deadline, NULL);
    if (st == OK)
        st = net_tcp_open(net_svc(), addr, port, 0, 0, &c->s);
    if (st != OK)
        return st;
    c->open = true;
    c->peer = addr;
    struct netwait_sock ws;
    net_sock_waitable(&c->s, &ws);
    st = netwait_add_sock(c->w, &ws, NETWAIT_WRITE, TAG_SOCK, &c->sock_id);
    while (st == OK) {   /* each turn looks, or waits to the deadline */
        uint32_t state, ready;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        if (state == SOCKRING_STATE_OPEN)
            return OK;
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK ? err : ERR_PEER_CLOSED;
        st = wait_for(c, TAG_SOCK, deadline, &ready);
    }
    return st;
}

status_t conn_write(struct conn *c, const void *data, size_t n, uint64_t deadline)
{
    status_t st = netwait_modify(c->w, c->sock_id, NETWAIT_WRITE);
    for (size_t done = 0; st == OK && done < n;) {   /* each turn writes, or waits */
        size_t k = net_write_some(&c->s, (const uint8_t *)data + done, n - done);
        done += k;
        if (k || done == n)
            continue;
        uint32_t state, ready;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK ? err : ERR_PEER_CLOSED;
        st = wait_for(c, TAG_SOCK, deadline, &ready);
    }
    return st;
}

status_t conn_read(struct conn *c, void *buf, size_t cap, uint64_t deadline, size_t *got)
{
    status_t st = netwait_modify(c->w, c->sock_id, NETWAIT_READ);
    while (st == OK) {   /* each turn reads, ends, or waits */
        *got = net_read_some(&c->s, buf, cap);
        if (*got || sockring_at_end(&c->s.r.rx))
            return OK;
        uint32_t state, ready = 0;
        status_t err;
        net_tcp_status(&c->s, &state, &err);
        if (state == SOCKRING_STATE_CLOSED)
            return err != OK ? err : ERR_PEER_CLOSED;
        st = wait_for(c, TAG_SOCK, deadline, &ready);
    }
    return st;
}

void conn_close(struct conn *c)
{
    if (c->sock_id)
        (void)netwait_remove(c->w, c->sock_id);   /* ours: can't fail */
    c->sock_id = 0;
    if (c->open)
        net_close(&c->s);
    c->open = false;
}
