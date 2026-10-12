/* ext: the directory hash. A lookup in a plain directory is a walk of its
 * every entry; with the hash, the first lookup in a directory reads it in
 * whole (each name's hash and byte offset), and every later one is a
 * probe: the bucket for the name's hash, and for each record there of the
 * same directory and hash, the one block holding that offset, walked to
 * the entry and its name compared. A name the hash doesn't hold isn't in
 * the directory.
 *
 * That last rule is why the hash must never miss a name: every entry
 * added or removed (dirent.c) is added or removed here too, a directory
 * whose records can't all be kept is dropped from the hash (its lookups
 * walk again), and the hash starts over after a transaction is given up
 * (ext_discard: its removals are undone on the disk). A record that turns
 * out not to start an entry is a broken promise: the directory is dropped
 * and the lookup walks. A directory is known by its inode number and
 * generation, so a number freed and reused is another directory.
 *
 * Memory: the caller's region, carved once: the slots for directories,
 * the records, the buckets. When the records run out the least recently
 * used directory goes; a directory with more names than the whole hash
 * holds is never kept (its lookups are walks, counted in `linear`).
 *
 * The hash also keeps, per directory, where an add may start looking for
 * a gap (room_from and room_need: every block before room_from lacks a
 * gap of room_need bytes), so filling a big directory doesn't walk it
 * whole for each new name. */
#include "names_int.h"

#define SLOT_NAMES 64      /* records per directory slot, when carving */
#define SLOTS_MIN  4
#define SLOTS_MAX  256

/* FNV-1a: names are short, and only the bucket and a compare depend on it. */
static uint32_t name_hash(const char *s, uint32_t len)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static uint32_t slots_for(uint32_t nents)
{
    uint32_t n = nents / SLOT_NAMES;
    return n < SLOTS_MIN ? SLOTS_MIN : n > SLOTS_MAX ? SLOTS_MAX : n;
}

static uint32_t buckets_for(uint32_t nents)
{
    uint32_t n = 1;
    while (n < nents)
        n <<= 1;
    return n;
}

size_t ext_names_mem(uint32_t nents)
{
    return 16 + sizeof(struct ext_names) + slots_for(nents) * sizeof(struct ext_names_dir) +
           (size_t)nents * sizeof(struct ext_names_ent) + (size_t)buckets_for(nents) * 4;
}

static void reset(struct ext_names *nm)
{
    __builtin_memset(nm->dirs, 0, nm->ndirs * sizeof(nm->dirs[0]));
    __builtin_memset(nm->heads, 0, (size_t)buckets_for(nm->nents) * 4);
    for (uint32_t i = 0; i < nm->nents; i++)
        nm->ents[i].dnext = i + 1 < nm->nents ? i + 2 : 0;
    nm->free = 1;
}

status_t ext_names_init(struct ext_fs *fs, void *mem, size_t size)
{
    uint32_t n = (uint32_t)(size / (sizeof(struct ext_names_ent) + 8) > UINT32_MAX / 2
                                ? UINT32_MAX / 2
                                : size / (sizeof(struct ext_names_ent) + 8));
    while (n >= 16 && ext_names_mem(n) > size)
        n--;
    if (!mem || n < 16)
        return ERR_NO_MEMORY;
    uintptr_t p = ((uintptr_t)mem + 7) & ~(uintptr_t)7;
    struct ext_names *nm = (struct ext_names *)p;
    __builtin_memset(nm, 0, sizeof(*nm));
    p += sizeof(*nm);
    nm->ndirs = slots_for(n);
    nm->dirs = (struct ext_names_dir *)p;
    p += nm->ndirs * sizeof(nm->dirs[0]);
    nm->nents = n;
    nm->ents = (struct ext_names_ent *)p;
    p += (size_t)n * sizeof(nm->ents[0]);
    nm->heads = (uint32_t *)p;
    nm->shift = 64 - (uint32_t)__builtin_ctz(buckets_for(n));
    nm->discards = fs->cache.discards;
    reset(nm);
    fs->names = nm;
    return OK;
}

void ext_names_reset(struct ext_fs *fs)
{
    if (fs->names)
        reset(fs->names);
}

/* fs's hash, in step with the cache: NULL when there is none. */
static struct ext_names *names(struct ext_fs *fs)
{
    struct ext_names *nm = fs->names;
    if (nm && nm->discards != fs->cache.discards) {
        reset(nm);
        nm->discards = fs->cache.discards;
    }
    return nm;
}

static uint32_t *bucket(struct ext_names *nm, uint32_t slot, uint32_t hash)
{
    uint64_t k = ((uint64_t)slot << 32 | hash) * 0x9E3779B97F4A7C15ull;
    return &nm->heads[nm->shift >= 64 ? 0 : k >> nm->shift];
}

static struct ext_names_dir *slot_of(struct ext_names *nm, const struct ext_inode *dir)
{
    for (uint32_t i = 0; i < nm->ndirs; i++)
        if (nm->dirs[i].ino == dir->ino && nm->dirs[i].gen == dir->d.generation)
            return &nm->dirs[i];
    return NULL;
}

/* Take record i out of its bucket and its directory, onto the free list. */
static void remove_ent(struct ext_names *nm, uint32_t i)
{
    struct ext_names_ent *e = &nm->ents[i];
    struct ext_names_dir *d = &nm->dirs[e->dir];
    for (uint32_t *p = bucket(nm, e->dir, e->hash); *p; p = &nm->ents[*p - 1].next)
        if (*p == i + 1) {
            *p = e->next;
            break;
        }
    if (e->dprev)
        nm->ents[e->dprev - 1].dnext = e->dnext;
    else
        d->first = e->dnext;
    if (e->dnext)
        nm->ents[e->dnext - 1].dprev = e->dprev;
    d->count--;
    e->dnext = nm->free;
    nm->free = i + 1;
}

static void drop_slot(struct ext_names *nm, struct ext_names_dir *d)
{
    while (d->first)
        remove_ent(nm, d->first - 1);
    __builtin_memset(d, 0, sizeof(*d));
}

/* Drop the least recently used directory other than `keep`; false if none. */
static bool evict_other(struct ext_names *nm, const struct ext_names_dir *keep)
{
    struct ext_names_dir *old = NULL;
    for (uint32_t i = 0; i < nm->ndirs; i++) {
        struct ext_names_dir *d = &nm->dirs[i];
        if (d->ino && d != keep && (!old || d->used < old->used))
            old = d;
    }
    if (old)
        drop_slot(nm, old);
    return old != NULL;
}

/* Record name hash -> pos for directory slot d; false if no room is left
 * even after dropping every other directory. */
static bool insert(struct ext_names *nm, struct ext_names_dir *d, uint32_t hash, uint64_t pos)
{
    while (!nm->free)
        if (!evict_other(nm, d))
            return false;
    uint32_t i = nm->free - 1, slot = (uint32_t)(d - nm->dirs);
    struct ext_names_ent *e = &nm->ents[i];
    nm->free = e->dnext;
    uint32_t *head = bucket(nm, slot, hash);
    *e = (struct ext_names_ent){ .pos = pos, .hash = hash, .next = *head, .dnext = d->first,
                                 .dprev = 0, .dir = slot };
    *head = i + 1;
    if (d->first)
        nm->ents[d->first - 1].dprev = i + 1;
    d->first = i + 1;
    d->count++;
    return true;
}

/* A slot for dir: a free one, or the least recently used one emptied. */
static struct ext_names_dir *take_slot(struct ext_names *nm, const struct ext_inode *dir)
{
    struct ext_names_dir *d = NULL;
    for (uint32_t i = 0; i < nm->ndirs && !d; i++)
        if (!nm->dirs[i].ino)
            d = &nm->dirs[i];
    if (!d) {
        d = &nm->dirs[0];
        for (uint32_t i = 1; i < nm->ndirs; i++)
            if (nm->dirs[i].used < d->used)
                d = &nm->dirs[i];
        drop_slot(nm, d);
    }
    d->ino = dir->ino;
    d->gen = dir->d.generation;
    d->used = ++nm->clock;
    return d;
}

/* Read dir in whole into a slot. ERR_NO_RESOURCES: it doesn't fit. */
static status_t build(struct ext_fs *fs, struct ext_names *nm, struct ext_inode *dir,
                      struct ext_names_dir **out)
{
    struct ext_names_dir *d = take_slot(nm, dir);
    struct ext_dentry de;
    nm->builds++;
    for (uint64_t pos = 0;;) {
        status_t st = ext_dir_next(fs, dir, &pos, &de);
        if (st == OK && de.ino && !insert(nm, d, name_hash(de.name, de.name_len), de.pos))
            st = ERR_NO_RESOURCES;
        if (st != OK) {
            drop_slot(nm, d);
            return st;
        }
        if (!de.ino)
            break;
    }
    *out = d;
    return OK;
}

/* The entry at pos, if it is named `name`: OK (filled in), ERR_NOT_FOUND
 * (another name with the same hash), ERR_BAD_STATE (no entry starts at
 * pos: the hash is out of step). */
static status_t check_entry(struct ext_fs *fs, struct ext_inode *dir, uint64_t pos,
                            const struct ext_name *name, struct ext_dentry *out)
{
    struct ext_buf *b;
    enum ext_dirblk kind;
    if (pos >> fs->block_log > UINT32_MAX)
        return ERR_BAD_STATE;
    status_t st = ext_dir_block(fs, dir, (uint32_t)(pos >> fs->block_log), &b, &kind);
    if (st != OK)
        return st;
    uint32_t want = (uint32_t)(pos & (fs->block_size - 1));
    uint32_t limit = kind == EXT_DIRBLK_LEAF ? ext_leaf_limit(fs) : fs->block_size, off = 0;
    while (off < want && off < limit)
        off += ext_rec_len(fs, ((const struct ext_dirent *)(b->data + off))->rec_len);
    const struct ext_dirent *d = (const struct ext_dirent *)(b->data + off);
    if (off != want || off >= limit || !d->inode || kind == EXT_DIRBLK_DX_NODE)
        st = ERR_BAD_STATE;
    else if (d->name_len != name->len || __builtin_memcmp(d->name, name->s, name->len))
        st = ERR_NOT_FOUND;
    if (st == OK) {
        out->ino = d->inode;
        out->type = fs->filetype && d->file_type < EXT_FT_MAX ? d->file_type : EXT_FT_UNKNOWN;
        out->name_len = (uint8_t)name->len;
        out->pos = pos;
        __builtin_memcpy(out->name, name->s, name->len);
        out->name[name->len] = '\0';
    }
    ext_brelse(fs, b);
    return st;
}

/* Probe d's records for name. */
static status_t probe(struct ext_fs *fs, struct ext_names *nm, struct ext_names_dir *d,
                      struct ext_inode *dir, const struct ext_name *name, struct ext_dentry *out)
{
    uint32_t h = name_hash(name->s, name->len), slot = (uint32_t)(d - nm->dirs);
    for (uint32_t i = *bucket(nm, slot, h); i; i = nm->ents[i - 1].next) {
        const struct ext_names_ent *e = &nm->ents[i - 1];
        if (e->dir != slot || e->hash != h)
            continue;
        status_t st = check_entry(fs, dir, e->pos, name, out);
        if (st != ERR_NOT_FOUND)
            return st;
    }
    return ERR_NOT_FOUND;
}

status_t ext_lookup(struct ext_fs *fs, struct ext_inode *dir, const struct ext_name *name,
                    struct ext_dentry *out)
{
    struct ext_names *nm = names(fs);
    if (ext_itype(dir) != EXT_FT_DIR)
        return ERR_NOT_DIR;
    if (!nm || !name->len || name->len > EXT_NAME_MAX)
        return ext_dir_lookup(fs, dir, name->s, name->len, out);
    struct ext_names_dir *d = slot_of(nm, dir);
    status_t st = d ? OK : build(fs, nm, dir, &d);
    if (st == OK) {
        d->used = ++nm->clock;
        st = probe(fs, nm, d, dir, name, out);
        if (st == ERR_BAD_STATE)
            drop_slot(nm, d);
    }
    if (st == ERR_NO_RESOURCES || st == ERR_BAD_STATE) {
        nm->linear++;
        return ext_dir_lookup(fs, dir, name->s, name->len, out);
    }
    if (st == OK)
        nm->hits++;
    else if (st == ERR_NOT_FOUND)
        nm->misses++;
    return st;
}

void ext_names_added(struct ext_fs *fs, const struct ext_inode *dir, const struct ext_name *n,
                     uint64_t pos)
{
    struct ext_names *nm = names(fs);
    struct ext_names_dir *d = nm ? slot_of(nm, dir) : NULL;
    if (d && !insert(nm, d, name_hash(n->s, n->len), pos))
        drop_slot(nm, d);
}

void ext_names_removed(struct ext_fs *fs, const struct ext_inode *dir, const char *name,
                       uint32_t len, uint64_t pos)
{
    struct ext_names *nm = names(fs);
    struct ext_names_dir *d = nm ? slot_of(nm, dir) : NULL;
    if (!d)
        return;
    uint32_t h = name_hash(name, len), slot = (uint32_t)(d - nm->dirs);
    for (uint32_t i = *bucket(nm, slot, h); i; i = nm->ents[i - 1].next)
        if (nm->ents[i - 1].dir == slot && nm->ents[i - 1].pos == pos) {
            remove_ent(nm, i - 1);
            return;
        }
    drop_slot(nm, d);   /* it had no record of the entry: out of step */
}

void ext_names_forget(struct ext_fs *fs, uint32_t ino)
{
    struct ext_names *nm = names(fs);
    for (uint32_t i = 0; nm && i < nm->ndirs; i++)
        if (nm->dirs[i].ino == ino)
            drop_slot(nm, &nm->dirs[i]);
}

uint32_t ext_names_room_from(struct ext_fs *fs, const struct ext_inode *dir, uint32_t need)
{
    struct ext_names *nm = names(fs);
    struct ext_names_dir *d = nm ? slot_of(nm, dir) : NULL;
    return d && need >= d->room_need ? d->room_from : 0;
}

void ext_names_room_found(struct ext_fs *fs, const struct ext_inode *dir, uint32_t lblk,
                          uint32_t need)
{
    struct ext_names *nm = names(fs);
    struct ext_names_dir *d = nm ? slot_of(nm, dir) : NULL;
    if (d) {
        d->room_from = lblk;
        d->room_need = need;
    }
}

void ext_names_room_freed(struct ext_fs *fs, const struct ext_inode *dir, uint32_t lblk)
{
    struct ext_names *nm = names(fs);
    struct ext_names_dir *d = nm ? slot_of(nm, dir) : NULL;
    if (d && lblk < d->room_from)
        d->room_from = lblk;
}
