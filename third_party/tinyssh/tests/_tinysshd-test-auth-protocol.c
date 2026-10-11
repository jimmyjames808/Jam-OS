/*
20260905
Public domain.
*/

#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "buf.h"
#include "crypto_uint32.h"
#include "global.h"
#include "packet.h"
#include "ssh.h"
#include "str.h"

static unsigned char payloadspace[2048];
static struct buf payload;

static void appendframe(void) {
    unsigned char *x = packet.recvbuf.buf + packet.recvbuf.len;
    crypto_uint32 packetlen = 1 + payload.len + 10;
    long long i;

    crypto_uint32_store_bigendian(x, packetlen);
    x[4] = 10;
    for (i = 0; i < payload.len; ++i) x[5 + i] = payload.buf[i];
    for (i = 0; i < 10; ++i) x[5 + payload.len + i] = 0;
    packet.recvbuf.len += packetlen + 4;
}

static void service(const char *name, int trailing) {
    buf_purge(&payload);
    buf_putnum8(&payload, SSH_MSG_SERVICE_REQUEST);
    buf_putstring(&payload, name);
    if (trailing) buf_putnum8(&payload, 0);
    appendframe();
}

static void auth(const unsigned char *name, long long namelen,
                 const char *service_name, const char *method, int trailing) {
    buf_purge(&payload);
    buf_putnum8(&payload, SSH_MSG_USERAUTH_REQUEST);
    buf_putstringlen(&payload, name, namelen);
    buf_putstring(&payload, service_name);
    buf_putstring(&payload, method);
    if (trailing) buf_putnum8(&payload, 0);
    appendframe();
}

static void disconnectpacket(void) {
    buf_purge(&payload);
    buf_putnum8(&payload, SSH_MSG_DISCONNECT);
    appendframe();
}

static void message(unsigned char ch) {
    buf_purge(&payload);
    buf_putnum8(&payload, ch);
    appendframe();
}

static int waitstatus(pid_t pid, int expected) {
    int status;

    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == expected;
}

static void childinit(struct buf *b1, unsigned char *space1, struct buf *b2,
                      unsigned char *space2) {
    int fd;

    global_init();
    buf_init(&payload, payloadspace, sizeof payloadspace);
    buf_init(b1, space1, GLOBAL_BSIZE);
    buf_init(b2, space2, GLOBAL_BSIZE);
    packet.recvbuf.len = PACKET_ZEROBYTES;
    fd = open("/dev/null", O_WRONLY);
    if (fd == -1 || dup2(fd, 1) == -1) _exit(110);
    close(fd);
}

static int rejectmethod(const char *method) {
    unsigned char space1[GLOBAL_BSIZE];
    unsigned char space2[GLOBAL_BSIZE];
    struct buf b1, b2;
    static const unsigned char user[] = "test-user";
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        int ret;
        childinit(&b1, space1, &b2, space2);
        service("ssh-userauth", 0);
        auth(user, sizeof user - 1, "ssh-connection", method, 0);
        disconnectpacket();
        ret = packet_auth(&b1, &b2, 0);
        if (ret || packet.sendpacketid != 2 || packet.receivepacketid != 3)
            _exit(100);
        _exit(0);
    }
    return waitstatus(pid, 0);
}

static int noneauthorized(void) {
    unsigned char space1[GLOBAL_BSIZE];
    unsigned char space2[GLOBAL_BSIZE];
    struct buf b1, b2;
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        struct passwd *pw;
        int ret;

        childinit(&b1, space1, &b2, space2);
        pw = getpwuid(geteuid());
        if (!pw) _exit(100);
        service("ssh-userauth", 0);
        auth((const unsigned char *) pw->pw_name, str_len(pw->pw_name),
             "ssh-connection", "none", 0);
        ret = packet_auth(&b1, &b2, 1);
        if (!ret || packet.sendpacketid != 2 || packet.receivepacketid != 2)
            _exit(101);
        if (!str_equaln(packet.name, str_len(packet.name), pw->pw_name))
            _exit(102);
        _exit(0);
    }
    return waitstatus(pid, 0);
}

enum malformed_mode {
    BAD_SERVICE,
    SERVICE_TRAILING,
    BAD_AUTH_SERVICE,
    LONG_USER,
    TRUNCATED_AUTH,
    NUL_USER,
    NONE_TRAILING
};

static int malformed(enum malformed_mode mode) {
    unsigned char space1[GLOBAL_BSIZE];
    unsigned char space2[GLOBAL_BSIZE];
    unsigned char name[LOGIN_NAME_MAX + 1] = {0};
    struct buf b1, b2;
    struct passwd *pw;
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        long long namelen;

        childinit(&b1, space1, &b2, space2);
        pw = getpwuid(geteuid());
        if (!pw) _exit(100);
        namelen = str_len(pw->pw_name);
        if (namelen > LOGIN_NAME_MAX - 2) _exit(100);
        {
            long long i;
            for (i = 0; i < namelen; ++i) name[i] = pw->pw_name[i];
        }

        service(mode == BAD_SERVICE ? "ssh-connection" : "ssh-userauth",
                mode == SERVICE_TRAILING);
        if (mode == TRUNCATED_AUTH) {
            buf_purge(&payload);
            buf_putnum8(&payload, SSH_MSG_USERAUTH_REQUEST);
            buf_putnum32(&payload, 10);
            appendframe();
        }
        else {
            if (mode == LONG_USER) namelen = sizeof name;
            if (mode == NUL_USER) {
                name[namelen++] = 0;
                name[namelen++] = 'x';
            }
            auth(name, namelen,
                 mode == BAD_AUTH_SERVICE ? "ssh-userauth" : "ssh-connection",
                 "none", mode == NONE_TRAILING);
        }
        {
            int ret = packet_auth(&b1, &b2,
                                  mode == NUL_USER || mode == NONE_TRAILING);

            if (mode == BAD_SERVICE) {
                if (ret || packet.sendpacketid != 1 ||
                    packet.receivepacketid != 1)
                    _exit(100);
            }
        }
        _exit(0);
    }
    return waitstatus(pid, mode == BAD_SERVICE ? 0 : 111);
}

static int unexpected(unsigned char ch, int afterservice, int disconnect) {
    unsigned char space1[GLOBAL_BSIZE];
    unsigned char space2[GLOBAL_BSIZE];
    struct buf b1, b2;
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        crypto_uint32 expectedpackets = afterservice + 1;
        int ret;

        childinit(&b1, space1, &b2, space2);
        if (afterservice) service("ssh-userauth", 0);
        message(ch);
        ret = packet_auth(&b1, &b2, 0);
        if (!disconnect) _exit(100);
        if (ret || packet.sendpacketid != expectedpackets ||
            packet.receivepacketid != expectedpackets)
            _exit(101);
        _exit(0);
    }
    return waitstatus(pid, disconnect ? 0 : 111);
}

static int attemptlimit(void) {
    unsigned char space1[GLOBAL_BSIZE];
    unsigned char space2[GLOBAL_BSIZE];
    struct buf b1, b2;
    static const unsigned char user[] = "test-user";
    pid_t pid = fork();

    if (pid == -1) return 0;
    if (pid == 0) {
        int i;
        int ret;

        childinit(&b1, space1, &b2, space2);
        packet.flagauthorized = 1;
        service("ssh-userauth", 0);
        for (i = 0; i < 32; ++i)
            auth(user, sizeof user - 1, "ssh-connection", "unknown", 0);
        ret = packet_auth(&b1, &b2, 0);
        if (ret || packet.sendpacketid != 33 || packet.receivepacketid != 33)
            _exit(100);
        _exit(0);
    }
    return waitstatus(pid, 0);
}

int main(void) {
    int ok = 1;

    if (!rejectmethod("none")) ok = 0;
    if (!rejectmethod("password")) ok = 0;
    if (!rejectmethod("hostbased")) ok = 0;
    if (!rejectmethod("keyboard-interactive")) ok = 0;
    if (!rejectmethod("unknown")) ok = 0;
    if (!noneauthorized()) ok = 0;
    if (!malformed(BAD_SERVICE)) ok = 0;
    if (!malformed(SERVICE_TRAILING)) ok = 0;
    if (!malformed(BAD_AUTH_SERVICE)) ok = 0;
    if (!malformed(LONG_USER)) ok = 0;
    if (!malformed(TRUNCATED_AUTH)) ok = 0;
    if (!malformed(NUL_USER)) ok = 0;
    if (!malformed(NONE_TRAILING)) ok = 0;
    if (!unexpected(SSH_MSG_CHANNEL_OPEN, 0, 1)) ok = 0;
    if (!unexpected(SSH_MSG_CHANNEL_OPEN, 1, 1)) ok = 0;
    if (!unexpected(SSH_MSG_KEXINIT, 0, 0)) ok = 0;
    if (!unexpected(SSH_MSG_KEXINIT, 1, 0)) ok = 0;
    if (!attemptlimit()) ok = 0;
    return ok ? 0 : 111;
}
