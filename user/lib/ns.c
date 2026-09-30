/* The namespace: this program's mount points and the `fs` channel behind
 * each (<os.h> "files" has the model and SR_NS's encoding).
 *
 * The table starts empty. Mounts come from whoever started us, as ns_msg
 * messages on the SR_NS channel (taken here before every lookup, so a
 * mount sent to a running program is there for its next call), and from
 * ns_mount (init mounts its own). Each mount remembers which of the two it
 * came from: an NS_SET replaces only the starter's.
 *
 * ns_update is the starter's side: it takes back what the program hasn't
 * read before it sends the whole namespace, so a program that never looks
 * never holds more than one message. A lookup hands out a duplicate of the
 * mount's channel, so a call in progress keeps a channel of its own while
 * another thread replaces the mount.
 *
 * One lock guards the table; nothing blocks under it except the one
 * bounded wait for the starter's first message. */
#include <os.h>
#include "ns.h"

#define TAKE_MAX 64   /* messages taken in one go: a flooding starter can't hold a lookup */
#define BACK_MAX 1024 /* messages ns_update takes back: a channel end holds no more */

struct mount {
    char     path[NS_NAME_MAX];   /* "/data" */
    handle_t fs;                  /* its `fs` channel */
    bool     given;               /* from the starter (SR_NS), not ns_mount */
};

static struct mount mounts[NS_MAX_MOUNTS];   /* the first nmounts, in mount order; lock */
static unsigned nmounts;                     /* lock */
static bool started;        /* the starter's first message was waited for; lock */
static bool starter_gone;   /* SR_NS's other end is closed and its queue is empty; lock */
static bool lock;           /* a spinlock: test-and-set, released with a clear */

const char *const NS_ALL[] = { "*", NULL };

static void ns_lock(void)
{
    while (__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE))
        jam_nanosleep(now() + 20 * NS_PER_US);
}

static void ns_unlock(void)
{
    __atomic_clear(&lock, __ATOMIC_RELEASE);
}

/* "/name": one name of 1 to NS_NAME_MAX - 2 bytes, not "." or "..". path
 * need not be NUL-terminated past NS_NAME_MAX bytes. */
static bool valid_point(const char *path)
{
    size_t n = strnlen(path, NS_NAME_MAX);
    if (n < 2 || n == NS_NAME_MAX || path[0] != '/')
        return false;
    return !strchr(path + 1, '/') && strcmp(path + 1, ".") && strcmp(path + 1, "..");
}

/* fs (consumed) at path, which is valid; given: it came from the starter. */
static status_t mount_locked(const char *path, handle_t fs, bool given)
{
    for (unsigned i = 0; i < nmounts; i++)
        if (!strcmp(mounts[i].path, path)) {
            jam_handle_close(mounts[i].fs);
            mounts[i].fs = fs;
            mounts[i].given = given;
            return OK;
        }
    if (nmounts == NS_MAX_MOUNTS) {
        jam_handle_close(fs);
        return ERR_NO_RESOURCES;
    }
    memcpy(mounts[nmounts].path, path, strlen(path) + 1);
    mounts[nmounts].fs = fs;
    mounts[nmounts++].given = given;
    return OK;
}

static status_t unmount_locked(const char *path)
{
    for (unsigned i = 0; i < nmounts; i++) {
        if (strcmp(mounts[i].path, path))
            continue;
        jam_handle_close(mounts[i].fs);
        for (unsigned k = i + 1; k < nmounts; k++)
            mounts[k - 1] = mounts[k];
        nmounts--;
        return OK;
    }
    return ERR_NOT_FOUND;
}

/* path is one of m's (checked, n bytes long) paths. */
static bool in_msg(const struct ns_msg *m, const char *path)
{
    for (uint32_t i = 0; i < m->count; i++)
        if (!strncmp(m->path[i], path, NS_NAME_MAX))
            return true;
    return false;
}

/* NS_SET: the starter's mounts that m doesn't list go (its handles then
 * mount as an NS_MOUNT's). */
static void set_locked(const struct ns_msg *m)
{
    for (unsigned i = 0; i < nmounts;) {
        if (mounts[i].given && !in_msg(m, mounts[i].path))
            (void)unmount_locked(mounts[i].path);   /* the next one moves to i */
        else
            i++;
    }
}

/* One message from the starter: n bytes, nh handles (all consumed). */
static void apply_locked(const struct ns_msg *m, uint32_t n, const handle_t *hs, uint32_t nh)
{
    bool mounting = m->kind == NS_MOUNT || m->kind == NS_SET;
    bool ok = n >= NS_MSG_SIZE(0) && m->count <= NS_MAX_MOUNTS && n == NS_MSG_SIZE(m->count) &&
              !m->reserved && ((mounting && nh == m->count) || (m->kind == NS_UNMOUNT && !nh));
    if (!ok) {
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        return;
    }
    if (m->kind == NS_SET)
        set_locked(m);
    for (uint32_t i = 0; i < m->count; i++) {
        bool valid = valid_point(m->path[i]);
        if (mounting && !valid)
            jam_handle_close(hs[i]);
        else if (mounting)
            (void)mount_locked(m->path[i], hs[i], true);   /* a full table closes it */
        else if (valid)
            (void)unmount_locked(m->path[i]);              /* not mounted: nothing to do */
    }
}

/* Take a message no ns_msg can be (n bytes, nh handles) off ch and close
 * its handles: left queued it would block every later one. false if it
 * can't be taken (out of memory). */
static bool drop(handle_t ch, uint32_t n, uint32_t nh)
{
    uint8_t *bytes = malloc(n ? n : 1);
    handle_t *hs = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = ch, .bytes_cap = n, .bytes = (uint64_t)(uintptr_t)bytes,
        .actual_bytes = (uint64_t)(uintptr_t)&n2, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = nh, .actual_handles = (uint64_t)(uintptr_t)&nh2,
    };
    bool ok = bytes && hs && jam_channel_read(&a) == OK;
    for (uint32_t i = 0; ok && i < nh2; i++)
        jam_handle_close(hs[i]);
    free(hs);
    free(bytes);
    return ok;
}

/* What the starter has sent so far (TAKE_MAX messages at most) into the
 * table; how many messages were taken. */
static unsigned take_some_locked(handle_t ch)
{
    unsigned taken = 0;
    for (; taken < TAKE_MAX; taken++) {
        struct ns_msg m;
        handle_t hs[NS_MAX_MOUNTS];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = ch, .bytes_cap = sizeof(m), .bytes = (uint64_t)(uintptr_t)&m,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = NS_MAX_MOUNTS, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL && drop(ch, n, nh))
            continue;
        if (st == ERR_PEER_CLOSED)
            starter_gone = true;
        if (st != OK)
            break;
        apply_locked(&m, n, hs, nh);
    }
    return taken;
}

/* Everything the starter has sent so far, into the table. The first time,
 * wait until its first message is taken: it is written right after our
 * start, and a starter's ns_update may take it back just as we wake, to
 * write the next one. Timed out or failed: we go on with what there is
 * (no mounts). */
static void take_locked(void)
{
    handle_t ch = startup_handle(SR_NS);
    if (!ch || starter_gone)
        return;
    if (started) {
        (void)take_some_locked(ch);
        return;
    }
    uint64_t deadline = now() + NS_FIRST_WAIT;
    signals_t seen;
    while (!take_some_locked(ch) && !starter_gone &&
           jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen) == OK)
        ;
    started = true;
}

/* ---- paths ------------------------------------------------------------------------ */

/* The next name of p ("a" of "//a/b"): its length, *name at it; 0 at the
 * end of the path. */
static size_t next_name(const char *p, const char **name)
{
    while (*p == '/')
        p++;
    size_t l = 0;
    while (p[l] && p[l] != '/')
        l++;
    *name = p;
    return l;
}

static bool is_dot(const char *name, size_t l)
{
    return l == 1 && name[0] == '.';
}

static bool is_dotdot(const char *name, size_t l)
{
    return l == 2 && name[0] == '.' && name[1] == '.';
}

/* The names of in (NUL-terminated) joined by '/' into out, "." dropped and
 * ".." taking the name before it (none: it stays at the root). The result
 * is never longer than in. */
static void clean(const char *in, char *out)
{
    size_t n = 0, l;
    const char *name;
    for (const char *p = in; (l = next_name(p, &name)) != 0; p = name + l) {
        if (is_dotdot(name, l)) {
            while (n && out[n - 1] != '/')
                n--;
            if (n)
                n--;   /* the slash before it */
        } else if (!is_dot(name, l)) {
            if (n)
                out[n++] = '/';
            memcpy(out + n, name, l);
            n += l;
        }
    }
    out[n] = '\0';
}

status_t fs_path_clean(const uint8_t path[FS_PATH_MAX], char out[FS_PATH_MAX])
{
    if (strnlen((const char *)path, FS_PATH_MAX) == FS_PATH_MAX)
        return ERR_INVALID_ARGS;
    clean((const char *)path, out);
    return OK;
}

status_t ns_resolve(const char *path, uint8_t rel[FS_PATH_MAX], handle_t *fs, unsigned *mount)
{
    if (!path || path[0] != '/' || strnlen(path, FS_PATH_MAX) == FS_PATH_MAX)
        return ERR_INVALID_ARGS;
    /* The mount: the first real name. "." and ".." before it stay at "/". */
    const char *name, *p = path;
    size_t len;
    while ((len = next_name(p, &name)) != 0 && (is_dot(name, len) || is_dotdot(name, len)))
        p = name + len;
    memset(rel, 0, FS_PATH_MAX);
    rel[0] = '/';
    if (!len) {
        *fs = HANDLE_INVALID;
        *mount = 0;
        return OK;
    }
    clean(name + len, (char *)rel + 1);   /* shorter than path: it fits */

    status_t st = ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    for (unsigned i = 0; i < nmounts; i++) {
        if (strlen(mounts[i].path + 1) != len || memcmp(mounts[i].path + 1, name, len))
            continue;
        st = jam_handle_duplicate(mounts[i].fs, RIGHT_SAME, fs);
        *mount = i;
        break;
    }
    ns_unlock();
    return st;
}

/* ---- the table, from outside ---------------------------------------------------- */

status_t ns_mount(const char *path, handle_t fs)
{
    if (!path || !valid_point(path)) {
        jam_handle_close(fs);
        return ERR_INVALID_ARGS;
    }
    ns_lock();
    take_locked();   /* the starter's mounts first: ours replaces one of the same name */
    status_t st = mount_locked(path, fs, false);
    ns_unlock();
    return st;
}

status_t ns_unmount(const char *path)
{
    if (!path || !valid_point(path))
        return ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    status_t st = unmount_locked(path);
    ns_unlock();
    return st;
}

bool ns_mount_at(unsigned i, char out[NS_NAME_MAX])
{
    ns_lock();
    take_locked();
    bool have = i < nmounts;
    if (have)
        memcpy(out, mounts[i].path, NS_NAME_MAX);
    ns_unlock();
    return have;
}

status_t ns_channel(const char *path, handle_t *out)
{
    status_t st = ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    for (unsigned i = 0; path && i < nmounts; i++)
        if (!strcmp(mounts[i].path, path)) {
            st = jam_handle_duplicate(mounts[i].fs, RIGHT_SAME, out);
            break;
        }
    ns_unlock();
    return st;
}

static bool listed(const char *const *paths, const char *path)
{
    if (paths[0] && !strcmp(paths[0], NS_ALL[0]))
        return true;
    for (unsigned i = 0; paths[i]; i++)
        if (!strcmp(paths[i], path))
            return true;
    return false;
}

/* A `kind` message (NS_MOUNT or NS_SET) of our mounts listed in paths,
 * each with a duplicate of its channel, written on to. */
static status_t send_ours(handle_t to, uint32_t kind, const char *const *paths)
{
    struct ns_msg m;
    handle_t hs[NS_MAX_MOUNTS];
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    status_t st = OK;
    ns_lock();
    take_locked();
    for (unsigned i = 0; st == OK && i < nmounts; i++) {
        if (!listed(paths, mounts[i].path))
            continue;
        st = jam_handle_duplicate(mounts[i].fs, RIGHT_SAME, &hs[m.count]);
        if (st == OK)
            memcpy(m.path[m.count++], mounts[i].path, NS_NAME_MAX);
    }
    ns_unlock();
    if (st == OK)
        st = jam_channel_write(to, &m, NS_MSG_SIZE(m.count), hs, m.count);
    for (uint32_t i = 0; st != OK && i < m.count; i++)
        jam_handle_close(hs[i]);   /* not sent: still ours */
    return st;
}

status_t ns_send(handle_t to, const char *const *paths)
{
    return send_ours(to, NS_MOUNT, paths);
}

/* Read every message still waiting on back (the program's end) and close
 * the handles they carry: they are ours, and out of date. */
static void take_back(handle_t back)
{
    for (unsigned i = 0; i < BACK_MAX; i++) {
        struct ns_msg m;
        handle_t hs[NS_MAX_MOUNTS];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = back, .bytes_cap = sizeof(m), .bytes = (uint64_t)(uintptr_t)&m,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = NS_MAX_MOUNTS, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL && drop(back, n, nh))
            continue;   /* not one of ours: gone anyway */
        if (st != OK)
            return;     /* empty (or the program's end is gone) */
        for (uint32_t k = 0; k < nh; k++)
            jam_handle_close(hs[k]);
    }
}

status_t ns_update(handle_t to, handle_t back, const char *const *paths)
{
    if (back)
        take_back(back);
    return send_ours(to, NS_SET, paths);
}

status_t ns_send_one(handle_t to, const char *path, handle_t fs)
{
    struct ns_msg m;
    memset(&m, 0, sizeof(m));
    m.kind = fs ? NS_MOUNT : NS_UNMOUNT;
    m.count = 1;
    status_t st = path && valid_point(path) ? OK : ERR_INVALID_ARGS;
    if (st == OK) {
        memcpy(m.path[0], path, strlen(path) + 1);
        st = jam_channel_write(to, &m, NS_MSG_SIZE(1), &fs, fs ? 1 : 0);
    }
    if (st != OK && fs)
        jam_handle_close(fs);
    return st;
}
