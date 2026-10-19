/* The descriptor table a spawned child gets (docs/M13-PLAN.md section 5):
 * a copy of ours, posix_spawn's file actions applied to it, then each of
 * its descriptors exported into the hand-over (<posixfds.h>), and its
 * current directory (a directory description, as ours is: a channel goes
 * as itself, docs/DIRHANDLE-PLAN.md 4.5).
 *
 * The copy holds a reference to each description, not our descriptor
 * numbers: another thread closing or reusing one of ours mid-spawn
 * changes nothing in the copy, and close and dup2 only move references.
 * Nothing is exported (no handle made) until every action has worked. An
 * open action opens the file here (with O_CLOEXEC), takes a reference to
 * the new description and closes our descriptor at once; closefrom drops
 * every slot from its number up. chdir and fchdir
 * change only the copy's directory (chdir opens one as chdir(2) does,
 * fchdir shares the descriptor's), which the later actions' relative
 * paths start from (an open's by its text).
 *
 * Export asks each description what a child gets (ops->give, B1's
 * fd.h): new handles of the child's own (duplicates, or a file's fresh
 * open), so ours are never touched (but a socket's, which move: sock.h).
 * A description in two slots (a dup2) is given once, at the first, and
 * the other is a POSIXFDS_DUP of it: the child shares one description, as
 * dup2 makes it (a socket at 0 and 1, inetd's way, needs it: it can be
 * given only once).
 *
 * Everything here is the spawning thread's own (malloc'd per spawn): no
 * lock. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "files.h"
#include "spawn.h"

long spawn_absolute(const char *path, const char *dir, char *out)
{
    size_t lp = strnlen(path, PATH_MAX), ld = strnlen(dir, PATH_MAX);
    if (!lp)
        return -ENOENT;
    if (lp == PATH_MAX || ld + 1 + lp >= PATH_MAX)
        return -ENAMETOOLONG;
    char *joined = jam_malloc(ld + 1 + lp + 1);
    if (!joined)
        return -ENOMEM;
    size_t n = 0;
    if (path[0] != '/') {
        memcpy(joined, dir, ld);
        joined[ld] = '/';
        n = ld + 1;
    }
    memcpy(joined + n, path, lp + 1);
    out[0] = '/';   /* fs_path_clean gives the names without the leading '/' */
    status_t st = fs_path_clean(joined, (uint32_t)(n + lp), out + 1);
    jam_free(joined);
    return st == OK ? 0 : -EINVAL;
}

/* The copy's directory now d (its reference consumed). */
static void set_cwd(struct spawnfd *t, struct fd_desc *d)
{
    if (t->cwd)
        fd_put(t->cwd);
    t->cwd = d;
    jam_free(t->cwd_text);
    t->cwd_text = NULL;
}

/* The copy's directory's text: our current directory's as the layer keeps
 * it (a channel's asked of its service, a removed one's its last known),
 * so a child starts there and finds it gone, as Linux's child inherits a
 * removed directory, and a relative program path names nothing, never a
 * file of some other directory (docs/FORK-REVIEW-LIBC.md, finding 2). 0
 * and *out, or -errno. */
static long cwd_text(struct spawnfd *t, const char **out)
{
    _Static_assert(PATH_MAX == PATH_BYTES, "spawnfd's text holds dir_text_now's");
    if (!t->cwd_text) {
        char *text = jam_malloc(PATH_MAX);
        long e = !text ? -ENOMEM : t->cwd ? dir_text_now(t->cwd, text) : 0;
        if (!e && !t->cwd)
            memcpy(text, "/", 2);
        if (e) {
            jam_free(text);
            return e;
        }
        t->cwd_text = text;
    }
    *out = t->cwd_text;
    return 0;
}

long spawnfd_absolute(struct spawnfd *t, const char *path, char *out)
{
    const char *dir = "/";
    long e = path[0] == '/' ? 0 : cwd_text(t, &dir);
    return e ? e : spawn_absolute(path, dir, out);
}

long spawnfd_begin(struct spawnfd **out)
{
    struct spawnfd *t = jam_malloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    t->nslots = t->nexported = 0;
    t->desc = NULL;
    t->given = NULL;
    t->from = NULL;
    t->cwd = path_cwd();
    t->cwd_text = NULL;
    memset(&t->cwd_given, 0, sizeof(t->cwd_given));
    struct fd_desc *d;
    bool cloexec;
    for (long fd = fd_next(0, &d, &cloexec); fd >= 0; fd = fd_next(fd + 1, &d, &cloexec))
        t->slot[t->nslots++] = (struct spawnfd_slot){ .fd = (int32_t)fd, .desc = d,
                                                      .cloexec = cloexec };
    *out = t;
    return 0;
}

static struct spawnfd_slot *find(struct spawnfd *t, int fd)
{
    for (unsigned i = 0; i < t->nslots; i++)
        if (t->slot[i].fd == fd)
            return &t->slot[i];
    return NULL;
}

static void drop(struct spawnfd *t, int fd)
{
    struct spawnfd_slot *s = find(t, fd);
    if (s) {
        fd_put(s->desc);
        *s = t->slot[--t->nslots];
    }
}

/* The child's fd becomes d (the reference is consumed). */
static long set(struct spawnfd *t, int fd, struct fd_desc *d, bool cloexec)
{
    if (fd < 0 || fd >= FD_MAX) {
        fd_put(d);
        return -EBADF;
    }
    drop(t, fd);
    t->slot[t->nslots++] = (struct spawnfd_slot){ .fd = fd, .desc = d, .cloexec = cloexec };
    return 0;
}

static long act_dup2(struct spawnfd *t, const struct jam_spawn_op *op)
{
    struct spawnfd_slot *s = find(t, op->srcfd);
    if (!s)
        return -EBADF;
    if (op->srcfd == op->fd) {
        s->cloexec = false;   /* dup2 onto itself: the action clears FD_CLOEXEC */
        return 0;
    }
    fd_ref(s->desc);
    return set(t, op->fd, s->desc, false);
}

static long act_open(struct spawnfd *t, const struct jam_spawn_op *op)
{
    if (op->fd < 0 || op->fd >= FD_MAX)
        return -EBADF;
    char *abs = jam_malloc(PATH_MAX);
    long e = abs ? spawnfd_absolute(t, op->path, abs) : -ENOMEM;
    int fd = -1;
    if (!e) {
        fd = open(abs, op->oflag | O_CLOEXEC, (mode_t)op->mode);
        e = fd < 0 ? -errno : 0;
    }
    jam_free(abs);
    if (e)
        return e;
    struct fd_desc *d = fd_get(fd);
    (void)fd_close(fd);   /* ours only for the moment: the reference keeps it */
    if (!d)
        return -EBADF;    /* another thread closed it already */
    return set(t, op->fd, d, op->oflag & O_CLOEXEC);
}

/* chdir's directory from the copy's, opened as chdir opens one. */
static long act_chdir(struct spawnfd *t, const struct jam_spawn_op *op)
{
    struct fd_desc *d = NULL;
    long e = dir_cwd_open(t->cwd, op->path, &d);
    if (!e)
        set_cwd(t, d);
    return e;
}

/* The copy's descriptor's directory, shared as fchdir shares it. */
static long act_fchdir(struct spawnfd *t, const struct jam_spawn_op *op)
{
    struct spawnfd_slot *s = find(t, op->fd);
    if (!s)
        return -EBADF;
    if (!dir_is(s->desc))
        return -ENOTDIR;
    fd_ref(s->desc);
    set_cwd(t, s->desc);
    return 0;
}

/* Every descriptor from `from` up closed (addclosefrom_np): what the
 * actions before it put there too, as the child would close them. */
static void closefrom_copy(struct spawnfd *t, int from)
{
    for (unsigned i = 0; i < t->nslots;) {
        if (t->slot[i].fd >= from) {
            fd_put(t->slot[i].desc);
            t->slot[i] = t->slot[--t->nslots];
        } else {
            i++;
        }
    }
}

long spawnfd_apply(struct spawnfd *t, const struct jam_spawn_op *ops, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const struct jam_spawn_op *op = &ops[i];
        long e = 0;
        switch (op->cmd) {
        case JAM_SPAWN_CLOSE:  drop(t, op->fd); break;   /* closing a closed one: fine */
        case JAM_SPAWN_DUP2:   e = act_dup2(t, op); break;
        case JAM_SPAWN_OPEN:   e = act_open(t, op); break;
        case JAM_SPAWN_CHDIR:  e = act_chdir(t, op); break;
        case JAM_SPAWN_FCHDIR: e = act_fchdir(t, op); break;
        case JAM_SPAWN_CLOSEFROM: closefrom_copy(t, op->fd); break;
        default:               e = -EINVAL; break;
        }
        if (e)
            return e;
    }
    return 0;
}

/* The descriptor an earlier record of this hand-over gave d as, or -1
 * (each record's description is in `from`: a walk of the records, not of
 * the slots for each). */
static int32_t exported_as(struct spawnfd *t, const struct fd_desc *d)
{
    for (unsigned i = 0; i < t->nexported; i++)
        if (t->from[i] == d && t->desc[i].kind != POSIXFDS_DUP)
            return t->desc[i].fd;
    return -1;
}

/* A POSIXFDS_DUP record for a description given already as descriptor
 * first: the child shares it, as dup2 made it here. 0 or -ENOMEM. */
static long dup_of(struct fd_given *g, int32_t first)
{
    uint32_t *data = jam_malloc(sizeof(*data));
    if (!data)
        return -ENOMEM;
    *data = (uint32_t)first;
    *g = (struct fd_given){ .kind = POSIXFDS_DUP, .data = data, .data_len = sizeof(*data) };
    return 0;
}

/* Slot s given to the child as the next descriptor: 0, -ENOTSUP (its
 * kind is left out), or -errno. A description in two slots is given once,
 * and the later slot is a POSIXFDS_DUP of the first. */
static long export_one(struct spawnfd *t, const struct spawnfd_slot *s)
{
    unsigned i = t->nexported;
    struct fd_given *g = &t->given[i];
    memset(g, 0, sizeof(*g));
    int32_t first = exported_as(t, s->desc);
    long e = first >= 0          ? dup_of(g, first)
             : s->desc->ops->give ? s->desc->ops->give(s->desc, g)
                                  : -ENOTSUP;
    if (e) {
        fd_given_drop(g);
        return e;
    }
    if ((int)g->nhandles != posixfds_kind_handles(g->kind, NULL)) {
        fd_given_drop(g);   /* a kind's give that breaks the format: never sent */
        return -EIO;
    }
    t->desc[i] = (struct posixfds_desc){
        .fd = s->fd, .kind = g->kind, .status = g->status, .offset = g->offset,
        .data = g->data, .data_len = g->data_len, .handles = g->handles,
    };
    t->from[i] = s->desc;
    t->nexported++;
    return 0;
}

long spawnfd_export(struct spawnfd *t, struct posixfds_out *ho)
{
    unsigned n = t->nslots ? t->nslots : 1;
    t->desc = jam_calloc(n, sizeof(*t->desc));
    t->given = jam_calloc(n, sizeof(*t->given));
    t->from = jam_calloc(n, sizeof(*t->from));
    long e = t->desc && t->given && t->from ? 0 : -ENOMEM;
    for (unsigned i = 0; !e && i < t->nslots; i++) {
        if (t->slot[i].cloexec)
            continue;
        e = export_one(t, &t->slot[i]);
        if (e == -ENOTSUP)
            e = 0;
    }
    ho->fds = t->desc;
    ho->nfds = t->nexported;
    if (e) {
        posixfds_discard(ho);
        ho->nfds = 0;
        ho->cwd_dir = HANDLE_INVALID;
    }
    return e;
}

long spawnfd_export_cwd(struct spawnfd *t, struct posixfds_out *ho, bool text)
{
    struct fd_given *g = &t->cwd_given;
    ho->cwd = "/";
    ho->cwd_dir = HANDLE_INVALID;
    if (!t->cwd)
        return 0;
    if (!text) {
        long e = t->cwd->ops->give(t->cwd, g);
        if (!e && g->kind == POSIXFDS_DIR_CHANNEL) {
            ho->cwd_dir = g->handles[0];   /* the hand-over's now */
            g->handles[0] = HANDLE_INVALID;
            g->nhandles = 0;
            ho->cwd_data = g->data;
            ho->cwd_data_len = g->data_len;
            return 0;
        }
        if (!e) {
            ho->cwd = g->data;   /* a text one's path */
            return 0;
        }
        fd_given_drop(g);
        if (e == -ENOMEM)
            return e;
    }
    const char *dir;   /* narrowed, or a gone channel: its text, else "/" */
    long e = cwd_text(t, &dir);
    if (!e)
        ho->cwd = dir;
    return e == -ENOMEM ? e : 0;
}

void spawnfd_end(struct spawnfd *t, struct posixfds_out *ho)
{
    ho->fds = NULL;
    ho->nfds = 0;
    ho->cwd_dir = HANDLE_INVALID;   /* consumed with the hand-over */
    if (!t)
        return;
    fd_given_drop(&t->cwd_given);   /* its data: its handle went with the hand-over */
    if (t->cwd)
        fd_put(t->cwd);
    jam_free(t->cwd_text);
    for (unsigned i = 0; t->given && i < t->nexported; i++)
        jam_free(t->given[i].data);   /* the handles went with the hand-over */
    jam_free(t->desc);
    jam_free(t->given);
    jam_free(t->from);
    for (unsigned i = 0; i < t->nslots; i++)
        fd_put(t->slot[i].desc);
    jam_free(t);
}
