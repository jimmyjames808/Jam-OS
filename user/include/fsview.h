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
/* The hiding flags: every request on a hidden path is refused, reads too
 * (ERR_ACCESS_DENIED, as for a change a view refuses). Each hides one part
 * of the top-level `etc`; all three hide the whole of it (FS_VIEW_NO_ETC). */
#define FS_VIEW_HIDE_KEYS 4u   /* `etc/ssh` and everything in it: the SSH server's host
                                * key (init's ssh.c), only for the views made for the
                                * signer (<os.h> "grants": `:rk`) */
#define FS_VIEW_HIDE_ETC  8u   /* the rest of `etc`: every other name in it (the
                                * settings, `allow`, the history), its listing, and any
                                * request on `etc` itself but a stat */
#define FS_VIEW_HIDE_AUTH 16u  /* `etc/authorized_keys`: the keys that may log in, only
                                * for the SSH server's views (`:rk`, `:ra`) */
#define FS_VIEW_FLAGS     31u  /* all of them: any other bit is ERR_INVALID_ARGS */
/* The three hiding flags: `etc` doesn't exist for the view. A stat of it is
 * refused too, and a listing of the root leaves it out. Every program's
 * view has them (<os.h> "grants": `:r`, `:w`); only init and the shell it
 * starts hold a /data with `etc` in it, and the SSH server a view with its
 * own two files. */
#define FS_VIEW_NO_ETC (FS_VIEW_HIDE_KEYS | FS_VIEW_HIDE_ETC | FS_VIEW_HIDE_AUTH)

/* Is the cleaned path (fs_path_clean's form: names joined by '/', none
 * leading) at or under the top-level `etc` directory? The first name is
 * compared without ASCII case and without leading spaces or trailing dots
 * and spaces, which FAT ignores, so "ETC", "etc." and " Etc" are etc. */
bool     fs_view_in_etc(const char *clean);
/* Is the cleaned path at or under `etc/ssh` (both names compared as
 * fs_view_in_etc compares `etc`)? A whole directory, not its files by
 * name: FAT also opens a file by its short name (`ED2551~1.SK`), which a
 * check of the file's own name would let past. */
bool     fs_view_in_keys(const char *clean);
/* Does a view with `flags` hide the cleaned path from a request (stat: the
 * request is fs.stat, which `etc` itself answers unless all of it is
 * hidden)? `etc/authorized_keys` is the one file shown by its name: any
 * other name of it (FAT's short name, `AUTHOR~1`) is the rest of etc's,
 * hidden by FS_VIEW_HIDE_ETC; FS_VIEW_HIDE_AUTH without FS_VIEW_HIDE_ETC
 * hides the rest of etc as well, so no other name can reach it. */
bool     fs_view_hides(uint32_t flags, const char *clean, bool stat);
/* Whether a view with `flags` may make the request of n bytes at req:
 * OK, or ERR_ACCESS_DENIED. A request this can't read (too short, not an
 * fs method, a path it can't take apart) is OK here: the protocol's own
 * dispatch refuses it. */
status_t fs_view_check(uint32_t flags, const void *req, uint32_t n);

/* Serve one request on ch, a channel with view flags `flags` (0: the
 * whole filesystem): fs.view is answered here, by calling add(host, its
 * server end, the new flags) (add takes the handle, whatever it returns;
 * ERR_NO_RESOURCES when it can serve no more), every other request is
 * checked with fs_view_check and then given to fs_dispatch(ops, ctx, ...),
 * and statfs on a read-only view says read-only. Returns as fs_serve_one
 * does: OK once a message was handled, else the read's status
 * (ERR_SHOULD_WAIT: nothing queued; ERR_PEER_CLOSED: the client is gone),
 * or ERR_NO_MEMORY with the message still queued (its buffers come from
 * the heap). */
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
