/* The descriptor table a spawned child gets (docs/M13-PLAN.md section 5):
 * a copy of ours, posix_spawn's file actions applied to it, then each of
 * its descriptors exported into the hand-over (<posixfds.h>).
 *
 * The copy holds a reference to each description, not our descriptor
 * numbers: another thread closing or reusing one of ours mid-spawn
 * changes nothing in the copy, and close and dup2 only move references.
 * Nothing is exported (no handle made) until every action has worked. An
 * open action opens the file here (with O_CLOEXEC), takes a reference to
 * the new description and closes our descriptor at once. chdir and fchdir
 * change only the copy's directory, which the later actions' relative
 * paths start from.
 *
 * Export asks each description what a child gets (ops->give, B1's
 * fd.h): new handles of the child's own (duplicates, or a file's fresh
 * open), so ours are never touched. A description in two slots (a dup2)
 * is given twice: two descriptions in the child, not one shared (the
 * hand-over has no way to say "the same"; offsets are per process anyway,
 * question 5).
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
#include "spawn.h"

long spawn_absolute(const char *path, const char *dir, char *out)
{
    size_t lp = strnlen(path, PATH_MAX), ld = strnlen(dir, PATH_MAX);
    if (!lp)
        return -ENOENT;
    if (lp == PATH_MAX || ld + 1 + lp >= PATH_MAX)
        return -ENAMETOOLONG;
    char *joined = malloc(ld + 1 + lp + 1);
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
    free(joined);
    return st == OK ? 0 : -EINVAL;
}

uint32_t spawnfd_umask(uint32_t fallback)
{
    return fallback;
}

/* Our current directory into cwd: getcwd's, else the hand-over's, else
 * "/". */
static void our_cwd(char *cwd)
{
    if (getcwd(cwd, PATH_MAX) && cwd[0] == '/')
        return;
    const char *ho;
    if (lx_handover(&ho)) {
        snprintf(cwd, PATH_MAX, "%s", ho);
        return;
    }
    strcpy(cwd, "/");
}

long spawnfd_begin(struct spawnfd **out)
{
    struct spawnfd *t = malloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    t->nslots = t->nexported = 0;
    t->desc = NULL;
    t->given = NULL;
    our_cwd(t->cwd);
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
    char *abs = malloc(PATH_MAX);
    long e = abs ? spawn_absolute(op->path, t->cwd, abs) : -ENOMEM;
    int fd = -1;
    if (!e) {
        fd = open(abs, op->oflag | O_CLOEXEC, (mode_t)op->mode);
        e = fd < 0 ? -errno : 0;
    }
    free(abs);
    if (e)
        return e;
    struct fd_desc *d = fd_get(fd);
    (void)fd_close(fd);   /* ours only for the moment: the reference keeps it */
    if (!d)
        return -EBADF;    /* another thread closed it already */
    return set(t, op->fd, d, op->oflag & O_CLOEXEC);
}

static long act_chdir(struct spawnfd *t, const struct jam_spawn_op *op)
{
    char *abs = malloc(PATH_MAX);
    long e = abs ? spawn_absolute(op->path, t->cwd, abs) : -ENOMEM;
    bool dir = false;
    status_t st = e ? OK : fs_stat(abs, NULL, &dir, NULL);
    if (!e && st != OK)
        e = lx_err(st);
    else if (!e && !dir)
        e = -ENOTDIR;
    if (!e)
        strcpy(t->cwd, abs);
    free(abs);
    return e;
}

static long act_fchdir(struct spawnfd *t, const struct jam_spawn_op *op)
{
    struct spawnfd_slot *s = find(t, op->fd);
    if (!s)
        return -EBADF;
    struct stat st = { 0 };
    const struct fd_ops *ops = s->desc->ops;
    const char *path = ops->path ? ops->path(s->desc) : NULL;
    if (!path || !ops->stat || ops->stat(s->desc, &st) || !S_ISDIR(st.st_mode))
        return -ENOTDIR;
    if (strnlen(path, PATH_MAX) == PATH_MAX)
        return -ENAMETOOLONG;
    strcpy(t->cwd, path);
    return 0;
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
        default:               e = -EINVAL; break;
        }
        if (e)
            return e;
    }
    return 0;
}

/* Slot s given to the child as the next descriptor: 0, -ENOTSUP (its
 * kind is left out), or -errno. */
static long export_one(struct spawnfd *t, const struct spawnfd_slot *s)
{
    unsigned i = t->nexported;
    struct fd_given *g = &t->given[i];
    memset(g, 0, sizeof(*g));
    long e = s->desc->ops->give ? s->desc->ops->give(s->desc, g) : -ENOTSUP;
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
    t->nexported++;
    return 0;
}

long spawnfd_export(struct spawnfd *t, struct posixfds_out *ho)
{
    unsigned n = t->nslots ? t->nslots : 1;
    t->desc = calloc(n, sizeof(*t->desc));
    t->given = calloc(n, sizeof(*t->given));
    long e = t->desc && t->given ? 0 : -ENOMEM;
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
    }
    return e;
}

void spawnfd_end(struct spawnfd *t, struct posixfds_out *ho)
{
    ho->fds = NULL;
    ho->nfds = 0;
    if (!t)
        return;
    for (unsigned i = 0; t->given && i < t->nexported; i++)
        free(t->given[i].data);   /* the handles went with the hand-over */
    free(t->desc);
    free(t->given);
    for (unsigned i = 0; i < t->nslots; i++)
        fd_put(t->slot[i].desc);
    free(t);
}
