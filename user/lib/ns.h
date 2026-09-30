/* libos's namespace (ns.c), as its file calls (fs.c) and spawn (spawn.c)
 * use it. */
#pragma once

#include <os.h>

/* Where an absolute path leads: a duplicate of its mount's `fs` channel
 * (the caller closes it), the path inside the mount as fs.idl wants it
 * ("/logs/a.txt", "/" for the mount's root; zero-filled), and which mount
 * (an index that means something only until the namespace changes). The
 * root "/" is OK with *fs HANDLE_INVALID. ERR_INVALID_ARGS: not absolute,
 * or FS_PATH_MAX bytes or more; ERR_NOT_FOUND: under no mount. */
status_t ns_resolve(const char *path, uint8_t rel[FS_PATH_MAX], handle_t *fs, unsigned *mount);
