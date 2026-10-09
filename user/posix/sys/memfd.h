/* Memory files (memfd.c) as dev.c's /dev/shm names use them: an object
 * that several descriptions share, counted, with the file's size and
 * seals under its own lock. */
#pragma once

#include "kinds.h"

struct memobj;

/* A new object of size 0 called name (cut to the names' length), one
 * reference held; sealing: F_ADD_SEALS allowed (MFD_ALLOW_SEALING). NULL
 * and *err (-errno) if out of memory or the job can't pay. */
struct memobj *memobj_new(const char *name, bool sealing, long *err);
/* One more and one less user of m (the last frees it, and its VMO). */
void     memobj_ref(struct memobj *m);
void     memobj_unref(struct memobj *m);
/* ftruncate of m to size: 0 or -errno. */
long     memobj_truncate(struct memobj *m, uint64_t size);
/* The file's size. */
uint64_t memobj_size(struct memobj *m);
/* A description's private state on m (its own reference to m): the priv
 * of a memfd_ops description. NULL if out of memory. */
void    *memfd_priv_new(struct memobj *m);
