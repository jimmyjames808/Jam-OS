/* Descriptor passing: the two translation tables, a description to the
 * handle that carries it and a handle back to a description, and the
 * SCM_RIGHTS control data around them. fdpass.h has the model and the
 * rules (what crosses, duplicates never moves, the copy of a VMO that
 * can't be duplicated). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "fdpass.h"
#include "files.h"
#include "kinds.h"

#define READS(r)  (((r) & RIGHT_READ) != 0)
#define WRITES(r) (((r) & RIGHT_WRITE) != 0)

static bool said_refused;   /* the log line about a refused kind was written; atomic */

/* ---- descriptor -> handle ------------------------------------------------------------- */

/* The memfd's VMO with the description's rights, narrowed to the policy's. */
static long memfd_handle(struct fd_desc *d, const struct fdpass_policy *p, handle_t *out)
{
    uint64_t size;
    handle_t h;
    long r = memfd_vmo(d, &h, &size);
    if (r || p->memfd_rights == RIGHT_SAME) {
        if (!r)
            *out = h;
        return r;
    }
    struct handle_info hi;
    status_t st = jam_handle_info(h, &hi);
    if (st != OK) {
        jam_handle_close(h);
        return lx_err(st);
    }
    st = jam_handle_replace(h, hi.rights & p->memfd_rights, out);   /* h gone either way */
    return st == OK ? 0 : lx_err(st);
}

static long channel_handle(struct fd_desc *d, const struct fdpass_policy *p, handle_t *out)
{
    status_t st = jam_handle_duplicate(pipe_channel(d), RIGHT_SAME, out);
    return st == OK ? 0 : lx_err(st);
}

/* The table: a kind of description, and how it becomes a handle. */
static const struct {
    const struct fd_ops *ops;
    unsigned kind;   /* FDPASS_* */
    long (*take)(struct fd_desc *d, const struct fdpass_policy *p, handle_t *out);
} to_handle[] = {
    { &memfd_ops, FDPASS_MEMFD, memfd_handle },
    { &pipe_ops, FDPASS_CHANNEL, channel_handle },
    { &sockp_ops, FDPASS_CHANNEL, channel_handle },
};

long fdpass_to_handle(struct fd_desc *d, const struct fdpass_policy *p, handle_t *out)
{
    for (unsigned i = 0; i < sizeof(to_handle) / sizeof(to_handle[0]); i++)
        if (to_handle[i].ops == d->ops && (to_handle[i].kind & p->kinds))
            return to_handle[i].take(d, p, out);
    return -EINVAL;
}

/* ---- handle -> descriptor ------------------------------------------------------------- */

/* A copy of vmo's first size bytes (vmo consumed) in a new kept VMO: OK
 * with *out. */
static status_t copy_vmo(handle_t vmo, uint64_t size, handle_t *out)
{
    enum { CHUNK = 16384 };
    handle_t copy = HANDLE_INVALID;
    uint8_t *buf = malloc(CHUNK);
    status_t st = buf ? jam_vmo_create(size, VMO_KEEP_PAGES, &copy) : ERR_NO_MEMORY;
    for (uint64_t at = 0; st == OK && at < size; at += CHUNK) {
        uint64_t k = size - at < CHUNK ? size - at : CHUNK;
        st = jam_vmo_read(vmo, at, buf, k);
        if (st == OK)
            st = jam_vmo_write(copy, at, buf, k);
    }
    free(buf);
    jam_handle_close(vmo);
    if (st != OK && copy)
        jam_handle_close(copy);
    if (st == OK)
        *out = copy;
    return st;
}

static long memfd_desc(handle_t h, const struct handle_info *hi, struct fd_desc **out)
{
    uint64_t size = 0;
    bool copy = !(hi->rights & RIGHT_DUPLICATE);
    if (!READS(hi->rights) || jam_vmo_get_size(h, &size) != OK ||
        (copy && size > FDPASS_COPY_MAX)) {
        jam_handle_close(h);
        return -EINVAL;
    }
    uint32_t status = WRITES(hi->rights) ? O_RDWR : O_RDONLY;
    if (copy) {
        status_t st = copy_vmo(h, size, &h);   /* consumes h; h is the copy after */
        if (st != OK)
            return lx_err(st);
        status = O_RDONLY;   /* the copy is ours alone: a write would reach nobody */
    }
    return memfd_from_vmo(h, status, out);
}

static long channel_desc(handle_t h, const struct handle_info *hi, struct fd_desc **out)
{
    bool rd = READS(hi->rights), wr = WRITES(hi->rights);
    if (!rd && !wr) {
        jam_handle_close(h);
        return -EINVAL;
    }
    const struct posixfds_fd rec = { .kind = POSIXFDS_PIPE, .nhandles = 1,
                                     .status = rd && wr ? O_RDWR : rd ? O_RDONLY : O_WRONLY };
    return pipe_adopt(&rec, NULL, &h, out);   /* O_RDWR makes a socketpair end */
}

/* The table: an object type, and the description a handle to it makes. */
static const struct {
    uint32_t type;   /* OBJ_* */
    unsigned kind;   /* FDPASS_* */
    long (*make)(handle_t h, const struct handle_info *hi, struct fd_desc **out);
} to_desc[] = {
    { OBJ_VMO, FDPASS_MEMFD, memfd_desc },
    { OBJ_CHANNEL, FDPASS_CHANNEL, channel_desc },
};

/* The row for the object h names, among kinds (*hi filled), or -1. */
static int desc_row(handle_t h, unsigned kinds, struct handle_info *hi)
{
    if (jam_handle_info(h, hi) != OK)
        return -1;
    for (unsigned i = 0; i < sizeof(to_desc) / sizeof(to_desc[0]); i++)
        if (to_desc[i].type == hi->type && (to_desc[i].kind & kinds))
            return (int)i;
    return -1;
}

unsigned fdpass_kind_of(handle_t h, unsigned kinds)
{
    struct handle_info hi;
    int i = desc_row(h, kinds, &hi);
    return i < 0 ? 0 : to_desc[i].kind;
}

long fdpass_to_desc(handle_t h, unsigned kinds, struct fd_desc **out)
{
    struct handle_info hi;
    int i = desc_row(h, kinds, &hi);
    if (i < 0) {
        jam_handle_close(h);
        return -EINVAL;
    }
    return to_desc[i].make(h, &hi, out);
}

/* ---- a call's worth ------------------------------------------------------------------- */

/* "posix: sendmsg: a descriptor of kind <kind> can't cross this socket"
 * in the log, once a process: a program whose send fails is told why (an
 * old Wayland client making its pool from a file is told at once, not
 * shown a black window). */
static void say_refused(const struct fd_desc *d)
{
    if (__atomic_exchange_n(&said_refused, true, __ATOMIC_RELAXED))
        return;
    char line[96];
    int n = snprintf(line, sizeof(line),
                     "posix: sendmsg: a descriptor of kind %s can't cross this socket\n",
                     d->ops->name);
    if (n > 0)
        (void)jam_debug_write(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

long fdpass_fds_out(const int *fds, unsigned n, const struct fdpass_policy *p, handle_t *hs)
{
    long r = 0;
    unsigned made = 0;
    while (made < n && !r) {
        struct fd_desc *d = fd_get(fds[made]);
        r = d ? fdpass_to_handle(d, p, &hs[made]) : -EBADF;
        if (r == -EINVAL)
            say_refused(d);
        if (d)
            fd_put(d);
        if (!r)
            made++;
    }
    if (r)
        for (unsigned k = 0; k < made; k++)
            jam_handle_close(hs[k]);
    return r;
}

void fdpass_fds_in(handle_t *hs, unsigned n, unsigned kinds, struct fdpass_sink *s)
{
    s->got = 0;
    s->cut = false;
    for (unsigned i = 0; i < n; i++) {
        if (s->got == s->room) {
            jam_handle_close(hs[i]);
            s->cut = true;
            continue;
        }
        struct fd_desc *d = NULL;
        long r = fdpass_to_desc(hs[i], kinds, &d);     /* consumes hs[i] */
        long fd = r ? r : fd_install(d, 0, s->cloexec);   /* consumes d's reference */
        if (fd < 0)
            s->cut = true;
        else
            s->fds[s->got++] = (int)fd;
    }
}

/* ---- with records --------------------------------------------------------------------- */

/* The FDPASS_* bit of a description's kind (0: one that never crosses). */
static unsigned desc_kind(struct fd_desc *d)
{
    if (d->ops == &pipe_ops || d->ops == &sockp_ops)
        return FDPASS_CHANNEL;
    if (d->ops == &memfd_ops)
        return FDPASS_MEMFD;
    if (d->ops == &dev_ops || d->ops == &etc_ops)
        return FDPASS_NODE;
    return file_desc_is(d) ? FDPASS_FILE : 0;
}

/* The FDPASS_* bit of a hand-over record's kind (0: one that never
 * crosses). */
static unsigned record_kind(uint32_t kind)
{
    switch (kind) {
    case POSIXFDS_PIPE:
    case POSIXFDS_SEQPACKET: return FDPASS_CHANNEL;
    case POSIXFDS_MEMFD:     return FDPASS_MEMFD;
    case POSIXFDS_FILE:      return FDPASS_FILE;
    case POSIXFDS_DEV:
    case POSIXFDS_ETC:       return FDPASS_NODE;
    }
    return 0;
}

/* d as a record under p: 0 and *out, or -errno (-EINVAL: a kind p doesn't
 * pass). */
static long give_one(struct fd_desc *d, const struct fdpass_policy *p, struct fd_given *out)
{
    unsigned k = desc_kind(d);
    memset(out, 0, sizeof(*out));
    if (!(k & p->kinds))
        return -EINVAL;
    long r = k == FDPASS_CHANNEL ? pipe_pass(d, out) : d->ops->give(d, out);
    if (r || k != FDPASS_MEMFD || p->memfd_rights == RIGHT_SAME)
        return r;
    struct handle_info hi;
    status_t st = jam_handle_info(out->handles[0], &hi);
    if (st == OK)
        st = jam_handle_replace(out->handles[0], hi.rights & p->memfd_rights, &out->handles[0]);
    if (st != OK) {   /* a failed replace closed the handle already */
        out->nhandles = 0;
        fd_given_drop(out);
    }
    return lx_err(st);
}

long fdpass_gives_out(const int *fds, unsigned n, const struct fdpass_policy *p,
                      struct fd_given *gs)
{
    long r = 0;
    unsigned made = 0;
    while (made < n && !r) {
        struct fd_desc *d = fd_get(fds[made]);
        r = d ? give_one(d, p, &gs[made]) : -EBADF;
        if (r == -EINVAL)
            say_refused(d);
        if (d)
            fd_put(d);
        if (!r)
            made++;
    }
    if (r)
        fdpass_gives_drop(gs, made);
    return r;
}

void fdpass_gives_drop(struct fd_given *gs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        fd_given_drop(&gs[i]);
}

/* The n handles hs are of the object types types. */
static bool types_are(const handle_t *hs, const uint32_t *types, int n)
{
    for (int i = 0; i < n; i++) {
        struct handle_info hi;
        if (jam_handle_info(hs[i], &hi) != OK || hi.type != types[i])
            return false;
    }
    return true;
}

/* A string of len bytes: its NUL last and none before, at least 2. */
static bool string_ok(const char *s, uint32_t len)
{
    return len >= 2 && s[len - 1] == '\0' && strnlen(s, len) == len - 1;
}

/* A record's fields as another process may send them: the hand-over's
 * rules (fdmsg.c's) for the kinds that cross, and no leftover bytes with a
 * channel end (they would read as the end's own, before its first). */
static bool record_ok(const struct posixfds_fd *rec, const void *data)
{
    if ((rec->status & ~POSIXFDS_STATUS) ||
        (rec->status & POSIXFDS_O_ACCMODE) == POSIXFDS_O_ACCMODE ||
        rec->data_len > POSIXFDS_DATA_MAX || (rec->offset && rec->kind != POSIXFDS_FILE))
        return false;
    switch (rec->kind) {
    case POSIXFDS_PIPE:
    case POSIXFDS_SEQPACKET: return rec->data_len == 0;
    case POSIXFDS_DEV:
    case POSIXFDS_ETC:       return string_ok(data, rec->data_len);
    }
    return true;   /* a memfd's data and a file's: their adopters check or ignore it */
}

long fdpass_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                  unsigned kinds, struct fd_desc **out)
{
    uint32_t types[POSIXFDS_KIND_HANDLES];
    int nh = record_kind(rec->kind) & kinds ? posixfds_kind_handles(rec->kind, types) : -1;
    if (nh != (int)rec->nhandles || !types_are(hs, types, nh) || !record_ok(rec, data)) {
        for (uint32_t i = 0; i < rec->nhandles && i < POSIXFDS_KIND_HANDLES; i++)
            jam_handle_close(hs[i]);
        return -EINVAL;
    }
    struct posixfds_fd r = *rec;
    if (r.kind == POSIXFDS_FILE) {   /* its path is not believed: see fdpass.h */
        r.data_len = 0;
        data = NULL;
    }
    return fd_adopters[r.kind](&r, data, hs, out);
}

/* ---- SCM_RIGHTS ------------------------------------------------------------------------- */

long fdpass_cmsg_fds(const struct msghdr *m, int *fds, unsigned max)
{
    if (!m->msg_controllen)
        return 0;
    if (!m->msg_control || m->msg_controllen < sizeof(struct cmsghdr))
        return -EINVAL;
    unsigned n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
        size_t at = (size_t)((uint8_t *)c - (uint8_t *)m->msg_control);
        if (c->cmsg_len < CMSG_LEN(0) || c->cmsg_len > m->msg_controllen - at)
            return -EINVAL;   /* a block past the control data's end */
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            return -EINVAL;   /* credentials and the rest: not built */
        size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (k > max - n)
            return -EINVAL;
        memcpy(fds + n, CMSG_DATA(c), k * sizeof(int));
        n += (unsigned)k;
    }
    return n;
}

unsigned fdpass_cmsg_room(const struct msghdr *m)
{
    if (!m->msg_control || m->msg_controllen < CMSG_LEN(sizeof(int)))
        return 0;
    size_t k = (m->msg_controllen - CMSG_LEN(0)) / sizeof(int);
    return k > FDPASS_MAX ? FDPASS_MAX : (unsigned)k;
}

void fdpass_cmsg_put(struct msghdr *m, const int *fds, unsigned n)
{
    if (!n) {
        m->msg_controllen = 0;
        return;
    }
    struct cmsghdr *c = (struct cmsghdr *)m->msg_control;
    c->cmsg_len = CMSG_LEN(n * sizeof(int));
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(c), fds, n * sizeof(int));
    size_t used = CMSG_SPACE(n * sizeof(int));   /* Linux's count: the block, padded */
    if (used < m->msg_controllen)
        m->msg_controllen = used;
}
