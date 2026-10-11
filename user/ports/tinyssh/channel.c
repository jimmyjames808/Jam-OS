/*
20140124
20241209 - reformated using clang-format
Jan Mojzis
Public domain.

The 'channel' library is used to handle data from/to SSH channel (rfc4254).

Jam OS's override of third_party/tinyssh/channel.c (listed in
user/ports/tinyssh/OVERRIDES). What changed, and why:
 - The child is one of init's sessions (jamos_session_open,
   <jamos/session.h>: the Jam OS shell on a terminal whose screen and
   keyboard are a descriptor of ours, the master), in place of openpty,
   fork, login_tty, the dropped privileges and execve of the user's shell
   (channel_forkpty.c, channel_fork.c, channel_drop.c, loginshell.c: left
   out of the build). A pty-req only records the terminal's TERM and size;
   the session opens at the shell or exec request, with them. An exec
   request is a session that runs that one line (at most 240 bytes, the
   shell's line; longer, or a 9th session: the request fails, said in the
   log).
 - Every session has a terminal, with or without a pty-req (there is no
   other way into a shell): fd0 and fd1 are both the master, fd2 none. So
   nothing here closes the master but the end of the connection: its
   close hangs the session up. A remote EOF is not passed on (a terminal's
   end of input is a typed Ctrl+D) and the output's LF arrives as CR LF.
 - A window-change is TIOCSWINSZ on the master (the session's resize).
 - No login records (logsys.c left out: no utmp) and no ptsname.
 - The client's environment requests stay refused (upstream's
   packet_channel_request.c); SSH_CONNECTION and PATH, which upstream sets
   for its child, are not passed (a session takes no environment).
The rest is upstream's.
*/

#include <jamos/session.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "blocking.h"
#include "buf.h"
#include "bug.h"
#include "byte.h"
#include "channel.h"
#include "coe.h"
#include "connectioninfo.h"
#include "e.h"
#include "iptostr.h"
#include "limit.h"
#include "log.h"
#include "newenv.h"
#include "porttostr.h"
#include "purge.h"
#include "str.h"
#include "trymlock.h"

/* A session's TERM: at most 63 bytes of [A-Za-z0-9._+-] (session.idl). */
#define TERM_MAX 64

/*
1. channel not open:    maxpacket == 0, pid == 0
2. channel open:        maxpacket != 0, pid == 0
3. child executed:      maxpacket != 0, pid > 0, fd[01] != -1
4. child eof:           maxpacket != 0, pid > 0, fd[12] == -1
5. child died:          maxpacket != 0, pid == -1
*/

struct channel channel = {0};

/* The pty-req's TERM ("" without one, or one a session can't take: the
   shell's own). */
static char termname[TERM_MAX];

/*
The 'channel_open' function opens the channel.
It sets 'localwindow' and maxpacket, values obtained from
from SSH_MSG_CHANNEL_OPEN message.
Function also obtaines connection information.
*/
int channel_open(const char *user, crypto_uint32 id, crypto_uint32 remotewindow,
                 crypto_uint32 maxpacket, crypto_uint32 *localwindow) {

    if (!localwindow) bug_inval();
    if (!maxpacket) bug_proto();
    if (channel.maxpacket != 0) return 0;
    if (channel.pid != 0) return 0;

    /* copy user-name */
    if (!str_copyn(channel.user, sizeof channel.user, user)) bug_nomem();

    /* set id, maxpacket, remotewindow, localwindow */
    channel.id = id;
    channel.maxpacket = maxpacket;
    channel.remotewindow = remotewindow;
    channel.localwindow = *localwindow = CHANNEL_BUFSIZE;

    connectioninfo(channel.localip, channel.localport, channel.remoteip,
                   channel.remoteport);

    purge(channel.buf0, sizeof channel.buf0);
    return 1;
}

/* Is name a TERM a session takes? */
static int term_ok(const char *name) {

    long long i, len = str_len(name);

    if (len >= TERM_MAX) return 0;
    for (i = 0; i < len; ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' ||
            c == '-')
            continue;
        return 0;
    }
    return 1;
}

/*
The 'channel_openterminal' function records the terminal's TERM and its
initial windowsize, for the session the shell or exec request opens.
*/
int channel_openterminal(const char *name, crypto_uint32 a, crypto_uint32 b,
                         crypto_uint32 x, crypto_uint32 y) {

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid != 0) bug_proto();
    if (channel.flagterminal == 1) bug_proto();

    if (term_ok(name))
        str_copyn(termname, sizeof termname, name);
    else {
        log_d3("channel: TERM ", name, " left out: the shell's own instead");
        termname[0] = 0;
    }
    channel.flagterminal = 1;
    channel.a = a;
    channel.b = b;
    channel.x = x;
    channel.y = y;
    return 1;
}

/* A terminal size as a session takes it: 1 to 65535 columns and rows (0
   or more: the default, 80x24, or the most). */
static unsigned short size_of(crypto_uint32 n, unsigned short dflt) {

    if (n == 0) return dflt;
    if (n > 65535) return 65535;
    return (unsigned short) n;
}

/*
The 'channel_ptyresize' function sets new
terminal windowsize.
*/
void channel_ptyresize(crypto_uint32 a, crypto_uint32 b, crypto_uint32 x,
                       crypto_uint32 y) {

    struct winsize w;

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid <= 0) bug_proto();
    if (!a && !b && !x && !y) return;

    w.ws_col = size_of(a, 80);
    w.ws_row = size_of(b, 24);
    w.ws_xpixel = x > 65535 ? 65535 : x;
    w.ws_ypixel = y > 65535 ? 65535 : y;
    ioctl(channel.master, TIOCSWINSZ, &w);
}

/*
The 'channel_env' adds new environment variable
sent from client.
*/
int channel_env(const char *x, const char *y) {

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid != 0) bug_proto();
    if (!x || !y) bug_inval();
    return newenv_lowenv(x, y);
}

/*
The 'channel_exec' function opens the session: an interactive shell, or
the shell running command 'cmd'.
*/
int channel_exec(const char *cmd) {

    struct winsize w;
    int master;
    pid_t pid;

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid != 0) bug_proto();

    byte_zero(&w, sizeof w);
    w.ws_col = channel.flagterminal ? size_of(channel.a, 80) : 80;
    w.ws_row = channel.flagterminal ? size_of(channel.b, 24) : 24;
    if (jamos_session_open(&w, channel.flagterminal ? termname : "",
                           cmd ? cmd : "", &master, &pid) == -1) {
        log_w1("channel: unable to open a session");
        return 0;
    }
    log_i3("channel: ", cmd ? "a command" : "a shell", ": session open");
    coe_enable(master);
    blocking_disable(master);

    channel.pid = pid;
    channel.master = master;
    channel.fd0 = master;
    channel.fd1 = master;
    channel.fd2 = -1;
    channel.len0 = 0;
    /* EOF may arrive after channel open but before the session starts. */
    if (channel.remoteeof) channel_puteof();
    newenv_purge();
    return 1;
}

/*
The 'channel_put' function adds data from
client to childs buffer.
*/
void channel_put(unsigned char *buf, long long len) {

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid == 0) bug_proto();

    if (!buf || len < 0) bug_inval();
    if ((crypto_uint32) len > channel.localwindow) bug_proto();

    /* the child is gone or its standard input is already closed: discard
       the data instead of tearing down the whole connection, the channel
       still has to deliver the child's output and the exit-status */
    if (channel.pid < 0 || channel.fd0 == -1) return;

    if (channel.len0 + len > CHANNEL_BUFSIZE) bug_nomem();

    byte_copy(channel.buf0 + channel.len0, len, buf);
    channel.len0 += len;
    channel.localwindow -= len;
}

/* Account for extended data which this server intentionally discards. */
void channel_putextended(long long len) {

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid == 0) bug_proto();
    if (len < 0 || (crypto_uint32) len > channel.localwindow) bug_proto();
    channel.localwindow -= len;
}

/*
The 'channel_puteof' function adds information
that remote side closed standard output. The session's terminal has no
end of input to give: noted only.
*/
void channel_puteof(void) {

    if (channel.maxpacket == 0) bug_proto();
    channel.remoteeof = 1;
}

/*
The 'channel_close' function applies peer close as input EOF for an active
child. Before child startup it forgets the pty request.
*/
void channel_close(void) {

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid > 0) {
        channel_puteof();
        return;
    }
    if (channel.pid < 0) return;

    channel.flagterminal = 0;
    channel.master = -1;
    channel.fd0 = -1;
    channel.fd1 = -1;
    channel.fd2 = -1;
    channel.pid = -1;
    newenv_purge();
}

/*
The 'channel_putisready' function returns
if child is ready accept data.
*/
int channel_putisready(void) {

    if (channel.maxpacket == 0) return 0;
    if (channel.pid <= 0) return 0;
    if (channel.fd0 == -1) return 0;

    return (CHANNEL_BUFSIZE > channel.len0);
}

/*
The 'channel_read' function reads data the session's terminal shows. Its
end (EIO: the shell has ended and everything left was read) leaves the
master open: fd0 is the same descriptor.
*/
long long channel_read(unsigned char *buf, long long len) {

    long long r;

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid <= 0) bug_proto();
    if (channel.fd1 == -1) bug_proto();

    if (!buf || len < 0) bug_inval();

    if (channel.remotewindow <= 0) return 0;

    r = len;
    if (r > 1048576) r = 1048576;
    if (r > channel.remotewindow) r = channel.remotewindow;
    r = read(channel.fd1, buf, r);
    if (r == -1) {
        if (errno == EINTR) return 0;
        if (errno == EAGAIN) return 0;
        if (errno == EWOULDBLOCK) return 0;
    }
    if (r <= 0) {
        channel.fd1 = -1;
        return 0;
    }
    channel.remotewindow -= r;
    return r;
}

/*
The 'channel_extendedread' function: a session has no error output of its
own (the terminal shows both), so it is never called (fd2 is -1).
*/
long long channel_extendedread(unsigned char *buf, long long len) {

    (void) buf;
    (void) len;
    bug_proto();
    return 0;
}

/*
The 'channel_readisready' function returns
if we can read data from childs standard output.
*/
int channel_readisready(void) {

    if (channel.maxpacket == 0 || channel.pid == 0) return 0;
    if (channel.fd1 == -1) return 0;
    return (channel.remotewindow > 0);
}

/*
The 'channel_extendedreadisready' function returns
if we can read data from childs error output: never (above).
*/
int channel_extendedreadisready(void) { return 0; }

/*
The 'channel_write' function writes
data to childs standard input.
*/
int channel_write(void) {

    long long w;

    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid <= 0) bug_proto();
    if (channel.fd0 == -1) bug_proto();

    if (channel.len0 <= 0) return 1;

    w = write(channel.fd0, channel.buf0, channel.len0);
    if (w == -1) {
        if (errno == EINTR) return 1;
        if (errno == EAGAIN) return 1;
        if (errno == EWOULDBLOCK) return 1;
    }
    if (w <= 0) {
        /* write error (EIO once the session has ended): treat as a
           half-close, not a fatal connection error. Drop the pending
           input and let the loop drain the session's output, send
           exit-status and close the channel cleanly; the master stays
           open (it is fd1 too) */
        channel.fd0 = -1;
        purge(channel.buf0, channel.len0);
        channel.len0 = 0;
        return 1;
    }
    byte_copy(channel.buf0, channel.len0 - w, channel.buf0 + w);
    purge(channel.buf0 + channel.len0 - w, w);
    channel.len0 -= w;
    return 1;
}

/*
The 'channel_writeisready' function returns
if we can write data to childs standard input.
*/
int channel_writeisready(void) {

    if (channel.maxpacket == 0) return 0;
    if (channel.pid <= 0) return 0;
    if (channel.fd0 == -1) return 0;

    return (channel.len0 > 0);
}

/*
The 'channel_iseof' function returns
if child closed standard and error output.
*/
int channel_iseof(void) {

    if (channel.maxpacket == 0) return 0;
    if (channel.pid == 0) return 0;
    if (channel.pid == -1) return 1;
    return (channel.fd1 == -1 && channel.fd2 == -1);
}

/*
The 'channel_waitnohang' function returns
if child exited or was killed: the session's shell's code (2 when its
terminal hung up).
*/
int channel_waitnohang(int *s, int *e) {

    int r, status;

    if (!s || !e) bug_inval();
    if (channel.maxpacket == 0) bug_proto();
    if (channel.pid <= 0) return 0;

    do {
        r = waitpid(channel.pid, &status, WNOHANG);
    } while (r == -1 && errno == EINTR);
    if (r <= 0) return 0;

    if (WIFEXITED(status)) {
        *e = WEXITSTATUS(status);
        *s = 0;
    }
    else if (WIFSIGNALED(status)) {
        *e = 0;
        *s = WTERMSIG(status);
    }
    else { *e = *s = -1; }

    channel.pid = -1;
    return 1;
}

/*
Remove sensitive data from allocated memory.
*/
void channel_purge(void) {
    purge(&channel, sizeof channel);
    trymunlock(&channel, sizeof channel);
}

/*
Initialize channel structure.
*/
void channel_init(void) {
    trymlock(&channel, sizeof channel);
    purge(&channel, sizeof channel);
    channel.maxpacket = 0;
    channel.remoteeof = 0;
    channel.len0 = 0;
    channel.pid = 0;
    channel.flagterminal = 0;
    channel.fd0 = -1;
    channel.fd1 = -1;
    channel.fd2 = -1;
    channel.master = -1;
    channel.slave = -1;
}

int channel_getfd0(void) { return channel.fd0; }
int channel_getfd1(void) { return channel.fd1; }
int channel_getfd2(void) { return channel.fd2; }
long long channel_getlen0(void) { return channel.len0; }
crypto_uint32 channel_getid(void) { return channel.id; }
crypto_uint32 channel_getlocalwindow(void) { return channel.localwindow; }
void channel_incrementremotewindow(crypto_uint32 x) {
    if (x > 0xffffffffUL - channel.remotewindow) bug_proto();
    channel.remotewindow += x;
}
void channel_incrementlocalwindow(crypto_uint32 x) {
    if (x > 0xffffffffUL - channel.localwindow) bug();
    channel.localwindow += x;
}
