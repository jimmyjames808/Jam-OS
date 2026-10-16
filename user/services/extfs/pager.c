/* extfs: mapped files and the mount's pager (docs/M13.6-PLAN.md's U10;
 * fat's pager.c is the model, docs/M12.5-PLAN.md the design). volumes
 * makes one pager for the mount and hands it to every instance
 * (EXTFS_SR_PAGER); file.map answers a VMO made from it, and the kernel
 * asks extfs for that VMO's pages as its mappers touch them.
 *
 * The slots. Each VMO extfs made is a slot of maps[] in the state: its
 * file (inode number and generation), its kind and its key. The key is
 * XF_MAP_KEY(seq, slot): seq counts up from the clock at the first map of
 * a state set up empty, so a key is never used twice for a pager, even by
 * an instance that started fresh while the VMOs of the one before still
 * live. A VMO lives in its mappers, never in the state, and extfs names it
 * only by its key:
 *   - FILE_MAP_READ: the file's shared VMO. extfs holds a handle of its own
 *     (and the keeper a copy, slot XF_KEEP_MAP(i)) while the file is open
 *     through fs.open, so every map of it then shares its pages; once its
 *     last fs.open goes, extfs lets go (maps_let_go) and the VMO is its
 *     mappers' alone. A later map of the file then makes a new one.
 *   - FILE_MAP_EXEC: a VMO of the caller's own every time (the shell's
 *     vmo_make_exec_verified needs the only handle), never shared.
 * When the kernel says a VMO is gone (PAGER_REQ_GONE: no handle, no
 * mapping), extfs forgets its slot, one store, and answers with
 * PAGER_OP_DETACH, which frees the key. In that order: a death between
 * the two leaves a GONE the successor gets again (PAGER_NEXT_AGAIN) for a
 * key it doesn't know. A request for a key the state doesn't know (a VMO
 * a request that ran again made, or one of an instance whose state was
 * given up on) is answered with a detach: its mappers fail as for a
 * pulled stick.
 *
 * A mapped file keeps its inode. A slot holds its inode as an open does
 * (files_held): unlinked, or renamed over, while it is mapped, the inode
 * is on the orphan list with no name (on the disk: a cut frees it at the
 * next mount) and its pages stay readable, POSIX's rule, until the last
 * slot naming it and the last open go; then it is dropped (gone, after a
 * GONE; files.c's close_file, after a close). A successor keeps the
 * orphans its state's slots name (adopt.c's recover_orphans). fat cut such
 * a mapping off: FAT frees the clusters at once.
 *
 * A page request is read with the ext library (ext_read: the file's
 * extents, whole blocks from the device straight into the buffer, holes
 * zeros) and supplied whole: one pager_supply of its range (its waiters
 * wake when the call that brings its last page ends), the last page's tail
 * past the end of the file zero; pages past the end of the file are failed
 * ERR_OUT_OF_RANGE, a read error ERR_IO. A request changes nothing in the
 * state or on the disk, so it needs no slot, no transaction and no commit:
 * a successor asks again. A VMO holds at most the file's first
 * VMO_SMALL_MAX_SIZE bytes (a pager VMO's limit).
 *
 * Requests are served one per turn of main.c's loop (the pager's binding
 * is ONCE, made again after each), and before each fs request
 * (pager_reap, from files_reap): a mapping let go and then the file
 * removed are seen in that order. A pager VMO is never mapped or read by
 * extfs: its own fault would wait on itself.
 *
 * A mapped file that changes (files.c, fsops.c). A file mapped to read may
 * be written, truncated, renamed, removed or renamed over; its mappings
 * see each change as a read would (MAP_SHARED's rule). A file mapped to
 * run (FILE_MAP_EXEC: a program, docs/M12.5-PLAN.md's Q7) is ERR_BUSY to
 * an open for writing, an unlink and a rename over it while that VMO lives
 * (a rename of it, or an exchange, takes no byte or name from the inode
 * and is allowed: Linux's rule), and a file open for writing is never
 * mapped to run.
 *   - A write or truncate (maps_changed): the file's VMOs take its new size
 *     (PAGER_OP_SET_SIZE: past it, a load is a bus error and a copy
 *     fails), and the pages it changed are dropped (PAGER_OP_INVALIDATE:
 *     the next touch fetches them again).
 *   - A rename, a removal: nothing (the inode, and its pages, are the
 *     same).
 *
 * The ordering. A change runs inside its request (request.c): ext writes
 * file data to the device during the request (ordered mode), and the
 * request's metadata is committed in the journal before its answer.
 * Inside the request, after ext has changed the file, maps_changed drops
 * the pages and sets the size; then the commit, then the answer. (a)
 * extfs is one thread and serves page requests only between requests
 * (pager_event, pager_reap), each one read and supplied in one turn: so no
 * page is supplied between the drop and the answer, and none read before
 * the change lands after it. A page present after the answer was read
 * after the change: a client that saw the answer and then reads the
 * mapping sees the change. (b) A death before the commit: a successor
 * runs the request again (its data again from the slot's bounce buffer),
 * the drop and the size with it (doing them twice is doing them once); a
 * death after it: both done. (c) A request whose change isn't committed
 * after all (its transaction given up: a commit that failed, a request
 * that failed after ext wrote, a request in progress at two crashes)
 * leaves the disk as it was before it but for data written in place,
 * which a read sees too: the pages it dropped are fetched again from the
 * disk, and the sizes are set again from the inodes (maps_settle, and
 * pager_start at every start).
 *
 * The disk going: volumes closes the pager (its disk-gone path, with the
 * mount), which cuts off every VMO it made; a request extfs is reading
 * when its disk goes is cut off here too (read_request), so a mapper sees
 * one reason: the file's pager is gone. */
#include "extfs.h"

#define REQ_BUF  (PAGER_REQ_PAGES * PAGE_SIZE)   /* bytes read and supplied at a time */
#define REAP_MAX 16u                             /* requests pager_reap serves at most */

/* Each slot's part in extfs's own memory, by maps[]'s index. */
static struct {
    handle_t vmo;             /* extfs's own handle of a shared VMO, while its file is open */
    uint64_t size;            /* the VMO's size, as extfs last set it */
    uint64_t requests;        /* read requests answered by this instance */
    uint64_t pages;           /* pages it supplied */
    bool     mismatch_said;   /* its pages didn't match an approved program's: logged */
} mh[XF_MAX_MAPS];

static _Alignas(PAGE_SIZE) uint8_t buf[REQ_BUF];   /* a request's bytes, read and supplied */
static bool unknown_said;     /* a request for a key the state doesn't know was logged */
static bool arm_said;         /* the pager couldn't be waited on: logged */
static bool moved;            /* the running request changed a mapped file (maps_settle) */

static uint64_t page_up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* The size of a VMO of a file of n bytes: its pages, as far as a pager
 * VMO goes. */
static uint64_t vmo_size(uint64_t n)
{
    return n >= VMO_SMALL_MAX_SIZE ? VMO_SMALL_MAX_SIZE : page_up(n);
}

/* The slot whose VMO has `key`, or -1. */
static int slot_of(uint64_t key)
{
    unsigned i = XF_MAP_SLOT(key);
    return i < XF_MAX_MAPS && kept->maps[i].used && kept->maps[i].key == key ? (int)i : -1;
}

bool maps_has(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (kept->maps[i].used && kept->maps[i].ino == ino)
            return true;
    return false;
}

bool maps_exec(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (kept->maps[i].used && kept->maps[i].ino == ino &&
            kept->maps[i].kind == FILE_MAP_EXEC)
            return true;
    return false;
}

/* The file's inode as it is now: ERR_NOT_FOUND if it isn't the one mapped
 * (it can't be freed while a slot names it, so that is damage too). */
static status_t inode_of(unsigned i, struct ext_inode *in)
{
    const struct xf_map *m = &kept->maps[i];
    status_t st = ext_iget(&vol.fs, m->ino, EXT_IGET_UNLINKED, in);
    return st == OK && in->d.generation != m->igen ? ERR_NOT_FOUND : st;
}

/* A key for slot i. seq starts from the clock (ns since boot, far below
 * 2^56), which passes every key an earlier instance of this boot made:
 * each map took far more than a nanosecond. */
static uint64_t next_key(unsigned i)
{
    if (!kept->map_seq)
        kept->map_seq = now();
    return XF_MAP_KEY(++kept->map_seq, i);
}

/* Slot i's handle goes, and the keeper's copy. */
static void drop_handle(unsigned i)
{
    if (!mh[i].vmo)
        return;
    jam_handle_close(mh[i].vmo);
    mh[i].vmo = HANDLE_INVALID;
    kept_drop(XF_KEEP_MAP(i));
}

/* Inode ino is held by nothing now: if it has no name, it is dropped
 * (after the answer, inside a request; else now). */
static void maybe_drop(uint32_t ino, uint32_t igen)
{
    struct ext_inode in;
    if (files_held(ino) || !writable() ||
        ext_iget(&vol.fs, ino, EXT_IGET_UNLINKED, &in) != OK || in.d.generation != igen ||
        in.d.links_count)
        return;
    defer_drop(ino);
}

/* Slot i forgotten (one store), its handle with it; its inode dropped
 * if that was the last thing holding it and it has no name. */
static void forget(unsigned i)
{
    struct xf_map *m = &kept->maps[i];
    uint32_t ino = m->ino, igen = m->igen;
    m->used = false;
    drop_handle(i);
    memset(&mh[i], 0, sizeof(mh[i]));
    maybe_drop(ino, igen);
}

/* ---- file.map ----------------------------------------------------------------------- */

/* The slot of inode ino's shared VMO while extfs holds it, or -1. */
static int shared_of(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        const struct xf_map *m = &kept->maps[i];
        if (m->used && m->kind == FILE_MAP_READ && m->ino == ino && mh[i].vmo)
            return (int)i;
    }
    return -1;
}

/* A new VMO of `size` bytes for slot i, under a new key: the client's
 * handle into *out and, for a shared one, extfs's into *mine (kept). */
static status_t make_vmo(unsigned i, uint32_t kind, uint64_t size, uint64_t *key, handle_t *out,
                         handle_t *mine)
{
    handle_t v = HANDLE_INVALID, c = HANDLE_INVALID;
    *key = next_key(i);
    status_t st = jam_pager_create_vmo(vol.pager, *key, size, 0, &v);
    if (st != OK || kind != FILE_MAP_READ) {
        *out = v;
        return st;
    }
    st = jam_handle_duplicate(v, RIGHT_SAME, &c);
    if (st == OK)
        st = kept_put(XF_KEEP_MAP(i), &v, 1);   /* before the state records the slot */
    if (st != OK) {
        jam_handle_close(v);
        if (c)
            jam_handle_close(c);
        return st;
    }
    *out = c;
    *mine = v;
    return OK;
}

status_t maps_make(uint32_t ino, uint32_t igen, uint32_t flags, uint64_t fsize, handle_t *out)
{
    if (!vol.pager)
        return ERR_NOT_SUPPORTED;
    int s = flags == FILE_MAP_READ ? shared_of(ino) : -1;
    if (s >= 0) {
        status_t st = jam_handle_duplicate(mh[s].vmo, RIGHT_SAME, out);
        if (st == OK)
            op_made(XF_MADE_MAP_SHARED, (uint32_t)s);
        return st;
    }
    unsigned i = 0;
    while (i < XF_MAX_MAPS && kept->maps[i].used)
        i++;
    if (i == XF_MAX_MAPS)
        return ERR_NO_RESOURCES;
    uint64_t key = 0, size = vmo_size(fsize);
    handle_t client = HANDLE_INVALID, mine = HANDLE_INVALID;
    drop_handle(i);   /* none: a free slot's went with it */
    status_t st = make_vmo(i, flags, size, &key, &client, &mine);
    if (st != OK)
        return st;
    memset(&mh[i], 0, sizeof(mh[i]));
    mh[i].vmo = mine;
    mh[i].size = size;
    kept->maps[i] = (struct xf_map){
        .used = true, .kind = (uint8_t)flags, .ino = ino, .igen = igen, .key = key,
    };
    op_made(XF_MADE_MAP, i);
    test_die(XF_DIE_MAP);
    *out = client;
    return OK;
}

void maps_unmake(uint32_t i)
{
    if (i < XF_MAX_MAPS && kept->maps[i].used)
        forget(i);   /* its VMO's GONE then finds a key nobody knows */
}

void maps_let_go(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (kept->maps[i].used && kept->maps[i].ino == ino)
            drop_handle(i);
}

void maps_drop_unknown(void)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (!kept->maps[i].used)
            drop_handle(i);
}

/* ---- a successor's mapped files (adopt.c) --------------------------------------------- */

bool maps_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= XF_MAX_MAPS || !kept->maps[i].used || kept->maps[i].kind != FILE_MAP_READ ||
        n != 1 || mh[i].vmo)
        return false;
    mh[i].vmo = hs[0];
    return true;
}

const char *maps_check(void)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        const struct xf_map *m = &kept->maps[i];
        if (!m->used)
            continue;
        if ((m->kind != FILE_MAP_READ && m->kind != FILE_MAP_EXEC) ||
            XF_MAP_SLOT(m->key) != i || m->key >> 8 > kept->map_seq)
            return "a mapped file out of range";
        struct ext_inode in;
        if (inode_of(i, &in) != OK || (in.d.mode & EXT_S_IFMT) != EXT_S_IFREG)
            return "a mapped file that isn't on the volume";
    }
    return NULL;
}

void maps_adopt(void)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        struct xf_map *m = &kept->maps[i];
        /* Without a pager nothing can serve its pages (volumes hands
         * every instance the same one: this is a test's extfs). */
        if (m->used && !vol.pager)
            m->used = false;
        /* extfs holds a shared VMO while its file is open, and only then:
         * a close the keeper never heard of. */
        if (!m->used || !files_has(m->ino))
            drop_handle(i);
    }
}

status_t maps_remake(uint32_t i, handle_t *out)
{
    if (i >= XF_MAX_MAPS || !kept->maps[i].used || !vol.pager)
        return ERR_BAD_STATE;
    struct xf_map *m = &kept->maps[i];
    if (m->kind == FILE_MAP_READ)
        return mh[i].vmo ? jam_handle_duplicate(mh[i].vmo, RIGHT_SAME, out) : ERR_BAD_STATE;
    /* The caller's own VMO died with the instance: a new one, under a new
     * key (one store); the old key's GONE then finds a key nobody knows. */
    struct ext_inode in;
    status_t st = inode_of(i, &in);
    if (st != OK)
        return st;
    uint64_t key = 0, size = vmo_size(ext_isize(&in));
    handle_t v = HANDLE_INVALID, none = HANDLE_INVALID;
    st = make_vmo(i, FILE_MAP_EXEC, size, &key, &v, &none);
    if (st != OK)
        return st;
    m->key = key;
    mh[i].size = size;
    *out = v;
    return OK;
}

/* ---- the requests ----------------------------------------------------------------------- */

static void detach(uint64_t key)
{
    status_t st = jam_pager_op(vol.pager, key, PAGER_OP_DETACH, 0, 0, 0);
    if (st != OK && st != ERR_NOT_FOUND)
        printf("extfs %s: can't detach mapped file %lx: %s\n", vol.name, (unsigned long)key,
               status_str(st));
}

/* Fail [off, off + len) of key's VMO with status (ERR_IO, ERR_OUT_OF_RANGE). */
static void fail(uint64_t key, uint64_t off, uint64_t len, status_t status)
{
    status_t st = jam_pager_op(vol.pager, key, PAGER_OP_FAIL, off, len, status);
    if (st != OK && st != ERR_NOT_FOUND)
        printf("extfs %s: can't fail a page request of %lx: %s\n", vol.name,
               (unsigned long)key, status_str(st));
}

/* Slot i's VMO cut off for good: its mappers fail as for a pulled stick.
 * Then the slot is forgotten. */
static void cut(unsigned i)
{
    detach(kept->maps[i].key);
    forget(i);
}

/* ---- a mapped file that changes (the header's last parts) ----------------------------- */

/* A pager op slot i's mappers need to see the file as it is now
 * (PAGER_OP_SET_SIZE, PAGER_OP_INVALIDATE). The kernel refusing it (but
 * for a VMO gone already) cuts the VMO off instead: its mappers fail
 * rather than read bytes the file no longer has. False if it was cut. */
static bool must(unsigned i, uint32_t op, uint64_t off, uint64_t len)
{
    uint64_t key = kept->maps[i].key;
    status_t st = jam_pager_op(vol.pager, key, op, off, len, 0);
    if (st == OK || st == ERR_NOT_FOUND)
        return true;
    printf("extfs %s: mapped file %lx: can't %s (%s): cut off\n", vol.name, (unsigned long)key,
           op == PAGER_OP_SET_SIZE ? "follow its size" : "drop its old pages", status_str(st));
    cut(i);
    return false;
}

void maps_changed(uint32_t ino, uint64_t lo, uint64_t hi, uint64_t size)
{
    uint64_t vs = vmo_size(size);
    uint64_t first = lo & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = page_up(hi) < vs ? page_up(hi) : vs;
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        const struct xf_map *m = &kept->maps[i];
        if (!m->used || m->ino != ino || m->kind != FILE_MAP_READ)
            continue;   /* (one to run is never written while it lives: ERR_BUSY) */
        moved = true;
        if (mh[i].size != vs) {
            mh[i].size = vs;
            if (!must(i, PAGER_OP_SET_SIZE, vs, 0))
                continue;
        }
        if (first < end)
            (void)must(i, PAGER_OP_INVALIDATE, first, end - first);   /* (cut if not) */
    }
}

/* Every VMO the size its file has now (the header's (c)): a VMO gone or
 * cut off says ERR_NOT_FOUND, nothing to do. One to run is never resized
 * (sealed, it refuses): its file doesn't change while it lives. */
static void sizes_again(void)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        const struct xf_map *m = &kept->maps[i];
        struct ext_inode in;
        if (!m->used || inode_of(i, &in) != OK)
            continue;
        mh[i].size = vmo_size(ext_isize(&in));
        if (m->kind == FILE_MAP_READ)
            (void)jam_pager_op(vol.pager, m->key, PAGER_OP_SET_SIZE, mh[i].size, 0, 0);
    }
}

void maps_settle(bool done)
{
    if (moved && !done && vol.pager)
        sizes_again();
    moved = false;
}

/* pager_supply refused a range of slot i's: say why, and fail what is left. */
static void supply_failed(unsigned i, uint64_t key, uint64_t off, uint64_t len, status_t st)
{
    if (st == ERR_NOT_FOUND)
        return;   /* detached or gone meanwhile: nobody waits */
    if (st == ERR_IO) {
        /* A verified VMO (a program): the disk's bytes are not the
         * approved ones. The kernel failed those pages' requests. */
        if (!mh[i].mismatch_said)
            printf("extfs %s: mapped file %lx: the disk's pages don't match the approved "
                   "program's\n", vol.name, (unsigned long)key);
        mh[i].mismatch_said = true;
        return;
    }
    printf("extfs %s: mapped file %lx: pages not supplied: %s\n", vol.name, (unsigned long)key,
           status_str(st));
    fail(key, off, len, ERR_IO);
}

/* A read that failed: the disk gone (cut off, as volumes' close of the
 * pager will), else ERR_IO, the volume turned read-only if it found damage. */
static void read_failed(const struct pager_request *r, uint64_t at, status_t st, uint32_t ro)
{
    if (vol.disk_gone) {
        detach(r->key);
        return;
    }
    printf("extfs %s: mapped file %lx: can't read at %lu: %s\n", vol.name,
           (unsigned long)r->key, (unsigned long)at, status_str(st));
    fail(r->key, r->offset, r->len, ERR_IO);
    if ((vol.fs.ro & EXT_RO_DAMAGED) && !(ro & EXT_RO_DAMAGED))
        went_bad("a page request");
}

/* A read request: its range, as far as the file goes, supplied. */
static void read_request(const struct pager_request *r)
{
    int s = slot_of(r->key);
    if (s < 0) {
        if (!unknown_said)
            printf("extfs %s: page requests for mapped files it doesn't know: detached\n",
                   vol.name);
        unknown_said = true;
        detach(r->key);
        return;
    }
    unsigned i = (unsigned)s;
    uint32_t ro = vol.fs.ro;
    struct ext_inode in;
    status_t st = inode_of(i, &in);
    if (st != OK) {
        read_failed(r, r->offset, st, ro);
        return;
    }
    /* Clipped to the file and to the VMO: a request queued before a
     * shrink may ask past either. */
    uint64_t vs = vmo_size(ext_isize(&in)), off = r->offset, len = r->len ? r->len : PAGE_SIZE;
    if (mh[i].size < vs)
        vs = mh[i].size;
    if (off >= vs) {
        fail(r->key, off, len, ERR_OUT_OF_RANGE);
        return;
    }
    uint64_t end = len > vs - off ? vs : off + len;
    mh[i].requests++;
    reply_flush();   /* the last request's answer goes before the disk is read */
    for (uint64_t at = off, n = 0; at < end; at += n) {
        n = end - at < REQ_BUF ? end - at : REQ_BUF;
        size_t got = 0;
        st = ext_read(&vol.fs, &in, at, buf, n, &got);
        if (st != OK) {
            read_failed(r, at, st, ro);
            return;
        }
        memset(buf + got, 0, n - got);
        test_die(XF_DIE_SUPPLY);
        st = jam_pager_supply(vol.pager, r->key, at, buf, n);
        if (st != OK) {
            supply_failed(i, r->key, off, len, st);
            return;
        }
        mh[i].pages += n / PAGE_SIZE;
    }
    if (end < off + len)   /* the file is shorter than the request */
        fail(r->key, end, off + len - end, ERR_OUT_OF_RANGE);
}

/* A VMO with no handle and no mapping left: its slot forgotten (one
 * store), then the key freed; then its inode, if nothing holds it and it
 * has no name. */
static void gone(uint64_t key)
{
    int s = slot_of(key);
    if (s < 0) {
        detach(key);
        return;
    }
    unsigned i = (unsigned)s;
    struct xf_map *m = &kept->maps[i];
    uint32_t ino = m->ino, igen = m->igen;
    printf("extfs %s: mapped file %lx gone: %lu page requests, %lu pages supplied\n", vol.name,
           (unsigned long)key, (unsigned long)mh[i].requests, (unsigned long)mh[i].pages);
    m->used = false;
    drop_handle(i);   /* none: extfs's handle would have kept it */
    memset(&mh[i], 0, sizeof(mh[i]));
    detach(key);
    maybe_drop(ino, igen);
    defer_run();
}

/* One request, if one is queued: true if one was served. */
static bool serve(uint32_t flags)
{
    struct pager_request r;
    status_t st = jam_pager_next(vol.pager, flags, &r);
    if (st != OK) {
        if (st != ERR_SHOULD_WAIT)
            printf("extfs %s: can't read the pager's requests: %s\n", vol.name,
                   status_str(st));
        return false;
    }
    if (r.kind == PAGER_REQ_GONE)
        gone(r.key);
    else if (r.kind == PAGER_REQ_READ)
        read_request(&r);
    else
        printf("extfs %s: a page request of a kind it doesn't know (%u)\n", vol.name, r.kind);
    return true;
}

/* Wait (ONCE) for the next request. */
static void arm(void)
{
    status_t st = jam_port_bind(vol.port, vol.pager, XF_KEY_PAGER, SIG_READABLE,
                                PORT_BIND_ONCE);
    if (st != OK && !arm_said)
        printf("extfs %s: can't wait on the pager (%s): its requests end at their deadline\n",
               vol.name, status_str(st));
    arm_said |= st != OK;
}

void pager_start(void)
{
    if (!vol.pager)
        return;
    sizes_again();
    arm();
    /* What the instance before took and never answered, queued again
     * (and the first of the queue served). */
    (void)serve(PAGER_NEXT_AGAIN);
}

void pager_event(void)
{
    if (!vol.pager)
        return;
    (void)serve(0);
    arm();   /* fires at once if more is queued */
}

void pager_reap(void)
{
    for (unsigned n = 0; vol.pager && n < REAP_MAX && serve(0); n++)
        ;
}
