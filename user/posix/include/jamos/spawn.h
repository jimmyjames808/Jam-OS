/* posix_spawn's Jam OS extension (docs/M13-PLAN.md section 5): the
 * namespace a child gets.
 *
 * By default a child gets everything its parent has (every mount and
 * service, as the parent has them), as a Unix child sees its parent's
 * files. posix_spawnattr_setgrants_np narrows that to a grants list:
 * a NULL-terminated list of strings, each naming part of the parent's own
 * namespace ("/data", "/data:r", "/svc/net", "*", ...; <os.h>, "grants").
 * What the parent doesn't have is left out. An empty list (just NULL)
 * gives the child no namespace at all. */
#ifndef _JAMOS_SPAWN_H
#define _JAMOS_SPAWN_H

#include <spawn.h>

/* The child's namespace: grants (NULL: everything, the default). The list
 * is not copied: it must stay as it is until posix_spawn returns. 0. */
int posix_spawnattr_setgrants_np(posix_spawnattr_t *attr,
                                 const char *const *grants);
/* The list setgrants_np set, or NULL. 0. */
int posix_spawnattr_getgrants_np(const posix_spawnattr_t *attr,
                                 const char *const **grants);

#endif
