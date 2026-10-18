/* The namespace: this program's mount points and services, and the channel
 * behind each (<os.h> "files", "services" and "grants" have the model and
 * SR_NS's encoding).
 *
 * The table starts empty. Entries come from whoever started us, as the
 * IDL events of nschan (abi/idl/nschan.idl: mount, unmount, set) on the
 * SR_NS channel (taken here before every lookup, so a mount sent to a
 * running program is there for its next call), and from
 * ns_mount and ns_svc_set (init publishes its own). Each entry remembers
 * which of the two it came from: a `set` replaces only the starter's.
 * A path "/name" is a mount (an `fs` channel), "/svc/name" a service.
 *
 * ns_update is the starter's side: it takes back what the program hasn't
 * read before it sends the whole namespace, so a program that never looks
 * never holds more than one message. A lookup hands out a duplicate of the
 * entry's channel, so a call in progress keeps a channel of its own while
 * another thread replaces the entry. Sending a view (a grant with :r or
 * :w) calls the mount's service (fs.view) with the table unlocked.
 *
 * One lock guards the table; nothing blocks under it except the one
 * bounded wait for the starter's first message. svc_get's cache has its
 * own lock and never calls out under it.
 *
 * A forked child (fork.c) starts with both empty and their locks free
 * (ns_forked: its copy's values were its parent's): its parent sends it
 * its whole namespace on a new SR_NS, as a starter does. */
#include <fs_idl.h>
#include <fsview.h>
#include <idl/nschan.h>
#include <idl/svc.h>
#include <os.h>
#include "fork.h"
#include "ns.h"

#define TAKE_MAX 64   /* messages taken in one go: a flooding starter can't hold a lookup */
#define BACK_MAX 1024 /* messages ns_update takes back: a channel end holds no more */
#define SVC_DIR  "/svc/"
#define SVC_WAIT (5 * NS_PER_S)   /* svc.connect */

/* os.h's limits are nschan.idl's. */
_Static_assert(NS_NAME_MAX == NSCHAN_NAME_MAX && NS_MAX_MOUNTS == NSCHAN_MAX_MOUNTS &&
               NS_MAX_SVCS == NSCHAN_MAX_SVCS && NS_MAX_ENTRIES == NSCHAN_MAX_ENTRIES,
               "os.h's namespace limits are nschan.idl's");

enum kind { BAD, MOUNT, SERVICE };

struct entry {
    char     path[NS_NAME_MAX];   /* "/data", "/svc/music" */
    handle_t h;                   /* its `fs` channel, or the service's */
    bool     given;               /* from the starter (SR_NS), not ns_mount / ns_svc_set */
    bool     connect;             /* a service handing out a channel per opener */
};

static struct entry ents[NS_MAX_ENTRIES];   /* the first nents, in the order given; lock */
static unsigned nents;                      /* lock */
static bool started;        /* the starter's first message was waited for; lock */
static bool starter_gone;   /* SR_NS's other end is closed and its queue is empty; lock */
static lock_t lock;         /* lock_take's */

const char *const NS_ALL[] = { "*", NULL };

static void ns_lock(void)
{
    lock_take(&lock);
}

static void ns_unlock(void)
{
    lock_give(&lock);
}

/* A service's name: 1 to SVC_NAME_MAX bytes of [a-z0-9-], NUL-terminated
 * within SVC_NAME_MAX + 1 bytes. */
static bool valid_name(const char *name)
{
    size_t n = strnlen(name, SVC_NAME_MAX + 1);
    if (n < 1 || n > SVC_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

/* What path names: a mount "/name" (one name of 1 to NS_NAME_MAX - 2
 * bytes, not ".", ".." or "svc"), a service "/svc/<name>", or neither.
 * path need not be NUL-terminated past NS_NAME_MAX bytes. */
static enum kind kind_of(const char *path)
{
    size_t n = strnlen(path, NS_NAME_MAX);
    if (n < 2 || n == NS_NAME_MAX || path[0] != '/')
        return BAD;
    if (!strncmp(path, SVC_DIR, sizeof(SVC_DIR) - 1))
        return valid_name(path + sizeof(SVC_DIR) - 1) ? SERVICE : BAD;
    if (strchr(path + 1, '/') || !strcmp(path + 1, ".") || !strcmp(path + 1, "..") ||
        !strcmp(path + 1, "svc"))
        return BAD;
    return MOUNT;
}

static unsigned count_locked(enum kind k)
{
    unsigned n = 0;
    for (unsigned i = 0; i < nents; i++)
        n += kind_of(ents[i].path) == k;
    return n;
}

/* h (consumed) at path, whose kind is k; given: it came from the starter. */
static status_t put_locked(const char *path, enum kind k, handle_t h, bool given, bool connect)
{
    for (unsigned i = 0; i < nents; i++)
        if (!strcmp(ents[i].path, path)) {
            jam_handle_close(ents[i].h);
            ents[i] = (struct entry){ .h = h, .given = given, .connect = connect };
            memcpy(ents[i].path, path, strlen(path) + 1);
            return OK;
        }
    if (nents == NS_MAX_ENTRIES || count_locked(k) == (k == MOUNT ? NS_MAX_MOUNTS : NS_MAX_SVCS)) {
        jam_handle_close(h);
        return ERR_NO_RESOURCES;
    }
    ents[nents] = (struct entry){ .h = h, .given = given, .connect = connect };
    memcpy(ents[nents++].path, path, strlen(path) + 1);
    return OK;
}

static status_t remove_locked(const char *path)
{
    for (unsigned i = 0; i < nents; i++) {
        if (strcmp(ents[i].path, path))
            continue;
        jam_handle_close(ents[i].h);
        for (unsigned k = i + 1; k < nents; k++)
            ents[k - 1] = ents[k];
        nents--;
        return OK;
    }
    return ERR_NOT_FOUND;
}

/* The entries of one nschan event, checked: count records of NS_NAME_MAX
 * bytes, and a connect byte (0 or 1) for each, NULL for an unmount. */
struct ev {
    const uint8_t *paths;
    const uint8_t *connect;
    uint32_t       count;
};

static const char *ev_path(const struct ev *e, uint32_t i)
{
    return (const char *)e->paths + (size_t)i * NS_NAME_MAX;
}

/* path is one of e's paths. */
static bool in_ev(const struct ev *e, const char *path)
{
    for (uint32_t i = 0; i < e->count; i++)
        if (!strncmp(ev_path(e, i), path, NS_NAME_MAX))
            return true;
    return false;
}

/* `set`: the starter's entries that e doesn't list go (its handles then
 * go in as a `mount`'s). */
static void set_locked(const struct ev *e)
{
    for (unsigned i = 0; i < nents;) {
        if (ents[i].given && !in_ev(e, ents[i].path))
            (void)remove_locked(ents[i].path);   /* the next one moves to i */
        else
            i++;
    }
}

enum { EV_MOUNT, EV_UNMOUNT, EV_SET };

/* One event from the starter (all its handles consumed). The generated
 * reader has checked the arrays against their maximum; here the lengths
 * must agree with each other and the handles, or nothing is applied. */
static void apply_locked(unsigned kind, const uint8_t *paths, uint32_t paths_len,
                         const uint8_t *connect, uint32_t connect_len, const handle_t *hs,
                         uint32_t nh)
{
    bool mounting = kind != EV_UNMOUNT;
    struct ev e = { paths, connect, paths_len / NS_NAME_MAX };
    bool ok = paths_len % NS_NAME_MAX == 0 && e.count <= NS_MAX_ENTRIES &&
              (mounting ? connect_len == e.count && nh == e.count : !nh);
    for (uint32_t i = 0; ok && mounting && i < e.count; i++)
        ok = connect[i] <= 1;
    if (!ok) {
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        return;
    }
    if (kind == EV_SET)
        set_locked(&e);
    for (uint32_t i = 0; i < e.count; i++) {
        const char *path = ev_path(&e, i);
        enum kind k = kind_of(path);
        bool conn = mounting && connect[i];
        bool valid = k != BAD && (k == SERVICE || !conn);
        if (mounting && !valid)
            jam_handle_close(hs[i]);
        else if (mounting)
            (void)put_locked(path, k, hs[i], true, conn);   /* a full table closes it */
        else if (valid)
            (void)remove_locked(path);                      /* not there: nothing to do */
    }
}

/* nschan's handlers; the table is locked by whoever reads (the events carry
 * no context). */
static void on_mount(void *ctx, const uint8_t *paths, uint32_t paths_len, const uint8_t *connect,
                     uint32_t connect_len, const handle_t *hs, uint32_t nh)
{
    (void)ctx;
    apply_locked(EV_MOUNT, paths, paths_len, connect, connect_len, hs, nh);
}

static void on_unmount(void *ctx, const uint8_t *paths, uint32_t paths_len)
{
    (void)ctx;
    apply_locked(EV_UNMOUNT, paths, paths_len, NULL, 0, NULL, 0);
}

static void on_set(void *ctx, const uint8_t *paths, uint32_t paths_len, const uint8_t *connect,
                   uint32_t connect_len, const handle_t *hs, uint32_t nh)
{
    (void)ctx;
    apply_locked(EV_SET, paths, paths_len, connect, connect_len, hs, nh);
}

/* What the starter has sent so far (TAKE_MAX messages at most) into the
 * table; how many messages were taken. The generated reader drops, with
 * their handles closed, the events that break the format and the ones too
 * big for any: they count as taken. */
static unsigned take_some_locked(handle_t ch)
{
    static const struct nschan_ops ops = { .mount = on_mount, .unmount = on_unmount,
                                           .set = on_set };
    unsigned taken = 0;
    for (; taken < TAKE_MAX; taken++) {
        status_t st = nschan_serve_one(ch, &ops, NULL);
        if (st == ERR_PEER_CLOSED)
            starter_gone = true;
        if (st != OK)
            break;
    }
    return taken;
}

/* Everything the starter has sent so far, into the table. The first time,
 * wait until its first message is taken: it is written right after our
 * start, and a starter's ns_update may take it back just as we wake, to
 * write the next one. Timed out or failed: we go on with what there is
 * (nothing). */
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
    while (!take_some_locked(ch) && !starter_gone) {
        status_t st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st != OK && st != ERR_INTERRUPTED)   /* a POSIX program's signal ends no wait here */
            break;
    }
    started = true;
}

/* ---- paths ------------------------------------------------------------------------ */

/* The next name of p ("a" of "//a/b"), which ends at `end` or its NUL:
 * its length, *name at it; 0 at the end of the path. */
static size_t next_name(const char *p, const char *end, const char **name)
{
    while (p < end && *p == '/')
        p++;
    size_t l = 0;
    while (p + l < end && p[l] && p[l] != '/')
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

/* The names of in (len bytes, or up to its NUL) joined by '/' into out,
 * "." dropped and ".." taking the name before it (none: it stays at the
 * root), NUL-terminated. The result is never longer than in. */
static void clean(const char *in, size_t len, char *out)
{
    size_t n = 0, l;
    const char *name;
    for (const char *p = in; (l = next_name(p, in + len, &name)) != 0; p = name + l) {
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

status_t fs_path_clean(const char *path, uint32_t len, char *out)
{
    if (strnlen(path, len) != len)
        return ERR_INVALID_ARGS;
    clean(path, len, out);
    return OK;
}

/* The entry whose path is "/" + name[0..len): its index, or -1. lock. */
static int find_locked(const char *prefix, const char *name, size_t len)
{
    size_t pl = strlen(prefix);
    for (unsigned i = 0; i < nents; i++)
        if (strlen(ents[i].path) == pl + len && !memcmp(ents[i].path, prefix, pl) &&
            !memcmp(ents[i].path + pl, name, len))
            return (int)i;
    return -1;
}

status_t ns_resolve(const char *path, uint8_t rel[FS_PATH_MAX], handle_t *fs, unsigned *mount)
{
    if (!path || path[0] != '/')
        return ERR_INVALID_ARGS;
    if (strnlen(path, FS_PATH_MAX) == FS_PATH_MAX)
        return ERR_NAME_TOO_LONG;
    /* The mount: the first real name. "." and ".." before it stay at "/". */
    const char *name, *p = path;
    size_t len;
    const char *end = path + strlen(path);
    while ((len = next_name(p, end, &name)) != 0 && (is_dot(name, len) || is_dotdot(name, len)))
        p = name + len;
    memset(rel, 0, FS_PATH_MAX);
    rel[0] = '/';
    *fs = HANDLE_INVALID;
    *mount = 0;
    if (!len)
        return OK;
    clean(name + len, (size_t)(end - name - len), (char *)rel + 1);   /* shorter: it fits */

    status_t st = ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    if (len == 3 && !memcmp(name, "svc", 3)) {
        /* /svc itself, or one service in it: no channel to call */
        bool any = count_locked(SERVICE) > 0;
        int i = rel[1] ? find_locked(SVC_DIR, (const char *)rel + 1, strlen((char *)rel + 1)) : -1;
        st = (rel[1] ? i >= 0 : any) ? OK : ERR_NOT_FOUND;
        *mount = NS_AT_SVC;
    } else {
        int i = find_locked("/", name, len);
        if (i >= 0 && kind_of(ents[i].path) == MOUNT) {
            st = jam_handle_duplicate(ents[i].h, RIGHT_SAME, fs);
            *mount = (unsigned)i;
        }
    }
    ns_unlock();
    return st;
}

/* ---- the table, from outside ---------------------------------------------------- */

status_t ns_mount(const char *path, handle_t fs)
{
    if (!path || kind_of(path) != MOUNT) {
        jam_handle_close(fs);
        return ERR_INVALID_ARGS;
    }
    ns_lock();
    take_locked();   /* the starter's mounts first: ours replaces one of the same name */
    status_t st = put_locked(path, MOUNT, fs, false, false);
    ns_unlock();
    return st;
}

status_t ns_unmount(const char *path)
{
    if (!path || kind_of(path) != MOUNT)
        return ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    status_t st = remove_locked(path);
    ns_unlock();
    return st;
}

/* The i-th entry of kind k: its path into out (NS_NAME_MAX bytes). */
static bool entry_at(enum kind k, unsigned i, char out[NS_NAME_MAX])
{
    bool have = false;
    ns_lock();
    take_locked();
    for (unsigned e = 0; e < nents && !have; e++)
        if (kind_of(ents[e].path) == k && i-- == 0) {
            memcpy(out, ents[e].path, NS_NAME_MAX);
            have = true;
        }
    ns_unlock();
    return have;
}

bool ns_mount_at(unsigned i, char out[NS_NAME_MAX])
{
    return entry_at(MOUNT, i, out);
}

bool ns_svc_at(unsigned i, char out[SVC_NAME_MAX + 1])
{
    char path[NS_NAME_MAX];
    if (!entry_at(SERVICE, i, path))
        return false;
    memcpy(out, path + sizeof(SVC_DIR) - 1, SVC_NAME_MAX + 1);
    return true;
}

status_t ns_channel(const char *path, handle_t *out)
{
    status_t st = ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    for (unsigned i = 0; path && i < nents; i++)
        if (!strcmp(ents[i].path, path) && kind_of(path) == MOUNT) {
            st = jam_handle_duplicate(ents[i].h, RIGHT_SAME, out);
            break;
        }
    ns_unlock();
    return st;
}

/* "/svc/" + name into path (NS_NAME_MAX bytes); false if not a name. */
static bool svc_path(const char *name, char path[NS_NAME_MAX])
{
    if (!name || !valid_name(name))
        return false;
    snprintf(path, NS_NAME_MAX, "%s%s", SVC_DIR, name);
    return true;
}

status_t ns_svc_set(const char *name, handle_t h, bool connect)
{
    char path[NS_NAME_MAX];
    if (!svc_path(name, path)) {
        jam_handle_close(h);
        return ERR_INVALID_ARGS;
    }
    ns_lock();
    take_locked();
    status_t st = put_locked(path, SERVICE, h, false, connect);
    ns_unlock();
    return st;
}

status_t ns_svc_remove(const char *name)
{
    char path[NS_NAME_MAX];
    if (!svc_path(name, path))
        return ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    status_t st = remove_locked(path);
    ns_unlock();
    return st;
}

status_t svc_open(const char *name, handle_t *out)
{
    char path[NS_NAME_MAX];
    if (!svc_path(name, path))
        return ERR_INVALID_ARGS;
    handle_t h = HANDLE_INVALID;
    bool connect = false;
    status_t st = ERR_NOT_FOUND;
    ns_lock();
    take_locked();
    for (unsigned i = 0; i < nents && st == ERR_NOT_FOUND; i++)
        if (!strcmp(ents[i].path, path)) {
            st = jam_handle_duplicate(ents[i].h, RIGHT_SAME, &h);
            connect = ents[i].connect;
        }
    ns_unlock();
    if (st != OK || !connect) {
        if (st == OK)
            *out = h;
        return st;
    }
    st = svc_connect_until(h, now() + SVC_WAIT, out);   /* *out only on success */
    jam_handle_close(h);
    return st;
}

/* ---- svc_get's cache -------------------------------------------------------------- */

static struct {
    char     name[SVC_NAME_MAX + 1];   /* "" : a free slot */
    handle_t h;                        /* ours, handed out (never closed while open) */
} kept[NS_MAX_SVCS];
static lock_t kept_lock; /* lock_take's, like the table's; nothing is called under it */

static void kept_take(void)
{
    lock_take(&kept_lock);
}

static void kept_give(void)
{
    lock_give(&kept_lock);
}

void ns_forked(void)
{
    nents = 0;
    started = false;
    starter_gone = false;
    lock = (lock_t){ 0 };
    memset(kept, 0, sizeof(kept));
    kept_lock = (lock_t){ 0 };
}

/* The other end of h is gone. */
static bool dead(handle_t h)
{
    signals_t seen = 0;
    return jam_object_wait_one(h, SIG_PEER_CLOSED, 0, &seen) == OK && (seen & SIG_PEER_CLOSED);
}

/* kept's slot for name, or a free one; NS_MAX_SVCS if neither. kept_lock. */
static unsigned kept_slot(const char *name)
{
    unsigned free_slot = NS_MAX_SVCS;
    for (unsigned i = 0; i < NS_MAX_SVCS; i++) {
        if (!strcmp(kept[i].name, name))
            return i;
        if (!kept[i].name[0] && free_slot == NS_MAX_SVCS)
            free_slot = i;
    }
    return free_slot;
}

handle_t svc_get(const char *name)
{
    if (!name || !valid_name(name))
        return HANDLE_INVALID;
    kept_take();
    unsigned i = kept_slot(name);
    handle_t old = i < NS_MAX_SVCS ? kept[i].h : HANDLE_INVALID;
    kept_give();
    if (old && !dead(old))
        return old;
    /* None yet, or its service ended: open it again (no lock held). */
    handle_t fresh;
    if (svc_open(name, &fresh) != OK)
        return HANDLE_INVALID;
    kept_take();
    i = kept_slot(name);
    handle_t now_kept = i < NS_MAX_SVCS ? kept[i].h : HANDLE_INVALID;
    bool ours = i < NS_MAX_SVCS && now_kept == old;   /* nobody replaced it meanwhile */
    if (ours) {
        memcpy(kept[i].name, name, strlen(name) + 1);
        kept[i].h = fresh;
    }
    kept_give();
    if (!ours) {
        jam_handle_close(fresh);   /* another thread's is as good */
        return now_kept;
    }
    /* The dead one: calls on it fail either way (another thread holding
     * its value gets ERR_BAD_HANDLE instead of ERR_PEER_CLOSED). */
    if (old)
        jam_handle_close(old);
    return fresh;
}

/* ---- sending a namespace ------------------------------------------------------------ */

/* One grant (<os.h> "grants"), parsed. */
struct grant {
    bool        all;                /* "*": every entry */
    bool        mounts;             /* "*:r", "*:w": every mount */
    const char *point;              /* the path part ("/data", "/usb", "/svc/music"), not
                                     * NUL-terminated */
    size_t      len;                /* its length */
    bool        prefix;             /* ended in '*': every mount starting so */
    uint32_t    view;               /* FS_VIEW_* (":r", ":w"); 0: as we have it */
};

static bool parse_grant(const char *s, struct grant *g)
{
    *g = (struct grant){ .point = s };
    if (!strcmp(s, "*")) {
        g->all = true;
        return true;
    }
    size_t n = strnlen(s, NS_NAME_MAX + 4);
    if (n >= 3 && !strcmp(s + n - 3, ":rk")) {
        /* The SSH server's own (sshd's, its tinysshds' and their signer's):
         * the host key and the authorized keys, nothing else of etc. */
        g->view = FS_VIEW_READ_ONLY | FS_VIEW_HIDE_ETC;
        n -= 3;
    } else if (n >= 3 && !strcmp(s + n - 3, ":ra")) {
        /* The key check's (tinysshd-auth): the authorized keys alone. */
        g->view = FS_VIEW_READ_ONLY | FS_VIEW_HIDE_ETC | FS_VIEW_HIDE_KEYS;
        n -= 3;
    } else if (n >= 3 && !strcmp(s + n - 3, ":wp")) {
        /* bin/pkg's (`mount /data rw pkg`): no etc, and the packages' prefix
         * its to change. */
        g->view = FS_VIEW_GUARD_ETC | FS_VIEW_NO_ETC;
        n -= 3;
    } else if (n >= 2 && s[n - 2] == ':' && (s[n - 1] == 'r' || s[n - 1] == 'w')) {
        /* A program's view (every list's `mount`, <wants.h>): no etc in it
         * at all, whoever starts the program, and a writable one leaves the
         * packages' prefix alone (on every mount: the prefix may move to
         * another one, M13.6). */
        g->view = (s[n - 1] == 'r' ? FS_VIEW_READ_ONLY : FS_VIEW_GUARD_ETC | FS_VIEW_GUARD_PKG) |
                  FS_VIEW_NO_ETC;
        n -= 2;
    }
    if (n == 1 && s[0] == '*' && g->view) {
        g->mounts = true;
        return true;
    }
    if (n && s[n - 1] == '*') {
        g->prefix = true;
        n--;
    }
    g->len = n;
    return n >= 1 && n < NS_NAME_MAX && s[0] == '/';
}

/* Does grant g take entry e? A service only by its whole path (or "*"). */
static bool grant_takes(const struct grant *g, const struct entry *e, enum kind k)
{
    if (g->all)
        return true;
    if (g->mounts)
        return k == MOUNT;
    if (k == SERVICE)
        return !g->prefix && !g->view && strlen(e->path) == g->len &&
               !memcmp(e->path, g->point, g->len);
    if (g->prefix)
        return !strncmp(e->path, g->point, g->len);
    return strlen(e->path) == g->len && !memcmp(e->path, g->point, g->len);
}

/* Services that count what a program holds by the channel it calls on: a
 * grant that names one gives the program a channel of its own (our
 * entry's svc.connect, <os.h> "grants"), which hands out no more. The
 * broker's listeners (docs/CR-REVIEW-POSIX.md, Q2). */
static const char *const own_channel[] = { SVC_DIR SVC_UNIX };

/* Does grant g, which took entry e, give a channel of the program's own? */
static bool gets_own(const struct grant *g, const struct entry *e)
{
    for (unsigned i = 0; !g->all && e->connect && i < sizeof(own_channel) / sizeof(own_channel[0]);
         i++)
        if (!strcmp(e->path, own_channel[i]))
            return true;
    return false;
}

/* The entries grants take, duplicated into o (o->count of them). */
static status_t gather(const char *const *grants, struct ns_out *o)
{
    status_t st = OK;
    ns_lock();
    take_locked();
    for (unsigned i = 0; st == OK && i < nents; i++) {
        enum kind k = kind_of(ents[i].path);
        struct grant g;
        bool take = false;
        for (unsigned j = 0; grants[j] && !take; j++)
            take = parse_grant(grants[j], &g) && grant_takes(&g, &ents[i], k);
        if (!take)
            continue;
        uint32_t c = o->count;
        st = jam_handle_duplicate(ents[i].h, RIGHT_SAME, &o->hs[c]);
        if (st != OK)
            break;
        memcpy(o->paths[c], ents[i].path, NS_NAME_MAX);
        o->view[c] = k == MOUNT ? g.view : 0;
        o->connect[c] = ents[i].connect;
        o->own[c] = k == SERVICE && gets_own(&g, &ents[i]);
        o->count++;
    }
    ns_unlock();
    return st;
}

/* Each entry of o that wants a view, or a channel of its own, gets one in
 * place of its duplicate; one whose service won't make it is left out. */
static void make_views(struct ns_out *o)
{
    uint32_t keep = 0;
    for (uint32_t i = 0; i < o->count; i++) {
        handle_t h = o->hs[i];
        if (o->view[i] || o->own[i]) {
            handle_t v = HANDLE_INVALID;
            status_t st = o->view[i] ? fs_view_until(h, now() + NS_VIEW_WAIT, o->view[i], &v)
                                     : svc_connect_until(h, now() + SVC_WAIT, &v);
            jam_handle_close(h);
            if (st != OK)
                continue;   /* fail closed: no view, no mount; no channel, no service */
            h = v;
            o->connect[i] = o->own[i] ? 0 : o->connect[i];   /* it hands out no more */
        }
        memcpy(o->paths[keep], o->paths[i], NS_NAME_MAX);
        o->connect[keep] = o->connect[i];
        o->hs[keep++] = h;
    }
    o->count = keep;
}

status_t ns_prepare_in(const char *const *grants, struct ns_out *o)
{
    memset(o, 0, sizeof(*o));
    status_t st = grants ? gather(grants, o) : OK;
    if (st != OK) {
        ns_prepared_close(o);
        return st;
    }
    make_views(o);
    return OK;
}

status_t ns_prepare(const char *const *grants, struct ns_out **out)
{
    struct ns_out *o = jam_malloc(sizeof(*o));
    if (!o)
        return ERR_NO_MEMORY;
    status_t st = ns_prepare_in(grants, o);
    if (st != OK) {
        jam_free(o);
        return st;
    }
    *out = o;
    return OK;
}

void ns_prepared_close(struct ns_out *o)
{
    for (uint32_t i = 0; i < o->count; i++)
        jam_handle_close(o->hs[i]);
    o->count = 0;
}

void ns_prepared_drop(struct ns_out *o)
{
    if (o)
        ns_prepared_close(o);
    jam_free(o);
}

status_t ns_send_in(handle_t to, uint32_t kind, struct ns_out *o)
{
    uint32_t n = o->count;
    status_t st = kind == NS_SET ? nschan_set(to, o->paths[0], n * NS_NAME_MAX, o->connect, n,
                                              o->hs, n)
                                 : nschan_mount(to, o->paths[0], n * NS_NAME_MAX, o->connect, n,
                                                o->hs, n);
    /* The handles are gone whatever the status (a send's handles are always
     * moved): never closed after the write, as their values may already be
     * another thread's new handles. */
    o->count = 0;
    return st;
}

status_t ns_send_prepared(handle_t to, uint32_t kind, struct ns_out *o)
{
    status_t st = ns_send_in(to, kind, o);
    ns_prepared_drop(o);
    return st;
}

/* A `kind` event (NS_MOUNT or NS_SET) of what grants name, on to. */
static status_t send_ours(handle_t to, uint32_t kind, const char *const *grants)
{
    struct ns_out *o;
    status_t st = ns_prepare(grants, &o);
    return st == OK ? ns_send_prepared(to, kind, o) : st;
}

status_t ns_send(handle_t to, const char *const *grants)
{
    return send_ours(to, NS_MOUNT, grants);
}

/* Read every message still waiting on back (the program's end) and close
 * the handles they carry: they are ours, and out of date. (A reader with no
 * handlers drops each event and closes its handles.) */
static void take_back(handle_t back)
{
    static const struct nschan_ops none = { 0 };
    for (unsigned i = 0; i < BACK_MAX; i++)
        if (nschan_serve_one(back, &none, NULL) != OK)
            return;   /* empty (or the program's end is gone) */
}

status_t ns_update(handle_t to, handle_t back, const char *const *grants)
{
    if (back)
        take_back(back);
    return send_ours(to, NS_SET, grants);
}

status_t ns_send_one(handle_t to, const char *path, handle_t fs)
{
    if (!path || kind_of(path) == BAD) {
        if (fs)
            jam_handle_close(fs);   /* never sent */
        return ERR_INVALID_ARGS;
    }
    uint8_t rec[NS_NAME_MAX] = { 0 }, connect = 0;
    memcpy(rec, path, strlen(path) + 1);
    /* fs is gone whatever the write's status: never closed after it. */
    if (fs)
        return nschan_mount(to, rec, sizeof(rec), &connect, 1, &fs, 1);
    return nschan_unmount(to, rec, sizeof(rec));
}
