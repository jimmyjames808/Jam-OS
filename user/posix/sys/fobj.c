/* A file's one open in this process (struct fobj, files.h), which every
 * description of the file here shares.
 *
 * fat lets a file have one open that may write, and no other open at all
 * beside it but ones that read (user/services/fat/fileops.c, "The
 * lock"): a second open for writing is ERR_BUSY. POSIX lets a process
 * open a file as often as it likes, so the layer opens each file once:
 * open finds the file's object by its mount and fs.stat's id and shares
 * it, opens it again for writing when a description first needs to
 * write (closing the read-only open first, the only one fat would see),
 * and the descriptions keep only their own offset and status flags.
 *
 * Since this process then holds the file's only writable open, the
 * file's size is known here once it writes (`size`), so O_APPEND is the
 * layer's: a write at that size, under the object's lock, which every
 * description's transfers take. Another process can still read it (fat
 * shares its reads) but can't write it: that open is ERR_BUSY, EBUSY.
 *
 * A child given a writable file (fd_give: posix_spawn, execve, a shell's
 * `cmd > f` starting children) needs its own open, so we lend it ours:
 * the open is closed (lent) before the child's is made, and made again at
 * our next use of the file (fobj_back_locked), once the child has closed
 * its own. The service may see a close a moment after the call that
 * follows it (a child's end reaches it after the parent's wait does), so
 * an open made then is tried again on ERR_BUSY for up to SETTLE_NS. While
 * the child holds the file, our own use of it is EBUSY: a parent and a
 * child writing one file at once needs the file protocol's shared cursor
 * (M13 Q5's (B)), and so does a shared offset (each moves its own here).
 *
 * A mount whose files may have many writers (fs.statfs's
 * FS_FEAT_MULTI_WRITER: extfs) needs none of that: ERR_BUSY never comes,
 * a child given a writable file opens its own beside ours (nothing lent),
 * the file's size is the service's (another process may write it), and
 * O_APPEND is the service's FS_APPEND, exact across processes: an
 * O_APPEND description has an object of its own opened so (`append`),
 * shared with the other O_APPEND descriptions of the file here. Objects
 * are still one per file here (and per FS_APPEND): extfs serves a few
 * dozen opens in all.
 *
 * A file whose mount we don't know (one another process sent over a
 * socket: fdpass.h, its path not believed) is treated the same way, its
 * size the service's, and the service is asked nothing when it arrives:
 * its channel is the sender's to give, and one that never answers would
 * hold the recvmsg that takes it (FILE_TIMEOUT a descriptor), where Linux
 * never waits. A call on the descriptor that needs the size asks then, as
 * any call on a file waits for its service.
 *
 * Lock order: a description's lock, then its object's lock, then
 * objs_lock (never held across a call to the service). */
#include <stdlib.h>
#include <errno.h>
#include <fs_idl.h>
#include <string.h>
#include "files.h"

#define BUF_MAX   (1u << 20)          /* a bigger transfer buffer is a broken service (libos's) */
#define SETTLE_NS NS_PER_S            /* how long an open waits out ERR_BUSY from a close */
#define SETTLE_NAP (10 * NS_PER_MS)   /* ... between tries */

static struct fobj *objs;    /* every object; objs_lock */
static lock_t       objs_lock;

/* The open's channel and buffer (consumed) as a jfile, mapped, and the
 * file's size and id asked of its service (unless !ask: 0 and 0, the
 * service asked nothing). */
static status_t jf_map(handle_t ch, handle_t buf, uint32_t ff, bool ask, struct jfile *jf,
                       uint64_t *size, uint64_t *id)
{
    uint64_t vsz = 0, addr = 0;
    *jf = (struct jfile){ .ch = ch, .buf_vmo = buf, .flags = ff & (FS_READ | FS_WRITE) };
    *size = *id = 0;
    status_t st = jam_vmo_get_size(buf, &vsz);
    if (st == OK && (!vsz || vsz > BUF_MAX || (vsz & 4095)))
        st = ERR_IO;
    if (st == OK)
        st = jam_vmar_map(lx_self_vmar(), buf, 0, vsz,   /* a connection's: never forked */
                          VMAR_READ | (ff & FS_WRITE ? VMAR_WRITE : 0) | VMAR_CLONE_NONE, &addr);
    if (st == OK) {
        jf->buf = (uint8_t *)(uintptr_t)addr;
        jf->buf_size = (uint32_t)vsz;
        if (ask)
            st = file_stat_within(ch, FILE_TIMEOUT, size, NULL, NULL, id);
    }
    if (st != OK)
        file_close(jf);   /* unmaps what was mapped, closes both */
    else
        jf->size = *size;
    return st;
}

/* The object for mount m and id (opened FS_APPEND or not), or NULL.
 * objs_lock. */
static struct fobj *find_locked(const char *m, uint64_t id, bool append)
{
    for (struct fobj *o = objs; o; o = o->next)
        if (o->id == id && o->append == append && !strcmp(o->mount, m) && m[0])
            return o;
    return NULL;
}

/* fresh (one reference) entered, or dropped for the file's object if
 * there is one already that may do as much: one that can write when
 * fresh can, and read when fresh can (an open to write only is all a
 * file of mode 0200 allows: file.c open_file). */
static void settle(struct fobj *fresh, struct fobj **out)
{
    lock_take(&objs_lock);
    struct fobj *o = find_locked(fresh->mount, fresh->id, fresh->append);
    uint32_t has = o ? (o->jf.flags | (o->lent ? o->lent_flags : 0)) & (FS_READ | FS_WRITE) : 0;
    uint32_t wants = fresh->jf.flags & (FS_READ | FS_WRITE);
    bool share = o && (has & wants) == wants;
    if (share) {
        o->refs++;
    } else {
        fresh->next = objs;
        objs = fresh;
    }
    lock_give(&objs_lock);
    if (share) {
        file_close(&fresh->jf);
        free(fresh);
        fresh = o;
    }
    *out = fresh;
}

/* A new object around an open (consumed) with ff of a file on mount m
 * ("": not known, the service asked nothing: the file's header), whose
 * FS_FEAT_* are features. */
static long fresh_obj(handle_t ch, handle_t buf, uint32_t ff, const char *m, uint32_t features,
                      struct fobj **out)
{
    struct fobj *o = calloc(1, sizeof(*o));
    if (!o) {
        jam_handle_close(ch);
        jam_handle_close(buf);
        return -ENOMEM;
    }
    bool known = m[0] != '\0';
    status_t st = jf_map(ch, buf, ff, known, &o->jf, &o->size, &o->id);
    if (st != OK) {
        free(o);
        return lx_err(st);
    }
    o->refs = 1;
    o->multi = !known || (features & FS_FEAT_MULTI_WRITER) != 0;
    o->append = known && o->multi && (ff & FS_APPEND);
    o->attrs = (features & FS_FEAT_MODES) != 0;
    memcpy(o->mount, m, strlen(m) + 1);
    settle(o, out);
    return 0;
}

/* fs.open of p with ff and mode (a new file's bits); *ch and *buf on
 * success. */
static status_t open_raw(const struct path *p, uint32_t ff, uint32_t mode, handle_t *ch,
                         handle_t *buf)
{
    handle_t fs;
    uint64_t size;
    if (path_fs(p, &fs))
        return ERR_NOT_FOUND;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    status_t st = fs_open_within(fs, FILE_TIMEOUT, rel, len, ff, mode, ch, buf, &size);
    jam_handle_close(fs);
    return st;
}

status_t fobj_open_settled(const struct path *p, uint32_t ff, handle_t *ch, handle_t *buf)
{
    uint64_t end = now() + SETTLE_NS;
    for (;;) {
        status_t st = open_raw(p, ff, FS_MODE_FILE_DEFAULT, ch, buf);
        if (st != ERR_BUSY || !(ff & FS_WRITE) || now() >= end)
            return st;
        (void)jam_sleep_until(now() + SETTLE_NAP);   /* a signal's interrupt: one try sooner */
    }
}

static void mount_of(const struct path *p, char m[NS_NAME_MAX])
{
    size_t n = p->mount_len < NS_NAME_MAX ? p->mount_len : 0;
    memcpy(m, p->abs, n);
    m[n] = '\0';
}

/* Our object for p, found by its id, with a reference; NULL if none. */
static struct fobj *ours(const struct path *p)
{
    struct path_info i;
    char m[NS_NAME_MAX];
    if (path_info(p, &i))
        return NULL;
    mount_of(p, m);
    lock_take(&objs_lock);
    struct fobj *o = find_locked(m, i.id, false);
    if (o)
        o->refs++;
    lock_give(&objs_lock);
    return o;
}

/* o, open only to read, opened again to read and write (ff's other
 * flags: FS_TRUNCATE); on a refusal it is opened to read again. o->lock. */
static status_t upgrade_locked(struct fobj *o, const struct path *p, uint32_t ff)
{
    handle_t ch, buf;
    uint64_t size, id;
    file_close(&o->jf);   /* ours was the open in fat's way */
    status_t st = fobj_open_settled(p, FS_READ | FS_WRITE | (ff & FS_TRUNCATE), &ch, &buf);
    if (st == OK)
        st = jf_map(ch, buf, FS_READ | FS_WRITE, true, &o->jf, &size, &id);
    if (st == OK) {
        o->size = size;
        return OK;
    }
    if (open_raw(p, FS_READ, FS_MODE_FILE_DEFAULT, &ch, &buf) == OK)   /* back as it was, if it can be */
        (void)jf_map(ch, buf, FS_READ, true, &o->jf, &size, &id);
    return st;
}

/* fs.open said ERR_BUSY to a write: is the open in the way ours? */
static long busy(const struct path *p, uint32_t ff, struct fobj **out)
{
    struct fobj *o = ours(p);
    if (!o)
        return -EBUSY;   /* another process writes it, or reads it while we want to write */
    lock_take(&o->lock);
    status_t st = fobj_back_locked(o, p->abs);   /* lent to a child: ours again first */
    if (st == OK && !(o->jf.flags & FS_WRITE))
        st = upgrade_locked(o, p, ff);
    else if (st == OK && (ff & FS_TRUNCATE))
        st = file_truncate_within(o->jf.ch, FILE_TIMEOUT, 0);
    if (st == OK && (ff & FS_TRUNCATE))
        o->size = 0;
    lock_give(&o->lock);
    if (st != OK) {
        fobj_put(o);
        return path_err(p, st);
    }
    *out = o;
    return 0;
}

long fobj_open(const struct path *p, uint32_t ff, uint32_t mode, struct fobj **out)
{
    handle_t ch, buf;
    uint32_t features = path_features(p);
    if (!(features & FS_FEAT_MULTI_WRITER))
        ff &= ~(uint32_t)FS_APPEND;   /* one writer: the layer appends (above) */
    if (!p->links)
        ff &= ~(uint32_t)FS_NOFOLLOW;
    status_t st = open_raw(p, ff, mode, &ch, &buf);
    if (st == ERR_BUSY && (ff & FS_WRITE))
        return busy(p, ff, out);
    if (st != OK)
        return (ff & (FS_WRITE | FS_APPEND | FS_CREATE | FS_TRUNCATE)) ? path_err(p, st)
                                                                       : path_err_read(p, st);
    char m[NS_NAME_MAX];
    mount_of(p, m);
    return fresh_obj(ch, buf, ff, m, features, out);
}

long fobj_adopt(handle_t ch, handle_t buf, uint32_t ff, const char *path, struct fobj **out)
{
    struct path p;
    char m[NS_NAME_MAX] = "";
    uint32_t features = 0;
    if (path && !path_set(path, &p) && p.where == PATH_MOUNT) {
        mount_of(&p, m);
        features = path_features(&p);
    }
    return fresh_obj(ch, buf, ff, m, features, out);
}

void fobj_lend_locked(struct fobj *o)
{
    if (o->lent || !o->jf.ch || !(o->jf.flags & FS_WRITE))
        return;
    o->lent_flags = o->jf.flags;
    o->lent_end = o->size;
    file_close(&o->jf);
    o->lent = true;
}

status_t fobj_back_locked(struct fobj *o, const char *path)
{
    if (!o->lent)
        return OK;
    struct path p;
    handle_t ch, buf;
    uint64_t size, id;
    if (!path || path_set(path, &p) || p.where != PATH_MOUNT)
        return ERR_IO;   /* never: only a file with its path is given, so lent */
    status_t st = fobj_open_settled(&p, o->lent_flags, &ch, &buf);
    if (st == OK)
        st = jf_map(ch, buf, o->lent_flags, true, &o->jf, &size, &id);
    if (st != OK)
        return st;
    o->size = size;   /* what the child wrote counts: an append goes after it */
    o->lent = false;
    return OK;
}

void fobj_fork(struct fd_fork *f, struct fobj *o)
{
    if (fd_fork_first(f, &o->fork_gen)) {
        o->lock = (lock_t){ 0 };
        o->refs = 0;
        if (o->jf.ch) {   /* lent as to a spawned child, so opened again at the next use */
            o->lent = true;
            o->lent_flags = o->jf.flags | (o->append ? FS_APPEND : 0);
            o->lent_end = o->size;
        }
        o->jf = (struct jfile){ 0 };   /* its buffer isn't mapped here: VMAR_CLONE_NONE */
    }
    o->refs++;
}

void fobj_forked(struct lx_fork_io *io)
{
    objs_lock = (lock_t){ 0 };
    uint32_t gen = lx_fork_gen(io);
    for (struct fobj **pp = &objs; *pp;)
        if ((*pp)->fork_gen != gen)
            *pp = (*pp)->next;   /* a call of the parent's held it: left in the heap */
        else
            pp = &(*pp)->next;
}

void fobj_put(struct fobj *o)
{
    lock_take(&objs_lock);
    bool last = --o->refs == 0;
    if (last) {
        struct fobj **pp = &objs;
        while (*pp && *pp != o)
            pp = &(*pp)->next;
        if (*pp)
            *pp = o->next;
    }
    lock_give(&objs_lock);
    if (!last)
        return;
    if (o->jf.ch)
        file_close(&o->jf);
    free(o);
}
