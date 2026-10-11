/*
20260905
Public domain.
*/

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "buf.h"
#include "crypto_uint32.h"
#include "global.h"
#include "packet.h"
#include "packetparser.h"
#include "ssh.h"

static unsigned char outspace[TRANSPORT_PACKET_LIMIT];
static struct buf out;

static int mustfail(void (*fn)(void)) {
    int status;
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        global_init();
        buf_init(&out, outspace, sizeof outspace);
        fn();
        _exit(0);
    }
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 111;
}

static long long putplain(long long pos, const unsigned char *payload,
                          long long payloadlen, unsigned char paddinglen) {
    unsigned char *x = packet.recvbuf.buf + PACKET_ZEROBYTES + pos;
    crypto_uint32 packetlen = 1 + payloadlen + paddinglen;

    crypto_uint32_store_bigendian(x, packetlen);
    x[4] = paddinglen;
    if (payloadlen) {
        long long i;
        for (i = 0; i < payloadlen; ++i) x[5 + i] = payload[i];
    }
    for (pos = 0; pos < paddinglen; ++pos) x[5 + payloadlen + pos] = 0;
    return packetlen + 4;
}

static void prepare(const unsigned char *payload, long long payloadlen,
                    unsigned char paddinglen) {
    packet_init();
    packet.recvbuf.len = PACKET_ZEROBYTES;
    packet.recvbuf.len += putplain(0, payload, payloadlen, paddinglen);
    buf_purge(&out);
}

static int parservalid(void) {
    static const unsigned char x[] = {0x7f, 0x00, 0x00, 0x01, 0x00, 0xaa};
    unsigned char copy[1] = {0};
    crypto_uint8 ch;
    crypto_uint32 u;
    long long pos = 0;

    pos = packetparser_uint8(x, sizeof x, pos, &ch);
    if (ch != 0x7f || pos != 1) return 0;
    pos = packetparser_uint32(x, sizeof x, pos, &u);
    if (u != 256 || pos != 5) return 0;
    pos = packetparser_copy(x, sizeof x, pos, copy, sizeof copy);
    if (copy[0] != 0xaa) return 0;
    pos = packetparser_skip(x, sizeof x, pos, 0);
    return packetparser_end(x, sizeof x, pos) == sizeof x;
}

static void parseruint8short(void) {
    unsigned char x[1] = {0};
    crypto_uint8 ch;
    packetparser_uint8(x, 0, 0, &ch);
}

static void parseruint32short(void) {
    unsigned char x[3] = {0};
    crypto_uint32 u;
    packetparser_uint32(x, sizeof x, 0, &u);
}

static void parserskippastend(void) {
    unsigned char x[4] = {0};
    packetparser_skip(x, sizeof x, 1, 4);
}

static void parserendmismatch(void) {
    unsigned char x[2] = {0};
    packetparser_end(x, sizeof x, 1);
}

static int framevalid(void) {
    static const unsigned char payload[] = {SSH_MSG_UNIMPLEMENTED, 0, 0, 0, 7};

    prepare(payload, sizeof payload, 10);
    if (!packet_get(&out, 0)) return 0;
    if (out.len != sizeof payload || packet.receivepacketid != 1) return 0;
    if (packet.recvbuf.len != PACKET_ZEROBYTES) return 0;
    return out.buf[0] == SSH_MSG_UNIMPLEMENTED && out.buf[4] == 7;
}

static int framefragmented(void) {
    static const unsigned char payload[] = {SSH_MSG_UNIMPLEMENTED};
    long long framelen;
    long long i;

    packet_init();
    framelen = putplain(0, payload, sizeof payload, 10);
    for (i = 0; i < framelen; ++i) {
        packet.recvbuf.len = PACKET_ZEROBYTES + i;
        buf_purge(&out);
        if (!packet_get(&out, 0)) return 0;
        if (out.len != 0 || packet.receivepacketid != 0) return 0;
    }
    packet.recvbuf.len = PACKET_ZEROBYTES + framelen;
    if (!packet_get(&out, 0)) return 0;
    return out.len == 1 && out.buf[0] == payload[0] &&
           packet.receivepacketid == 1;
}

static int framemultiple(void) {
    static const unsigned char first[] = {SSH_MSG_UNIMPLEMENTED, 1};
    static const unsigned char second[] = {SSH_MSG_UNIMPLEMENTED, 2};
    long long pos;

    packet_init();
    packet.recvbuf.len = PACKET_ZEROBYTES;
    pos = putplain(0, first, sizeof first, 9);
    packet.recvbuf.len += pos;
    packet.recvbuf.len += putplain(pos, second, sizeof second, 9);
    buf_purge(&out);
    if (!packet_get(&out, 0) || out.len != 2 || out.buf[1] != 1) return 0;
    if (!packet_get(&out, 0) || out.len != 2 || out.buf[1] != 2) return 0;
    return packet.receivepacketid == 2 &&
           packet.recvbuf.len == PACKET_ZEROBYTES;
}

static int frameboundary(void) {
    static unsigned char payload[CHANNEL_PACKET_LIMIT];

    payload[0] = SSH_MSG_UNIMPLEMENTED;
    prepare(payload, sizeof payload, 4);
    if (!packet_get(&out, 0)) return 0;
    return out.len == sizeof payload && packet.receivepacketid == 1;
}

static int encryptedchannelboundary(void) {
    static unsigned char payload[CHANNEL_PACKET_LIMIT + 9];
    static const unsigned char cipher[] = "chacha20-poly1305@openssh.com";

    packet_init();
    sshcrypto_ciphers[0].flagenabled = 1;
    if (!sshcrypto_cipher_select(cipher, sizeof cipher - 1)) return 0;
    packet.flagkeys = 1;
    payload[0] = SSH_MSG_CHANNEL_DATA;
    crypto_uint32_store_bigendian(payload + 5, CHANNEL_PACKET_LIMIT);

    buf_purge(&out);
    buf_put(&out, payload, sizeof payload);
    packet_put(&out);
    if (packet.sendbuf.len <= CHANNEL_PACKET_LIMIT) return 0;

    packet.recvbuf.len = PACKET_ZEROBYTES;
    buf_put(&packet.recvbuf, packet.sendbuf.buf, packet.sendbuf.len);
    buf_purge(&out);
    if (!packet_get(&out, 0)) return 0;
    return out.len == sizeof payload && out.buf[0] == SSH_MSG_CHANNEL_DATA &&
           packet.receivepacketid == 1;
}

static int putvalid(void) {
    static const unsigned char payload[] = {SSH_MSG_UNIMPLEMENTED};
    crypto_uint32 packetlen;
    unsigned char paddinglen;

    packet_init();
    buf_purge(&out);
    buf_put(&out, payload, sizeof payload);
    packet_put(&out);
    if (packet.sendpacketid != 1 || packet.sendbuf.len < 6) return 0;
    packetlen = crypto_uint32_load_bigendian(packet.sendbuf.buf);
    paddinglen = packet.sendbuf.buf[4];
    if (packetlen + 4 != (crypto_uint32) packet.sendbuf.len) return 0;
    if (paddinglen < 4 || packet.sendbuf.buf[5] != payload[0]) return 0;
    return packetlen == (crypto_uint32) paddinglen + 2;
}

static void paddingzero(void) {
    static const unsigned char payload[] = {SSH_MSG_UNIMPLEMENTED};
    prepare(payload, sizeof payload, 0);
    packet_get(&out, 0);
}

static void paddingthree(void) {
    static const unsigned char payload[] = {SSH_MSG_UNIMPLEMENTED};
    prepare(payload, sizeof payload, 3);
    packet_get(&out, 0);
}

static void paddingconsumespacket(void) {
    unsigned char *x;

    packet_init();
    packet.recvbuf.len = PACKET_ZEROBYTES + 9;
    x = packet.recvbuf.buf + PACKET_ZEROBYTES;
    crypto_uint32_store_bigendian(x, 5);
    x[4] = 4;
    packet_get(&out, 0);
}

static void packettoolarge(void) {
    unsigned char *x;

    packet_init();
    packet.recvbuf.len = PACKET_ZEROBYTES + 4;
    x = packet.recvbuf.buf + PACKET_ZEROBYTES;
    crypto_uint32_store_bigendian(x, TRANSPORT_PACKET_LIMIT - 4 + 1);
    packet_get(&out, 0);
}

int main(void) {
    int ok = 1;

    global_init();
    buf_init(&out, outspace, sizeof outspace);

    if (!parservalid()) ok = 0;
    if (!mustfail(parseruint8short)) ok = 0;
    if (!mustfail(parseruint32short)) ok = 0;
    if (!mustfail(parserskippastend)) ok = 0;
    if (!mustfail(parserendmismatch)) ok = 0;
    if (!framevalid()) ok = 0;
    if (!framefragmented()) ok = 0;
    if (!framemultiple()) ok = 0;
    if (!frameboundary()) ok = 0;
    if (!encryptedchannelboundary()) ok = 0;
    if (!putvalid()) ok = 0;
    if (!mustfail(paddingzero)) ok = 0;
    if (!mustfail(paddingthree)) ok = 0;
    if (!mustfail(paddingconsumespacket)) ok = 0;
    if (!mustfail(packettoolarge)) ok = 0;

    global_purge();
    return ok ? 0 : 111;
}
