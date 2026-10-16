/* Memory (docs/M13-PLAN.md section 12): mmap, munmap, mprotect, madvise,
 * msync and brk, over VMOs and the address space's vmar calls.
 *
 * Each mmap is a VMO of its own, mapped with vmar_map and kept in libc's
 * table (maptable.h) with its handle: munmap of part of it unmaps that
 * part (the kernel splits the mapping, the table splits the entry), and
 * the VMO goes when nothing maps it any more. A part of private
 * anonymous memory unmapped or mapped over gives its pages back at once
 * (decommitted: nobody else could ever see them). madvise(MADV_DONTNEED)
 * and MADV_FREE decommit private memory's pages (they read as zero
 * after, as Linux's do); a MAP_SHARED mapping's pages are the memfd's
 * (or the other holders'), so they stay, as Linux keeps the data and
 * drops only its own view of it.
 * Pages are committed as they are touched and charged to the process's
 * job then.
 *
 * What the address space allows: never write with execute, and no
 * execute on anonymous memory (EACCES), but in JIT memory (below). A descriptor's
 * mapping is file.c's file_map (files.h): a file on a pager's mount
 * (fat) on its own pages, the file's VMO itself for MAP_SHARED (read
 * only) and for a read-only MAP_PRIVATE, a copy-on-write child of it for
 * a writable MAP_PRIVATE; a file elsewhere, or a memfd, as a copy or the
 * memfd's own VMO. Each is then mapped and kept in the table as an
 * anonymous one is; a read-only private mapping on a file's own pages is
 * marked (MT_PAGER_PRIVATE), and an mprotect that adds write to it puts
 * a child in its place first (the file's VMO is never writable).
 * msync has nothing to write (lx_msync says why). mremap is ENOSYS
 * (musl's realloc copies); mlock and its kin succeed (nothing is paged
 * out).
 *
 * Reservations (docs/CR-PLAN.md section 2): a big PROT_NONE mapping is
 * one VMO and one mapping like any other, and costs nothing until
 * touched; mprotect commits parts of it (the kernel splits the mapping;
 * pages still come on touch), and MAP_FIXED anonymous inside libc's own
 * private anonymous memory decommits in place (refresh_locked): the
 * same VMO, its pages there taken back, the new protection set, so the
 * table keeps one entry per reservation however it is cut. madvise
 * decommits too, and munmap gives a part's pages back. A mapping is at
 * most a VMO's size (VMO_MAX_SIZE, 128 TiB) and what the address space
 * can hold: ENOMEM past either.
 *
 * Any other MAP_FIXED over the table's mappings replaces
 * them in one step (vmar_map's VMAR_REPLACE): another thread reading the
 * range sees the old pages or the new ones, never a hole. The holes
 * between them are filled with the new mapping's pages first, as plain
 * fixed maps, so one that holds anything else (the program, its stack,
 * libjam's mappings) fails the call with ENOMEM before anything changed:
 * the replace never takes away what libc doesn't own. MAP_FIXED where
 * the table has nothing is a plain fixed map (ENOMEM over anything else),
 * and MAP_FIXED_NOREPLACE over anything is EEXIST.
 *
 * JIT memory (docs/CR-PLAN.md section 12): mmap with MAP_JIT (private
 * anonymous only, else EINVAL) in a program holding RIGHT_ROOT_JIT (its
 * list's `right jit`, approved) maps a JIT VMO (vmo_make_jit), the one
 * kind the kernel lets a mapping hold writable and executable; the table
 * marks it MT_JIT, and mmap and mprotect allow PROT_EXEC, alone or with
 * PROT_WRITE, there and nowhere else. A program may keep it writable and
 * executable (V8's way on x86-64), or switch it between PROT_READ |
 * PROT_WRITE and PROT_READ | PROT_EXEC with mprotect. Without the power
 * MAP_JIT is EPERM. MAP_FIXED | MAP_JIT over JIT memory decommits in place
 * as refresh_locked does; a MAP_FIXED without MAP_JIT over it (or with it
 * over other memory) replaces it, so a range is JIT memory exactly when the
 * last mmap there said MAP_JIT.
 *
 * brk: a 1 GiB reservation (one VMO, mapped on the first call, in the
 * table like any mapping), the break moving inside it; pages above a
 * lowered break are decommitted.
 *
 * One lock (mem_lock) covers the table and the break, and is held across
 * each call's kernel calls, so two threads never interleave a split. */
#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "files.h"
#include "maptable.h"

#define BRK_SIZE (1ull << 30)   /* the break's reservation */
#define PAGE     4096ull

#define MAP_KNOWN (MAP_TYPE | MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE | MAP_DENYWRITE | \
                   MAP_EXECUTABLE | MAP_LOCKED | MAP_POPULATE | MAP_NONBLOCK | MAP_STACK |  \
                   MAP_FIXED_NOREPLACE | MAP_JIT)
#define PROT_KNOWN (PROT_READ | PROT_WRITE | PROT_EXEC)

static lock_t   mem_lock;
static uint64_t brk_base, brk_cur;   /* the reservation and the break (0: not made) */
static handle_t brk_vmo;

static bool page_aligned(uint64_t x)
{
    return !(x & (PAGE - 1));
}

/* len rounded up to whole pages; 0 if it would wrap. */
static uint64_t page_round(uint64_t len)
{
    return len > UINT64_MAX - (PAGE - 1) ? 0 : (len + PAGE - 1) & ~(PAGE - 1);
}

/* PROT_* as vmar permissions; -EACCES for execute but in JIT memory (jit),
 * -EINVAL for a bit we don't know. Write and execute need read on x86. */
static long vmar_perms(long prot, bool jit, uint32_t *out)
{
    if (prot & ~(long)PROT_KNOWN)
        return -EINVAL;
    if ((prot & PROT_EXEC) && !jit)
        return -EACCES;
    *out = (prot ? VMAR_READ : 0) | (prot & PROT_WRITE ? VMAR_WRITE : 0) |
           (prot & PROT_EXEC ? VMAR_EXEC : 0);
    return 0;
}

/* Does [addr, addr + len) overlap one of the table's entries? */
static bool table_overlaps(uint64_t addr, uint64_t len)
{
    unsigned i = mt_first_after(addr);
    return i < mt_count() && mt_entries()[i].base < addr + len;
}

/* op (jam_vmo_commit or jam_vmo_decommit) on the VMO range behind each
 * table entry's part of [addr, end), skipping entries of the kinds in
 * skip. Advice: a part the op fails on is left as it was. */
static void parts_locked(uint64_t addr, uint64_t end, uint32_t skip,
                         status_t (*op)(handle_t vmo, uint64_t off, uint64_t len))
{
    const struct mt_entry *t = mt_entries();
    for (unsigned i = mt_first_after(addr); i < mt_count() && t[i].base < end; i++) {
        uint64_t s = t[i].base > addr ? t[i].base : addr;
        uint64_t f = t[i].base + t[i].len < end ? t[i].base + t[i].len : end;
        if (!(t[i].kind & skip))
            (void)op(t[i].vmo, t[i].vmo_off + (s - t[i].base), f - s);   /* advice */
    }
}

/* What drop_locked does to the address space and the pages. */
enum drop_how {
    DROP_UNMAP,      /* munmap: unmap each part, then give back its pages (drop_pages) */
    DROP_REPLACED,   /* the kernel has mapped something else there: give back the pages */
    DROP_FORGET,     /* someone else unmaps it (a thread's own stack, still running on it) */
};

/* Entry i's part [s, f), which nothing maps any more: its pages given
 * back now if the entry is private anonymous memory whose VMO lives on
 * (the rest of the entry, or another entry split from it, still maps it).
 * Nobody else holds that VMO, so the pages could never be seen again, yet
 * would stay committed and charged until the whole VMO goes: a
 * reservation's pieces unmapped or mapped over one by one would never
 * give anything back. A VMO no other part maps goes whole with its
 * mapping and handle. */
static void drop_pages(unsigned i, uint64_t s, uint64_t f)
{
    const struct mt_entry *e = &mt_entries()[i];
    bool whole = s == e->base && f == e->base + e->len;
    if ((e->kind & MT_ANON) && (!whole || mt_vmo_shared(i)))
        (void)jam_vmo_decommit(e->vmo, e->vmo_off + (s - e->base), f - s);   /* ours: can't fail */
}

/* Take the table's mappings in [addr, end) out of it: each entry's part
 * there, trimming, splitting or dropping the entry. A split needs a free
 * entry, checked before anything changes. With DROP_UNMAP each part is
 * unmapped first; a failure there stops it, with the parts before gone. */
static long drop_locked(uint64_t addr, uint64_t end, enum drop_how how)
{
    struct mt_entry *t = mt_entries();
    unsigned i = mt_first_after(addr);
    if (i < mt_count() && t[i].base < addr && t[i].base + t[i].len > end && !mt_room(1))
        return -ENOMEM;
    while (i < mt_count() && t[i].base < end) {
        struct mt_entry e = t[i];
        uint64_t s = e.base > addr ? e.base : addr;
        uint64_t f = e.base + e.len < end ? e.base + e.len : end;
        status_t st = how == DROP_UNMAP ? jam_vmar_unmap(lx_self_vmar(), s, f - s) : OK;
        if (st != OK)
            return lx_err(st);
        if (how != DROP_FORGET)
            drop_pages(i, s, f);
        if (s == e.base && f == e.base + e.len) {
            mt_remove(i);   /* i is now the next entry */
            continue;
        }
        if (s == e.base) {          /* the head went */
            t[i].base = f;
            t[i].vmo_off += f - e.base;
            t[i].len -= f - e.base;
        } else {                    /* the tail went, or the middle */
            t[i].len = s - e.base;
            if (f < e.base + e.len) {
                struct mt_entry tail = { f, e.base + e.len - f, e.vmo_off + (f - e.base), e.vmo,
                                         e.kind };
                mt_insert(&tail);
            }
        }
        i++;
    }
    return 0;
}

static long unmap_locked(uint64_t addr, uint64_t end)
{
    return drop_locked(addr, end, DROP_UNMAP);
}

void mem_forget(uint64_t addr, uint64_t len)
{
    if (!len || addr > UINT64_MAX - len)
        return;
    lock_take(&mem_lock);
    (void)drop_locked(addr, addr + len, DROP_FORGET);   /* a split's room: a stack is one entry */
    lock_give(&mem_lock);
}

/* The table whole across fork's clone (layer.h). In the child, JIT memory
 * is gone (the kernel leaves a writable JIT mapping out of a clone, and
 * the child holds no handle to the VMO: docs/FORK-PLAN.md section 1.10),
 * so its entries go, their handles forgotten, not closed (the parent's).
 * The other entries' handles are the parent's until FL2 gives them the
 * clone's children. */
void mem_fork(int step)
{
    if (step == LX_FORK_TAKE) {
        lock_take(&mem_lock);
        return;
    }
    if (step == LX_FORK_GIVE) {
        lock_give(&mem_lock);
        return;
    }
    mem_lock = (lock_t){ 0 };
    struct mt_entry *t = mt_entries();
    for (unsigned i = 0; i < mt_count(); i++)
        if (t[i].kind & MT_JIT)
            t[i].vmo = HANDLE_INVALID;   /* so mt_remove closes nothing */
    for (unsigned i = 0; i < mt_count();)
        if (t[i].kind & MT_JIT)
            mt_remove(i);
        else
            i++;
}

/* -errno for a mapping whose VMO couldn't be made: a length past what a
 * VMO may hold (VMO_MAX_SIZE, 128 TiB) is ENOMEM, Linux's answer for more
 * than the address space can give, which V8 and PartitionAlloc take as
 * the cue to reserve less. */
static long create_err(status_t st)
{
    return st == ERR_OUT_OF_RANGE ? -ENOMEM : lx_err(st);
}

/* A new VMO of len bytes for a mapping of kind (MT_*): JIT memory for
 * MT_JIT (EPERM without RIGHT_ROOT_JIT on our root), else plain. */
static long fresh_vmo(uint64_t len, uint32_t kind, handle_t *out)
{
    handle_t root = startup_handle(SR_RESOURCE), vmo;
    if ((kind & MT_JIT) && !root)
        return -EPERM;
    status_t st = jam_vmo_create(len, 0, &vmo);
    if (st != OK)
        return create_err(st);
    if (!(kind & MT_JIT)) {
        *out = vmo;
        return 0;
    }
    st = jam_vmo_make_jit(vmo, root, out);   /* vmo is gone, whatever the status */
    return st == ERR_ACCESS_DENIED || st == ERR_WRONG_TYPE ? -EPERM : create_err(st);
}

/* What a mapping maps: a VMO made for it (vmo HANDLE_INVALID: a new
 * anonymous one) or a descriptor's (file_map), from vmo_off; and the kind
 * the table records for it (maptable.h's MT_*). */
struct map_src {
    handle_t vmo;
    uint64_t vmo_off;
    uint32_t kind;
};

/* src's VMO (or a new one) mapped for len bytes with perms (at *addr
 * exactly if fixed), entered in the table. src->vmo is consumed. */
static long map_new(uint64_t *addr, uint64_t len, uint32_t perms, bool fixed, bool populate,
                    const struct map_src *src)
{
    handle_t vmo = src->vmo;
    if (!mt_room(1)) {
        if (vmo)
            jam_handle_close(vmo);
        return -ENOMEM;
    }
    long r = vmo ? 0 : fresh_vmo(len, src->kind, &vmo);
    if (r)
        return r;
    status_t st = jam_vmar_map(lx_self_vmar(), vmo, src->vmo_off, len,
                               perms | (fixed ? VMAR_FIXED : 0), addr);
    if (st != OK) {
        jam_handle_close(vmo);
        return st == ERR_ALREADY_BOUND || st == ERR_NO_RESOURCES ? -ENOMEM : lx_err(st);
    }
    if (populate)
        (void)jam_vmo_commit(vmo, src->vmo_off, len);   /* a hint: pages still come on touch */
    struct mt_entry e = { *addr, len, src->vmo_off, vmo, src->kind };
    mt_insert(&e);
    return 0;
}

/* The next hole the table leaves in [*at, end), moving *at to its start:
 * true and the hole [*at, *hole_end), or false if there is none. */
static bool next_hole(uint64_t *at, uint64_t end, uint64_t *hole_end)
{
    const struct mt_entry *t = mt_entries();
    for (unsigned i = mt_first_after(*at); *at < end; i++) {
        if (i >= mt_count() || t[i].base >= end) {
            *hole_end = end;
            return true;
        }
        if (t[i].base > *at) {
            *hole_end = t[i].base;
            return true;
        }
        *at = t[i].base + t[i].len;   /* inside entry i: on past it */
    }
    return false;
}

/* fill_holes' mappings in [addr, stop) unmapped again. */
static void unfill_holes(uint64_t addr, uint64_t stop)
{
    uint64_t hole_end;
    for (uint64_t a = addr; next_hole(&a, stop, &hole_end); a = hole_end)
        (void)jam_vmar_unmap(lx_self_vmar(), a, hole_end - a);   /* ours, just made */
}

/* MAP_FIXED's first step: vmo (from vmo_off at addr) mapped into every
 * hole the table leaves in [addr, end). A hole holding a mapping libc
 * didn't make fails it (ENOMEM), with the holes filled so far emptied. */
static long fill_holes(uint64_t addr, uint64_t end, handle_t vmo, uint64_t vmo_off,
                       uint32_t perms)
{
    uint64_t hole_end;
    for (uint64_t a = addr; next_hole(&a, end, &hole_end); a = hole_end) {
        uint64_t at = a;
        status_t st = jam_vmar_map(lx_self_vmar(), vmo, vmo_off + (a - addr), hole_end - a,
                                   perms | VMAR_FIXED, &at);
        if (st != OK) {
            unfill_holes(addr, a);
            return st == ERR_ALREADY_BOUND || st == ERR_NO_RESOURCES ? -ENOMEM : lx_err(st);
        }
    }
    return 0;
}

/* MAP_FIXED over the table's mappings: src's VMO (or a new one) in place
 * of [addr, addr + len) in one step, then the table forgets what was
 * replaced. src->vmo is consumed. */
static long replace_locked(uint64_t addr, uint64_t len, uint32_t perms, bool populate,
                           const struct map_src *src)
{
    handle_t vmo = src->vmo;
    if (!mt_room(2)) {   /* the new entry, and the tail of one that reaches past both ends */
        if (vmo)
            jam_handle_close(vmo);
        return -ENOMEM;
    }
    long r = vmo ? 0 : fresh_vmo(len, src->kind, &vmo);
    if (r)
        return r;
    status_t st;
    r = fill_holes(addr, addr + len, vmo, src->vmo_off, perms);
    uint64_t at = addr;
    if (!r && (st = jam_vmar_map(lx_self_vmar(), vmo, src->vmo_off, len,
                                 perms | VMAR_FIXED | VMAR_REPLACE, &at)) != OK) {
        unfill_holes(addr, addr + len);
        r = st == ERR_NO_RESOURCES ? -ENOMEM : lx_err(st);
    }
    if (r) {
        jam_handle_close(vmo);
        return r;
    }
    (void)drop_locked(addr, addr + len, DROP_REPLACED);   /* the kernel did; room checked */
    if (populate)
        (void)jam_vmo_commit(vmo, src->vmo_off, len);   /* a hint: pages still come on touch */
    struct mt_entry e = { addr, len, src->vmo_off, vmo, src->kind };
    mt_insert(&e);
    return 0;
}

/* Is [addr, end) all private anonymous memory libc made, JIT memory or
 * not as kind says (MT_ANON, alone or with MT_JIT): entries of that kind
 * one after another, no hole between them? */
static bool all_anon(uint64_t addr, uint64_t end, uint32_t kind)
{
    const struct mt_entry *t = mt_entries();
    if (end <= addr)
        return false;   /* wrapped */
    for (unsigned i = mt_first_after(addr); addr < end; i++) {
        if (i >= mt_count() || t[i].base > addr || (t[i].kind & (MT_ANON | MT_JIT)) != kind)
            return false;
        addr = t[i].base + t[i].len;
    }
    return true;
}

/* MAP_FIXED anonymous over all_anon memory (V8's and PartitionAlloc's
 * decommit and re-map inside a reservation): the same VMOs stay, with
 * their pages in the range decommitted (they read as zero after, as a
 * fresh mapping's do) and the range given perms. No VMO, table entry or
 * handle is made, so a reservation committed and decommitted a thousand
 * times costs no more than once. The protect goes first: it is the step
 * that can fail (the address space's cap on mappings, the job's charge
 * for them), and then nothing has changed; the decommit after it can't
 * (the VMOs are ours). Another thread finds the range mapped throughout:
 * its old pages until the decommit, zero pages after. */
static long refresh_locked(uint64_t addr, uint64_t len, uint32_t perms, bool populate)
{
    status_t st = jam_vmar_protect(lx_self_vmar(), addr, len, perms);
    if (st != OK)
        return st == ERR_NO_RESOURCES || st == ERR_NOT_FOUND ? -ENOMEM : lx_err(st);
    parts_locked(addr, addr + len, 0, jam_vmo_decommit);
    if (populate)
        parts_locked(addr, addr + len, 0, jam_vmo_commit);   /* a hint: pages still come on touch */
    return 0;
}

/* mmap's checks; 0 or -errno. */
static long mmap_check(long addr, long len, long prot, long flags, long off)
{
    if (len <= 0 || !page_round((uint64_t)len) || !page_aligned((uint64_t)off))
        return -EINVAL;
    if ((flags & ~(long)MAP_KNOWN) || !(flags & MAP_TYPE) || (flags & MAP_TYPE) > MAP_SHARED_VALIDATE)
        return -EINVAL;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && !page_aligned((uint64_t)addr))
        return -EINVAL;
    if ((flags & MAP_JIT) && ((flags & MAP_TYPE) != MAP_PRIVATE || !(flags & MAP_ANONYMOUS)))
        return -EINVAL;   /* JIT memory is private anonymous memory */
    uint32_t perms;
    return vmar_perms(prot, flags & MAP_JIT, &perms);
}

/* The table's kind for a mapping made with mmap's flags. */
static uint32_t mt_kind(long flags)
{
    if ((flags & MAP_TYPE) != MAP_PRIVATE)
        return MT_SHARED;
    if (flags & MAP_JIT)
        return MT_ANON | MT_JIT;
    return flags & MAP_ANONYMOUS ? MT_ANON : 0;
}

/* ---- files: a descriptor's mapping, write added over a file's pages, msync ------------ */

/* mmap of descriptor fd (file.c's file_map) as a mapping's source. */
static long file_source(long fd, long off, uint64_t size, long prot, long flags,
                        struct map_src *out)
{
    struct file_mapping m;
    uint32_t how = ((flags & MAP_TYPE) != MAP_PRIVATE ? FMAP_SHARED : 0) |
                   (prot & PROT_WRITE ? FMAP_WRITE : 0);
    long r = file_map(fd, off, size, how, &m);
    if (!r)
        *out = (struct map_src){ m.vmo, m.vmo_off,
                                 mt_kind(flags) | (m.pager_ro ? MT_PAGER_PRIVATE : 0) };
    return r;
}

/* The first entry in [addr, end) mapped straight on a file's pages
 * privately (MT_PAGER_PRIVATE), or mt_count(). */
static unsigned next_pager_private(uint64_t addr, uint64_t end)
{
    const struct mt_entry *t = mt_entries();
    unsigned i = mt_first_after(addr);
    while (i < mt_count() && t[i].base < end && !(t[i].kind & MT_PAGER_PRIVATE))
        i++;
    return i < mt_count() && t[i].base < end ? i : mt_count();
}

/* mprotect adding write over [addr, end): each part of a private mapping
 * straight on a file's pages becomes a copy-on-write child of the file's
 * VMO (its untouched pages still the file's, as before), mapped in its
 * place with perms in one step (VMAR_REPLACE): the file's VMO is never
 * writable. Charged in full now, as a writable private mapping is at
 * mmap: ENOMEM if the job can't pay. A part already replaced stays so if
 * a later one fails (as Linux may leave part of a range changed). */
static long unshare_locked(uint64_t addr, uint64_t end, uint32_t perms)
{
    for (unsigned i; (i = next_pager_private(addr, end)) < mt_count();) {
        const struct mt_entry *e = &mt_entries()[i];
        uint64_t s = e->base > addr ? e->base : addr;
        uint64_t f = e->base + e->len < end ? e->base + e->len : end;
        struct map_src child = { HANDLE_INVALID, 0, 0 };
        status_t st = jam_vmo_create_child(e->vmo, VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE,
                                           e->vmo_off + (s - e->base), f - s, &child.vmo);
        if (st != OK)
            return st == ERR_NO_MEMORY ? -ENOMEM : lx_err(st);
        long r = replace_locked(s, f - s, perms, false, &child);   /* takes child.vmo */
        if (r)
            return r;
        addr = f;
    }
    return 0;
}

/* Is all of [addr, end) JIT memory (MT_JIT entries, no hole)? */
static bool all_jit_locked(uint64_t addr, uint64_t end)
{
    return all_anon(addr, end, MT_ANON | MT_JIT);
}

/* Is all of [addr, end) libc's mappings? */
static bool covered_locked(uint64_t addr, uint64_t end)
{
    uint64_t hole_end;
    return !next_hole(&addr, end, &hole_end);
}

/* msync: nothing to write. A shared mapping of a file is read-only (the
 * pager never writes pages back: mmap refuses PROT_WRITE with it), a
 * memfd's pages are its file, and a private mapping's writes are never
 * the file's; what a program wrote with write() is on its medium already
 * (fat writes through). So only Linux's checks: the flags, the address,
 * and the range mapped (ENOMEM: here, libc's own mappings). */
long lx_msync(long addr, long len, long flags, long d, long e, long f)
{
    uint64_t size = page_round((uint64_t)len);
    if (!page_aligned((uint64_t)addr) || len < 0 || (len && !size) ||
        (uint64_t)addr > UINT64_MAX - size)
        return -EINVAL;
    if ((flags & ~(long)(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) ||
        ((flags & MS_ASYNC) && (flags & MS_SYNC)))
        return -EINVAL;
    lock_take(&mem_lock);
    bool whole = covered_locked((uint64_t)addr, (uint64_t)addr + size);
    lock_give(&mem_lock);
    return whole ? 0 : -ENOMEM;
}

long lx_mmap(long addr, long len, long prot, long flags, long fd, long off)
{
    long r = mmap_check(addr, len, prot, flags, off);
    if (r)
        return r;
    uint32_t perms;
    (void)vmar_perms(prot, flags & MAP_JIT, &perms);   /* checked above */
    uint64_t size = page_round((uint64_t)len), at = (uint64_t)addr;
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    struct map_src src = { HANDLE_INVALID, 0, mt_kind(flags) };
    if (src.kind & MT_SHARED)
        perms |= VMAR_CLONE_SHARE;   /* a forked child shares it (docs/FORK-PLAN.md 1.6) */
    if (!(flags & MAP_ANONYMOUS)) {   /* outside mem_lock: reading a file may malloc */
        r = file_source(fd, off, size, prot, flags, &src);
        if (r)
            return r;
    }
    lock_take(&mem_lock);
    if (!fixed || !table_overlaps(at, size)) {
        r = map_new(&at, size, perms, fixed, flags & MAP_POPULATE, &src);
    } else if ((flags & MAP_FIXED) && (src.kind & MT_ANON) && all_anon(at, at + size, src.kind)) {
        r = refresh_locked(at, size, perms, flags & MAP_POPULATE);
    } else if (flags & MAP_FIXED) {
        r = replace_locked(at, size, perms, flags & MAP_POPULATE, &src);
    } else {
        r = -EEXIST;
        if (src.vmo)
            jam_handle_close(src.vmo);
    }
    src.vmo = HANDLE_INVALID;
    if (r == -ENOMEM && (flags & MAP_FIXED_NOREPLACE))
        r = -EEXIST;
    lock_give(&mem_lock);
    return r ? r : (long)at;
}

long lx_munmap(long addr, long len, long c, long d, long e, long f)
{
    uint64_t size = page_round((uint64_t)len);
    if (!page_aligned((uint64_t)addr) || len <= 0 || !size || (uint64_t)addr > UINT64_MAX - size)
        return -EINVAL;
    lock_take(&mem_lock);
    long r = unmap_locked((uint64_t)addr, (uint64_t)addr + size);
    lock_give(&mem_lock);
    return r;
}

long lx_mprotect(long addr, long len, long prot, long d, long e, long f)
{
    uint64_t size = page_round((uint64_t)len);
    if (!page_aligned((uint64_t)addr) || len < 0 || (len && !size))
        return -EINVAL;
    uint32_t perms;
    bool jit = false;
    if ((prot & PROT_EXEC) && size && (uint64_t)addr <= UINT64_MAX - size) {
        lock_take(&mem_lock);   /* execute only in JIT memory */
        jit = all_jit_locked((uint64_t)addr, (uint64_t)addr + size);
        lock_give(&mem_lock);
    }
    long r = vmar_perms(prot, jit, &perms);
    if (r || !size)
        return r;
    if (perms & VMAR_WRITE) {   /* a private mapping on a file's pages gets its own first */
        lock_take(&mem_lock);
        r = mt_count() ? unshare_locked((uint64_t)addr, (uint64_t)addr + size, perms) : 0;
        lock_give(&mem_lock);
        if (r)
            return r;
    }
    status_t st = jam_vmar_protect(lx_self_vmar(), (uint64_t)addr, size, perms);
    if (st == ERR_NOT_FOUND)
        return -ENOMEM;   /* part of the range isn't mapped */
    return lx_err(st);
}

long lx_madvise(long addr, long len, long advice, long d, long e, long f)
{
    uint64_t size = page_round((uint64_t)len);
    if (!page_aligned((uint64_t)addr) || len < 0 || (len && !size) ||
        (uint64_t)addr > UINT64_MAX - size)
        return -EINVAL;
    switch (advice) {
    case MADV_DONTNEED:
    case MADV_FREE:
        lock_take(&mem_lock);
        parts_locked((uint64_t)addr, (uint64_t)addr + size, MT_SHARED, jam_vmo_decommit);
        lock_give(&mem_lock);
        return 0;
    case MADV_NORMAL: case MADV_RANDOM: case MADV_SEQUENTIAL: case MADV_WILLNEED:
    case MADV_DONTFORK: case MADV_DOFORK: case MADV_MERGEABLE: case MADV_UNMERGEABLE:
    case MADV_HUGEPAGE: case MADV_NOHUGEPAGE: case MADV_DONTDUMP: case MADV_DODUMP:
    case MADV_WIPEONFORK: case MADV_KEEPONFORK: case MADV_COLD: case MADV_PAGEOUT:
        return 0;   /* advice Jam OS has no use for */
    }
    return -EINVAL;
}

/* mlock and its kin: Jam OS never pages memory out, so every page is as
 * locked as it can be; only the arguments are checked, as Linux does. */
static long lock_range(long addr, long len)
{
    uint64_t a = (uint64_t)addr & ~(PAGE - 1);
    uint64_t size = page_round((uint64_t)len + ((uint64_t)addr - a));   /* its whole pages */
    if (len < 0 || (len && !size) || a > UINT64_MAX - size)
        return -EINVAL;
    return 0;
}

long lx_mlock(long addr, long len, long c, long d, long e, long f)
{
    return lock_range(addr, len);
}

long lx_munlock(long addr, long len, long c, long d, long e, long f)
{
    return lock_range(addr, len);
}

long lx_mlock2(long addr, long len, long flags, long d, long e, long f)
{
    return flags & ~(long)MLOCK_ONFAULT ? -EINVAL : lock_range(addr, len);
}

long lx_mlockall(long flags, long b, long c, long d, long e, long f)
{
    long known = MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT;
    return !flags || (flags & ~known) || flags == MCL_ONFAULT ? -EINVAL : 0;
}

long lx_munlockall(long a, long b, long c, long d, long e, long f)
{
    return 0;
}

long lx_mremap(long a, long b, long c, long d, long e, long f)
{
    return -ENOSYS;   /* callers (musl's realloc) copy instead */
}

/* The break's reservation, on the first brk. brk keeps a handle of its
 * own to the VMO: the table's may be closed (a munmap or a MAP_FIXED over
 * the whole break), and a closed handle's number can come back as another
 * object's. */
static bool brk_reserve(void)
{
    uint64_t addr = 0;
    const struct map_src fresh = { HANDLE_INVALID, 0, 0 };
    if (map_new(&addr, BRK_SIZE, VMAR_READ | VMAR_WRITE, false, false, &fresh))
        return false;
    handle_t vmo = mt_entries()[mt_first_after(addr)].vmo;
    if (jam_handle_duplicate(vmo, RIGHT_SAME, &brk_vmo) != OK) {
        (void)unmap_locked(addr, addr + BRK_SIZE);   /* ours, just made: can't fail */
        return false;
    }
    brk_base = brk_cur = addr;
    return true;
}

long lx_brk(long want, long b, long c, long d, long e, long f)
{
    lock_take(&mem_lock);
    if (!brk_base && !brk_reserve()) {
        lock_give(&mem_lock);
        return 0;   /* no break at all: musl's malloc uses mmap alone */
    }
    uint64_t to = (uint64_t)want;
    /* The break grows no further than RLIMIT_DATA (libc-test's memfill sets it to 0). */
    bool room = to <= brk_cur || to - brk_base <= rlimit_cur(RLIMIT_DATA);
    if (to >= brk_base && to <= brk_base + BRK_SIZE && room) {
        uint64_t keep = page_round(to), had = page_round(brk_cur);
        if (keep < had)
            (void)jam_vmo_decommit(brk_vmo, keep - brk_base, had - keep);   /* give pages back */
        brk_cur = to;
    }
    long r = (long)brk_cur;   /* the break, moved or not (Linux's answer either way) */
    lock_give(&mem_lock);
    return r;
}

uint32_t mem_kind_at(uint64_t addr)
{
    lock_take(&mem_lock);
    unsigned i = mt_first_after(addr);
    uint32_t kind = MEM_UNKNOWN;
    if (i < mt_count() && mt_entries()[i].base <= addr) {
        uint32_t k = mt_entries()[i].kind;
        kind = (k & MT_SHARED ? MEM_SHARED : 0) | (k & MT_PAGER_PRIVATE ? MEM_FILE : 0) |
               (brk_base && addr >= brk_base && addr < brk_base + BRK_SIZE ? MEM_BRK : 0);
    }
    lock_give(&mem_lock);
    return kind;
}
