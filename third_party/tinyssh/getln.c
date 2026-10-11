/*
20140323
20241207 - reformated using clang-format
Jan Mojzis
Public domain.
*/

#include <poll.h>
#include <unistd.h>
#include "e.h"
#include "getln.h"

static int getch(int fd, char *x) {

    int r;
    struct pollfd p;

    for (;;) {
        r = read(fd, x, 1);
        if (r == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                p.fd = fd;
                p.events = POLLIN | POLLERR;
                poll(&p, 1, -1);
                continue;
            }
        }
        break;
    }
    return r;
}

/*
The function 'getln_' reads line from filedescriptor 'fd' into
buffer 'xv' of length 'xmax'.  A NUL byte is rejected unless
'nul_as_newline' is set, in which case it terminates the line.
*/
static int getln_(int fd, void *xv, long long xmax, int nul_as_newline) {

    long long xlen;
    int r;
    char ch;
    char *x = (char *) xv;

    if (xmax < 1) {
        errno = EINVAL;
        return -1;
    }
    x[0] = 0;
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    xlen = 0;
    for (;;) {
        if (xlen >= xmax - 1) {
            x[xmax - 1] = 0;
            errno = ENOMEM;
            return -1;
        }
        r = getch(fd, &ch);
        if (r != 1) break;
        if (ch == 0) {
            if (!nul_as_newline) {
                x[xlen] = 0;
                errno = EPROTO;
                return -1;
            }
            ch = '\n';
        }
        x[xlen++] = ch;
        if (ch == '\n') break;
    }
    x[xlen] = 0;
    return r;
}

int getln(int fd, void *xv, long long xmax) {
    return getln_(fd, xv, xmax, 0);
}

int getln_nul_as_newline(int fd, void *xv, long long xmax) {
    return getln_(fd, xv, xmax, 1);
}
