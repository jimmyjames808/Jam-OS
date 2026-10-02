/* speed: the network's throughput, against tools/speed.py on the Mac
 * (bin/speed; the shell's `speed`). Bytes flow one way for a few seconds
 * over one TCP connection (or as UDP datagrams), and both ends say how
 * many went through and how fast (MB/s: 10^6 bytes a second, and Mbit/s).
 *
 *   speed <host> [port] [-t s]       connect; this sends (TCP)
 *   speed <host> [port] -r [-t s]    connect; the Mac sends, this receives
 *   speed <host> [port] -u [-t s]    UDP datagrams from here to the Mac
 *   speed -l [port]                  listen: the Mac's `speed.py client`
 *                                    connects (the listen permission: this
 *                                    program's list has `svc net listen`)
 * The port is 5201 unless given (TCP and UDP).
 *
 * The protocol (tools/speed.py speaks the same; numbers big-endian):
 *   hello, from the side that connects, 16 bytes: "JSPD", u8 version 1,
 *     u8 mode (0: the connecting side sends, 1: the listening side
 *     sends), u16 0, u32 seconds (1..60), u32 0.
 *   then the sender's bytes for that long, and its FIN.
 *   report, from the receiver once it saw the FIN (both modes), 16 bytes:
 *     "JSPR", u32 ms from its first byte to the FIN, u64 bytes; then its
 *     FIN.
 *   UDP: datagrams of SPEED_DGRAM bytes, "JSPU", u32 run, u32 seq, u32 0,
 *     then filler; at the end "JSPE", u32 run, u32 datagrams sent, u32 0
 *     (sent a few times), answered by "JSPR", u32 run, u32 datagrams
 *     received, u32 ms from the first to the last, u64 bytes.
 *
 * Files: main.c the arguments and the lines; tcp.c both TCP directions
 * and the listener; udp.c the datagrams. Exit: 0, 1 (it failed), 2
 * (usage). */
#pragma once

#include <net.h>
#include <os.h>

#define SPEED_PORT    5201u
#define SPEED_SECONDS 5u
#define SPEED_MAX_S   60u
#define SPEED_HELLO   16u
#define SPEED_REPORT  16u
#define SPEED_DGRAM   1472u               /* one frame's worth */
#define SPEED_WAIT    (10 * NS_PER_S)     /* a connection, a hello, a report */

/* main.c: a line about n bytes in ns ("12.3 MB in 5.00 s: 2.4 MB/s, 19.6 Mbit/s"). */
void say_rate(const char *what, uint64_t n, uint64_t ns);
void put32(uint8_t *p, uint32_t v);
uint32_t get32(const uint8_t *p);
void put64(uint8_t *p, uint64_t v);
uint64_t get64(const uint8_t *p);

/* tcp.c */
int tcp_client(uint32_t addr, uint16_t port, bool receive, uint32_t seconds);
int tcp_listen(uint16_t port);
/* udp.c */
int udp_client(uint32_t addr, uint16_t port, uint32_t seconds);
