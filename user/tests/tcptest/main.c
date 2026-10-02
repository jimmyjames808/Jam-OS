/* tcptest: the arguments, the stream helpers and `send` (tcptest.h says
 * what each mode does). */
#include <ipv4.h>
#include <net.h>
#include <os.h>
#include <sha256.h>
#include <wants.h>
#include "tcptest.h"

/* What it is given when the shell runs it (<wants.h>): the network, and
 * the permission to listen (`serve`). */
JAM_WANTS("svc net listen\n");

#define CHUNK (16u * 1024)

void tt_fill(uint32_t seed, uint64_t from, uint8_t *buf, size_t n)
{
    for (size_t k = 0; k < n; k++)
        buf[k] = tt_byte(seed, from + k);
}

bool tt_same(uint32_t seed, uint64_t from, const uint8_t *buf, size_t n)
{
    for (size_t k = 0; k < n; k++)
        if (buf[k] != tt_byte(seed, from + k))
            return false;
    return true;
}

uint64_t tt_rate10(uint64_t n, uint64_t ns)
{
    return ns ? n * 10 * NS_PER_S / ns / (1024 * 1024) : 0;
}

/* A decimal number: the whole of s. */
static bool number(const char *s, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (max - (uint64_t)(*s - '0')) / 10)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

static int fail(const char *what, status_t st)
{
    printf("tcptest: FAIL: %s: %s\n", what, status_str(st));
    return 1;
}

/* Our bytes out, then the shutdown. */
static status_t send_all(struct net_sock *s, uint64_t bytes, uint64_t deadline)
{
    static uint8_t buf[CHUNK];
    for (uint64_t at = 0; at < bytes;) {
        size_t n = bytes - at < CHUNK ? (size_t)(bytes - at) : CHUNK;
        tt_fill(0xa1, at, buf, n);
        status_t st = net_write(s, buf, n, deadline, NULL);
        if (st != OK)
            return st;
        at += n;
    }
    return net_shutdown(s);
}

/* The peer's bytes to its end, checked; their hash said. */
static status_t recv_all(struct net_sock *s, uint64_t bytes, uint64_t deadline)
{
    static uint8_t buf[CHUNK];
    struct sha256 h;
    uint8_t sum[SHA256_BYTES];
    uint64_t got = 0;
    sha256_init(&h);
    for (;;) {   /* each turn reads bytes, or ends */
        size_t n;
        status_t st = net_read(s, buf, sizeof(buf), deadline, &n);
        if (st != OK)
            return st;
        if (!n)
            break;
        if (got + n > bytes || !tt_same(0xb2, got, buf, n)) {
            printf("tcptest: FAIL: wrong bytes at %lu\n", (unsigned long)got);
            return ERR_INTERNAL;
        }
        sha256_add(&h, buf, n);
        got += n;
    }
    sha256_done(&h, sum);
    printf("tcptest: %lu bytes in, sha256 ", (unsigned long)got);
    for (unsigned k = 0; k < SHA256_BYTES; k++)
        printf("%02x", sum[k]);
    printf("\n");
    return got == bytes ? OK : ERR_OUT_OF_RANGE;
}

static int do_send(uint32_t addr, uint16_t port, uint64_t bytes)
{
    struct net_sock s;
    uint64_t deadline = now() + TT_WAIT, t0 = now();
    status_t st = net_wait_up(net_svc(), deadline, NULL);
    if (st == OK)
        st = net_tcp_connect(net_svc(), addr, port, deadline, &s);
    if (st != OK)
        return fail("connect", st);
    uint64_t t1 = now();
    st = send_all(&s, bytes, deadline);
    uint64_t t2 = now();
    if (st == OK)
        st = recv_all(&s, bytes, deadline);
    uint64_t t3 = now();
    if (st == OK)
        st = net_tcp_wait_closed(&s, deadline);
    net_close(&s);
    if (st != OK)
        return fail("send", st);
    printf("tcptest: PASS: connected in %lu ms; %lu bytes out at %lu.%lu MB/s, "
           "in at %lu.%lu MB/s\n",
           (unsigned long)((t1 - t0) / NS_PER_MS), (unsigned long)bytes,
           (unsigned long)(tt_rate10(bytes, t2 - t1) / 10),
           (unsigned long)(tt_rate10(bytes, t2 - t1) % 10),
           (unsigned long)(tt_rate10(bytes, t3 - t2) / 10),
           (unsigned long)(tt_rate10(bytes, t3 - t2) % 10));
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t port, n, bytes;
    uint32_t addr;
    if (argc == 5 && !strcmp(argv[1], "send") && ipv4_parse(argv[2], &addr, NULL) &&
        number(argv[3], 65535, &port) && number(argv[4], 1ull << 40, &bytes))
        return do_send(addr, (uint16_t)port, bytes);
    if (argc == 5 && !strcmp(argv[1], "serve") && number(argv[2], 65535, &port) &&
        number(argv[3], 200, &n) && number(argv[4], 1ull << 40, &bytes))
        return tt_serve((uint16_t)port, (uint32_t)n, bytes);
    printf("usage: tcptest send <address> <port> <bytes> | serve <port> <conns> <bytes>\n");
    return 2;
}
