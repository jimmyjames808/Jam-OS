/* libos's namespace (ns.c), as its file calls (fs.c) and spawn (spawn.c)
 * use it. */
#pragma once

#include <os.h>

/* *mount for /svc and the services in it. */
#define NS_AT_SVC 0xffffffffu

/* Where an absolute path leads: a duplicate of its mount's `fs` channel
 * (the caller closes it), the path inside the mount as fs.idl wants it
 * ("/logs/a.txt", "/" for the mount's root; zero-filled), and which mount
 * (an index that means something only until the namespace changes). The
 * root "/" is OK with *fs HANDLE_INVALID and *mount 0; "/svc" and a
 * service in it are OK with *fs HANDLE_INVALID, *mount NS_AT_SVC and rel
 * "/" or "/<name>". ERR_INVALID_ARGS: not absolute, or FS_PATH_MAX bytes
 * or more; ERR_NOT_FOUND: under no mount (or no such service). */
status_t ns_resolve(const char *path, uint8_t rel[FS_PATH_MAX], handle_t *fs, unsigned *mount);

/* A namespace message made ahead of its sending (spawn: the views are
 * asked for before the program starts, so it never waits for them). */
struct ns_out;
/* What grants name (<os.h> "grants"; NULL: nothing), views made, into a
 * new *out. */
status_t ns_prepare(const char *const *grants, struct ns_out **out);
/* Send it on `to` as a `kind` message (NS_MOUNT or NS_SET); o is freed
 * and its handles sent or closed, whatever happens. */
status_t ns_send_prepared(handle_t to, uint32_t kind, struct ns_out *o);
/* Free one that won't be sent, closing its handles. */
void     ns_prepared_drop(struct ns_out *o);
