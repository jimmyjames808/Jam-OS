/* Views of a filesystem (<fsview.h>): the checks every filesystem service
 * runs on a view's requests, and the one-request server loop step that
 * answers fs.view itself.
 *
 * The check reads only the fields it needs (the method's path fields and
 * open's flags), from a request of exactly that method's size: anything
 * else is left to the generated dispatch, which refuses it. A path is
 * judged as the service will walk it: fs_path_clean resolves "." and ".."
 * first, so "/x/../etc/allow" is etc's. */
#include <fsview.h>

/* Lower case for ASCII letters, the rest as it is. */
static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool fs_view_in_etc(const char *clean)
{
    size_t start = 0, end = 0;
    while (clean[end] && clean[end] != '/')
        end++;
    while (start < end && clean[start] == ' ')
        start++;
    while (end > start && (clean[end - 1] == '.' || clean[end - 1] == ' '))
        end--;
    return end - start == 3 && lower(clean[start]) == 'e' && lower(clean[start + 1]) == 't' &&
           lower(clean[start + 2]) == 'c';
}

/* A path field (256 bytes, maybe without its NUL) at or under etc. One
 * that can't be cleaned counts as etc's: refusing it costs nothing, the
 * service would refuse it too. */
static bool field_in_etc(const uint8_t path[FS_PATH_MAX])
{
    char clean[FS_PATH_MAX];
    return fs_path_clean(path, clean) != OK || fs_view_in_etc(clean);
}

/* A change at path (two paths for a rename, else to is NULL). */
static status_t change(uint32_t flags, const uint8_t *path, const uint8_t *to)
{
    if (flags & FS_VIEW_READ_ONLY)
        return ERR_ACCESS_DENIED;
    if ((flags & FS_VIEW_GUARD_ETC) && (field_in_etc(path) || (to && field_in_etc(to))))
        return ERR_ACCESS_DENIED;
    return OK;
}

status_t fs_view_check(uint32_t flags, const void *req, uint32_t n)
{
    const struct idl_req_hdr *h = req;
    if (!flags || n < sizeof(*h))
        return OK;
    switch (h->ordinal) {
    case FS_OPEN: {
        const struct fs_open_req *q = req;
        const uint32_t writes = FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_APPEND;
        return n == sizeof(*q) && (q->flags & writes) ? change(flags, q->path, NULL) : OK;
    }
    case FS_MKDIR: {
        const struct fs_mkdir_req *q = req;
        return n == sizeof(*q) ? change(flags, q->path, NULL) : OK;
    }
    case FS_UNLINK: {
        const struct fs_unlink_req *q = req;
        return n == sizeof(*q) ? change(flags, q->path, NULL) : OK;
    }
    case FS_RENAME: {
        const struct fs_rename_req *q = req;
        return n == sizeof(*q) ? change(flags, q->from, q->to) : OK;
    }
    default:
        return OK;   /* stat, readdir, sync, statfs: they change nothing */
    }
}

/* fs.view on a channel with `flags`: a new channel, served by the host. */
static void answer_view(handle_t ch, uint32_t flags, const struct fs_view_req *q, uint32_t n,
                        status_t (*add)(void *host, handle_t ch, uint32_t flags), void *host)
{
    if (n != sizeof(*q) || (q->flags & ~FS_VIEW_FLAGS)) {
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return;
    }
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st == OK && (st = add(host, mine, flags | q->flags)) != OK)
        jam_handle_close(theirs);   /* add took mine */
    if (st != OK) {
        idl_reply_status(ch, q, n, st);
        return;
    }
    struct fs_view_rep r = { .txid = q->txid, .status = OK };
    if (jam_channel_write(ch, &r, sizeof(r), &theirs, 1) != OK)
        jam_handle_close(theirs);   /* the client is gone */
}

status_t fs_view_serve_one(handle_t ch, uint32_t flags, const struct fs_ops *ops, void *ctx,
                           status_t (*add)(void *host, handle_t ch, uint32_t flags), void *host)
{
    _Alignas(8) uint8_t q[FS_REQ_MAX];
    _Alignas(8) uint8_t r[FS_REP_MAX];
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ch, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *h = (const struct idl_req_hdr *)q;
    if (n >= sizeof(*h) && h->ordinal == FS_VIEW) {
        answer_view(ch, flags, (const struct fs_view_req *)q, n, add, host);
        return OK;
    }
    st = fs_view_check(flags, q, n);
    if (st != OK) {
        idl_reply_status(ch, q, n, st);
        return OK;
    }
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = fs_dispatch(ops, ctx, q, n, r, rhs, &rhn);
    if (h->ordinal == FS_STATFS && rn == sizeof(struct fs_statfs_rep) &&
        (flags & FS_VIEW_READ_ONLY))
        ((struct fs_statfs_rep *)r)->read_only = 1;
    if (!rn || drv_channel_write(ch, r, rn, rhs, rhn) != OK)
        idl_close_all(rhs, rhn);   /* not sent: still ours */
    return OK;
}
