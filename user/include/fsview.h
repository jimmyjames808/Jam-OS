/* Views: narrower `fs` channels onto one filesystem (fs.view,
 * abi/idl/fs.idl), for the filesystem services that hand them out (fat,
 * and libos's fsserver for the bootfs server and the tests' RAM
 * filesystem).
 *
 * A view is a channel with flags. The service checks every request on it
 * with fs_view_check before its own code sees it, on the path as it will
 * be walked ("." and ".." resolved by fs_path_clean), so a request that
 * the flags refuse never reaches the filesystem. Who makes views: whoever
 * gives a program a mount (libos's namespace, <os.h> "services and views"):
 * every mount a program gets from the shell is a view. */
#pragma once

#include <fs_idl.h>

#define FS_VIEW_READ_ONLY 1u   /* every change refused; statfs says read-only */
#define FS_VIEW_GUARD_ETC 2u   /* every change at or under the top-level `etc` refused */
#define FS_VIEW_FLAGS     3u   /* all of them: any other bit is ERR_INVALID_ARGS */

/* Is the cleaned path (fs_path_clean's form: names joined by '/', none
 * leading) at or under the top-level `etc` directory? The first name is
 * compared without ASCII case and without leading spaces or trailing dots
 * and spaces, which FAT ignores, so "ETC", "etc." and " Etc" are etc. */
bool     fs_view_in_etc(const char *clean);
/* Whether a view with `flags` may make the request of n bytes at req:
 * OK, or ERR_ACCESS_DENIED. A request this can't read (too short, not an
 * fs method, a path field without its NUL) is OK here: the protocol's own
 * dispatch refuses it. */
status_t fs_view_check(uint32_t flags, const void *req, uint32_t n);

/* Serve one request on ch, a channel with view flags `flags` (0: the
 * whole filesystem): fs.view is answered here, by calling add(host, its
 * server end, the new flags) (add takes the handle, whatever it returns;
 * ERR_NO_RESOURCES when it can serve no more), every other request is
 * checked with fs_view_check and then given to fs_dispatch(ops, ctx, ...),
 * and statfs on a read-only view says read-only. Returns as fs_serve_one
 * does: OK once a message was handled, else the read's status
 * (ERR_SHOULD_WAIT: nothing queued; ERR_PEER_CLOSED: the client is gone). */
status_t fs_view_serve_one(handle_t ch, uint32_t flags, const struct fs_ops *ops, void *ctx,
                           status_t (*add)(void *host, handle_t ch, uint32_t flags), void *host);

/* One channel's server, as fs_view_serve_one's arguments. */
struct fs_view_server {
    uint32_t             flags;   /* the channel's view flags (0: the whole filesystem) */
    const struct fs_ops *ops;     /* the filesystem's methods ... */
    void                *ctx;     /* ... and their ctx */
    /* serves a new view (fs.view): takes ch, whatever it returns */
    status_t (*add)(void *host, handle_t ch, uint32_t flags);
    void                *host;    /* add's */
};

/* What fs_view_serve_one does with a request, for one already read (a
 * service that reads requests itself, into a slot of its state): the n
 * bytes at req, which carried no handles (one that did is the caller's to
 * refuse), answered into rep (FS_REP_MAX bytes) and rhs (IDL_REP_HANDLES
 * slots; *rhn of them). Returns the reply's length (0: no reply, the
 * request had no txid). No I/O but add's; the caller sends the reply with
 * the handles, or closes them if it can't. */
uint32_t fs_view_dispatch(const struct fs_view_server *v, const void *req, uint32_t n, void *rep,
                          handle_t *rhs, uint32_t *rhn);
