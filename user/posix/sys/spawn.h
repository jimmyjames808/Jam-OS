/* What spawn.c (posix_spawn, execve) and spawnfd.c (the copy of our
 * descriptor table a child's hand-over is made from) share. */
#pragma once

#include <limits.h>
#include "fd.h"
#include "proc.h"

/* One descriptor of the child's table while a spawn builds it. */
struct spawnfd_slot {
    int32_t         fd;        /* the child's descriptor */
    struct fd_desc *desc;      /* the description it gets (a reference of the copy's) */
    bool            cloexec;   /* FD_CLOEXEC: left out of the hand-over */
};

/* A child's descriptor table while a spawn builds it: the descriptions
 * its descriptors are, and its current directory. Made by spawnfd_begin,
 * freed by spawnfd_end. */
struct spawnfd {
    char                cwd[PATH_MAX];           /* the child's directory: absolute */
    struct spawnfd_slot slot[FD_MAX];            /* the child's descriptors */
    unsigned            nslots;
    /* What spawnfd_export made (one each per descriptor sent). */
    struct posixfds_desc *desc;
    struct fd_given     *given;
    unsigned            nexported;
};

/* A copy of our table (a reference to each description) and our current
 * directory into a new *out. 0 or -errno. */
long     spawnfd_begin(struct spawnfd **out);
/* posix_spawn's file actions (n of them, in order) applied to the copy.
 * 0, or the first one's failure (-errno). */
long     spawnfd_apply(struct spawnfd *t, const struct jam_spawn_op *ops, unsigned n);
/* The copy as a hand-over's descriptors: ho->fds and ho->nfds, each with
 * handles of its own (posixfds_write or posixfds_discard consumes them).
 * A descriptor with FD_CLOEXEC is left out, and so is one whose kind no
 * child gets. 0 or -errno (nothing left to discard then). */
long     spawnfd_export(struct spawnfd *t, struct posixfds_out *ho);
/* Free t (its references dropped) and what spawnfd_export made for ho
 * (not its handles: they were consumed). t may be NULL. */
void     spawnfd_end(struct spawnfd *t, struct posixfds_out *ho);

/* The absolute, cleaned path for path (a relative one from dir: "." and
 * ".." walked, no doubled '/') into out (PATH_MAX bytes). 0 or -errno. */
long     spawn_absolute(const char *path, const char *dir, char *out);
