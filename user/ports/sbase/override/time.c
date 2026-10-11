/* See LICENSE file for copyright and license details. */
/* Jam OS override of sbase's time.c (user/ports/sbase/OVERRIDES): Jam OS has no fork, so the
 * fork-then-execvp that starts the command is posix_spawnp, whose failure
 * is reported the way the forked child's was.
 * Everything else is upstream's file, byte for byte. The change is marked
 * "Jam OS:" below. */
#include <sys/times.h>
#include <sys/wait.h>

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <unistd.h>

extern char **environ;

#include "util.h"

static void
usage(void)
{
	eprintf("usage: %s [-p] cmd [arg ...]\n", argv0);
}

int
main(int argc, char *argv[])
{
	pid_t pid;
	struct tms tms; /* user and sys times */
	clock_t r0, r1; /* real time */
	long ticks;     /* per second */
	int status, savederrno, ret = 0;

	ARGBEGIN {
	case 'p':
		break;
	default:
		usage();
	} ARGEND

	if (!argc)
		usage();

	if ((ticks = sysconf(_SC_CLK_TCK)) <= 0)
		eprintf("sysconf _SC_CLK_TCK:");

	if ((r0 = times(&tms)) == (clock_t)-1)
		eprintf("times:");

	/* Jam OS: posix_spawnp for fork + execvp. A command that can't be started
	 * ends with the status the forked child's _exit gave: 127 for a missing
	 * one, 126 otherwise. */
	if ((savederrno = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ))) {
		errno = savederrno;
		weprintf("execvp %s:", argv[0]);
		status = (126 + (savederrno == ENOENT)) << 8;
	} else {
		waitpid(pid, &status, 0);
	}

	if ((r1 = times(&tms)) == (clock_t)-1)
		eprintf("times:");

	if (WIFSIGNALED(status)) {
		fprintf(stderr, "Command terminated by signal %d\n",
		        WTERMSIG(status));
		ret = 128 + WTERMSIG(status);
	}

	fprintf(stderr, "real %f\nuser %f\nsys %f\n",
	        (r1 - r0)      / (double)ticks,
	        tms.tms_cutime / (double)ticks,
	        tms.tms_cstime / (double)ticks);

	if (WIFEXITED(status))
		ret = WEXITSTATUS(status);

	return ret;
}
