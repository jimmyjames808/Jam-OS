/* extfs: mapped files and the mount's pager (docs/M13.6-PLAN.md's U10,
 * docs/WRITEMAP-PLAN.md's WE1; fat's pager.c is the model,
 * docs/M12.5-PLAN.md the design). volumes makes one pager for the mount
 * and hands it to every instance (EXTFS_SR_PAGER); file.map answers a VMO
 * made from it, and the kernel asks extfs for that VMO's pages as its
 * mappers touch them.
 *
 * The slots. Each VMO extfs made is a slot of maps[] in the state: its
 * file (inode number and generation), its kind and its key. A VMO lives in
 * its mappers (and, while it has dirty pages, in the kernel's hold: a
 * store outlives its mapping), never in the state, and extfs names it only
 * by its key:
 *   - FILE_MAP_READ and FILE_MAP_WRITE: the file's shared VMO, one per
 *     file, made writable (PAGER_VMO_WRITABLE) so that a store through any
 *     mapping is every other mapper's at once. Its key is the file's
 *     (XF_SHARED_KEY: the inode's number and generation), so the kernel's
 *     key table is the index of the live ones: a map asks for the live VMO
 *     (PAGER_VMO_FIND) and makes one only if there is none, and sets a
 *     found one's size from the inode. extfs holds a handle of its own
 *     (and the keeper a copy, slot XF_KEEP_MAP(i)) while the file is open
 *     through fs.open; once its last fs.open goes, extfs lets go
 *     (maps_let_go) and the VMO is its mappers' alone, found again by the
 *     next map while it lives. FILE_MAP_READ hands out a handle without
 *     RIGHT_WRITE (never mapped writable); FILE_MAP_WRITE, on a file open
 *     to read and write, one with it, and the slot remembers that
 *     (written) until the VMO's GONE.
 *   - FILE_MAP_EXEC: a VMO of the caller's own every time (the shell's
 *     vmo_make_exec_verified needs the only handle), never shared and
 *     never writable, under XF_MAP_KEY(seq, slot): seq counts up from the
 *     clock at the first map of a state set up empty, so such a key is
 *     never used twice for a pager, even by an instance that started fresh
 *     while the VMOs of the one before still live.
 * When the kernel says a VMO is gone (PAGER_REQ_GONE: no handle, no
 * mapping, no dirty page), extfs forgets its slot, one store, and answers
 * with PAGER_OP_DETACH, which frees the key. In that order: a death
 * between the two leaves a GONE the successor gets again
 * (PAGER_NEXT_AGAIN) for a key it doesn't know. A map of the file before
 * the GONE is answered makes the VMO again under the same key, which takes
 * the GONE over (the kernel withdraws it). A page or write-back request for
 * a file's key the state doesn't know (a VMO of an instance whose state was
 * given up on: a fresh start's) gets a slot from the inode the key names
 * (maps_key: its number and generation, a file with a name, else it is
 * gone: a fresh start's mount dropped the orphans) and is served; any other
 * key the state doesn't know (one to run whose request ran again, a file
 * gone) is answered with a detach: its mappers fail as for a pulled stick.
 *
 * A mapped file keeps its inode. A slot holds its inode as an open does
 * (files_held): unlinked, or renamed over, while it is mapped, the inode
 * is on the orphan list with no name (on the disk: a cut frees it at the
 * next mount) and its pages stay readable and writable, POSIX's rule,
 * until the last slot naming it and the last open go; then it is dropped
 * (gone, after a GONE; files.c's close_file, after a close). A successor
 * keeps the orphans its state's slots name (adopt.c's recover_orphans).
 * fat cut such a mapping off: FAT frees the clusters at once.
 *
 * A page request is read with the ext library (ext_read: the file's
 * extents, whole blocks from the device straight into the buffer, holes
 * zeros) and supplied whole: one pager_supply of its range (its waiters
 * wake when the call that brings its last page ends), the last page's tail
 * past the end of the file zero; pages past the end of the file are failed
 * ERR_OUT_OF_RANGE, a read error ERR_IO. A request changes nothing in the
 * state or on the disk, so it needs no slot, no transaction and no commit:
 * a successor asks again. A VMO holds at most the file's first
 * VMO_SMALL_MAX_SIZE bytes (a pager VMO's limit). A dirty page is never
 * asked for: the kernel keeps it until it is written back (wback.c, which
 * serves PAGER_REQ_WRITEBACK).
 *
 * Requests are served one per turn of main.c's loop (the pager's binding
 * is ONCE, made again after each), and before each fs request
 * (pager_reap, from files_reap): a mapping let go and then the file
 * removed are seen in that order. A pager VMO is never mapped or read by
 * extfs: its own fault would wait on itself.
 *
 * One cache: a mapped file that changes (files.c, fsops.c). A file mapped
 * to read or write may be written, truncated, renamed, removed or renamed
 * over; its mappings see each change as a read would (MAP_SHARED's rule),
 * and a read sees its mappings' stores:
 *   - A write (maps_wrote): its bytes, on the disk, are written into the
 *     shared VMO's pages that are present too (pager_write; absent ones
 *     stay absent, the disk has the bytes): a page a mapper stored to
 *     keeps its stores and takes the write's bytes, and its next
 *     write-back writes both.
 *   - Growth (a write past the end, a truncate up) and a shrink
 *     (maps_resized; a write's growth in maps_wrote): the bytes of the
 *     last page past the old end (growing) or the new one (shrinking)
 *     zeroed first, as a mapper's stores past the end are never the
 *     file's; then the VMO's size (PAGER_OP_SET_SIZE: past it a load is a
 *     bus error and a copy fails; dirty pages past it go, Linux's rule).
 *   - A read (maps_peek): the disk's bytes, then the shared VMO's dirty
 *     and writing pages laid over them (PAGER_DIRTY_PEEK), for a VMO ever
 *     handed out writable (no other has a dirty page).
 *   - A write or truncate that failed (maps_changed): the pages of its
 *     range dropped (PAGER_OP_INVALIDATE: the next touch fetches a clean
 *     one again; a dirty one stays, newer than the disk), the size as the
 *     inode has it.
 *   - A rename, a removal: nothing (the inode, its key and its pages are
 *     the same).
 * A file mapped to run (FILE_MAP_EXEC: a program, docs/M12.5-PLAN.md's Q7)
 * is ERR_BUSY to an open for writing, an unlink and a rename over it while
 * that VMO lives (a rename of it, or an exchange, takes no byte or name
 * from the inode and is allowed: Linux's rule), and so to FILE_MAP_WRITE.
 * Q7 the other way: a file open for writing, or whose shared VMO was handed
 * out writable and still lives (its pages may hold stores the disk hasn't:
 * Linux's ETXTBSY), is never mapped to run.
 *
 * The ordering. A change runs inside its request (request.c): ext writes
 * file data to the device during the request (ordered mode), and the
 * request's metadata is committed in the journal before its answer.
 * Inside the request, after ext has changed the file, the pages take the
 * change (maps_wrote, maps_resized; maps_changed after a failure); then
 * the commit, then the answer. (a) extfs is one thread and serves page
 * requests only between requests (pager_event, pager_reap), each one read
 * and supplied in one turn, as a write-back is (begin, data, commit,
 * written: wback.c; file.sync and fs.sync write back inside their own
 * requests, which change no file): so no page is supplied, and none
 * written back, between the
 * change and the answer, and none read before the change lands after it.
 * A page present after the answer was read after the change, or took it:
 * a client that saw the answer and then reads the mapping sees the
 * change. pager_write never meets a page being written back but after a
 * death (and the successor makes such pages dirty again). (b) pager_write
 * comes before the commit. A death before the commit: a successor runs the
 * request again (its data again from the slot's bounce buffer), the
 * pages' bytes and the size with it (doing them twice is doing them
 * once); a death after it: both done. Writing the pages after the commit
 * could lose a write: a successor would answer a committed write whose
 * bytes never reached a dirty page, and that page's next write-back would
 * put the old bytes back over it. (c) A request whose change isn't
 * committed after all (its transaction given up: a commit that failed, a
 * request that failed after ext wrote, a request in progress at two
 * crashes) leaves the disk as it was before it but for data written in
 * place, which a read sees too: the pages it wrote into are dropped
 * (maps_settle: clean ones are fetched again from the disk), and the sizes
 * are set again from the inodes (maps_settle, and pager_start at every
 * start). A dirty page keeps the given-up bytes, and its next write-back
 * writes them: a given-up write may so appear through a page a mapper had
 * stored to. Rare (a volume error, two crashes in one request), and named
 * here rather than paid for.
 *
 * The disk going: volumes closes the pager (its disk-gone path, with the
 * mount), which cuts off every VMO it made (dirty pages lost, as a write
 * in progress is); a request extfs is reading when its disk goes is cut
 * off here too (read_request), so a mapper sees one reason: the file's
 * pager is gone. */
#include "extfs.h"

#define REQ_BUF  (PAGER_REQ_PAGES * PAGE_SIZE)   /* bytes read and supplied at a time */
#define REAP_MAX 16u                             /* requests pager_reap serves at most */

/* Each slot's part in extfs's own memory, by maps[]'s index. */
static struct {
    handle_t vmo;             /* extfs's own handle of a shared VMO, while its file is open */
    uint64_t size;            /* the VMO's size, as extfs last set it */
    uint64_t lo, hi;          /* the bytes the running request wrote into its pages */
    uint64_t requests;        /* read requests answered by this instance */
    uint64_t pages;           /* pages it supplied */
    bool     mismatch_said;   /* its pages didn't match an approved program's: logged */
} mh[XF_MAX_MAPS];

static _Alignas(PAGE_SIZE) uint8_t buf[REQ_BUF];   /* a request's bytes, read and supplied */
static const uint8_t zeros[PAGE_SIZE];             /* a last page's tail */
static bool unknown_said;     /* a request for a key the state doesn't know was logged */
static bool arm_said;         /* the pager couldn't be waited on: logged */
static bool peek_said;        /* a read couldn't see its mappings' stores: logged */
static bool moved;            /* the running request changed a mapped file (maps_settle) */
static bool again_done;       /* PAGER_NEXT_AGAIN made (maps_again) ... */
static bool again_has;        /* ... and the request it gave not served yet: */
static struct pager_request again_first;

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

/* The slot whose VMO has `key`, or -1: one to run by its slot, a shared
 * one by its file's key. */
static int slot_of(uint64_t key)
{
    if (!(key & XF_SHARED_BIT)) {
        unsigned i = XF_MAP_SLOT(key);
        return i < XF_MAX_MAPS && kept->maps[i].used && kept->maps[i].key == key ? (int)i : -1;
    }
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (kept->maps[i].used && kept->maps[i].key == key)
            return (int)i;
    return -1;
}

/* Is slot i inode ino's shared VMO? */
static bool shared(unsigned i, uint32_t ino)
{
    const struct xf_map *m = &kept->maps[i];
    return m->used && m->ino == ino && m->kind == FILE_MAP_READ;
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

bool maps_written(uint32_t ino)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (shared(i, ino) && kept->maps[i].written)
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

/* A key for slot i, to run. seq starts from the clock (ns since boot, far
 * below 2^55), which passes every key an earlier instance of this boot
 * made: each map took far more than a nanosecond. */
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

/* A handle of shared VMO v for a mapper: without RIGHT_WRITE but to write. */
static status_t hand_out(handle_t v, uint32_t flags, handle_t *out)
{
    return jam_handle_duplicate(v, flags == FILE_MAP_WRITE ? RIGHT_SAME : PAGER_VMO_RIGHTS, out);
}

static int free_slot(void)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        if (!kept->maps[i].used)
            return (int)i;
    return -1;
}

/* The live VMO under a file's key (*found; its size set from the file's,
 * fsize: it may be one an instance before made), or a new one. */
static status_t shared_vmo(uint64_t key, uint64_t fsize, handle_t *v, bool *found)
{
    status_t st = jam_pager_create_vmo(vol.pager, key, 0, PAGER_VMO_FIND, v);
    *found = st == OK;
    if (st == ERR_NOT_FOUND)
        return jam_pager_create_vmo(vol.pager, key, vmo_size(fsize), PAGER_VMO_WRITABLE, v);
    if (st == OK) {
        st = jam_pager_op(vol.pager, key, PAGER_OP_SET_SIZE, vmo_size(fsize), 0, 0);
        if (st != OK) {
            jam_handle_close(*v);
            *v = HANDLE_INVALID;
        }
    }
    return st;
}

/* A map of the file's shared VMO answered from slot i: what it handed
 * out, and the mark adopt.c makes the answer again from. */
static void made_shared(unsigned i, uint32_t flags, uint32_t kind)
{
    struct xf_map *m = &kept->maps[i];
    m->written |= flags == FILE_MAP_WRITE;
    m->made_write = flags == FILE_MAP_WRITE;
    op_made(kind, i);
}

static status_t make_shared(uint32_t ino, uint32_t igen, uint32_t flags, uint64_t fsize,
                            handle_t *out)
{
    if (ino > XF_SHARED_INO_MAX)
        return ERR_NOT_SUPPORTED;
    uint64_t key = XF_SHARED_KEY(ino, igen);
    int s = slot_of(key);
    if (s >= 0 && mh[s].vmo) {   /* held: its file has been open since it was made or found */
        status_t st = hand_out(mh[s].vmo, flags, out);
        if (st == OK)
            made_shared((unsigned)s, flags, XF_MADE_MAP_SHARED);
        return st;
    }
    int i = s >= 0 ? s : free_slot();
    if (i < 0)
        return ERR_NO_RESOURCES;
    handle_t v = HANDLE_INVALID, c = HANDLE_INVALID;
    bool found = false;
    status_t st = shared_vmo(key, fsize, &v, &found);
    if (st == OK)
        st = hand_out(v, flags, &c);
    if (st == OK)
        st = kept_put(XF_KEEP_MAP(i), &v, 1);   /* before the state records the slot */
    if (st != OK) {
        if (v)
            jam_handle_close(v);
        if (c)
            jam_handle_close(c);
        return st;
    }
    /* A slot whose VMO died (its GONE not answered yet) has a new one,
     * never handed out; one found that this instance's state doesn't
     * name may have been handed out writable by an instance before. */
    struct xf_map *m = &kept->maps[i];
    bool written = found && (s < 0 || m->written);
    if (s < 0)
        memset(&mh[i], 0, sizeof(mh[i]));
    mh[i].vmo = v;
    mh[i].size = vmo_size(fsize);
    *m = (struct xf_map){
        .used = true, .kind = FILE_MAP_READ, .written = written, .ino = ino, .igen = igen,
        .key = key,
    };
    /* Never dropped if the request runs again or its reply fails: the
     * key is the file's, so the request run again finds the slot and its
     * VMO (the keeper's copy), and the slot goes at its VMO's GONE. */
    made_shared((unsigned)i, flags, XF_MADE_MAP_SHARED);
    test_die(XF_DIE_MAP);
    *out = c;
    return OK;
}

/* A VMO to run, of the caller's own: a new slot, a new key. */
static status_t make_exec(uint32_t ino, uint32_t igen, uint64_t fsize, handle_t *out)
{
    int i = free_slot();
    if (i < 0)
        return ERR_NO_RESOURCES;
    uint64_t key = next_key((unsigned)i), size = vmo_size(fsize);
    handle_t v = HANDLE_INVALID;
    drop_handle((unsigned)i);   /* none: a free slot's went with it */
    status_t st = jam_pager_create_vmo(vol.pager, key, size, 0, &v);
    if (st != OK)
        return st;
    memset(&mh[i], 0, sizeof(mh[i]));
    mh[i].size = size;
    kept->maps[i] = (struct xf_map){
        .used = true, .kind = FILE_MAP_EXEC, .ino = ino, .igen = igen, .key = key,
    };
    op_made(XF_MADE_MAP, (unsigned)i);
    test_die(XF_DIE_MAP);
    *out = v;
    return OK;
}

status_t maps_make(uint32_t ino, uint32_t igen, uint32_t flags, uint64_t fsize, handle_t *out)
{
    if (!vol.pager)
        return ERR_NOT_SUPPORTED;
    return flags == FILE_MAP_EXEC ? make_exec(ino, igen, fsize, out)
                                  : make_shared(ino, igen, flags, fsize, out);
}

void maps_unmake(uint32_t i)
{
    if (i < XF_MAX_MAPS && kept->maps[i].used && kept->maps[i].kind == FILE_MAP_EXEC)
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
        bool bad = m->kind == FILE_MAP_READ
                       ? m->ino > XF_SHARED_INO_MAX || m->key != XF_SHARED_KEY(m->ino, m->igen) ||
                             slot_of(m->key) != (int)i   /* a file's key in one slot */
                       : m->kind != FILE_MAP_EXEC || (m->key & XF_SHARED_BIT) ||
                             XF_MAP_SLOT(m->key) != i || m->key >> 8 > kept->map_seq;
        if (bad)
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
        return mh[i].vmo ? hand_out(mh[i].vmo, m->made_write ? FILE_MAP_WRITE : FILE_MAP_READ, out)
                         : ERR_BAD_STATE;
    /* The caller's own VMO died with the instance: a new one, under a new
     * key (one store); the old key's GONE then finds a key nobody knows. */
    struct ext_inode in;
    status_t st = inode_of(i, &in);
    if (st != OK)
        return st;
    uint64_t key = next_key(i), size = vmo_size(ext_isize(&in));
    handle_t v = HANDLE_INVALID;
    st = jam_pager_create_vmo(vol.pager, key, size, 0, &v);
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

void maps_detach(uint64_t key)
{
    detach(key);
}

int maps_slot_of(uint64_t key)
{
    return slot_of(key);
}

/* ---- keys from the inode (a fresh start's) ------------------------------------------------ */

static bool from_key_said;    /* a slot was made from a key: logged */

status_t maps_key(uint64_t key, struct ext_inode *in, int *slot)
{
    int s = slot_of(key);
    *slot = s;
    if (s >= 0)
        return inode_of((unsigned)s, in);
    if (!(key & XF_SHARED_BIT))
        return ERR_NOT_FOUND;
    /* A file with a name: an orphan the empty state didn't hold was
     * dropped at the mount, so an inode without one is gone (or freed and
     * being taken again). */
    uint32_t ino = (uint32_t)(key >> 32) & XF_SHARED_INO_MAX, igen = (uint32_t)key;
    status_t st = ext_iget(&vol.fs, ino, 0, in);
    if (st == OK && (in->d.generation != igen || (in->d.mode & EXT_S_IFMT) != EXT_S_IFREG))
        st = ERR_NOT_FOUND;
    if (st == ERR_OUT_OF_RANGE || st == ERR_INVALID_ARGS)
        st = ERR_NOT_FOUND;
    int i = st == OK ? free_slot() : -1;
    if (i < 0)
        return st;   /* (OK with no slot: served without one, its inode not held) */
    memset(&mh[i], 0, sizeof(mh[i]));
    mh[i].size = vmo_size(ext_isize(in));
    kept->maps[i] = (struct xf_map){
        .used = true, .kind = FILE_MAP_READ, .written = true, .ino = ino, .igen = igen, .key = key,
    };
    *slot = i;
    if (!from_key_said)
        printf("extfs %s: mapped files an instance before made, served from their inodes\n",
               vol.name);
    from_key_said = true;
    (void)jam_pager_op(vol.pager, key, PAGER_OP_SET_SIZE, mh[i].size, 0, 0);
    return OK;
}

/* ---- one cache: a mapped file that changes (the header's) ----------------------------- */

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

/* n bytes of data at off into slot i's present pages, as far as its VMO
 * goes (pager_write), the range kept for maps_settle. Refused, the VMO is
 * cut off (its mappers would read bytes the file no longer has): false. */
static bool put(unsigned i, uint64_t off, const void *data, uint64_t n)
{
    if (off >= mh[i].size)
        return true;
    if (n > mh[i].size - off)
        n = mh[i].size - off;
    if (!n)
        return true;
    if (mh[i].lo == mh[i].hi) {
        mh[i].lo = off;
        mh[i].hi = off + n;
    } else {
        mh[i].lo = off < mh[i].lo ? off : mh[i].lo;
        mh[i].hi = off + n > mh[i].hi ? off + n : mh[i].hi;
    }
    uint64_t key = kept->maps[i].key;
    status_t st = jam_pager_write(vol.pager, key, off, data, n);
    if (st == OK || st == ERR_NOT_FOUND)
        return true;
    printf("extfs %s: mapped file %lx: can't write its pages (%s): cut off\n", vol.name,
           (unsigned long)key, status_str(st));
    cut(i);
    return false;
}

/* Slot i's bytes from t to the end of t's page zeroed: past the file's
 * end, what a mapper stored there is never the file's. */
static bool zero_tail(unsigned i, uint64_t t)
{
    return put(i, t, zeros, page_up(t) - t);
}

/* Slot i's VMO the size of a file of `size` bytes. */
static bool resize(unsigned i, uint64_t size)
{
    uint64_t vs = vmo_size(size);
    if (mh[i].size == vs)
        return true;
    mh[i].size = vs;
    return must(i, PAGER_OP_SET_SIZE, vs, 0);
}

void maps_wrote(uint32_t ino, uint64_t off, const void *data, uint64_t n, uint64_t old,
                uint64_t size)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        if (!shared(i, ino))
            continue;   /* (one to run is never written while it lives: ERR_BUSY) */
        moved = true;
        (void)((size <= old || zero_tail(i, old)) && put(i, off, data, n) && resize(i, size));
    }
}

void maps_resized(uint32_t ino, uint64_t old, uint64_t size)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        if (!shared(i, ino))
            continue;
        moved = true;
        (void)(zero_tail(i, size < old ? size : old) && resize(i, size));
    }
}

void maps_changed(uint32_t ino, uint64_t lo, uint64_t hi, uint64_t size)
{
    uint64_t vs = vmo_size(size);
    uint64_t first = lo & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = page_up(hi) < vs ? page_up(hi) : vs;
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        if (!shared(i, ino))
            continue;
        moved = true;
        if (!resize(i, size))
            continue;
        if (first < end)
            (void)must(i, PAGER_OP_INVALIDATE, first, end - first);   /* (cut if not) */
    }
}

/* Slot i's dirty and writing pages laid over the n bytes at off that
 * came from the disk (a range of them at a time, in buf). */
static status_t peek(unsigned i, uint64_t off, uint8_t *data, uint64_t n)
{
    uint64_t key = kept->maps[i].key, end = off + n < mh[i].size ? off + n : mh[i].size;
    for (uint64_t at = off & ~(uint64_t)(PAGE_SIZE - 1); at < end;) {
        uint64_t asked = page_up(end) - at < REQ_BUF ? page_up(end) - at : REQ_BUF;
        struct pager_run run = { .offset = at, .len = asked };
        status_t st = jam_pager_dirty(vol.pager, key, PAGER_DIRTY_PEEK, &run, buf);
        if (st == ERR_NOT_FOUND)
            return OK;   /* gone, or cut off: nothing is dirty */
        if (st != OK) {
            if (!peek_said)
                printf("extfs %s: mapped file %lx: a read can't see its mappings' stores (%s)\n",
                       vol.name, (unsigned long)key, status_str(st));
            peek_said = true;
            return ERR_IO;
        }
        if (!run.len) {   /* none in the range */
            at += asked;
            continue;
        }
        uint64_t lo = run.offset > off ? run.offset : off;
        uint64_t hi = run.offset + run.len < end ? run.offset + run.len : end;
        if (lo < hi)
            memcpy(data + (lo - off), buf + (lo - at), hi - lo);
        at = run.offset + run.len;
    }
    return OK;
}

status_t maps_peek(uint32_t ino, uint64_t off, uint8_t *data, uint64_t n)
{
    for (unsigned i = 0; vol.pager && i < XF_MAX_MAPS; i++) {
        if (!shared(i, ino) || !kept->maps[i].written)
            continue;
        status_t st = peek(i, off, data, n);
        if (st != OK)
            return st;
    }
    return OK;
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

/* The pages the given-up request wrote into dropped (the header's (c)):
 * a clean one is fetched again from the disk, a dirty one stays. */
static void drop_written(void)
{
    for (unsigned i = 0; i < XF_MAX_MAPS; i++) {
        uint64_t first = mh[i].lo & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = page_up(mh[i].hi) < mh[i].size ? page_up(mh[i].hi) : mh[i].size;
        if (kept->maps[i].used && first < end)
            (void)jam_pager_op(vol.pager, kept->maps[i].key, PAGER_OP_INVALIDATE, first,
                               end - first, 0);
    }
}

void maps_settle(bool done)
{
    if (!moved)
        return;
    if (!done && vol.pager) {
        sizes_again();
        drop_written();
    }
    for (unsigned i = 0; i < XF_MAX_MAPS; i++)
        mh[i].lo = mh[i].hi = 0;
    moved = false;
}

/* ---- serving the pager ------------------------------------------------------------------ */

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
    int s = -1;
    uint32_t ro = vol.fs.ro;
    struct ext_inode in;
    status_t st = maps_key(r->key, &in, &s);
    if (s < 0 && (st == OK || st == ERR_NOT_FOUND)) {
        if (!unknown_said)
            printf("extfs %s: page requests for mapped files it doesn't know: detached\n",
                   vol.name);
        unknown_said = true;
        detach(r->key);
        return;
    }
    if (st != OK) {
        read_failed(r, r->offset, st, ro);
        return;
    }
    unsigned i = (unsigned)s;
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

/* A VMO with no handle, no mapping and no dirty page left: its slot
 * forgotten (one store), then the key freed; then its inode, if nothing
 * holds it and it has no name. */
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
static bool serve(void)
{
    struct pager_request r = again_first;
    status_t st = OK;
    if (again_has)
        again_has = false;
    else
        st = jam_pager_next(vol.pager, 0, &r);
    if (st != OK) {
        if (st != ERR_SHOULD_WAIT)
            printf("extfs %s: can't read the pager's requests: %s\n", vol.name,
                   status_str(st));
        return false;
    }
    if (r.kind == PAGER_REQ_GONE) {
        gone(r.key);
    } else if (r.kind == PAGER_REQ_READ) {
        read_request(&r);
    } else if (r.kind == PAGER_REQ_WRITEBACK) {
        wback_request(&r);
    } else {
        printf("extfs %s: a page request of a kind it doesn't know (%u)\n", vol.name, r.kind);
    }
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

void maps_again(void)
{
    if (!vol.pager || again_done)
        return;
    again_done = true;
    status_t st = jam_pager_next(vol.pager, PAGER_NEXT_AGAIN, &again_first);
    again_has = st == OK;
    if (st != OK && st != ERR_SHOULD_WAIT)
        printf("extfs %s: can't read the pager's requests: %s\n", vol.name, status_str(st));
}

void pager_start(void)
{
    if (!vol.pager)
        return;
    sizes_again();
    wback_start();
    arm();
    /* What the instance before took and never answered, queued again (if
     * a request run again from its slot hasn't yet), and the first of the
     * queue served. */
    maps_again();
    (void)serve();
}

void pager_event(void)
{
    if (!vol.pager)
        return;
    (void)serve();
    arm();   /* fires at once if more is queued */
}

void pager_reap(void)
{
    for (unsigned n = 0; vol.pager && n < REAP_MAX && serve(); n++)
        ;
}
