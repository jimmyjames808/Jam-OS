/* ttyname_r without /proc: musl's reads the link /proc/self/fd/<fd>,
 * which Jam OS has no /proc for. A terminal descriptor knows its own name
 * (user/posix/sys/tty.c: TTY_NAME, /dev/tty for a console's terminal). */
#include <errno.h>
#include <unistd.h>
#include "glue.h"

int ttyname_r(int fd, char *name, size_t size)
{
	if (!isatty(fd))
		return errno;   /* EBADF, or ENOTTY */
	return __jam_ttyname(fd, name, size);
}
