/* tcptest: TCP end to end through netstack, against tools/netpeer.py's
 * TCP side (tools/tcppeer.py) in tools/tcp-test.sh:
 *
 *   tcptest send <address> <port> <bytes> [<ring bytes>]
 *       connect (with rings of <ring bytes> each way, a <sockring.h> ring
 *       size: NET_TCP_BULK for a scaled window; else the defaults);
 *       send <bytes> of stream 0xa1 and shut down; read the
 *       peer's <bytes> of stream 0xb2 to its end, every byte checked (and
 *       their SHA-256 said); then wait until the connection is CLOSED with
 *       no error (both FINs acked). The speeds both ways are said.
 *   tcptest serve <port> <conns> <bytes>
 *       listen on <port> (the program's list has `svc net listen`) and
 *       serve <conns> connections from one wait set (<netwait.h>) that
 *       holds the listener and every connection at once: from each, read
 *       <bytes> of stream (its peer's port & 0xff) to its end, checked;
 *       then send <bytes> of stream (peer port + 1) & 0xff and shut down;
 *       done when it is CLOSED with no error.
 *
 * Byte i of stream `seed` is (i * 131 + (i >> 8) * 7 + seed) & 0xff, as
 * tools/tcppeer.py makes them. The last line is "tcptest: PASS ..." or
 * "tcptest: FAIL ..."; exit 0 or 1. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define TT_WAIT (60 * NS_PER_S)   /* a whole run, at most */

static inline uint8_t tt_byte(uint32_t seed, uint64_t i)
{
    return (uint8_t)(i * 131 + (i >> 8) * 7 + seed);
}

/* Bytes `from`.. of stream seed into buf, and whether buf holds them. */
void tt_fill(uint32_t seed, uint64_t from, uint8_t *buf, size_t n);
bool tt_same(uint32_t seed, uint64_t from, const uint8_t *buf, size_t n);
/* MB/s (in tenths) of n bytes in ns nanoseconds. */
uint64_t tt_rate10(uint64_t n, uint64_t ns);

/* serve.c */
int tt_serve(uint16_t port, uint32_t conns, uint64_t bytes);
