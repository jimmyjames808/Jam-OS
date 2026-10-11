/* A POSIX program that defines names a port might well use and the call
 * layer uses inside libc.a too (`make check`, posix.mk's posix-link-check):
 * it must link and say nothing of the layer's, since a program's own
 * names are the program's (the layer's are all reserved ones, __jl_*). */
#include <unistd.h>

int fd_get(int fd)
{
    return fd;
}

int path_get(const char *p)
{
    return p != 0;
}

long lx_read(long a)
{
    return a;
}

int sig_call(int s)
{
    return s;
}

int main(void)
{
    static const char ok[] = "linked\n";
    return write(1, ok, sizeof(ok) - 1) == (ssize_t)(sizeof(ok) - 1) && fd_get(0) == 0 &&
                   path_get("/") && lx_read(1) == 1 && sig_call(2) == 2
               ? 0
               : 1;
}
