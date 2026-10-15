/* musl's posix_spawn on Jam OS: overrides upstream's src/process/posix_spawn.c
 * (user/posix/musl/OVERRIDES). Upstream's clones a vfork child that applies
 * the file actions to itself and calls execve; Jam OS has no fork of any
 * kind, so the parent does it all (docs/M13-PLAN.md section 5): this file
 * reads musl's attributes and file actions into a plain request and the
 * call layer's spawn.c starts the program with libjam's spawn and the
 * POSIX hand-over.
 *
 * posix_spawnp (musl's, unchanged) calls this with __fn set to
 * __execvpe: that means "look the name up in PATH", which spawn.c does
 * itself. posix_spawnattr_setgrants_np (<jamos/spawn.h>) keeps its list in
 * the attributes' spare bytes, where posix_spawnattr_init's zeroes mean
 * "everything we have". posix_spawn_file_actions_addclosefrom_np
 * (glibc's, which Chromium's launcher uses) is here too: it needs musl's
 * file action list. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include "../../../include/jamos/spawn.h"
#include "glue.h"
/* Upstream's own layout of a file action (not a header musl installs). */
#include "../../../../../third_party/musl/src/process/fdop.h"

_Static_assert(sizeof(((posix_spawnattr_t *)0)->__pad) >= sizeof(const char *const *),
               "the grants list fits the attributes' spare bytes");

int posix_spawnattr_setgrants_np(posix_spawnattr_t *attr,
                                 const char *const *grants)
{
	memcpy(attr->__pad, &grants, sizeof(grants));
	return 0;
}

int posix_spawnattr_getgrants_np(const posix_spawnattr_t *attr,
                                 const char *const **grants)
{
	memcpy(grants, attr->__pad, sizeof(*grants));
	return 0;
}

/* The first 64 signals of a sigset_t, bit n - 1 for signal n. */
static unsigned long long sigbits(const sigset_t *s)
{
	unsigned long long v;
	memcpy(&v, s, sizeof(v));
	return v;
}

/* The file actions, oldest first (musl keeps them newest first), into a
 * malloc'd array: *out (NULL for none) and *n. 0 or ENOMEM. */
static int read_actions(const posix_spawn_file_actions_t *fa, struct jam_spawn_op **out,
                        unsigned *n)
{
	struct fdop *op = fa ? fa->__actions : 0, *last = 0;
	unsigned count = 0;
	for (; op; op = op->next, count++)
		last = op;
	*out = 0;
	*n = 0;
	if (!count)
		return 0;
	struct jam_spawn_op *ops = malloc(count * sizeof(*ops));
	if (!ops)
		return ENOMEM;
	unsigned i = 0;
	for (op = last; op; op = op->prev, i++) {
		ops[i] = (struct jam_spawn_op){ .cmd = op->cmd, .fd = op->fd, .srcfd = op->srcfd,
		                                .oflag = op->oflag, .mode = op->mode, .path = 0 };
		if (op->cmd == FDOP_OPEN || op->cmd == FDOP_CHDIR)
			ops[i].path = op->path;
	}
	*out = ops;
	*n = count;
	return 0;
}

/* glibc's addclosefrom_np, which musl lacks: our number after musl's. */
#define FDOP_CLOSEFROM 6

_Static_assert(FDOP_CLOSE == JAM_SPAWN_CLOSE && FDOP_DUP2 == JAM_SPAWN_DUP2 &&
               FDOP_OPEN == JAM_SPAWN_OPEN && FDOP_CHDIR == JAM_SPAWN_CHDIR &&
               FDOP_FCHDIR == JAM_SPAWN_FCHDIR && FDOP_CLOSEFROM == JAM_SPAWN_CLOSEFROM,
               "the file actions keep musl's numbers");

/* Close every descriptor from `from` up in the child (glibc 2.34's
 * meaning; declared in our <spawn.h>, user/posix/include/spawn.h). As
 * musl's addclose: EBADF for a negative one, ENOMEM. */
int posix_spawn_file_actions_addclosefrom_np(posix_spawn_file_actions_t *fa, int from)
{
	if (from < 0) return EBADF;
	struct fdop *op = malloc(sizeof *op);
	if (!op) return ENOMEM;
	op->cmd = FDOP_CLOSEFROM;
	op->fd = from;
	if ((op->next = fa->__actions)) op->next->prev = op;
	op->prev = 0;
	fa->__actions = op;
	return 0;
}

int posix_spawn(pid_t *restrict res, const char *restrict path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[restrict], char *const envp[restrict])
{
	static const posix_spawnattr_t none;
	const posix_spawnattr_t *a = attr ? attr : &none;
	struct jam_spawn_req r = {
		.path = path, .argv = argv, .envp = envp, .flags = a->__flags, .pgrp = a->__pgrp,
		.sigdef = sigbits(&a->__def), .sigmask = sigbits(&a->__mask),
	};
	posix_spawnattr_getgrants_np(a, &r.grants);
	if (a->__fn == (void *)__execvpe) {
		r.search = getenv("PATH");
		if (!r.search)
			r.search = "/usr/local/bin:/bin:/usr/bin";   /* musl's execvp's */
	}
	struct jam_spawn_op *ops;
	int cs, ec = read_actions(fa, &ops, &r.nops);
	if (ec)
		return ec;
	r.ops = ops;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	ec = __jam_posix_spawn(res, &r);
	pthread_setcancelstate(cs, 0);
	free(ops);
	return ec;
}
