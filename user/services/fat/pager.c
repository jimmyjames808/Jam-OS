/* fat: mapped files and the mount's pager (docs/M12.5-PLAN.md, "fat as
 * the first pager"). volumes makes one pager for the mount and hands it to
 * every instance (FAT_SR_PAGER); file.map answers a VMO made from it, and
 * the kernel asks fat for that VMO's pages as its mappers touch them.
 *
 * The slots. Each VMO fat made is a slot of maps[] in the state: its file
 * (an entry of opens[], which it holds open as a slot of files[] does),
 * its kind and its key. The key is FAT_MAP_KEY(seq, slot): seq counts up
 * from the clock at the first map of a state set up empty, so a key is
 * never used twice for a pager, even by an instance that started fresh
 * while the VMOs of the one before still live. A VMO lives in its mappers,
 * never in the state, and fat names it only by its key:
 *   - FILE_MAP_READ: the file's shared VMO. fat holds a handle of its own
 *     (and the keeper a copy, slot FAT_KEEP_MAP(i)) while the file is open
 *     through fs.open, so every map of it then shares its pages; once the
 *     file's last fs.open goes, fat lets go (maps_let_go) and the VMO is
 *     its mappers' alone. A later map of the file then makes a new one.
 *   - FILE_MAP_EXEC: a VMO of the caller's own every time (the shell's
 *     vmo_make_exec_verified needs the only handle), never shared.
 * When the kernel says a VMO is gone (PAGER_REQ_GONE: no handle, no
 * mapping), fat forgets its slot, one store as a view's drop is, and
 * answers with PAGER_OP_DETACH, which frees the key. In that order: a
 * death between the two leaves a GONE the successor gets again
 * (PAGER_NEXT_AGAIN) for a key it doesn't know. A request for a key the
 * state doesn't know (a VMO an undone file.map made, or one of an instance
 * whose state was given up on) is answered with a detach: its mappers fail
 * as for a pulled stick.
 *
 * A page request is read with FatFs and supplied whole (one pager_supply
 * for its range: its waiters wake when the call that brings its last page
 * ends), the last page's tail past the end of the file zero; pages past
 * the end of the file are failed ERR_OUT_OF_RANGE, a read error ERR_IO.
 * A request changes nothing in the state, so it needs no slot and no
 * commit: a successor asks again. That holds for FatFs's objects too: the
 * read runs on copies in fat's own memory (a cursor FIL per slot, and a
 * copy of the volume with its window never written back), so a death in
 * the middle of one leaves the state's FIL and window as they were, and a
 * read never writes the sector an FS_GATHER file's write left in the
 * window. A mapped file is never open for writing (fileops.c), so its
 * chain of clusters doesn't change under a cursor.
 *
 * Requests are served one per turn of main.c's loop (the pager's binding
 * is ONCE, made again after each), and before each fs request
 * (pager_reap): a mapping let go and then the file removed are seen in
 * that order. A pager VMO is never mapped or read by fat: its own fault
 * would wait on itself. */
#include "fat.h"

#define REQ_BUF  (PAGER_REQ_PAGES * PAGE_SIZE)   /* bytes read and supplied at a time */
#define REAP_MAX 16u                             /* requests pager_reap serves at most */
#define FIL_DIRTY 0x80u   /* ff.c's FA_DIRTY: a FIL's sector buffer holds bytes not written */

/* Each slot's part in fat's own memory, by maps[]'s index. */
static struct {
    handle_t vmo;             /* fat's own handle of a shared VMO, while its file is open */
    bool     cur_ok;          /* cur is a cursor into the slot's file */
    FIL      cur;             /* where the last read of its pages left off (a copy of the
                               * file's FIL, its volume `scratch`) */
    uint64_t requests;        /* read requests answered by this instance */
    uint64_t pages;           /* pages it supplied */
    bool     mismatch_said;   /* its pages didn't match an approved program's: logged */
} mh[FAT_MAX_MAPS];

static FATFS scratch;         /* the volume as a read sees it: kept->fs, never written back */
static _Alignas(PAGE_SIZE) uint8_t buf[REQ_BUF];   /* a request's bytes, read and supplied */
static bool unknown_said;     /* a request for a key the state doesn't know was logged */
static bool arm_said;         /* the pager couldn't be waited on: logged */

static uint64_t page_up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* The slot whose VMO has `key`, or -1. */
static int slot_of(uint64_t key)
{
    unsigned i = FAT_MAP_SLOT(key);
    return i < FAT_MAX_MAPS && kept->maps[i].used && kept->maps[i].key == key ? (int)i : -1;
}

bool maps_use_open(uint32_t o)
{
    for (unsigned i = 0; i < FAT_MAX_MAPS; i++)
        if (kept->maps[i].used && kept->maps[i].open == o)
            return true;
    return false;
}

/* A key for slot i. seq starts from the clock (ns since boot, far below
 * 2^56), which passes every key an earlier instance of this boot made:
 * each map took far more than a nanosecond. */
static uint64_t next_key(unsigned i)
{
    if (!kept->map_seq)
        kept->map_seq = now();
    return FAT_MAP_KEY(++kept->map_seq, i);
}

/* Slot i's handle goes, and the keeper's copy (after the commit if an
 * operation runs: keeper.c). */
static void drop_handle(unsigned i)
{
    if (!mh[i].vmo)
        return;
    jam_handle_close(mh[i].vmo);
    mh[i].vmo = HANDLE_INVALID;
    kept_drop(FAT_KEEP_MAP(i));
}

/* ---- file.map --------------------------------------------------------------------- */

/* The slot of opens[o]'s shared VMO while fat holds it, or -1. */
static int shared_of(uint32_t o)
{
    for (unsigned i = 0; i < FAT_MAX_MAPS; i++) {
        const struct fat_map *m = &kept->maps[i];
        if (m->used && m->kind == FILE_MAP_READ && m->open == o && mh[i].vmo)
            return (int)i;
    }
    return -1;
}

/* A new VMO of `size` bytes for slot i, under a new key: the client's
 * handle into *out and, for a shared one, fat's into *mine (kept). */
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
        st = kept_put(FAT_KEEP_MAP(i), &v, 1);   /* before the state records the slot */
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

status_t maps_make(uint32_t o, uint32_t flags, handle_t *out, uint64_t *size)
{
    if (!vol.pager)
        return ERR_NOT_SUPPORTED;
    uint64_t fsz = f_size(&kept->opens[o].fil);
    int s = flags == FILE_MAP_READ ? shared_of(o) : -1;
    if (s >= 0) {
        status_t st = jam_handle_duplicate(mh[s].vmo, RIGHT_SAME, out);
        if (st != OK)
            return st;
        op_made(FAT_MADE_MAP, (uint32_t)s);
        *size = fsz;
        return OK;
    }
    unsigned i = 0;
    while (i < FAT_MAX_MAPS && kept->maps[i].used)
        i++;
    if (i == FAT_MAX_MAPS)
        return ERR_NO_RESOURCES;
    uint64_t key = 0;
    handle_t client = HANDLE_INVALID, mine = HANDLE_INVALID;
    drop_handle(i);   /* none: a free slot's went with it */
    status_t st = make_vmo(i, flags, fsz, &key, &client, &mine);
    if (st != OK)
        return st;
    undo_map(&kept->maps[i]);
    kept->maps[i] = (struct fat_map){
        .used = true, .kind = (uint8_t)flags, .open = o, .key = key, .size = page_up(fsz),
    };
    memset(&mh[i], 0, sizeof(mh[i]));
    mh[i].vmo = mine;
    op_made(FAT_MADE_MAP, i);
    test_die(FAT_DIE_MAP);
    *out = client;
    *size = fsz;
    return OK;
}

void maps_let_go(uint32_t o)
{
    for (unsigned i = 0; i < FAT_MAX_MAPS; i++)
        if (kept->maps[i].used && kept->maps[i].open == o)
            drop_handle(i);
}

void maps_drop_unknown(void)
{
    for (unsigned i = 0; i < FAT_MAX_MAPS; i++) {
        if (kept->maps[i].used)
            continue;
        drop_handle(i);
        mh[i].cur_ok = false;
    }
}

/* ---- a successor's mapped files (adopt.c) --------------------------------------------- */

bool maps_take(uint32_t i, const handle_t *hs, unsigned n)
{
    if (i >= FAT_MAX_MAPS || !kept->maps[i].used || kept->maps[i].kind != FILE_MAP_READ ||
        n != 1 || mh[i].vmo)
        return false;
    mh[i].vmo = hs[0];
    return true;
}

void maps_adopt(void)
{
    for (unsigned i = 0; i < FAT_MAX_MAPS; i++) {
        struct fat_map *m = &kept->maps[i];
        /* Without a pager nothing can serve its pages (volumes hands
         * every instance the same one: this is a test's fat). */
        if (m->used && !vol.pager)
            m->used = false;
        /* fat holds a shared VMO while its file is open, and only then:
         * a close that committed before the keeper heard of it. */
        if (!m->used || !kept->opens[m->open].refs)
            drop_handle(i);
    }
}

status_t maps_remake(uint32_t i, handle_t *out)
{
    if (i >= FAT_MAX_MAPS || !kept->maps[i].used || !vol.pager)
        return ERR_BAD_STATE;
    struct fat_map *m = &kept->maps[i];
    if (m->kind == FILE_MAP_READ)
        return mh[i].vmo ? jam_handle_duplicate(mh[i].vmo, RIGHT_SAME, out) : ERR_BAD_STATE;
    /* The caller's own VMO died with the instance: a new one, under a new
     * key (one store); the old key's GONE then finds a key nobody knows. */
    uint64_t key = 0;
    handle_t v = HANDLE_INVALID, none = HANDLE_INVALID;
    status_t st = make_vmo(i, FILE_MAP_EXEC, m->size, &key, &v, &none);
    if (st != OK)
        return st;
    m->key = key;
    mh[i].cur_ok = false;
    *out = v;
    return OK;
}

const char *maps_check(void)
{
    for (unsigned i = 0; i < FAT_MAX_MAPS; i++) {
        const struct fat_map *m = &kept->maps[i];
        if (!m->used)
            continue;
        if ((m->kind != FILE_MAP_READ && m->kind != FILE_MAP_EXEC) || m->open >= FAT_OPENS ||
            FAT_MAP_SLOT(m->key) != i || m->key >> 8 > kept->map_seq ||
            m->size % PAGE_SIZE || m->size > VMO_MAX_SIZE)
            return "a mapped file out of range";
    }
    return NULL;
}

/* ---- the requests --------------------------------------------------------------------- */

static void detach(uint64_t key)
{
    status_t st = jam_pager_op(vol.pager, key, PAGER_OP_DETACH, 0, 0, 0);
    if (st != OK && st != ERR_NOT_FOUND)
        printf("fat %s: can't detach mapped file %lx: %s\n", vol.name, (unsigned long)key,
               status_str(st));
}

/* Fail [off, off + len) of key's VMO with status (ERR_IO, ERR_OUT_OF_RANGE). */
static void fail(uint64_t key, uint64_t off, uint64_t len, status_t status)
{
    status_t st = jam_pager_op(vol.pager, key, PAGER_OP_FAIL, off, len, status);
    if (st != OK && st != ERR_NOT_FOUND)
        printf("fat %s: can't fail a page request of %lx: %s\n", vol.name, (unsigned long)key,
               status_str(st));
}

/* n bytes (whole pages) of slot i's file at `at` into buf, read from the
 * file (fsz bytes long) and zero past its end. */
static status_t read_pages(unsigned i, uint64_t at, uint32_t n, uint64_t fsz)
{
    const struct fat_open *o = &kept->opens[kept->maps[i].open];
    if (o->fil.flag & FIL_DIRTY)
        return ERR_BAD_STATE;   /* written: fileops.c never maps such a file */
    scratch = kept->fs;
    scratch.wflag = 0;   /* its window's changes are the volume's to write, not ours */
    FIL *c = &mh[i].cur;
    if (!mh[i].cur_ok) {
        *c = o->fil;
        c->flag &= (BYTE)~FA_WRITE;   /* a seek never grows it */
        mh[i].cur_ok = true;
    }
    c->obj.fs = &scratch;
    c->err = 0;
    uint32_t want = fsz - at < n ? (uint32_t)(fsz - at) : n;
    UINT got = 0;
    FRESULT fr = f_lseek(c, (FSIZE_t)at);
    if (fr == FR_OK)
        fr = f_read(c, buf, want, &got);
    if (fr == FR_OK && got != want)
        fr = FR_INT_ERR;
    if (fr != FR_OK) {
        mh[i].cur_ok = false;   /* where it stopped is unknown */
        return fr_status(fr);
    }
    memset(buf + got, 0, n - got);
    return OK;
}

/* pager_supply refused a range of slot i's: say why, and fail what is left. */
static void supply_failed(unsigned i, uint64_t key, uint64_t off, uint64_t len, status_t st)
{
    if (st == ERR_NOT_FOUND)
        return;   /* detached or gone meanwhile: nobody waits */
    if (st == ERR_IO) {
        /* A verified VMO (a program from /data): the disk's bytes are not
         * the approved ones. The kernel failed those pages' requests. */
        if (!mh[i].mismatch_said)
            printf("fat %s: mapped file %lx: the disk's pages don't match the approved "
                   "program's\n", vol.name, (unsigned long)key);
        mh[i].mismatch_said = true;
        return;
    }
    printf("fat %s: mapped file %lx: pages not supplied: %s\n", vol.name, (unsigned long)key,
           status_str(st));
    fail(key, off, len, ERR_IO);
}

/* A read request: its range, as far as the file goes, supplied. */
static void read_request(const struct pager_request *r)
{
    int s = slot_of(r->key);
    if (s < 0) {
        if (!unknown_said)
            printf("fat %s: page requests for mapped files it doesn't know: detached\n",
                   vol.name);
        unknown_said = true;
        detach(r->key);
        return;
    }
    unsigned i = (unsigned)s;
    const struct fat_map *m = &kept->maps[i];
    uint64_t fsz = f_size(&kept->opens[m->open].fil), off = r->offset;
    if (off >= m->size) {
        fail(r->key, off, r->len ? r->len : PAGE_SIZE, ERR_OUT_OF_RANGE);
        return;
    }
    uint64_t end = r->len > m->size - off ? m->size : off + r->len;
    uint64_t data_end = page_up(fsz) < end ? page_up(fsz) : end;
    mh[i].requests++;
    reply_flush();   /* the last request's answer goes before the disk is read */
    for (uint64_t at = off, n = 0; at < data_end; at += n) {
        n = data_end - at < REQ_BUF ? data_end - at : REQ_BUF;
        status_t st = read_pages(i, at, (uint32_t)n, fsz);
        if (st != OK) {
            printf("fat %s: mapped file %lx: can't read %lu bytes at %lu: %s\n", vol.name,
                   (unsigned long)r->key, (unsigned long)n, (unsigned long)at, status_str(st));
            fail(r->key, off, end - off, ERR_IO);
            return;
        }
        test_die(FAT_DIE_SUPPLY);
        st = jam_pager_supply(vol.pager, r->key, at, buf, n);
        if (st != OK) {
            supply_failed(i, r->key, off, end - off, st);
            return;
        }
        mh[i].pages += n / PAGE_SIZE;
    }
    if (data_end < end)   /* the file is shorter than its VMO */
        fail(r->key, data_end, end - data_end, ERR_OUT_OF_RANGE);
}

/* A VMO with no handle and no mapping left: its slot forgotten (one
 * store), then the key freed. */
static void gone(uint64_t key)
{
    int s = slot_of(key);
    if (s >= 0) {
        unsigned i = (unsigned)s;
        printf("fat %s: mapped file %lx gone: %lu page requests, %lu pages supplied\n",
               vol.name, (unsigned long)key, (unsigned long)mh[i].requests,
               (unsigned long)mh[i].pages);
        kept->maps[i].used = false;
        drop_handle(i);   /* none: fat's handle would have kept it */
        memset(&mh[i], 0, sizeof(mh[i]));
    }
    detach(key);
}

/* One request, if one is queued: true if one was served. */
static bool serve(uint32_t flags)
{
    struct pager_request r;
    status_t st = jam_pager_next(vol.pager, flags, &r);
    if (st != OK) {
        if (st != ERR_SHOULD_WAIT)
            printf("fat %s: can't read the pager's requests: %s\n", vol.name, status_str(st));
        return false;
    }
    if (r.kind == PAGER_REQ_GONE)
        gone(r.key);
    else if (r.kind == PAGER_REQ_READ)
        read_request(&r);
    else
        printf("fat %s: a page request of a kind it doesn't know (%u)\n", vol.name, r.kind);
    return true;
}

/* Wait (ONCE) for the next request. */
static void arm(void)
{
    status_t st = jam_port_bind(vol.port, vol.pager, FAT_KEY_PAGER, SIG_READABLE,
                                PORT_BIND_ONCE);
    if (st != OK && !arm_said)
        printf("fat %s: can't wait on the pager (%s): its requests end at their deadline\n",
               vol.name, status_str(st));
    arm_said |= st != OK;
}

void pager_start(void)
{
    if (!vol.pager)
        return;
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
