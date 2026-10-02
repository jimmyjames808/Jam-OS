/* The namespace: this program's mount points and services, and the channel
 * behind each (<os.h> "files", "services" and "grants" have the model and
 * SR_NS's encoding).
 *
 * The table starts empty. Entries come from whoever started us, as ns_msg
 * messages on the SR_NS channel (taken here before every lookup, so a
 * mount sent to a running program is there for its next call), and from
 * ns_mount and ns_svc_set (init publishes its own). Each entry remembers
 * which of the two it came from: an NS_SET replaces only the starter's.
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
 * own lock and never calls out under it. */
#include <fs_idl.h>
#include <fsview.h>
#include <idl/svc.h>
#include <os.h>
#include "ns.h"

#define TAKE_MAX 64   /* messages taken in one go: a flooding starter can't hold a lookup */
#define BACK_MAX 1024 /* messages ns_update takes back: a channel end holds no more */
#define SVC_DIR  "/svc/"
#define SVC_WAIT (5 * NS_PER_S)   /* svc.connect */

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
static bool lock;           /* lock_take's */

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

/* path is one of m's (checked, n bytes long) paths. */
static bool in_msg(const struct ns_msg *m, const char *path)
{
    for (uint32_t i = 0; i < m->count; i++)
        if (!strncmp(m->path[i], path, NS_NAME_MAX))
            return true;
    return false;
}

/* NS_SET: the starter's entries that m doesn't list go (its handles then
 * go in as an NS_MOUNT's). */
static void set_locked(const struct ns_msg *m)
{
    for (unsigned i = 0; i < nents;) {
        if (ents[i].given && !in_msg(m, ents[i].path))
            (void)remove_locked(ents[i].path);   /* the next one moves to i */
        else
            i++;
    }
}

/* One message from the starter: n bytes, nh handles (all consumed). */
static void apply_locked(const struct ns_msg *m, uint32_t n, const handle_t *hs, uint32_t nh)
{
    bool mounting = m->kind == NS_MOUNT || m->kind == NS_SET;
    bool ok = n >= NS_MSG_SIZE(0) && m->count <= NS_MAX_ENTRIES && n == NS_MSG_SIZE(m->count) &&
              !(m->connect >> m->count) &&
              ((mounting && nh == m->count) || (m->kind == NS_UNMOUNT && !nh && !m->connect));
    if (!ok) {
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        return;
    }
    if (m->kind == NS_SET)
        set_locked(m);
    for (uint32_t i = 0; i < m->count; i++) {
        enum kind k = kind_of(m->path[i]);
        bool connect = m->connect >> i & 1;
        bool valid = k != BAD && (k == SERVICE || !connect);
        if (mounting && !valid)
            jam_handle_close(hs[i]);
        else if (mounting)
            (void)put_locked(m->path[i], k, hs[i], true, connect);   /* a full table closes it */
        else if (valid)
            (void)remove_locked(m->path[i]);                         /* not there: nothing to do */
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

/* Read one ns_msg off ch into *m and hs (NS_MAX_ENTRIES of them):
 * channel_read's status; one too big for any ns_msg is taken off and
 * dropped (OK with *nh = 0 and *n = 0, which no check passes). */
static status_t read_msg(handle_t ch, struct ns_msg *m, handle_t *hs, uint32_t *n, uint32_t *nh)
{
    struct channel_read_args a = {
        .h = ch, .bytes_cap = sizeof(*m), .bytes = (uint64_t)(uintptr_t)m,
        .actual_bytes = (uint64_t)(uintptr_t)n, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = NS_MAX_ENTRIES, .actual_handles = (uint64_t)(uintptr_t)nh,
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_BUFFER_TOO_SMALL && drop(ch, *n, *nh)) {
        *n = *nh = 0;
        return OK;
    }
    return st;
}

/* What the starter has sent so far (TAKE_MAX messages at most) into the
 * table; how many messages were taken. */
static unsigned take_some_locked(handle_t ch)
{
    unsigned taken = 0;
    for (; taken < TAKE_MAX; taken++) {
        struct ns_msg m;
        handle_t hs[NS_MAX_ENTRIES];
        uint32_t n = 0, nh = 0;
        status_t st = read_msg(ch, &m, hs, &n, &nh);
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
    if (!path || path[0] != '/' || strnlen(path, FS_PATH_MAX) == FS_PATH_MAX)
        return ERR_INVALID_ARGS;
    /* The mount: the first real name. "." and ".." before it stay at "/". */
    const char *name, *p = path;
    size_t len;
    while ((len = next_name(p, &name)) != 0 && (is_dot(name, len) || is_dotdot(name, len)))
        p = name + len;
    memset(rel, 0, FS_PATH_MAX);
    rel[0] = '/';
    *fs = HANDLE_INVALID;
    *mount = 0;
    if (!len)
        return OK;
    clean(name + len, (char *)rel + 1);   /* shorter than path: it fits */

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
static bool kept_lock;   /* lock_take's, like the table's; nothing is called under it */

static void kept_take(void)
{
    lock_take(&kept_lock);
}

static void kept_give(void)
{
    lock_give(&kept_lock);
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
    size_t n = strnlen(s, NS_NAME_MAX + 3);
    if (n >= 2 && s[n - 2] == ':' && (s[n - 1] == 'r' || s[n - 1] == 'w')) {
        g->view = s[n - 1] == 'r' ? FS_VIEW_READ_ONLY : FS_VIEW_GUARD_ETC;
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

/* What goes out: our entries that a grant takes, each with a duplicate of
 * its channel (to be turned into a view) and the flags of its view. */
struct ns_out {
    struct ns_msg m;
    handle_t      hs[NS_MAX_ENTRIES];
    uint32_t      view[NS_MAX_ENTRIES];
};

/* The entries grants take, duplicated into o (o->m.count of them). */
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
        uint32_t c = o->m.count;
        st = jam_handle_duplicate(ents[i].h, RIGHT_SAME, &o->hs[c]);
        if (st != OK)
            break;
        memcpy(o->m.path[c], ents[i].path, NS_NAME_MAX);
        o->view[c] = k == MOUNT ? g.view : 0;
        o->m.connect |= (uint32_t)ents[i].connect << c;
        o->m.count++;
    }
    ns_unlock();
    return st;
}

/* Each entry of o that wants a view gets one in place of its duplicate;
 * one whose service won't make it is left out. */
static void make_views(struct ns_out *o)
{
    uint32_t keep = 0, connect = 0;
    for (uint32_t i = 0; i < o->m.count; i++) {
        handle_t h = o->hs[i];
        if (o->view[i]) {
            handle_t v = HANDLE_INVALID;
            status_t st = fs_view_until(h, now() + NS_VIEW_WAIT, o->view[i], &v);
            jam_handle_close(h);
            if (st != OK)
                continue;   /* fail closed: no view, no mount */
            h = v;
        }
        memcpy(o->m.path[keep], o->m.path[i], NS_NAME_MAX);
        connect |= (o->m.connect >> i & 1) << keep;
        o->hs[keep++] = h;
    }
    o->m.count = keep;
    o->m.connect = connect;
}

status_t ns_prepare(const char *const *grants, struct ns_out **out)
{
    struct ns_out *o = calloc(1, sizeof(*o));
    if (!o)
        return ERR_NO_MEMORY;
    status_t st = grants ? gather(grants, o) : OK;
    if (st != OK) {
        ns_prepared_drop(o);
        return st;
    }
    make_views(o);
    *out = o;
    return OK;
}

void ns_prepared_drop(struct ns_out *o)
{
    for (uint32_t i = 0; o && i < o->m.count; i++)
        jam_handle_close(o->hs[i]);
    free(o);
}

status_t ns_send_prepared(handle_t to, uint32_t kind, struct ns_out *o)
{
    o->m.kind = kind;
    status_t st = jam_channel_write(to, &o->m, NS_MSG_SIZE(o->m.count), o->hs, o->m.count);
    if (st == OK)
        o->m.count = 0;   /* the handles went with it */
    ns_prepared_drop(o);
    return st;
}

/* A `kind` message (NS_MOUNT or NS_SET) of what grants name, on to. */
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
 * the handles they carry: they are ours, and out of date. */
static void take_back(handle_t back)
{
    for (unsigned i = 0; i < BACK_MAX; i++) {
        struct ns_msg m;
        handle_t hs[NS_MAX_ENTRIES];
        uint32_t n = 0, nh = 0;
        if (read_msg(back, &m, hs, &n, &nh) != OK)
            return;   /* empty (or the program's end is gone) */
        for (uint32_t k = 0; k < nh; k++)
            jam_handle_close(hs[k]);
    }
}

status_t ns_update(handle_t to, handle_t back, const char *const *grants)
{
    if (back)
        take_back(back);
    return send_ours(to, NS_SET, grants);
}

status_t ns_send_one(handle_t to, const char *path, handle_t fs)
{
    struct ns_msg m;
    memset(&m, 0, sizeof(m));
    m.kind = fs ? NS_MOUNT : NS_UNMOUNT;
    m.count = 1;
    status_t st = path && kind_of(path) != BAD ? OK : ERR_INVALID_ARGS;
    if (st == OK) {
        memcpy(m.path[0], path, strlen(path) + 1);
        st = jam_channel_write(to, &m, NS_MSG_SIZE(1), &fs, fs ? 1 : 0);
    }
    if (st != OK && fs)
        jam_handle_close(fs);
    return st;
}
