/* Views of a filesystem (<fsview.h>): the checks every filesystem service
 * runs on a view's requests, and the one-request server loop step that
 * answers fs.view itself.
 *
 * The check reads only the fields it needs (the method's paths and open's
 * flags), from a request it can read: anything else is left to the
 * generated dispatch, which refuses it. A path is
 * judged as the service will walk it: fs_path_clean resolves "." and ".."
 * first, so "/x/../etc/allow" is etc's. A view where etc doesn't exist
 * (FS_VIEW_NO_ETC) also has it left out of the root's listing. */
#include <fsview.h>

/* Lower case for ASCII letters, the rest as it is. */
static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* Is the name at s (up to its '/' or end; *len: that length) `want`
 * (lower case) as FAT compares it: without ASCII case, leading spaces, or
 * trailing dots and spaces? */
static bool name_is(const char *s, size_t *len, const char *want)
{
    size_t start = 0, end = 0, n = strlen(want);
    while (s[end] && s[end] != '/')
        end++;
    *len = end;
    while (start < end && s[start] == ' ')
        start++;
    while (end > start && (s[end - 1] == '.' || s[end - 1] == ' '))
        end--;
    if (end - start != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (lower(s[start + i]) != want[i])
            return false;
    return true;
}

bool fs_view_in_etc(const char *clean)
{
    size_t n;
    return name_is(clean, &n, "etc");
}

bool fs_view_in_keys(const char *clean)
{
    size_t n, m;
    return name_is(clean, &n, "etc") && clean[n] == '/' && name_is(clean + n + 1, &m, "ssh");
}

bool fs_view_hides(uint32_t flags, const char *clean, bool stat)
{
    size_t n, m;
    if (!(flags & FS_VIEW_NO_ETC) || !name_is(clean, &n, "etc"))
        return false;
    if (flags & FS_VIEW_HIDE_AUTH)
        flags |= FS_VIEW_HIDE_ETC;   /* no other name of the file gets past */
    if (!clean[n])   /* etc itself: a stat, unless nothing in it shows */
        return (flags & FS_VIEW_HIDE_ETC) &&
               (!stat || (flags & FS_VIEW_NO_ETC) == FS_VIEW_NO_ETC);
    const char *rest = clean + n + 1;
    if (name_is(rest, &m, "ssh"))
        return flags & FS_VIEW_HIDE_KEYS;
    if (name_is(rest, &m, "authorized_keys") && !rest[m])
        return flags & FS_VIEW_HIDE_AUTH;
    return flags & FS_VIEW_HIDE_ETC;
}

/* A path (len bytes) at or under etc. One that can't be cleaned counts as
 * etc's: refusing it costs nothing, the service would refuse it too. */
static bool path_in_etc(const char *path, uint32_t len)
{
    char clean[FS_PATH_LEN + 1];
    return fs_path_clean(path, len, clean) != OK || fs_view_in_etc(clean);
}

/* A path (len bytes) that a view with flags hides from a request (stat:
 * fs.stat); one that can't be cleaned counts, as for path_in_etc. */
static bool path_hidden(uint32_t flags, const char *path, uint32_t len, bool stat)
{
    char clean[FS_PATH_LEN + 1];
    return fs_path_clean(path, len, clean) != OK || fs_view_hides(flags, clean, stat);
}

/* A change at path (two paths for a rename, else to is NULL). */
static status_t change(uint32_t flags, const char *path, uint32_t len, const char *to,
                       uint32_t to_len)
{
    if (flags & FS_VIEW_READ_ONLY)
        return ERR_ACCESS_DENIED;
    if ((flags & FS_VIEW_GUARD_ETC) &&
        (path_in_etc(path, len) || (to && path_in_etc(to, to_len))))
        return ERR_ACCESS_DENIED;
    return OK;
}

/* The path (a str) at *o of the request of n bytes: false if it can't be
 * read (the dispatch refuses the request then). */
static bool path_at(const void *req, uint32_t n, uint32_t *o, const char **path, uint32_t *len)
{
    const void *p = NULL;
    if (!idl_get(req, n, o, FS_PATH_LEN, 1, &p, len))
        return false;
    *path = p;
    return true;
}

/* For the hiding flags (FS_VIEW_NO_ETC): does the view hide any path of
 * the request (n bytes)? Every method that takes a path, reads included. */
static bool hidden_request(uint32_t flags, const void *req, uint32_t n)
{
    static const struct { uint32_t ordinal, fixed; bool two; } with_paths[] = {
        { FS_OPEN, sizeof(struct fs_open_req), false },
        { FS_STAT, sizeof(struct fs_stat_req), false },
        { FS_READDIR, sizeof(struct fs_readdir_req), false },
        { FS_MKDIR, sizeof(struct fs_mkdir_req), false },
        { FS_UNLINK, sizeof(struct fs_unlink_req), false },
        { FS_RENAME, sizeof(struct fs_rename_req), true },
    };
    const struct idl_req_hdr *h = req;
    bool stat = h->ordinal == FS_STAT;
    for (unsigned i = 0; i < sizeof(with_paths) / sizeof(with_paths[0]); i++) {
        if (h->ordinal != with_paths[i].ordinal)
            continue;
        const char *path;
        uint32_t len, o = with_paths[i].fixed;
        if (n < o || !path_at(req, n, &o, &path, &len))
            return false;   /* the dispatch refuses it */
        if (path_hidden(flags, path, len, stat))
            return true;
        return with_paths[i].two && path_at(req, n, &o, &path, &len) &&
               path_hidden(flags, path, len, false);
    }
    return false;
}

status_t fs_view_check(uint32_t flags, const void *req, uint32_t n)
{
    const struct idl_req_hdr *h = req;
    const char *path = NULL, *to = NULL;
    uint32_t len = 0, to_len = 0, o = 0;
    if (!flags || n < sizeof(*h))
        return OK;
    if ((flags & FS_VIEW_NO_ETC) && hidden_request(flags, req, n))
        return ERR_ACCESS_DENIED;
    switch (h->ordinal) {
    case FS_OPEN: {
        const struct fs_open_req *q = req;
        const uint32_t writes = FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_APPEND;
        o = sizeof(*q);
        if (n < sizeof(*q) || !(q->flags & writes) || !path_at(req, n, &o, &path, &len))
            return OK;
        return change(flags, path, len, NULL, 0);
    }
    case FS_MKDIR:
        o = sizeof(struct fs_mkdir_req);
        break;
    case FS_UNLINK:
        o = sizeof(struct fs_unlink_req);
        break;
    case FS_RENAME:
        o = sizeof(struct fs_rename_req);
        if (n < o || !path_at(req, n, &o, &path, &len) || !path_at(req, n, &o, &to, &to_len))
            return OK;
        return change(flags, path, len, to, to_len);
    default:
        return OK;   /* stat, readdir, sync, statfs: they change nothing */
    }
    if (n < o || !path_at(req, n, &o, &path, &len))
        return OK;
    return change(flags, path, len, NULL, 0);
}

/* A bare status for the request at req (n bytes) into rep: its length
 * (0: no txid, no reply). */
static uint32_t status_reply(const void *req, uint32_t n, void *rep, status_t st)
{
    if (n < sizeof(uint32_t))
        return 0;
    struct idl_rep_hdr *r = rep;
    r->txid = ((const struct idl_req_hdr *)req)->txid;
    r->status = st;
    return sizeof(*r);
}

/* fs.view on a channel with v's flags: a new channel, served by the host;
 * the client's end goes with the reply. */
static uint32_t answer_view(const struct fs_view_server *v, const struct fs_view_req *q,
                            uint32_t n, void *rep, handle_t *rhs, uint32_t *rhn)
{
    if (n != sizeof(*q) || (q->flags & ~FS_VIEW_FLAGS))
        return status_reply(q, n, rep, ERR_INVALID_ARGS);
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st == OK && (st = v->add(v->host, mine, v->flags | q->flags)) != OK)
        jam_handle_close(theirs);   /* add took mine */
    if (st != OK)
        return status_reply(q, n, rep, st);
    *(struct fs_view_rep *)rep = (struct fs_view_rep){ .txid = q->txid, .status = OK };
    rhs[0] = theirs;
    *rhn = 1;
    return sizeof(struct fs_view_rep);
}

/* fs.readdir for a view where etc doesn't exist (FS_VIEW_NO_ETC): the
 * filesystem's own (ctx: its ops and ctx), with `etc` left out of the
 * root's batches. A batch that held only etc goes on to the next one: an
 * empty batch that isn't the last is a broken one to the client. */
struct no_etc {
    const struct fs_ops *ops;
    void                *ctx;
};

static status_t readdir_no_etc(void *ctx, const char *path, uint32_t path_len, uint32_t cookie,
                               uint32_t *next, uint16_t *count, uint8_t *entries,
                               uint32_t *entries_len)
{
    const struct no_etc *w = ctx;
    char clean[FS_PATH_LEN + 1], name[FS_NAME_LEN + 1];
    for (;;) {
        status_t st = w->ops->readdir(w->ctx, path, path_len, cookie, next, count, entries,
                                      entries_len);
        if (st != OK || fs_path_clean(path, path_len, clean) != OK || clean[0])
            return st;   /* only the root holds the top-level etc */
        uint32_t at = 0, kept = 0;
        uint16_t left = 0;
        struct fs_dirent e;
        while (at < *entries_len) {
            uint32_t from = at;
            size_t n;
            if (!fs_dirent_get(entries, *entries_len, &at, &e))
                return ERR_INTERNAL;   /* never pass on what we can't judge */
            memcpy(name, e.name, e.name_len);
            name[e.name_len] = '\0';
            if (name_is(name, &n, "etc"))
                continue;
            memmove(entries + kept, entries + from, at - from);
            kept += at - from;
            left++;
        }
        *entries_len = kept;
        *count = left;
        if (left || !*next)
            return OK;
        cookie = *next;
    }
}

uint32_t fs_view_dispatch(const struct fs_view_server *v, const void *req, uint32_t n, void *rep,
                          handle_t *rhs, uint32_t *rhn)
{
    *rhn = 0;
    const struct idl_req_hdr *h = req;
    if (n >= sizeof(*h) && h->ordinal == FS_VIEW)
        return answer_view(v, req, n, rep, rhs, rhn);
    status_t st = fs_view_check(v->flags, req, n);
    if (st != OK)
        return status_reply(req, n, rep, st);
    uint32_t rn;
    if (h->ordinal == FS_READDIR && (v->flags & FS_VIEW_NO_ETC) == FS_VIEW_NO_ETC &&
        v->ops->readdir) {
        struct no_etc w = { .ops = v->ops, .ctx = v->ctx };
        const struct fs_ops ops = { .readdir = readdir_no_etc };
        rn = fs_dispatch(&ops, &w, req, n, rep, rhs, rhn);
    } else {
        rn = fs_dispatch(v->ops, v->ctx, req, n, rep, rhs, rhn);
    }
    if (h->ordinal == FS_STATFS && rn == sizeof(struct fs_statfs_rep) &&
        (v->flags & FS_VIEW_READ_ONLY))
        ((struct fs_statfs_rep *)rep)->read_only = 1;
    return rn;
}

status_t fs_view_serve_one(handle_t ch, uint32_t flags, const struct fs_ops *ops, void *ctx,
                           status_t (*add)(void *host, handle_t ch, uint32_t flags), void *host)
{
    /* Off the stack: an fs request and reply are 16 KiB together. */
    uint8_t *q = malloc(IDL_ROUND8(FS_REQ_MAX) + FS_REP_MAX);
    if (!q)
        return ERR_NO_MEMORY;
    uint8_t *r = q + IDL_ROUND8(FS_REQ_MAX);
    handle_t hs[IDL_READ_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(ch, q, FS_REQ_MAX, &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        st = idl_drain(ch, n, nh);
    if (st != OK) {
        free(q);
        return st;
    }
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(ch, q, n, ERR_INVALID_ARGS);
        free(q);
        return OK;
    }
    const struct fs_view_server v = {
        .flags = flags, .ops = ops, .ctx = ctx, .add = add, .host = host,
    };
    handle_t rhs[IDL_REP_HANDLES];
    uint32_t rhn = 0;
    uint32_t rn = fs_view_dispatch(&v, q, n, r, rhs, &rhn);
    if (!rn)
        idl_close_all(rhs, rhn);   /* no reply: never sent */
    else   /* the reply's handles are gone whatever the write's status */
        (void)drv_channel_write(ch, r, rn, rhs, rhn);
    free(q);
    return OK;
}
