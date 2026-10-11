/*
20260905
*/

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "buf.h"
#include "global.h"
#include "packet.h"
#include "ssh.h"
#include "str.h"
#include "sig.h"

static unsigned char bspace[1024];
static struct buf b;

static int mode(const char *x, const char *y) {
    return str_equaln(x, str_len(x), y);
}

static void packet_ignore(void) {
    buf_purge(&b);
    buf_putnum8(&b, SSH_MSG_IGNORE);
    buf_putstring(&b, "ignored");
    packet_put(&b);
}

static void packet_debug(void) {
    buf_purge(&b);
    buf_putnum8(&b, SSH_MSG_DEBUG);
    buf_putnum8(&b, 0);
    buf_putstring(&b, "debug");
    buf_putstring(&b, "");
    packet_put(&b);
}

static void packet_kexinit(int flagstrict) {
    buf_purge(&b);
    buf_putnum8(&b, SSH_MSG_KEXINIT);
    buf_putrandombytes(&b, 16);
    if (flagstrict)
        buf_putstring(&b, "curve25519-sha256,kex-strict-c-v00@openssh.com");
    else
        buf_putstring(&b, "curve25519-sha256");
    buf_putstring(&b, "ssh-ed25519");
    buf_putstring(&b, "chacha20-poly1305@openssh.com");
    buf_putstring(&b, "chacha20-poly1305@openssh.com");
    buf_putstring(&b, "hmac-sha2-256");
    buf_putstring(&b, "hmac-sha2-256");
    buf_putstring(&b, "none");
    buf_putstring(&b, "none");
    buf_putstring(&b, "");
    buf_putstring(&b, "");
    buf_putnum8(&b, 0);
    buf_putnum32(&b, 0);
    packet_put(&b);
}

static void packet_kexdh_init(void) {
    unsigned char clientpk[32] = {9};

    buf_purge(&b);
    buf_putnum8(&b, SSH_MSG_KEXDH_INIT);
    buf_putstringlen(&b, clientpk, sizeof clientpk);
    packet_put(&b);
}

static int packet_disconnect(void) {
    buf_purge(&b);
    buf_putnum8(&b, SSH_MSG_DISCONNECT);
    buf_putnum32(&b, SSH_DISCONNECT_BY_APPLICATION);
    buf_putstring(&b, "test finished");
    buf_putstring(&b, "");
    packet_put(&b);
    return packet_sendall();
}

int main(int argc, char **argv) {
    int flagstrict, flagbefore, flagdebug, flagmultiple, accepted, status;
    int tochild[2] = {-1, -1};
    int fromchild[2] = {-1, -1};
    pid_t pid;

    if (argc < 3) _exit(100);
    flagstrict = mode(argv[1], "strict-before-ignore") ||
                 mode(argv[1], "strict-before-debug") ||
                 mode(argv[1], "strict-during-ignore") ||
                 mode(argv[1], "strict-during-debug");
    flagbefore = mode(argv[1], "nonstrict-before-ignore") ||
                 mode(argv[1], "nonstrict-before-debug") ||
                 mode(argv[1], "nonstrict-before-multiple") ||
                 mode(argv[1], "strict-before-ignore") ||
                 mode(argv[1], "strict-before-debug");
    flagdebug = mode(argv[1], "nonstrict-before-debug") ||
                mode(argv[1], "nonstrict-during-debug") ||
                mode(argv[1], "strict-before-debug") ||
                mode(argv[1], "strict-during-debug");
    flagmultiple = mode(argv[1], "nonstrict-before-multiple");
    if (!flagstrict && !flagbefore && !flagdebug && !flagmultiple &&
        !mode(argv[1], "nonstrict-during-ignore") &&
        !mode(argv[1], "nonstrict-during-debug"))
        _exit(100);
    argv += 2;

    if (pipe(tochild) == -1) _exit(111);
    if (pipe(fromchild) == -1) _exit(111);
    pid = fork();
    if (pid == -1) _exit(111);
    if (pid == 0) {
        close(tochild[1]);
        close(fromchild[0]);
        close(2);
        if (dup2(tochild[0], 0) == -1) _exit(111);
        if (dup2(fromchild[1], 1) == -1) _exit(111);
        close(tochild[0]);
        close(fromchild[1]);
        execvp(*argv, argv);
        _exit(111);
    }
    close(tochild[0]);
    close(fromchild[1]);
    if (dup2(fromchild[0], 0) == -1) _exit(111);
    if (dup2(tochild[1], 1) == -1) _exit(111);
    close(fromchild[0]);
    close(tochild[1]);
    sig_ignore(SIGPIPE);

    global_init();
    buf_init(&b, bspace, sizeof bspace);
    if (!packet_hello_receive()) _exit(111);
    if (!packet_hello_send()) _exit(111);
    if (!packet_getall(&b, SSH_MSG_KEXINIT)) _exit(111);

    if (flagbefore) {
        if (flagdebug)
            packet_debug();
        else
            packet_ignore();
        if (flagmultiple) {
            packet_debug();
            packet_ignore();
        }
    }
    packet_kexinit(flagstrict);
    if (!flagbefore) {
        if (flagdebug)
            packet_debug();
        else
            packet_ignore();
    }
    packet_kexdh_init();
    if (!packet_sendall())
        accepted = 0;
    else
        accepted = packet_getall(&b, SSH_MSG_KEXDH_REPLY);

    if (accepted) packet_disconnect();
    close(1);
    close(0);
    if (waitpid(pid, &status, 0) != pid) _exit(111);
    if (flagstrict == accepted) _exit(111);
    _exit(0);
}
