/* Memory (docs/M13-PLAN.md section 12): anonymous mmap, munmap, mprotect,
 * madvise and brk, over VMOs and the address space's vmar calls.
 *
 * Each mmap is a VMO of its own, mapped with vmar_map and kept in libc's
 * table (maptable.h) with its handle: munmap of part of it unmaps that
 * part (the kernel splits the mapping, the table splits the entry), and
 * the VMO goes when nothing maps it any more; madvise(MADV_DONTNEED) and
 * MADV_FREE decommit its pages (they read as zero after, as Linux's do).
 * Pages are committed as they are touched and charged to the process's
 * job then.
 *
 * What the address space allows: never write with execute, and no
 * execute on anonymous memory at all (EACCES: no JIT). A descriptor's
 * mapping is file.c's file_map: MAP_PRIVATE a copy of the file (or
 * memfd) in a VMO of its own, MAP_SHARED a memfd's own VMO (of a file:
 * ENODEV until M12.5's pager); either is then mapped and kept in the
 * table as an anonymous one is. mremap is ENOSYS
 * (musl's realloc copies); mlock and its kin succeed (nothing is paged
 * out). MAP_FIXED over the table's mappings replaces
 * them in one step (vmar_map's VMAR_REPLACE): another thread reading the
 * range sees the old pages or the new ones, never a hole. The holes
 * between them are filled with the new mapping's pages first, as plain
 * fixed maps, so one that holds anything else (the program, its stack,
 * libjam's mappings) fails the call with ENOMEM before anything changed:
 * the replace never takes away what libc doesn't own. MAP_FIXED where
 * the table has nothing is a plain fixed map (ENOMEM over anything else),
 * and MAP_FIXED_NOREPLACE over anything is EEXIST.
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
                   MAP_FIXED_NOREPLACE)
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

/* PROT_* as vmar permissions; -EACCES for execute, -EINVAL for a bit we
 * don't know. Write needs read on x86. */
static long vmar_perms(long prot, uint32_t *out)
{
    if (prot & ~(long)PROT_KNOWN)
        return -EINVAL;
    if (prot & PROT_EXEC)
        return -EACCES;
    *out = (prot & (PROT_READ | PROT_WRITE) ? VMAR_READ : 0) | (prot & PROT_WRITE ? VMAR_WRITE : 0);
    return 0;
}

/* Does [addr, addr + len) overlap one of the table's entries? */
static bool table_overlaps(uint64_t addr, uint64_t len)
{
    unsigned i = mt_first_after(addr);
    return i < mt_count() && mt_entries()[i].base < addr + len;
}

/* Unmap the table's mappings in [addr, end): each entry's part there,
 * trimming, splitting or dropping the entry. A split needs a free entry,
 * checked before anything changes. With kernel false the address space
 * is left alone (it is being unmapped by someone else: mem_forget). */
static long drop_locked(uint64_t addr, uint64_t end, bool kernel)
{
    struct mt_entry *t = mt_entries();
    unsigned i = mt_first_after(addr);
    if (i < mt_count() && t[i].base < addr && t[i].base + t[i].len > end && !mt_room(1))
        return -ENOMEM;
    while (i < mt_count() && t[i].base < end) {
        struct mt_entry e = t[i];
        uint64_t s = e.base > addr ? e.base : addr;
        uint64_t f = e.base + e.len < end ? e.base + e.len : end;
        status_t st = kernel ? jam_vmar_unmap(lx_self_vmar(), s, f - s) : OK;
        if (st != OK)
            return lx_err(st);
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
                struct mt_entry tail = { f, e.base + e.len - f, e.vmo_off + (f - e.base), e.vmo, 0 };
                mt_insert(&tail);
            }
        }
        i++;
    }
    return 0;
}

static long unmap_locked(uint64_t addr, uint64_t end)
{
    return drop_locked(addr, end, true);
}

void mem_forget(uint64_t addr, uint64_t len)
{
    if (!len || addr > UINT64_MAX - len)
        return;
    lock_take(&mem_lock);
    (void)drop_locked(addr, addr + len, false);   /* a split's room: a stack is one entry */
    lock_give(&mem_lock);
}

/* What a mapping maps: a VMO made for it (vmo HANDLE_INVALID: a new
 * anonymous one) or a descriptor's (file_map), from vmo_off. */
struct map_src {
    handle_t vmo;
    uint64_t vmo_off;
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
    status_t st = vmo ? OK : jam_vmo_create(len, 0, &vmo);
    if (st != OK)
        return lx_err(st);
    st = jam_vmar_map(lx_self_vmar(), vmo, src->vmo_off, len, perms | (fixed ? VMAR_FIXED : 0),
                      addr);
    if (st != OK) {
        jam_handle_close(vmo);
        return st == ERR_ALREADY_BOUND || st == ERR_NO_RESOURCES ? -ENOMEM : lx_err(st);
    }
    if (populate)
        (void)jam_vmo_commit(vmo, src->vmo_off, len);   /* a hint: pages still come on touch */
    struct mt_entry e = { *addr, len, src->vmo_off, vmo, 0 };
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
    status_t st = vmo ? OK : jam_vmo_create(len, 0, &vmo);
    if (st != OK)
        return lx_err(st);
    long r = fill_holes(addr, addr + len, vmo, src->vmo_off, perms);
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
    (void)drop_locked(addr, addr + len, false);   /* the kernel unmapped them; room checked */
    if (populate)
        (void)jam_vmo_commit(vmo, src->vmo_off, len);   /* a hint: pages still come on touch */
    struct mt_entry e = { addr, len, src->vmo_off, vmo, 0 };
    mt_insert(&e);
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
    uint32_t perms;
    return vmar_perms(prot, &perms);
}

long lx_mmap(long addr, long len, long prot, long flags, long fd, long off)
{
    long r = mmap_check(addr, len, prot, flags, off);
    if (r)
        return r;
    uint32_t perms;
    (void)vmar_perms(prot, &perms);   /* checked above */
    uint64_t size = page_round((uint64_t)len), at = (uint64_t)addr;
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    struct map_src src = { HANDLE_INVALID, 0 };
    if (!(flags & MAP_ANONYMOUS)) {   /* outside mem_lock: reading a file may malloc */
        bool shared = (flags & MAP_TYPE) != MAP_PRIVATE;
        r = file_map(fd, off, size, shared, &src.vmo, &src.vmo_off);
        if (r)
            return r;
    }
    lock_take(&mem_lock);
    if (!fixed || !table_overlaps(at, size)) {
        r = map_new(&at, size, perms, fixed, flags & MAP_POPULATE, &src);
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
    long r = vmar_perms(prot, &perms);
    if (r || !size)
        return r;
    status_t st = jam_vmar_protect(lx_self_vmar(), (uint64_t)addr, size, perms);
    if (st == ERR_NOT_FOUND)
        return -ENOMEM;   /* part of the range isn't mapped */
    return lx_err(st);
}

/* Decommit the table's pages in [addr, end). */
static void decommit_locked(uint64_t addr, uint64_t end)
{
    const struct mt_entry *t = mt_entries();
    for (unsigned i = mt_first_after(addr); i < mt_count() && t[i].base < end; i++) {
        uint64_t s = t[i].base > addr ? t[i].base : addr;
        uint64_t f = t[i].base + t[i].len < end ? t[i].base + t[i].len : end;
        (void)jam_vmo_decommit(t[i].vmo, t[i].vmo_off + (s - t[i].base), f - s);   /* advice */
    }
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
        if (mt_count())
            decommit_locked((uint64_t)addr, (uint64_t)addr + size);
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
    const struct map_src fresh = { HANDLE_INVALID, 0 };
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
