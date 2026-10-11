/*
20260905
Jan Mojzis
Public domain.
*/

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "buf.h"
#include "global.h"
#include "packet.h"
#include "ssh.h"

static unsigned char bspace[1024];
static struct buf b;

static crypto_uint32 load32(const unsigned char *x) {
    crypto_uint32 u = x[0];

    u = (u << 8) | x[1];
    u = (u << 8) | x[2];
    return (u << 8) | x[3];
}

static int request(crypto_uint8 wantreply, int flagextra) {
    crypto_uint32 packetlen;
    crypto_uint8 paddinglen;

    buf_purge(&b);
    buf_putnum8(&b, SSH_MSG_GLOBAL_REQUEST);
    buf_putstring(&b, "keepalive@openssh.com");
    buf_putnum8(&b, wantreply);
    if (flagextra) buf_putnum32(&b, 123456789);
    if (!packet_global_request(&b)) return 0;
    if (b.len != 0) return 0;

    if (!wantreply) return packet.sendbuf.len == 0;
    if (packet.sendbuf.len < 6) return 0;
    packetlen = load32(packet.sendbuf.buf);
    if (packetlen + 4 != packet.sendbuf.len) return 0;
    paddinglen = packet.sendbuf.buf[4];
    if (paddinglen < 4) return 0;
    if (packetlen != (crypto_uint32) paddinglen + 2) return 0;
    if (packet.sendbuf.buf[5] != SSH_MSG_REQUEST_FAILURE) return 0;
    buf_purge(&packet.sendbuf);
    return 1;
}

static int malformed(void) {
    int status;
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        buf_purge(&b);
        buf_putnum8(&b, SSH_MSG_GLOBAL_REQUEST);
        buf_putnum32(&b, 32);
        packet_global_request(&b);
        _exit(0);
    }
    if (waitpid(pid, &status, 0) != pid) return 0;
    if (!WIFEXITED(status)) return 0;
    return WEXITSTATUS(status) == 111;
}

int main(void) {
    int ok = 1;

    global_init();
    buf_init(&b, bspace, sizeof bspace);
    if (!request(0, 0)) ok = 0;
    if (!request(1, 0)) ok = 0;
    if (!request(1, 1)) ok = 0;
    if (!malformed()) ok = 0;
    global_purge();
    return ok ? 0 : 111;
}
